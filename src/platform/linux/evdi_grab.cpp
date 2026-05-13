/**
 * @file src/platform/linux/evdi_grab.cpp
 * @brief CPU-side capture backend that consumes frames from an EVDI virtual
 *        display via libevdi. Pairs with virtual_display.cpp, which owns the
 *        EVDI device, EDID, and registered buffer; this file just drives the
 *        capture hot loop and hands buffers to Apollo's encoder pipeline.
 *
 *        Why this exists: cosmic-comp page-flips to the EVDI DRM card, but
 *        the kernel needs a userspace consumer of those flips. Without one,
 *        flips fail with EBUSY and the EVDI head becomes inert. KMS capture
 *        of the EVDI card therefore can't work — the kernel never has fresh
 *        pixels to expose via DMA-BUF. libevdi is the only path that
 *        actually consumes the rendered framebuffer.
 */

// standard includes
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <thread>
#include <tuple>
#include <unistd.h>
#include <vector>

// local includes
#include "src/config.h"
#include "src/input.h"
#include "src/logging.h"
#include "src/rtsp.h"
#include "src/platform/common.h"
#include "src/stream.h"
#include "src/video.h"
#include "virtual_display.h"

using namespace std::literals;

namespace platf {

  namespace {
    // CPU-side image that owns its own BGRA pixel buffer. memcpy'd from the
    // VDISPLAY-owned capture buffer each frame so that the encoder pipeline
    // can hold onto images independently of the EVDI consumer loop.
    struct evdi_img_t: public img_t {
      ~evdi_img_t() override {
        delete[] data;
        data = nullptr;
      }
    };

    struct CompositorLayout {
      int evdi_offset_x;
      int evdi_offset_y;
      int total_width;   ///< max(x + width) across all enabled outputs
      int total_height;  ///< max(y + height) across all enabled outputs
      std::string evdi_output_name;  ///< cosmic-comp's name for the EVDI head (e.g. "DVI-I-2")
      int evdi_current_width;
      int evdi_current_height;
      std::vector<std::string> other_enabled_outputs;  ///< names of every non-EVDI enabled output
    };

    // Parse a single output block's "position X Y" line.
    bool parse_position_line(const std::string &block, int &x, int &y) {
      size_t p = block.find("position ");
      if (p == std::string::npos) {
        return false;
      }
      p += sizeof("position ") - 1;
      char *end = nullptr;
      long lx = std::strtol(block.c_str() + p, &end, 10);
      if (!end || end == block.c_str() + p) {
        return false;
      }
      long ly = std::strtol(end, &end, 10);
      x = static_cast<int>(lx);
      y = static_cast<int>(ly);
      return true;
    }

    // Parse the first "mode W H ... current=#true" line in the block.
    bool parse_current_mode(const std::string &block, int &w, int &h) {
      size_t p = 0;
      while (p < block.size()) {
        size_t mode_pos = block.find("mode ", p);
        if (mode_pos == std::string::npos) {
          return false;
        }
        size_t eol = block.find('\n', mode_pos);
        std::string line = block.substr(mode_pos, eol - mode_pos);
        if (line.find("current=#true") != std::string::npos) {
          char *end = nullptr;
          long lw = std::strtol(line.c_str() + 5, &end, 10);  // skip "mode "
          if (!end || end == line.c_str() + 5) {
            return false;
          }
          long lh = std::strtol(end, &end, 10);
          w = static_cast<int>(lw);
          h = static_cast<int>(lh);
          return true;
        }
        p = eol == std::string::npos ? block.size() : eol;
      }
      return false;
    }

    // Walks cosmic-randr's KDL output and pulls out:
    // - the EVDI head's position (identified by the "APOLLO VDISP" EDID model)
    // - the total compositor desktop extent (so abs-pointer events scale to
    //   the whole desktop and we can offset into the EVDI region)
    bool find_compositor_layout(CompositorLayout &out) {
      FILE *pipe = ::popen("timeout 3 cosmic-randr list --kdl 2>/dev/null", "r");
      if (!pipe) {
        return false;
      }

      std::string buf;
      std::array<char, 4096> chunk;
      while (auto n = ::fread(chunk.data(), 1, chunk.size(), pipe)) {
        buf.append(chunk.data(), n);
      }
      int rc = ::pclose(pipe);
      if (rc != 0 || buf.empty()) {
        return false;
      }

      bool found_evdi = false;
      int total_w = 0;
      int total_h = 0;

      size_t pos = 0;
      while (pos < buf.size()) {
        size_t output_pos = buf.find("output \"", pos);
        if (output_pos == std::string::npos) {
          break;
        }
        size_t name_start = output_pos + sizeof("output \"") - 1;
        size_t name_end = buf.find('"', name_start);
        std::string output_name = (name_end != std::string::npos) ?
                                    buf.substr(name_start, name_end - name_start) :
                                    std::string {};
        size_t brace_open = buf.find('{', output_pos);
        if (brace_open == std::string::npos) {
          break;
        }
        size_t brace_close = buf.find("}\n", brace_open);
        if (brace_close == std::string::npos) {
          brace_close = buf.find('}', brace_open);
        }
        if (brace_close == std::string::npos) {
          break;
        }
        std::string block = buf.substr(brace_open, brace_close - brace_open);

        int x = 0, y = 0, w = 0, h = 0;
        bool have_pos = parse_position_line(block, x, y);
        bool have_mode = parse_current_mode(block, w, h);

        // Only enabled outputs contribute to the desktop extent. KDL marks
        // disabled outputs explicitly; treat anything we can parse position
        // and mode for as enabled enough.
        if (have_pos && have_mode) {
          total_w = std::max(total_w, x + w);
          total_h = std::max(total_h, y + h);
        }

        bool is_evdi = block.find("APOLLO VDISP") != std::string::npos;

        if (is_evdi) {
          if (!found_evdi && have_pos) {
            out.evdi_offset_x = x;
            out.evdi_offset_y = y;
            out.evdi_output_name = output_name;
            out.evdi_current_width = w;
            out.evdi_current_height = h;
            found_evdi = true;
          }
        } else if (have_pos && have_mode && !output_name.empty()) {
          // Track non-EVDI enabled outputs so the caller can choose to
          // disable them while streaming (so the cursor is forced onto the
          // EVDI). Only tracks outputs that look enabled (have both a
          // position and a current mode in the KDL output).
          out.other_enabled_outputs.push_back(output_name);
        }

        pos = brace_close + 1;
      }

      out.total_width = total_w;
      out.total_height = total_h;
      return found_evdi && total_w > 0 && total_h > 0;
    }

    // Tell cosmic-comp to switch the EVDI output to the desired mode if it
    // isn't already there. cosmic-comp tends to remember the last mode it
    // set on an output and ignore the EDID's "preferred" mode on subsequent
    // hotplugs, so we have to nudge it explicitly.
    bool force_evdi_mode(const std::string &output_name, int width, int height) {
      if (output_name.empty() || width <= 0 || height <= 0) {
        return false;
      }
      char cmd[256];
      std::snprintf(cmd, sizeof(cmd),
                    "cosmic-randr mode \"%s\" %d %d 2>/dev/null",
                    output_name.c_str(), width, height);
      int rc = std::system(cmd);
      if (rc != 0) {
        BOOST_LOG(warning) << "[evdi_grab] cosmic-randr mode '" << output_name
                            << " " << width << "x" << height
                            << "' returned " << rc;
        return false;
      }
      return true;
    }

    // Persistent state file: a newline-separated list of output names we
    // disabled during a streaming session. Written when we disable, deleted
    // when we restore on clean exit. Read at init() to recover from a
    // previous session that didn't restore (apollo crash, unclean reboot).
    // Lives in ~/.cache so it survives across apollo restarts but not full
    // disk loss.
    std::string disabled_state_file() {
      const char *home = std::getenv("HOME");
      if (!home) return {};
      std::string path = std::string(home) + "/.cache/apollo";
      ::mkdir(path.c_str(), 0700);
      return path + "/disabled-outputs";
    }

    void persist_disabled_outputs(const std::vector<std::string> &outputs) {
      auto path = disabled_state_file();
      if (path.empty()) return;
      FILE *f = std::fopen(path.c_str(), "w");
      if (!f) return;
      for (const auto &name : outputs) {
        std::fprintf(f, "%s\n", name.c_str());
      }
      std::fclose(f);
    }

    void clear_disabled_state() {
      auto path = disabled_state_file();
      if (!path.empty()) std::remove(path.c_str());
    }

    std::vector<std::string> read_disabled_state() {
      std::vector<std::string> result;
      auto path = disabled_state_file();
      if (path.empty()) return result;
      FILE *f = std::fopen(path.c_str(), "r");
      if (!f) return result;
      char buf[256];
      while (std::fgets(buf, sizeof(buf), f)) {
        std::string line(buf);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        if (!line.empty()) result.push_back(line);
      }
      std::fclose(f);
      return result;
    }

    // Second state file: the most recent snapshot of physical outputs
    // observed enabled at session init. This is the *desired* post-stream
    // state. Two reasons it's separate from disabled_state_file():
    //
    //  1. It refreshes every time a session start sees a healthy layout
    //     with physicals enabled. So a user adding a new monitor doesn't
    //     have to restart apollo — the first stream after the hotplug
    //     observes the new layout and the snapshot picks it up.
    //
    //  2. The destructor's restore unions this snapshot with the
    //     session-local disabled list. If a previous session crashed and
    //     left physicals off, the current session sees an empty layout
    //     (find_compositor_layout's other_enabled_outputs is empty), but
    //     the snapshot survives across crashes and rescues the restore.
    std::string known_physicals_state_file() {
      const char *home = std::getenv("HOME");
      if (!home) return {};
      std::string path = std::string(home) + "/.cache/apollo";
      ::mkdir(path.c_str(), 0700);
      return path + "/known-physicals";
    }

    void persist_known_physicals(const std::vector<std::string> &outputs) {
      auto path = known_physicals_state_file();
      if (path.empty() || outputs.empty()) return;
      FILE *f = std::fopen(path.c_str(), "w");
      if (!f) return;
      for (const auto &name : outputs) {
        std::fprintf(f, "%s\n", name.c_str());
      }
      std::fclose(f);
    }

    std::vector<std::string> read_known_physicals() {
      std::vector<std::string> result;
      auto path = known_physicals_state_file();
      if (path.empty()) return result;
      FILE *f = std::fopen(path.c_str(), "r");
      if (!f) return result;
      char buf[256];
      while (std::fgets(buf, sizeof(buf), f)) {
        std::string line(buf);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        if (!line.empty()) result.push_back(line);
      }
      std::fclose(f);
      return result;
    }

    // Cosmic-comp's auto-tiling rewrites window geometry when windows
    // map, which breaks fullscreen-borderless games launched via
    // gamescope on the EVDI head — the game's window comes up tiled
    // instead of fullscreen, the gamescope viewport is wrong, and
    // streaming clients see a smashed-up frame.
    //
    // Two config knobs together control this:
    //   - autotile (bool): whether tiling is on
    //   - autotile_behavior: "PerWorkspace" tracks per-workspace state,
    //     "Global" makes the bool above apply to every workspace live.
    //
    // Just flipping `autotile` to false isn't enough when behavior is
    // PerWorkspace: any workspaces that already exist (including the
    // freshly-allocated one for the EVDI head, which cosmic-comp
    // creates *before* we hit the 1.5s gate) keep their per-workspace
    // tiling=true. Flipping behavior to Global at the same time forces
    // the bool to apply globally and immediately untiles every
    // workspace including the EVDI one.
    //
    // cosmic-comp watches both files via inotify, so writes propagate
    // to the running compositor immediately — no IPC, no restart
    // needed.
    std::string cosmic_autotile_config_path() {
      const char *home = std::getenv("HOME");
      if (!home) return {};
      return std::string(home) + "/.config/cosmic/com.system76.CosmicComp/v1/autotile";
    }

    std::string cosmic_autotile_behavior_config_path() {
      const char *home = std::getenv("HOME");
      if (!home) return {};
      return std::string(home) + "/.config/cosmic/com.system76.CosmicComp/v1/autotile_behavior";
    }

    std::string cosmic_tiling_state_file() {
      const char *home = std::getenv("HOME");
      if (!home) return {};
      std::string path = std::string(home) + "/.cache/apollo";
      ::mkdir(path.c_str(), 0700);
      return path + "/prev-autotile";
    }

    std::string cosmic_tiling_behavior_state_file() {
      const char *home = std::getenv("HOME");
      if (!home) return {};
      std::string path = std::string(home) + "/.cache/apollo";
      ::mkdir(path.c_str(), 0700);
      return path + "/prev-autotile-behavior";
    }

    // Read the file at `path` as a small text payload. Returns empty
    // string if the file is missing or unreadable.
    std::string read_small_file(const std::string &path) {
      FILE *f = std::fopen(path.c_str(), "r");
      if (!f) return {};
      char buf[32];
      size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
      std::fclose(f);
      if (n == 0) return {};
      buf[n] = '\0';
      return std::string(buf, n);
    }

    bool write_small_file(const std::string &path, std::string_view data) {
      FILE *f = std::fopen(path.c_str(), "w");
      if (!f) return false;
      bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
      std::fclose(f);
      return ok;
    }

    // Disable cosmic-comp's autotile for the stream. Backs up both
    // `autotile` and `autotile_behavior` so the destructor can put
    // them back exactly as the user had them. Then writes
    // autotile=false and autotile_behavior=Global; cosmic-comp's
    // inotify picks both up and untiles every workspace immediately,
    // including the freshly-created EVDI workspace.
    bool cosmic_tiling_disable() {
      auto autotile_cfg = cosmic_autotile_config_path();
      auto autotile_state = cosmic_tiling_state_file();
      auto behavior_cfg = cosmic_autotile_behavior_config_path();
      auto behavior_state = cosmic_tiling_behavior_state_file();
      if (autotile_cfg.empty() || autotile_state.empty() ||
          behavior_cfg.empty() || behavior_state.empty()) {
        return false;
      }

      // Snapshot, but only if no prior session left one behind. If a
      // second client joins mid-stream (or any other path re-enters
      // disable before restore has run), the on-disk autotile config
      // is already "false"/"Global" from the first disable. Re-
      // snapshotting would clobber the user's real pre-session value
      // with that "false", and the eventual restore would leave
      // autotile turned off. The first disable's snapshot wins; the
      // matching restore deletes the state files at session end,
      // resetting the cycle for the next session.
      //
      // cosmic-comp's defaults when config files are missing:
      // autotile=true, autotile_behavior=PerWorkspace.
      if (::access(autotile_state.c_str(), F_OK) != 0) {
        std::string current_autotile = read_small_file(autotile_cfg);
        if (current_autotile.empty()) current_autotile = "true";
        if (!write_small_file(autotile_state, current_autotile)) return false;
      }

      if (::access(behavior_state.c_str(), F_OK) != 0) {
        std::string current_behavior = read_small_file(behavior_cfg);
        if (current_behavior.empty()) current_behavior = "PerWorkspace";
        if (!write_small_file(behavior_state, current_behavior)) return false;
      }

      // Order matters slightly: flip behavior FIRST so the live bool
      // change is what cosmic-comp acts on globally, not whatever the
      // per-workspace caches held.
      if (!write_small_file(behavior_cfg, "Global")) return false;
      if (!write_small_file(autotile_cfg, "false")) return false;
      return true;
    }

    // Restore both `autotile` and `autotile_behavior` from their
    // backup state files. No-op when state files are missing (the
    // common case at apollo startup after a clean shutdown).
    bool cosmic_tiling_restore() {
      auto autotile_cfg = cosmic_autotile_config_path();
      auto autotile_state = cosmic_tiling_state_file();
      auto behavior_cfg = cosmic_autotile_behavior_config_path();
      auto behavior_state = cosmic_tiling_behavior_state_file();
      if (autotile_cfg.empty() || autotile_state.empty() ||
          behavior_cfg.empty() || behavior_state.empty()) {
        return false;
      }

      // Nothing to do if neither state file is present.
      bool have_autotile_state = ::access(autotile_state.c_str(), F_OK) == 0;
      bool have_behavior_state = ::access(behavior_state.c_str(), F_OK) == 0;
      if (!have_autotile_state && !have_behavior_state) return true;

      // Restore behavior FIRST so when autotile flips back to its
      // saved value, per-workspace caches don't get a brief "Global=
      // true" moment that retiles every existing workspace.
      bool ok = true;
      if (have_behavior_state) {
        std::string backed_up = read_small_file(behavior_state);
        if (backed_up.empty()) backed_up = "PerWorkspace";
        if (write_small_file(behavior_cfg, backed_up)) {
          std::remove(behavior_state.c_str());
        } else {
          ok = false;
        }
      }
      if (have_autotile_state) {
        std::string backed_up = read_small_file(autotile_state);
        if (backed_up.empty()) backed_up = "true";
        if (write_small_file(autotile_cfg, backed_up)) {
          std::remove(autotile_state.c_str());
        } else {
          ok = false;
        }
      }
      return ok;
    }

    // Mode snapshot: at disable time we record the *current* mode of each
    // output we're disabling, so that on restore we can re-set that exact
    // mode rather than falling back to whatever cosmic-comp considers
    // "preferred". The Samsung's EDID-preferred is 3840x1080 while the user
    // runs it at its native 5120x1440 — preferred-mode fallback put them
    // back at 3840x1080 after each stream.
    //
    // Format: one line per output, `NAME WIDTH HEIGHT REFRESH_MILLIHZ`.
    // Refresh is in milliHz to match cosmic-randr's KDL mode-line format
    // (e.g. 119999 for 119.999 Hz).
    struct ModeSnapshot {
      std::string name;
      int w {0};
      int h {0};
      int refresh {0};  ///< milliHz
    };

    std::string disabled_modes_state_file() {
      const char *home = std::getenv("HOME");
      if (!home) return {};
      std::string path = std::string(home) + "/.cache/apollo";
      ::mkdir(path.c_str(), 0700);
      return path + "/disabled-output-modes";
    }

    void persist_disabled_modes(const std::vector<ModeSnapshot> &modes) {
      auto path = disabled_modes_state_file();
      if (path.empty() || modes.empty()) return;
      FILE *f = std::fopen(path.c_str(), "w");
      if (!f) return;
      for (const auto &m : modes) {
        std::fprintf(f, "%s %d %d %d\n", m.name.c_str(), m.w, m.h, m.refresh);
      }
      std::fclose(f);
    }

    std::vector<ModeSnapshot> read_disabled_modes() {
      std::vector<ModeSnapshot> result;
      auto path = disabled_modes_state_file();
      if (path.empty()) return result;
      FILE *f = std::fopen(path.c_str(), "r");
      if (!f) return result;
      char buf[256];
      while (std::fgets(buf, sizeof(buf), f)) {
        char name[128];
        int w = 0, h = 0, r = 0;
        if (std::sscanf(buf, "%127s %d %d %d", name, &w, &h, &r) == 4) {
          result.push_back({name, w, h, r});
        }
      }
      std::fclose(f);
      return result;
    }

    void clear_disabled_modes() {
      auto path = disabled_modes_state_file();
      if (!path.empty()) std::remove(path.c_str());
    }

    // Pull the *current* mode of each named output from cosmic-randr's KDL
    // listing. Used at disable time to record what to restore to. Outputs
    // without a current=#true mode in the listing (already disabled, or
    // freshly hotplugged) are simply omitted from the result.
    std::vector<ModeSnapshot> capture_current_modes(
        const std::vector<std::string> &names) {
      std::vector<ModeSnapshot> result;
      if (names.empty()) return result;
      std::string kdl;
      if (FILE *p = ::popen("timeout 3 cosmic-randr list --kdl 2>/dev/null", "r")) {
        std::array<char, 4096> chunk;
        while (auto n = ::fread(chunk.data(), 1, chunk.size(), p)) {
          kdl.append(chunk.data(), n);
        }
        ::pclose(p);
      }
      if (kdl.empty()) return result;
      for (const auto &name : names) {
        std::string header = "output \"" + name + "\"";
        size_t out_pos = kdl.find(header);
        if (out_pos == std::string::npos) continue;
        size_t modes_pos = kdl.find("modes {", out_pos);
        if (modes_pos == std::string::npos) continue;
        size_t modes_close = kdl.find("}", modes_pos);
        if (modes_close == std::string::npos) continue;
        size_t cur = kdl.find("current=#true", modes_pos);
        if (cur == std::string::npos || cur > modes_close) continue;
        // Walk back to start of the line so sscanf has the right slice.
        size_t line_start = kdl.rfind('\n', cur);
        line_start = (line_start == std::string::npos) ? 0 : line_start + 1;
        int w = 0, h = 0, r = 0;
        // Line shape: "    mode W H R [current=#true] [preferred=#true]"
        if (std::sscanf(kdl.c_str() + line_start, " mode %d %d %d", &w, &h, &r) == 3) {
          result.push_back({name, w, h, r});
        }
      }
      return result;
    }

    // Build a mutated KDL output config from `cosmic-randr list --kdl`,
    // forcing the `enabled=` flag for each named output in the disable
    // and enable sets, and pipe the result to `cosmic-randr kdl`. This
    // sends the entire desired state as ONE wlr-output-management IPC
    // request, so cosmic-comp processes the full transition under a
    // single backend.lock() acquisition. Separate `cosmic-randr enable`
    // / `disable` commands issue separate IPC requests, which leaves a
    // window between them where cosmic-comp's render scheduler can
    // contend for the same lock and deadlock — see the plan notes for
    // the freeze investigation.
    //
    // Returns true on success, false if the safety gate refused (would
    // leave 0 enabled outputs) or if cosmic-randr failed.
    bool apply_output_kdl_sync(const std::vector<std::string> &disable_set,
                                const std::vector<std::string> &enable_set,
                                const std::vector<ModeSnapshot> &mode_override = {}) {
      // Read current state.
      std::string current;
      if (FILE *p = ::popen("timeout 3 cosmic-randr list --kdl 2>/dev/null", "r")) {
        std::array<char, 4096> chunk;
        while (auto n = ::fread(chunk.data(), 1, chunk.size(), p)) {
          current.append(chunk.data(), n);
        }
        ::pclose(p);
      }
      if (current.empty()) {
        BOOST_LOG(warning) << "[evdi_grab] cosmic-randr list --kdl returned empty";
        return false;
      }

      auto in_set = [](const std::vector<std::string> &set, std::string_view name) {
        for (const auto &s : set) {
          if (s == name) return true;
        }
        return false;
      };

      // Walk lines. For every `output "NAME" enabled=...` header line,
      // rewrite the enabled= flag if NAME is in either set, and count
      // resulting enabled outputs for the safety gate.
      std::string out;
      out.reserve(current.size() + 64);
      int enabled_count = 0;
      size_t pos = 0;
      while (pos <= current.size()) {
        size_t eol = current.find('\n', pos);
        bool last = (eol == std::string::npos);
        std::string_view line {current.data() + pos, (last ? current.size() : eol) - pos};

        std::string mutated {line};
        if (line.substr(0, 8) == "output \"") {
          size_t name_end = line.find('"', 8);
          if (name_end != std::string_view::npos) {
            std::string_view name = line.substr(8, name_end - 8);
            bool want_disable = in_set(disable_set, name);
            bool want_enable = in_set(enable_set, name);
            // Replace the enabled=#... token in this line.
            size_t eq = mutated.find("enabled=#");
            if (eq != std::string::npos) {
              size_t end = mutated.find_first_of(" {", eq);
              if (end == std::string::npos) end = mutated.size();
              std::string repl = "enabled=#";
              if (want_disable) {
                repl += "false";
              } else if (want_enable) {
                repl += "true";
              } else {
                // Preserve whatever value is currently in the line.
                repl = std::string(mutated.substr(eq, end - eq));
              }
              mutated.replace(eq, end - eq, repl);
            }
            // Count effective enabled state.
            if (want_enable) {
              enabled_count++;
            } else if (!want_disable && mutated.find("enabled=#true") != std::string::npos) {
              enabled_count++;
            }
          }
        }

        out.append(mutated);
        if (last) break;
        out.push_back('\n');
        pos = eol + 1;
      }

      if (enabled_count <= 0) {
        BOOST_LOG(warning) << "[evdi_grab] kdl apply refused — would leave 0 outputs enabled";
        return false;
      }

      // Post-process: for every output we're flipping to enabled, make
      // sure its modes block has at least one mode marked current=#true.
      // cosmic-randr's `list --kdl` output OMITS current=#true for
      // disabled outputs, so a naive enabled flip leaves the apply path
      // with no mode to set — it then panics with `ModeNotFound` (exit
      // 101). Walk the assembled document, locate each enable-target's
      // modes block, and inject ` current=#true` on its preferred mode
      // (or the first mode if none is preferred) if no current already
      // exists.
      for (const auto &name : enable_set) {
        std::string header = "output \"" + name + "\"";
        size_t out_pos = out.find(header);
        if (out_pos == std::string::npos) continue;
        size_t modes_pos = out.find("modes {", out_pos);
        if (modes_pos == std::string::npos) continue;
        size_t modes_close = out.find("}", modes_pos);
        if (modes_close == std::string::npos) continue;
        std::string_view modes_block {out.data() + modes_pos, modes_close - modes_pos};
        if (modes_block.find("current=#true") != std::string_view::npos) {
          continue;  // already has a current mode, nothing to do
        }
        size_t inject_eol = std::string::npos;
        // Highest priority: caller passed a snapshot of the mode this
        // output had before we disabled it. Find that exact mode line
        // (matched on `mode W H REFRESH`) and re-mark it current.
        const ModeSnapshot *override_mode = nullptr;
        for (const auto &m : mode_override) {
          if (m.name == name) {
            override_mode = &m;
            break;
          }
        }
        if (override_mode) {
          char needle[64];
          std::snprintf(needle, sizeof(needle), "    mode %d %d %d",
                        override_mode->w, override_mode->h, override_mode->refresh);
          size_t mode_line = out.find(needle, modes_pos);
          if (mode_line != std::string::npos && mode_line < modes_close) {
            inject_eol = out.find('\n', mode_line);
            if (inject_eol > modes_close) inject_eol = std::string::npos;
          } else {
            BOOST_LOG(warning) << "[evdi_grab] mode override " << override_mode->w
                               << "x" << override_mode->h << "@"
                               << override_mode->refresh << " not found in modes "
                               << "block for " << name << "; falling back to preferred";
          }
        }
        // Fallback: the mode marked preferred=#true.
        if (inject_eol == std::string::npos) {
          size_t pref_pos = out.find("preferred=#true", modes_pos);
          if (pref_pos != std::string::npos && pref_pos < modes_close) {
            inject_eol = out.find('\n', pref_pos);
            if (inject_eol > modes_close) inject_eol = std::string::npos;
          }
        }
        // Last resort: the first `mode ` line inside the block.
        if (inject_eol == std::string::npos) {
          size_t first_mode = out.find("    mode ", modes_pos);
          if (first_mode == std::string::npos || first_mode > modes_close) continue;
          inject_eol = out.find('\n', first_mode);
          if (inject_eol == std::string::npos || inject_eol > modes_close) continue;
        }
        out.insert(inject_eol, " current=#true");
      }

      // Dump the mutated payload to disk before sending so we can debug
      // the kdl parser rejecting it. Overwritten on each apply — keep only
      // the most recent attempt. cosmic-randr's stderr is also captured so
      // we get the kdl crate's parse-error location when it panics.
      if (FILE *dump = std::fopen("/tmp/apollo-last-kdl-payload.kdl", "w")) {
        std::fwrite(out.data(), 1, out.size(), dump);
        std::fclose(dump);
      }

      // Pipe to cosmic-randr kdl. One IPC request, atomic apply, atomic
      // rollback on failure inside cosmic-comp.
      FILE *w = ::popen("timeout 5 cosmic-randr kdl 2>/tmp/apollo-last-kdl-stderr.log", "w");
      if (!w) {
        BOOST_LOG(warning) << "[evdi_grab] failed to launch cosmic-randr kdl";
        return false;
      }
      size_t wrote = ::fwrite(out.data(), 1, out.size(), w);
      int rc = ::pclose(w);
      if (rc != 0 || wrote != out.size()) {
        BOOST_LOG(warning) << "[evdi_grab] cosmic-randr kdl exit=" << rc
                            << " wrote=" << wrote << "/" << out.size()
                            << " — payload at /tmp/apollo-last-kdl-payload.kdl"
                            << ", stderr at /tmp/apollo-last-kdl-stderr.log";
        return false;
      }
      return true;
    }

    // Fire the atomic kdl apply in a detached thread so the caller (the
    // capture loop, OutputRestoreGuard destructor) doesn't block on the
    // cosmic-randr round-trip. Apollo's session::join has a 10-second
    // hang-detect that core-dumps the process if a capture thread
    // doesn't return on disconnect; this preserves that liveness while
    // still doing the work synchronously off-thread.
    void apply_output_kdl_async(std::vector<std::string> disable_set,
                                  std::vector<std::string> enable_set) {
      std::thread([dis = std::move(disable_set), en = std::move(enable_set)]() {
        apply_output_kdl_sync(dis, en);
      }).detach();
    }

    // Compatibility shim for single-output flips. Routes through the
    // atomic apply so multi-output transitions issued back-to-back
    // (e.g. disable A then disable B from a loop) still all share the
    // safety gate and atomic semantics. Calls site that wants atomic
    // multi-output behavior should call apply_output_kdl_* directly.
    bool toggle_output(const std::string &output_name, bool enable) {
      if (output_name.empty()) return false;
      if (enable) {
        apply_output_kdl_async({}, {output_name});
      } else {
        apply_output_kdl_async({output_name}, {});
      }
      return true;
    }

    // Health check for cosmic-comp's wlr-output-management responder.
    // The wedges that bite us are characterised by `cosmic-randr list`
    // hanging — backend.lock() is held, the responder can't service
    // new requests, and any kdl apply we issue piles up behind the
    // contention until something terminal. Returns true if cosmic-randr
    // exits 0 within the timeout (compositor healthy enough to accept
    // an apply). Cheap — we run `list` not `list --kdl`, so we only
    // pay for the protocol handshake + a few hundred bytes of plain
    // text, not the full kdl serialization.
    bool cosmic_comp_responsive(int timeout_ms = 1500) {
      char cmd[64];
      std::snprintf(cmd, sizeof(cmd),
                    "timeout %d.%03d cosmic-randr list >/dev/null 2>&1",
                    timeout_ms / 1000, timeout_ms % 1000);
      int rc = std::system(cmd);
      return WIFEXITED(rc) && WEXITSTATUS(rc) == 0;
    }

    class evdi_display_t: public display_t {
    public:
      evdi_display_t(const std::string &name, const video::config_t &config):
          display_name_(name),
          mem_type_(mem_type_e::system),
          delay_(std::chrono::nanoseconds(1'000'000'000ll / std::max(config.framerate, 1))),
          requested_width_(config.width),
          requested_height_(config.height) {
      }

      bool init() {
        // Seed width/height/stride from the CLIENT-requested dimensions
        // BEFORE the probe runs. The base display_t's `width`/`height`
        // members are not value-initialized, so reading them before the
        // probe writes valid data hands swscaler stack garbage. The probe
        // also returns rc==1 (timeout) on a fresh EVDI device that hasn't
        // received its first mode_changed event yet, and that path leaves
        // the caller's CapturedFrame{} untouched — fallback dims must come
        // from somewhere stable.
        width = requested_width_;
        height = requested_height_;
        env_width = width;
        env_height = height;
        stride_ = width * 4;

        VDISPLAY::CapturedFrame probe {};
        // Register the capture buffer with VDISPLAY. This allocates the BGRA
        // buffer that the EVDI kernel module will write into.
        if (!VDISPLAY::registerCaptureBuffer(display_name_)) {
          BOOST_LOG(error) << "[evdi_grab] Failed to register capture buffer for "
                           << display_name_;
          return false;
        }

        // Force-enable the EVDI output in cosmic-comp BEFORE the first
        // captureFrame. After a previous session's restore disabled the
        // EVDI, cosmic-comp persists per-make/model state ("Arlotto
        // Comnet Inc / APOLLO VDISP") and re-applies enabled=#false to
        // the fresh hotplug. With the EVDI marked disabled, cosmic-comp
        // doesn't page-flip into its buffer — libevdi's evdi_grab_pixels
        // then spins on its GEM MMAP ioctl returning EAGAIN at ~80k
        // calls/sec, burning 70%+ CPU forever in init(). Verified live
        // via strace + gdb backtrace on a hung apollo: stack was
        // ioctl → evdi_grab_pixels → captureFrame → init() →
        // probe_encoders → nvhttp::launch.
        //
        // Poll briefly for cosmic-comp to surface the EVDI head, then
        // if it shows current_width==0 (= no mode set = disabled in the
        // kdl listing), issue an EVDI-only kdl apply to enable it.
        // The apply is single-output and adds no other outputs to the
        // disable_set, so it can't deadlock cosmic-comp the way prior
        // multi-output applies did. Sync because we MUST land the
        // enable before captureFrame; an async apply would race the
        // grab_pixels spin and we'd still hang.
        {
          auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(800);
          std::string evdi_name;
          bool found_and_enabled = false;
          while (std::chrono::steady_clock::now() < deadline) {
            CompositorLayout pre {};
            if (find_compositor_layout(pre) && !pre.evdi_output_name.empty()) {
              evdi_name = pre.evdi_output_name;
              if (pre.evdi_current_width > 0) {
                found_and_enabled = true;
                break;
              }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
          }
          if (!found_and_enabled && !evdi_name.empty()) {
            BOOST_LOG(info) << "[evdi_grab] Pre-init: EVDI '" << evdi_name
                            << "' shows as disabled in cosmic-comp — "
                            "force-enabling before captureFrame to break "
                            "the libevdi grab_pixels EAGAIN spin";
            apply_output_kdl_sync({}, {evdi_name});
            // Give cosmic-comp a tick to start page-flipping into the EVDI
            // before we ask libevdi for a frame.
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
          } else if (!found_and_enabled) {
            BOOST_LOG(warning) << "[evdi_grab] Pre-init: EVDI not found in "
                                  "cosmic-randr listing after 800ms — "
                                  "proceeding anyway, captureFrame may "
                                  "spin if cosmic-comp didn't pick up the "
                                  "hotplug";
          }
        }

        // Drive one capture cycle to pick up the buffer dimensions. EVDI
        // copies the EDID-advertised resolution to the buffer header on the
        // first request_update, so a single call is enough.
        int rc = VDISPLAY::captureFrame(display_name_, &probe, 500);
        if (rc < 0) {
          BOOST_LOG(error) << "[evdi_grab] Initial captureFrame failed for "
                           << display_name_;
          return false;
        }

        // NOTE: do NOT force a cosmic-randr mode switch here. Apollo's
        // encoder-probe phase creates multiple display_t instances back-to-
        // back, each with a different probe config (1920x1080, etc.). If we
        // force-switch the EVDI mode on every init(), the mode flickers
        // during probe, the buffer registration races with mode_changed
        // events, and probe.width/height collapse to 0 → swscaler errors
        // and black frames on the client. cosmic-comp picks the EDID's
        // preferred mode (which is the client's requested resolution since
        // createVirtualDisplay generates the EDID for exactly that res), so
        // the right behavior is to accept whatever it picked.
        // rc==1 (timeout) keeps the requested-resolution seeds above; the
        // capture loop will retry and learn the real dims later.

        if (probe.width > 0) width = probe.width;
        if (probe.height > 0) height = probe.height;
        env_width = width;
        env_height = height;
        if (probe.stride > 0) stride_ = probe.stride;
        else stride_ = width * 4;

        // Find where cosmic-comp placed the EVDI Wayland output and the total
        // compositor desktop extent. The offset lets the input pipeline
        // translate client-side absolute coords into the EVDI region; the
        // total extent is what uinput's ABS axes get scaled against.
        CompositorLayout layout {};
        // Recover from a previous session that didn't restore physical
        // outputs (apollo crash, hard reset). The state file lists outputs
        // that were disabled but never re-enabled — restore them BEFORE we
        // query the compositor layout so they show up in subsequent
        // find_compositor_layout calls and our normal disable/restore cycle
        // can take ownership of them again.
        {
          // Session init runs immediately after the EVDI head is attached
          // and cosmic-comp is still processing the EVDI mode-set. ANY
          // cosmic-randr kdl apply issued in this window deadlocks the
          // compositor (system-wide freeze, hard reset required). So we
          // do not apply anything here — recovery is exclusively driven
          // by evdi_grab_startup_recovery() at apollo startup (which runs
          // before any EVDI exists) and by the destructor's systemd
          // restart kick. If leftover state files are present at session
          // init time, something is unusual (startup recovery didn't fire
          // or failed); log it but don't try to be heroic.
          auto leftover = read_disabled_state();
          if (!leftover.empty()) {
            BOOST_LOG(warning) << "[evdi_grab] Unexpected leftover state at "
                               << "session init: " << leftover.size()
                               << " outputs. Skipping in-process restore "
                               << "(unsafe in this window); recovery will "
                               << "run at next apollo restart.";
          }
        }

        if (find_compositor_layout(layout)) {
          offset_x = layout.evdi_offset_x;
          offset_y = layout.evdi_offset_y;
          env_width = layout.total_width;
          env_height = layout.total_height;
          BOOST_LOG(info) << "[evdi_grab] EVDI head at compositor offset ("
                          << offset_x << ", " << offset_y << "), total desktop "
                          << env_width << "x" << env_height;
          // Save which physical outputs to disable; the actual disable
          // happens in capture() and is paired with a scope guard that
          // restores them on every exit path. This way a "session paused"
          // (without full display_t teardown) immediately gives the user
          // their desk back, and the next "session resumed" re-disables.
          evdi_output_name_ = layout.evdi_output_name;
          disabled_outputs_ = layout.other_enabled_outputs;
          // Refresh the cross-session snapshot of physicals. If a future
          // session starts with physicals already disabled (because a
          // previous one crashed before restoring), its destructor will
          // union this snapshot into its restore set so monitors come back
          // without an apollo restart. Refreshed on every healthy init so
          // newly-hotplugged monitors are picked up automatically.
          if (!layout.other_enabled_outputs.empty()) {
            persist_known_physicals(layout.other_enabled_outputs);
          }
        } else {
          BOOST_LOG(warning) << "[evdi_grab] Could not determine compositor "
                                "layout via cosmic-randr; absolute mouse "
                                "coords may be off";
        }

        BOOST_LOG(info) << "[evdi_grab] Capture initialized: " << width << "x"
                        << height << " stride=" << stride_;
        return true;
      }

      capture_e capture(const push_captured_image_cb_t &push_captured_image_cb,
                        const pull_free_image_cb_t &pull_free_image_cb,
                        bool * /*cursor*/) override {
        // Defer the cosmic-randr disable until the EVDI is actively
        // delivering frames. If we disable Samsung while cosmic-comp is
        // still finishing the EVDI mode-set, cosmic-comp tries to migrate
        // the desktop to an output that isn't rendering yet and the whole
        // compositor (and every client) deadlocks. The freeze was system-
        // wide and forced a hard reboot during testing.
        //
        // Wait for kFramesBeforeDisable successful captures first — that
        // confirms cosmic-comp is page-flipping into the EVDI and our
        // consumer is draining the flips. After that the layout swap is
        // safe.
        struct OutputRestoreGuard {
          bool armed {false};
          bool tiling_was_disabled {false};
          ~OutputRestoreGuard() {
            BOOST_LOG(info) << "[evdi_grab][debug] ~OutputRestoreGuard fired";
            // Multi-client gate: if other streaming sessions are still
            // active, this destructor is firing for ONE of several
            // concurrent clients (e.g. Phone disconnected but Mac is
            // still streaming). All the cleanup below — autotile
            // restore, in-process physicals restore, apollo restart
            // fallback — is GLOBAL state that the still-active client
            // is relying on. Doing it here would kick them off / black-
            // screen them. The LAST session ending will run this code;
            // do nothing here.
            //
            // Counter is incremented at session::start and decremented
            // at the END of session::join (after videoThread.join()
            // returns). This destructor runs from inside videoThread's
            // shutdown — i.e. before our decrement — so we still count
            // ourselves: `> 1` means "another session is also alive".
            //
            // CRITICAL: read the lock-free atomic, NOT
            // rtsp_stream::session_count(). The latter calls
            // server.clear(false) → acquires _session_slots.lock(),
            // and that lock is HELD right now by clear(true) further
            // up the stack (which is what called session::join in the
            // first place). Re-entering it would deadlock for 10s
            // until apollo's hang-detect SIGABRTs the process. Verified
            // live with diagnostic logs: pull_free_image_cb shutdown
            // → ~OutputRestoreGuard fires → 10s gap → Fatal hang detect.
            const auto active = stream::session::active_session_count();
            if (active > 1) {
              BOOST_LOG(info) << "[evdi_grab] Another streaming session is "
                              << "still active (" << (active - 1)
                              << " remaining) — skipping global cleanup "
                              << "so they stay streaming; cleanup will run "
                              << "when the last client disconnects";
              return;
            }

            // Restore cosmic-comp autotile first — it's a fast file
            // write that cosmic-comp picks up via inotify almost
            // instantly. Always do this when we disabled it, even if
            // the physicals-armed flag is false (e.g. empty-physicals
            // branch).
            if (tiling_was_disabled) {
              if (cosmic_tiling_restore()) {
                BOOST_LOG(info) << "[evdi_grab] Restored cosmic-comp autotile";
              } else {
                BOOST_LOG(warning) << "[evdi_grab] Failed to restore cosmic-comp "
                                      "autotile — state file kept for startup "
                                      "recovery";
              }
            }
            if (!armed) return;
            // In-process restore with restart fallback.
            //
            // The deadlock that froze prior in-process restore attempts
            // had a specific shape: while apollo's libevdi is consuming
            // EVDI frames AND cosmic-comp's render scheduler is flipping
            // frames to the EVDI plane, a cosmic-randr kdl apply hits the
            // same backend.lock() that the render scheduler is waiting
            // on, and the chain wedges the whole compositor. Restarting
            // apollo broke the cycle because the process death tears
            // down libevdi, which makes cosmic-comp drop the EVDI output
            // and stop trying to flip to it.
            //
            // We can simulate that "EVDI is out of the render path" state
            // without exec'ing a new process: apollo's normal session
            // teardown calls VDISPLAY::removeVirtualDisplay →
            // evdi.disconnect, which signals a monitor hot-unplug.
            // Cosmic-comp processes it asynchronously and the EVDI
            // either disappears from cosmic-randr's listing or appears
            // with enabled=#false. Once we observe that state, the kdl
            // apply has no EVDI plane to fight with and lands cleanly.
            //
            // If after 3s the EVDI is still in cosmic-comp's listing as
            // enabled (e.g. the unplug didn't propagate, or the user's
            // session teardown sequence stalled), we fall back to the
            // proven systemctl-restart path so the user isn't stuck.
            // Either way state files persisted at disable time drive the
            // restore — same data feeds both paths.
            std::thread([]() {
              // Do NOT call VDISPLAY::disconnectAllEvdiMonitors() here.
              // Apollo's session-cleanup path will call removeVirtualDisplay
              // shortly after this destructor fires, and that path already
              // does evdi.disconnect + evdi.close on the shared handle when
              // refcount hits 0. Calling disconnect ourselves here races
              // with that — two threads operating on the same evdi_handle,
              // and evdi.close in the cleanup path blocks waiting for the
              // duplicate disconnect to settle in cosmic-comp. That race
              // is what hits apollo's 10-second session::join hang-detect
              // → SIGABRT → forced restart.

              // Poll cosmic-randr for the EVDI being out of the render
              // path. With the explicit disconnect above this should
              // settle in well under a second on a healthy compositor,
              // but we give it up to 8 seconds since cosmic-comp
              // occasionally takes a few render cycles to acknowledge.
              auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(8);
              bool evdi_clear = false;
              bool aborted_for_new_session = false;
              while (std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(150));

                // Abort if a new client connected after we spawned this
                // thread. session::join decrements running_sessions
                // AFTER videoThread.join returns (i.e. after we
                // detached this thread), so by the time we run, the
                // counter is 0 if no one else connected. A nonzero
                // value here means a new session is live and owns the
                // EVDI now. Continuing would either:
                //   - keep polling for an EVDI that's intentionally
                //     active for the new session (never sees disabled,
                //     hits the 8s fallback, systemctl restarts apollo,
                //     KICKS THE NEW CLIENT). Verified live: Mac
                //     disconnect → phone connect immediately → 8s
                //     later restore thread fired systemctl restart
                //     mid-stream.
                //   - in the apply path: re-enable physicals while a
                //     stream is in flight, breaking the new client's
                //     streaming experience.
                // Hand off cleanly: state files stay on disk for the
                // NEW session's destructor (or apollo startup recovery
                // if it crashes) to handle.
                if (stream::session::active_session_count() > 0) {
                  BOOST_LOG(info) << "[evdi_grab] New session started while "
                                  << "restore thread was polling — aborting "
                                  << "in-process restore; new session now "
                                  << "owns the EVDI and the restore will be "
                                  << "the responsibility of its destructor "
                                  << "or apollo's startup recovery";
                  aborted_for_new_session = true;
                  break;
                }

                std::string kdl;
                if (FILE *p = ::popen("timeout 3 cosmic-randr list --kdl 2>/dev/null", "r")) {
                  std::array<char, 4096> chunk;
                  while (auto n = ::fread(chunk.data(), 1, chunk.size(), p)) {
                    kdl.append(chunk.data(), n);
                  }
                  ::pclose(p);
                }
                if (kdl.empty()) continue;
                // EVDI heads are identifiable by EDID model "APOLLO VDISP".
                // "Out of the render path" means either the block is
                // absent from the listing, or its enabled flag is #false
                // (cosmic-comp doesn't render to disabled outputs).
                size_t evdi_pos = kdl.find("APOLLO VDISP");
                if (evdi_pos == std::string::npos) {
                  evdi_clear = true;
                  break;
                }
                size_t header_pos = kdl.rfind("output \"", evdi_pos);
                if (header_pos != std::string::npos) {
                  size_t header_end = kdl.find('{', header_pos);
                  if (header_end != std::string::npos) {
                    std::string_view header {kdl.data() + header_pos,
                                              header_end - header_pos};
                    if (header.find("enabled=#false") != std::string_view::npos) {
                      evdi_clear = true;
                      break;
                    }
                  }
                }
              }

              if (aborted_for_new_session) {
                // Bailed because a new session connected mid-poll. Do
                // NOT apply physicals-restore (would compete with the
                // new stream) and do NOT fall back to systemctl
                // restart (would kick the new client). State files
                // remain on disk; the new session's own destructor or
                // apollo's next startup recovery will pick them up.
                return;
              }

              if (evdi_clear) {
                // EVDI is out of cosmic-comp's render path. Safe to
                // issue the atomic kdl apply for the physicals.
                auto leftover = read_disabled_state();
                auto snapshot = read_known_physicals();
                auto modes = read_disabled_modes();
                std::vector<std::string> restore_targets = leftover;
                for (const auto &name : snapshot) {
                  bool already = false;
                  for (const auto &e : restore_targets) {
                    if (e == name) { already = true; break; }
                  }
                  if (!already) restore_targets.push_back(name);
                }
                if (restore_targets.empty()) {
                  BOOST_LOG(info) << "[evdi_grab] In-process restore: nothing to do";
                  return;
                }
                BOOST_LOG(info) << "[evdi_grab] In-process restore: "
                                << restore_targets.size()
                                << " outputs (leftover=" << leftover.size()
                                << ", snapshot=" << snapshot.size()
                                << ", modes=" << modes.size() << ")";
                if (apply_output_kdl_sync({}, restore_targets, modes)) {
                  clear_disabled_state();
                  clear_disabled_modes();
                  BOOST_LOG(info) << "[evdi_grab] In-process restore complete";
                  return;
                }
                BOOST_LOG(warning) << "[evdi_grab] In-process kdl apply failed";
              } else {
                BOOST_LOG(warning) << "[evdi_grab] EVDI still active in "
                                      "cosmic-comp after 3s — restore would "
                                      "deadlock";
              }

              // Fallback path. State files are still on disk (we only
              // clear them on confirmed success above), so the new
              // apollo's evdi_grab_startup_recovery() will pick up the
              // exact same restore intent without any data loss.
              BOOST_LOG(info) << "[evdi_grab] Falling back to apollo restart "
                                 "for monitor restore";
              std::system("systemctl --user restart --no-block apollo.service");
            }).detach();
          }
        };
        OutputRestoreGuard restore_guard {false};

        // We only disable the physical outputs once we've confirmed this
        // capture loop is an actual streaming session (not a transient
        // encoder-probe display_t). The probe path runs capture for at most
        // a few hundred milliseconds before being destroyed; a real stream
        // runs continuously. A 1.5-second wall-clock gate after capture()
        // entry cleanly discriminates the two.
        //
        // Earlier versions also required N successful frame pushes, but on
        // a mostly-idle desktop captureFrame returns timeout-rather-than-
        // fresh and the counter could take 20+ seconds to reach. With the
        // atomic kdl apply now stable, the time gate alone is sufficient.
        const auto disable_after = std::chrono::steady_clock::now() +
                                    std::chrono::milliseconds(1500);
        // Separate from restore_guard.armed so we can mark the
        // disable-decision as "done" without arming the destructor's
        // restore work. The empty-physicals branch wants the check to
        // not re-fire each frame, but doesn't want the destructor to
        // actually do anything (which would break the resume path).
        bool disable_check_done = false;

        auto next_frame = std::chrono::steady_clock::now();
        sleep_overshoot_logger.reset();

        while (true) {
          auto now = std::chrono::steady_clock::now();
          if (next_frame > now) {
            std::this_thread::sleep_for(next_frame - now);
            sleep_overshoot_logger.first_point(next_frame);
            sleep_overshoot_logger.second_point_now_and_log();
          }
          next_frame += delay_;
          if (next_frame < now) {
            next_frame = now + delay_;
          }

          // Time-only streaming-detection gate.
          if (!disable_check_done && now >= disable_after) {
            disable_check_done = true;

            // Disable cosmic-comp autotile for the duration of this
            // stream regardless of whether we end up disabling
            // physicals. Auto-tiling breaks fullscreen-borderless
            // games launched via gamescope on the EVDI head — the
            // game window comes up tiled, the gamescope viewport is
            // wrong, and the client sees a broken frame. Doing this
            // at the 1.5s gate (not at session init) skips the
            // encoder-probe display_t instances that only exist for
            // a few hundred ms.
            if (cosmic_tiling_disable()) {
              BOOST_LOG(info) << "[evdi_grab] Disabled cosmic-comp autotile "
                              << "for stream session";
              restore_guard.tiling_was_disabled = true;
            } else {
              BOOST_LOG(warning) << "[evdi_grab] Failed to disable cosmic-comp "
                                    "autotile (config path missing?) — "
                                    "fullscreen-borderless games may tile";
            }

            if (disabled_outputs_.empty()) {
              // No physicals tracked this session — either find_compositor_
              // layout saw only the EVDI (race with cosmic-comp's hotplug
              // settling) or the user genuinely has no physicals. Either
              // way, do NOTHING on disconnect. If we armed the restore,
              // the destructor's force-disconnect of the EVDI would break
              // the resume path (cosmic-comp drops the EVDI output, and
              // the next "resume" RTSP session reuses the same launch_
              // session — no new evdi.connect is issued — so the client
              // gets a black screen).
              //
              // Safety net for the "previous session actually disabled
              // physicals and crashed before restoring" scenario lives in
              // evdi_grab_startup_recovery(), which fires at apollo
              // startup with the disabled-outputs state file + known-
              // physicals snapshot. No need to also try to be heroic
              // here.
              BOOST_LOG(info) << "[evdi_grab] No physical outputs to disable "
                              << "this session — skipping disable AND restore "
                              << "(EVDI stays connected so resume works)";
              static thread_local bool logged_empty {false};
              logged_empty = true;

              // BUT: still force the EVDI to enabled=#true via a kdl
              // apply. After many connect/disconnect cycles, cosmic-comp's
              // wlr-output-management keeps a per-make/model state
              // (Arlotto Comnet Inc / "APOLLO VDISP") and persists the
              // last enabled flag across hotplugs. If a previous
              // session ever flipped the EVDI to disabled (e.g. the
              // destructor's restore path did so when bringing physicals
              // back), the new hotplug at a different connector slot
              // STILL comes up disabled. No frames render, encoder
              // probe hangs for 180s with Initial Ping Timeout, client
              // shows a black screen / stuck "Starting…" UI.
              //
              // This apply only touches the EVDI's enabled flag — all
              // other outputs are passed through verbatim — so it can't
              // disable physicals as a side-effect. Async because we
              // don't need to block the capture loop on it.
              if (!evdi_output_name_.empty()) {
                BOOST_LOG(info) << "[evdi_grab] Force-enabling EVDI '"
                                << evdi_output_name_ << "' to override "
                                << "any stale cosmic-comp disabled state "
                                << "(no physicals being touched)";
                apply_output_kdl_async({}, {evdi_output_name_});
              }

              // Intentionally NOT setting armed=true. The destructor is a
              // no-op when armed=false, so the EVDI stays attached and
              // resume can continue using the same virtual display.
            } else {
              // Pre-flight health check. The recurring wedges have all
              // had the same fingerprint in journalctl: an apply that
              // sat for >5s and returned WEXITSTATUS=124 (timeout),
              // followed by `cosmic-randr list` itself hanging until
              // the user logs out. Verified live: 3-output atomic apply
              // (disable DP-3 + HDMI-A-2 + enable EVDI) timed out
              // mid-Steam-BP-launch, Steam's GLX/Vulkan main loop
              // stalled, full log-out-log-in needed to recover.
              //
              // If cosmic-comp can't even respond to `cosmic-randr list`
              // within 1.5s, its wlr-output-management responder is
              // already lock-contended. Adding our apply on top is what
              // pushes it from "contended" to "wedged". Skip the apply
              // entirely in that state — let the user stream in
              // degraded mode (physicals still on) instead of breaking
              // their desktop.
              if (!cosmic_comp_responsive()) {
                BOOST_LOG(warning) << "[evdi_grab] cosmic-comp wlr-output-"
                                      "management unresponsive (cosmic-randr "
                                      "list didn't return in 1.5s) — skipping "
                                      "physicals disable for this session. "
                                      "Stream continues with physicals on. "
                                      "Restore guard NOT armed (nothing to "
                                      "restore). Cycle pressure has built up; "
                                      "consider apollo restart between "
                                      "sessions.";
                // Still ensure EVDI is enabled so streaming works
                // (init()'s pre-check should have done this; this is
                // belt-and-suspenders).
                if (!evdi_output_name_.empty()) {
                  apply_output_kdl_async({}, {evdi_output_name_});
                }
              } else {
                // Capture the current mode of each output BEFORE we
                // disable them, so the restore path can put them back at
                // the exact mode the user was running.
                auto modes = capture_current_modes(disabled_outputs_);
                persist_disabled_outputs(disabled_outputs_);
                persist_disabled_modes(modes);

                BOOST_LOG(info) << "[evdi_grab] Disabling "
                                << disabled_outputs_.size()
                                << " physicals (preserved " << modes.size()
                                << " modes) via SPLIT kdl applies "
                                << "(one output per apply, 500ms gap, "
                                << "health-check between)";

                // Spawn a detached thread to do the split applies so the
                // capture loop continues pushing frames during the
                // 2-3 second sequence.
                //
                // Why split? A single atomic apply that flips 2 physicals
                // off (and historically also EVDI on) wedges cosmic-comp
                // under cycle pressure — the backend.lock window is too
                // wide for cosmic-comp to clean up its render scheduler
                // state for both outputs at once. Single-output applies
                // give cosmic-comp time to fully process each transition
                // before the next one lands. The 500ms gap + health
                // check between applies catches the wedge early if it
                // happens — we abort the remaining disables instead of
                // pushing more applies into a stuck responder.
                //
                // restore_guard is armed BEFORE the apply runs so the
                // destructor's restore path still fires on disconnect
                // even if the disables abort partway. Re-enabling an
                // output that was never actually disabled is harmless
                // (idempotent: enabled=#true on an enabled output).
                restore_guard.armed = true;
                std::thread([targets = disabled_outputs_,
                             evdi = evdi_output_name_]() mutable {
                  // EVDI enable as safety net — should already be
                  // enabled from init()'s pre-check, but the apply is
                  // a no-op in that case.
                  if (!evdi.empty()) {
                    apply_output_kdl_sync({}, {evdi});
                    std::this_thread::sleep_for(std::chrono::milliseconds(300));
                  }

                  for (const auto &name : targets) {
                    BOOST_LOG(info) << "[evdi_grab] Split disable: " << name;
                    if (!apply_output_kdl_sync({name}, {})) {
                      BOOST_LOG(warning) << "[evdi_grab] Split disable of "
                                          << name << " failed — aborting "
                                             "remaining disables";
                      return;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    // Health check between applies. If cosmic-comp went
                    // unresponsive after the last apply, stop pushing
                    // more applies into it.
                    if (!cosmic_comp_responsive(1500)) {
                      BOOST_LOG(warning) << "[evdi_grab] cosmic-comp went "
                                            "unresponsive after disabling "
                                          << name
                                          << " — aborting further disables "
                                             "to avoid full wedge";
                      return;
                    }
                  }
                  BOOST_LOG(info) << "[evdi_grab] Split disable sequence "
                                     "completed cleanly";
                }).detach();
              }
            }
          }

          VDISPLAY::CapturedFrame frame {};
          int rc = VDISPLAY::captureFrame(display_name_, &frame, 1000);

          if (rc < 0) {
            BOOST_LOG(warning) << "[evdi_grab] captureFrame returned error, "
                                  "requesting reinit";
            return capture_e::reinit;
          }

          std::shared_ptr<img_t> img_out;
          if (!pull_free_image_cb(img_out)) {
            BOOST_LOG(info) << "[evdi_grab][debug] pull_free_image_cb returned false (shutdown) — exiting loop";
            return capture_e::ok;  // shutdown
          }
          auto *img = static_cast<evdi_img_t *>(img_out.get());

          if (rc == 1) {
            // Timeout. Apollo expects a heartbeat push so the encoder keeps
            // ticking even if no new frame arrived (avoids stream stalls
            // when the desktop is idle).
            if (!push_captured_image_cb(std::move(img_out), false)) {
              BOOST_LOG(info) << "[evdi_grab][debug] push_captured_image_cb(heartbeat) returned false — exiting loop";
              return capture_e::ok;
            }
            continue;
          }

          // rc == 0 — frame is fresh.

          if (img->width != width || img->height != height) {
            // The img_t pool may have allocated for a previous size; resize.
            delete[] img->data;
            img->width = width;
            img->height = height;
            img->row_pitch = stride_;
            img->pixel_pitch = 4;
            img->data = new uint8_t[static_cast<size_t>(stride_) * height];
          }

          // cosmic-comp page-flips XBGR8888 to the EVDI framebuffer (memory
          // order RGBX), but Apollo's avcodec pipeline expects BGRX
          // (AV_PIX_FMT_BGR0). Swap R and B as we copy. The compiler
          // auto-vectorizes this loop with -O3 to roughly memcpy speed.
          {
            const uint8_t *src = frame.data;
            uint8_t *dst = img->data;
            const size_t pixels = static_cast<size_t>(width) * height;
            for (size_t i = 0; i < pixels; ++i) {
              const uint8_t r = src[i * 4 + 0];
              const uint8_t g = src[i * 4 + 1];
              const uint8_t b = src[i * 4 + 2];
              const uint8_t x = src[i * 4 + 3];
              dst[i * 4 + 0] = b;
              dst[i * 4 + 1] = g;
              dst[i * 4 + 2] = r;
              dst[i * 4 + 3] = x;
            }
          }
          // EVDI delivers cursor pixels and position out-of-band; paint them
          // onto the captured frame so the client actually sees a pointer.
          VDISPLAY::blendCursor(display_name_, img->data, width, height, stride_);
          img->frame_timestamp = std::chrono::steady_clock::now();

          if (!push_captured_image_cb(std::move(img_out), true)) {
            BOOST_LOG(info) << "[evdi_grab][debug] push_captured_image_cb(frame) returned false — exiting loop";
            return capture_e::ok;
          }
        }
      }

      std::shared_ptr<img_t> alloc_img() override {
        auto img = std::make_shared<evdi_img_t>();
        img->width = width;
        img->height = height;
        img->pixel_pitch = 4;
        img->row_pitch = stride_;
        img->data = new uint8_t[static_cast<size_t>(stride_) * height];
        return img;
      }

      std::unique_ptr<avcodec_encode_device_t> make_avcodec_encode_device(pix_fmt_e /*pix_fmt*/) override {
        // EVDI gives us BGRA in system memory. The encoder pipeline will
        // upload to the GPU via avcodec (e.g. via NVENC's GPU->RAM->GPU
        // path) so we just hand back a vanilla CPU encode device.
        return std::make_unique<avcodec_encode_device_t>();
      }

      int dummy_img(img_t *img) override {
        if (!img) {
          return -1;
        }
        if (img->width != width || img->height != height) {
          delete[] img->data;
          img->width = width;
          img->height = height;
          img->pixel_pitch = 4;
          img->row_pitch = stride_;
          img->data = new uint8_t[static_cast<size_t>(stride_) * height];
        }
        std::memset(img->data, 0, static_cast<size_t>(stride_) * height);
        return 0;
      }

      ~evdi_display_t() override {
        VDISPLAY::unregisterCaptureBuffer(display_name_);
        // Physical-output restore now happens via the RAII scope guard
        // inside capture(), which fires on every capture() exit (including
        // pause). The destructor only fires on full session teardown — by
        // then capture() has already returned and the guard has already
        // run. We do NOT cosmic-randr disable the EVDI here either:
        // combined with the persistent EVDI device that's kept across
        // sessions, it caused a hard freeze on reconnect during testing.
      }

    private:
      std::string display_name_;
      mem_type_e mem_type_;
      std::chrono::nanoseconds delay_;
      int stride_ {0};
      int requested_width_ {0};
      int requested_height_ {0};
      std::string evdi_output_name_;          ///< cosmic-comp's name for our EVDI head, for disable on teardown
      std::vector<std::string> disabled_outputs_;  ///< physical outputs we disabled on init, to re-enable on teardown
    };

  }  // namespace

  std::shared_ptr<display_t> evdi_display(mem_type_e hwdevice_type,
                                            const std::string &display_name,
                                            const video::config_t &config) {
    auto disp = std::make_shared<evdi_display_t>(display_name, config);
    if (!disp->init()) {
      return nullptr;
    }
    return disp;
  }

  std::vector<std::string> evdi_display_names() {
    // Active virtual displays change over time; return whatever VDISPLAY
    // currently knows about.
    return VDISPLAY::matchDisplay("");
  }

  // Called once at apollo startup from misc.cpp::init(). If a prior session
  // left ~/.cache/apollo/disabled-outputs on disk (because apollo crashed,
  // was killed by its own hang-detect, or the user hard-reset before the
  // restore could fire), re-enable those outputs via a single atomic kdl
  // apply and delete the state file. Runs synchronously — startup isn't on
  // the capture-thread watchdog path, so blocking briefly is fine. Safe to
  // call even if there's no state file: it's a no-op.
  void evdi_grab_startup_recovery() {
    // Restore cosmic-comp autotile FIRST. If a prior session crashed
    // mid-stream, the autotile config is stuck at "false" on disk and
    // every output is currently un-tiled. Do this before any other
    // recovery so the user's desk session has tiling back even if the
    // output-restore path fails for some reason. No-op when there's
    // no state file (clean previous shutdown).
    if (::access(cosmic_tiling_state_file().c_str(), F_OK) == 0 ||
        ::access(cosmic_tiling_behavior_state_file().c_str(), F_OK) == 0) {
      if (cosmic_tiling_restore()) {
        BOOST_LOG(info) << "[evdi_grab] startup recovery: restored cosmic-comp "
                           "autotile + behavior from previous session's backup";
      } else {
        BOOST_LOG(warning) << "[evdi_grab] startup recovery: failed to restore "
                              "cosmic-comp autotile";
      }
    }

    auto leftover = read_disabled_state();
    auto snapshot = read_known_physicals();
    auto modes = read_disabled_modes();
    // Union the disabled-outputs leftover (from a session that crashed
    // before its destructor cleared the file) with the cross-session
    // known-physicals snapshot (refreshed every healthy session init). If
    // the state file was cleared but physicals are still off, the snapshot
    // is the only thing that can rescue the user without a manual
    // cosmic-randr enable.
    std::vector<std::string> restore_targets = leftover;
    for (const auto &name : snapshot) {
      bool already_in = false;
      for (const auto &existing : restore_targets) {
        if (existing == name) {
          already_in = true;
          break;
        }
      }
      if (!already_in) restore_targets.push_back(name);
    }
    if (restore_targets.empty()) {
      return;
    }
    BOOST_LOG(info) << "[evdi_grab] startup recovery: re-enabling "
                    << restore_targets.size() << " outputs (leftover="
                    << leftover.size() << ", snapshot=" << snapshot.size()
                    << ", preserved_modes=" << modes.size()
                    << ") via atomic kdl";
    if (apply_output_kdl_sync({}, restore_targets, modes)) {
      clear_disabled_state();
      clear_disabled_modes();
      BOOST_LOG(info) << "[evdi_grab] startup recovery complete";
    } else {
      BOOST_LOG(warning) << "[evdi_grab] startup recovery apply failed — "
                            "state files kept for retry on first stream session";
    }
  }

}  // namespace platf
