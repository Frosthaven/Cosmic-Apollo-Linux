/**
 * @file src/platform/linux/virtual_display.cpp
 * @brief Virtual display implementation for Linux using EVDI.
 *
 * This implementation provides virtual display support on Linux using
 * EVDI (Extensible Virtual Display Interface) for creating true virtual
 * displays that are separate from physical monitors.
 *
 * When EVDI is not available, it falls back to a passthrough mode that
 * uses the existing physical monitor for capture.
 */

// standard includes
#include <atomic>
#include <cmath>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

// platform includes
#include <cerrno>
#include <chrono>
#include <ctime>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

// local includes
#include "misc.h"
#include "src/config.h"
#include "src/logging.h"
#include "virtual_display.h"

using namespace std::literals;
namespace fs = std::filesystem;

namespace VDISPLAY {

  // ============================================================================
  // EVDI Types and Function Pointers (loaded dynamically)
  // ============================================================================

  // EVDI structures (matching evdi_lib.h)
  struct evdi_lib_version {
    int version_major;
    int version_minor;
    int version_patchlevel;
  };

  struct evdi_device_context;
  typedef struct evdi_device_context *evdi_handle;

  enum evdi_device_status {
    EVDI_AVAILABLE,
    EVDI_UNRECOGNIZED,
    EVDI_NOT_PRESENT
  };

  struct evdi_mode {
    int width;
    int height;
    int refresh_rate;
    int bits_per_pixel;
    unsigned int pixel_format;
  };

  struct evdi_rect {
    int x1, y1, x2, y2;
  };

  struct evdi_buffer {
    int id;
    void *buffer;
    int width;
    int height;
    int stride;
    struct evdi_rect *rects;
    int rect_count;
  };

  struct evdi_cursor_set {
    int32_t hot_x;
    int32_t hot_y;
    uint32_t width;
    uint32_t height;
    uint8_t enabled;
    uint32_t buffer_length;
    uint32_t *buffer;
    uint32_t pixel_format;
    uint32_t stride;
  };

  struct evdi_cursor_move {
    int32_t x;
    int32_t y;
  };

  struct evdi_ddcci_data {
    uint16_t address;
    uint16_t flags;
    uint32_t buffer_length;
    uint8_t *buffer;
  };

  struct evdi_event_context {
    void (*dpms_handler)(int dpms_mode, void *user_data);
    void (*mode_changed_handler)(struct evdi_mode mode, void *user_data);
    void (*update_ready_handler)(int buffer_to_be_updated, void *user_data);
    void (*crtc_state_handler)(int state, void *user_data);
    void (*cursor_set_handler)(struct evdi_cursor_set cursor_set, void *user_data);
    void (*cursor_move_handler)(struct evdi_cursor_move cursor_move, void *user_data);
    void (*ddcci_data_handler)(struct evdi_ddcci_data ddcci_data, void *user_data);
    void *user_data;
  };

  // EVDI function pointer types
  typedef evdi_device_status (*fn_evdi_check_device)(int device);
  typedef evdi_handle (*fn_evdi_open)(int device);
  typedef int (*fn_evdi_add_device)(void);
  typedef void (*fn_evdi_close)(evdi_handle handle);
  typedef void (*fn_evdi_connect)(evdi_handle handle, const unsigned char *edid,
                                   const unsigned int edid_length,
                                   const uint32_t sku_area_limit);
  typedef void (*fn_evdi_disconnect)(evdi_handle handle);
  typedef void (*fn_evdi_grab_pixels)(evdi_handle handle, struct evdi_rect *rects, int *num_rects);
  typedef void (*fn_evdi_register_buffer)(evdi_handle handle, struct evdi_buffer buffer);
  typedef void (*fn_evdi_unregister_buffer)(evdi_handle handle, int bufferId);
  typedef bool (*fn_evdi_request_update)(evdi_handle handle, int bufferId);
  typedef void (*fn_evdi_handle_events)(evdi_handle handle, struct evdi_event_context *evtctx);
  typedef int (*fn_evdi_get_event_ready)(evdi_handle handle);
  typedef void (*fn_evdi_get_lib_version)(struct evdi_lib_version *version);
  typedef void (*fn_evdi_enable_cursor_events)(evdi_handle handle, bool enable);

  // EVDI function pointers (loaded at runtime)
  static struct {
    void *lib_handle;
    fn_evdi_check_device check_device;
    fn_evdi_open open;
    fn_evdi_add_device add_device;
    fn_evdi_close close;
    fn_evdi_connect connect;
    fn_evdi_disconnect disconnect;
    fn_evdi_grab_pixels grab_pixels;
    fn_evdi_register_buffer register_buffer;
    fn_evdi_unregister_buffer unregister_buffer;
    fn_evdi_request_update request_update;
    fn_evdi_handle_events handle_events;
    fn_evdi_get_event_ready get_event_ready;
    fn_evdi_get_lib_version get_lib_version;
    fn_evdi_enable_cursor_events enable_cursor_events;
    bool loaded;
  } evdi = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
            nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
            nullptr, false};

  // ============================================================================
  // Standard 1920x1080 EDID (used for virtual display)
  // ============================================================================

  // EDID for a generic 1920x1080@60Hz monitor
  static const unsigned char default_edid[] = {
    0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00,  // Header
    0x1E, 0x6D,  // Manufacturer ID (LG Display)
    0x00, 0x00,  // Product code
    0x01, 0x01, 0x01, 0x01,  // Serial number
    0x00, 0x1D,  // Week/Year of manufacture
    0x01, 0x04,  // EDID version 1.4
    0xB5,        // Video input (digital, 8-bit color depth, DisplayPort)
    0x3C, 0x22,  // Width/height in cm (60x34 = approx 27")
    0x78,        // Gamma 2.2
    0x3A,        // Features (RGB, preferred timing)
    // Chromaticity
    0xFC, 0x81, 0xA4, 0x55, 0x4D, 0x9D, 0x25, 0x12, 0x50, 0x54,
    // Established timings
    0x21, 0x08, 0x00,
    // Standard timings
    0xD1, 0xC0,  // 1920x1080@60Hz
    0x81, 0x80,  // 1280x1024@60Hz
    0x81, 0xC0,  // 1280x720@60Hz
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    // Detailed timing descriptor: 1920x1080@60Hz
    0x02, 0x3A,  // Pixel clock: 148.5 MHz
    0x80, 0x18, 0x71, 0x38, 0x2D, 0x40,
    0x58, 0x2C, 0x45, 0x00,
    0x56, 0x50, 0x21, 0x00, 0x00, 0x1E,
    // Display name descriptor
    0x00, 0x00, 0x00, 0xFC, 0x00,
    'A', 'P', 'O', 'L', 'L', 'O', ' ', 'V', 'D', 'I', 'S', 'P', '\n',
    // Display range limits
    0x00, 0x00, 0x00, 0xFD, 0x00,
    0x32, 0x4B, 0x1E, 0x51, 0x11, 0x00, 0x0A, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    // Extension flag and checksum (calculated)
    0x00, 0x00
  };

  // ============================================================================
  // Global State
  // ============================================================================

  static std::mutex vdisplay_mutex;
  static DRIVER_STATUS driver_status = DRIVER_STATUS::UNKNOWN;
  static std::atomic<bool> watchdog_running {false};
  static std::thread watchdog_thread;
  static bool evdi_available = false;

  // Virtual display info structure
  struct VirtualDisplayInfo {
    std::string name;
    std::string guid_str;
    uint32_t width;
    uint32_t height;
    uint32_t fps;
    int device_index;      // EVDI device index
    evdi_handle handle;    // EVDI handle
    int drm_fd;            // DRM fd for card
    bool active;
    bool using_evdi;       // true if using EVDI, false if passthrough
  };

  static std::map<std::string, VirtualDisplayInfo> virtual_displays;

  // Per-display capture state. Kept in a separate map keyed by GUID string so
  // VirtualDisplayInfo stays copyable (atomic/mutex would break that). Held by
  // unique_ptr because mutex/atomic also disallow move from external code.
  struct CaptureState {
    std::vector<uint8_t> buffer;        ///< CPU memory the kernel writes pixels into
    std::vector<evdi_rect> dirty_rects; ///< preallocated grab_pixels output
    int buffer_id;                       ///< id passed to evdi_register_buffer
    int width;                            ///< buffer width in pixels
    int height;                           ///< buffer height in pixels
    int stride;                           ///< buffer stride in bytes
    std::atomic<bool> update_ready;      ///< set by update_ready_handler callback
    std::atomic<bool> mode_ready;        ///< set true after mode_changed callback or first frame
    std::atomic<int> mode_width;         ///< latest mode width reported by EVDI (0 = unknown)
    std::atomic<int> mode_height;        ///< latest mode height reported by EVDI
    std::mutex grab_mutex;                ///< serializes grab_pixels with reads

    // Cursor compositing. EVDI delivers cursor pixels and position via
    // cursor_set / cursor_move callbacks rather than baking them into the
    // primary framebuffer. We keep a local copy so the capture loop can
    // composite it onto the output before handing the frame to the encoder.
    std::mutex cursor_mutex;
    std::vector<uint8_t> cursor_pixels;  ///< BGRA, width*height*4 bytes
    int cursor_width;
    int cursor_height;
    int cursor_hot_x;
    int cursor_hot_y;
    std::atomic<int> cursor_pos_x;
    std::atomic<int> cursor_pos_y;
    std::atomic<bool> cursor_enabled;

    CaptureState():
        buffer_id(0),
        width(0),
        height(0),
        stride(0),
        update_ready(false),
        mode_ready(false),
        mode_width(0),
        mode_height(0),
        cursor_width(0),
        cursor_height(0),
        cursor_hot_x(0),
        cursor_hot_y(0),
        cursor_pos_x(0),
        cursor_pos_y(0),
        cursor_enabled(false) {
    }
  };

  static std::map<std::string, std::unique_ptr<CaptureState>> capture_states;

  // Persistent EVDI device pool. Each EVDI device, once handed to
  // cosmic-comp as a DRM output, is held by the compositor for its entire
  // lifetime. If we allocate a fresh slot per stream, /dev/dri fills up
  // until libevdi's 16-slot limit is hit. Instead we keep one open EVDI
  // handle around between sessions and reconnect with a new EDID on each
  // new stream — cosmic-comp sees that as a monitor hotplug, the same
  // Wayland output morphs to the new resolution, no extra device required.
  // Shared EVDI device model: only ONE physical EVDI device exists at
  // a time, regardless of how many clients are streaming concurrently.
  // The FIRST session to connect decides the resolution / refresh rate
  // (via EDID); subsequent sessions become viewers of the same device
  // and stream at whatever the first session set up. When ALL sessions
  // disconnect, the device is fully torn down so the next "first"
  // session can pick a fresh resolution.
  //
  // shared_evdi.refcount tracks the number of active VirtualDisplayInfo
  // entries pointing at this device. Each VirtualDisplayInfo still
  // owns its own capture buffer (multiple evdi_register_buffer calls
  // with different buffer IDs on the same handle), so the sessions
  // can encode independently without sharing pixel data.
  struct SharedEvdiState {
    int device_index {-1};
    evdi_handle handle {nullptr};
    int drm_fd {-1};
    int refcount {0};
  };
  static SharedEvdiState shared_evdi;

  // ============================================================================
  // EVDI Library Loading
  // ============================================================================

  static bool load_evdi_library() {
    if (evdi.loaded) {
      return true;
    }

    // Try to load libevdi.so
    const char *lib_names[] = {
      "libevdi.so.1",
      "libevdi.so",
      "/usr/lib/libevdi.so.1",
      "/usr/lib/libevdi.so",
      "/usr/local/lib/libevdi.so.1",
      "/usr/local/lib/libevdi.so"
    };

    for (const auto &lib_name : lib_names) {
      evdi.lib_handle = dlopen(lib_name, RTLD_NOW);
      if (evdi.lib_handle) {
        BOOST_LOG(info) << "[VDISPLAY] Loaded EVDI library: " << lib_name;
        break;
      }
    }

    if (!evdi.lib_handle) {
      BOOST_LOG(warning) << "[VDISPLAY] Could not load libevdi.so: " << dlerror();
      BOOST_LOG(warning) << "[VDISPLAY] Virtual display will use passthrough mode.";
      BOOST_LOG(warning) << "[VDISPLAY] Install 'evdi' package for full virtual display support.";
      return false;
    }

    // Load function pointers
    #define LOAD_EVDI_FUNC(name) \
      evdi.name = (fn_evdi_##name)dlsym(evdi.lib_handle, "evdi_" #name); \
      if (!evdi.name) { \
        BOOST_LOG(error) << "[VDISPLAY] Failed to load evdi_" #name; \
        dlclose(evdi.lib_handle); \
        evdi.lib_handle = nullptr; \
        return false; \
      }

    LOAD_EVDI_FUNC(check_device);
    LOAD_EVDI_FUNC(open);
    LOAD_EVDI_FUNC(add_device);
    LOAD_EVDI_FUNC(close);
    LOAD_EVDI_FUNC(connect);
    LOAD_EVDI_FUNC(disconnect);
    LOAD_EVDI_FUNC(grab_pixels);
    LOAD_EVDI_FUNC(register_buffer);
    LOAD_EVDI_FUNC(unregister_buffer);
    LOAD_EVDI_FUNC(request_update);
    LOAD_EVDI_FUNC(handle_events);
    LOAD_EVDI_FUNC(get_event_ready);
    LOAD_EVDI_FUNC(get_lib_version);
    // Optional — only present in libevdi >= 1.5; tolerate missing.
    evdi.enable_cursor_events = (fn_evdi_enable_cursor_events) dlsym(evdi.lib_handle, "evdi_enable_cursor_events");

    #undef LOAD_EVDI_FUNC

    // Check library version
    evdi_lib_version version;
    evdi.get_lib_version(&version);
    BOOST_LOG(info) << "[VDISPLAY] EVDI library version: "
                    << version.version_major << "."
                    << version.version_minor << "."
                    << version.version_patchlevel;

    evdi.loaded = true;
    return true;
  }

  static void unload_evdi_library() {
    if (evdi.lib_handle) {
      dlclose(evdi.lib_handle);
      evdi.lib_handle = nullptr;
    }
    evdi.loaded = false;
  }

  // ============================================================================
  // EVDI Module Check
  // ============================================================================

  static bool check_evdi_module_loaded() {
    // Check if evdi kernel module is loaded
    std::ifstream modules("/proc/modules");
    std::string line;
    while (std::getline(modules, line)) {
      if (line.find("evdi") != std::string::npos) {
        BOOST_LOG(info) << "[VDISPLAY] EVDI kernel module is loaded.";
        return true;
      }
    }

    // Also check sysfs
    if (fs::exists("/sys/module/evdi")) {
      BOOST_LOG(info) << "[VDISPLAY] EVDI kernel module detected via sysfs.";
      return true;
    }

    BOOST_LOG(warning) << "[VDISPLAY] EVDI kernel module is not loaded.";
    BOOST_LOG(warning) << "[VDISPLAY] Try: sudo modprobe evdi";
    return false;
  }

  // ============================================================================
  // Utility Functions
  // ============================================================================

  static std::string generate_display_name(const uuid_util::uuid_t &guid) {
    return "VIRTUAL-" + guid.string().substr(0, 8);
  }

  static int find_available_evdi_device(int exclude_slot = -1) {
    // Find next available EVDI device, skipping `exclude_slot` if asked.
    // The exclude is for the resolution-change recreate path: cosmic-comp
    // keeps an fd open to the slot we just closed, so the kernel still
    // reports it as EVDI_AVAILABLE (apollo's fd is gone), but cosmic-comp's
    // cached DRM connector state for that slot would carry over. Forcing
    // a different slot gives cosmic-comp a fresh hotplug for the new EDID.

    // First pass: any existing EVDI_AVAILABLE slot?
    for (int i = 0; i < 16; i++) {
      if (i == exclude_slot) continue;
      if (evdi.check_device(i) == EVDI_AVAILABLE) {
        return i;
      }
    }

    // None available — ask the EVDI module to add one. libevdi's
    // evdi_add_device() writes 1 to /sys/devices/evdi/add; the kernel
    // then picks the next free DRM minor, which after a previous
    // remove_all can be 6, 7, 8, …, not the next sequential slot
    // number. So after the sysfs write we have to scan ALL slots to
    // find the newly-created one — checking the same slot index in a
    // tight loop (the previous implementation) misses devices that
    // landed at a higher index and burns a full second per missed slot.
    int result = evdi.add_device();
    if (result <= 0) {
      BOOST_LOG(warning) << "[VDISPLAY] evdi_add_device returned " << result;
      return -1;
    }
    for (int retry = 0; retry < 40; retry++) {  // up to 2 seconds
      for (int i = 0; i < 16; i++) {
        if (i == exclude_slot) continue;
        if (evdi.check_device(i) == EVDI_AVAILABLE) {
          BOOST_LOG(info) << "[VDISPLAY] Added new EVDI device at slot " << i;
          return i;
        }
      }
      std::this_thread::sleep_for(50ms);
    }
    BOOST_LOG(warning) << "[VDISPLAY] add_device succeeded but no slot became "
                         "EVDI_AVAILABLE within 2s";
    return -1;
  }

  static void calculate_edid_checksum(unsigned char *edid, size_t block_size = 128) {
    uint8_t checksum = 0;
    for (size_t i = 0; i < block_size - 1; i++) {
      checksum += edid[i];
    }
    edid[block_size - 1] = 256 - checksum;
  }

  static void create_detailed_timing_descriptor(unsigned char *dtd, uint32_t width, uint32_t height, uint32_t refresh_rate) {
    // Calculate timing parameters based on CVT (Coordinated Video Timings)
    // These are approximate values for common resolutions

    uint32_t h_blank, v_blank, h_front, h_sync, v_front, v_sync;
    uint32_t pixel_clock_khz;

    if (width == 3840 && height == 2160) {
      // 4K UHD @ 60Hz
      h_blank = 560;
      v_blank = 90;
      h_front = 176;
      h_sync = 88;
      v_front = 8;
      v_sync = 10;
      pixel_clock_khz = 533250; // 533.25 MHz
    } else if (width == 2560 && height == 1440) {
      // 1440p @ 60Hz
      h_blank = 160;
      v_blank = 44;
      h_front = 48;
      h_sync = 32;
      v_front = 3;
      v_sync = 5;
      pixel_clock_khz = 241500; // 241.5 MHz
    } else if (width == 1920 && height == 1080) {
      // 1080p @ 60Hz
      h_blank = 280;
      v_blank = 45;
      h_front = 88;
      h_sync = 44;
      v_front = 4;
      v_sync = 5;
      pixel_clock_khz = 148500; // 148.5 MHz
    } else if (width == 1280 && height == 720) {
      // 720p @ 60Hz
      h_blank = 370;
      v_blank = 30;
      h_front = 110;
      h_sync = 40;
      v_front = 5;
      v_sync = 5;
      pixel_clock_khz = 74250; // 74.25 MHz
    } else {
      // Generic calculation for other resolutions
      // Using simplified CVT formula
      double h_period = (1000000.0 / refresh_rate - 550) / (height + 3);
      double h_total = width + (width * 0.15); // ~15% horizontal blanking
      pixel_clock_khz = static_cast<uint32_t>((h_total / h_period) * 1000);
      h_blank = static_cast<uint32_t>(width * 0.15);
      v_blank = 45;
      h_front = h_blank / 4;
      h_sync = h_blank / 4;
      v_front = 3;
      v_sync = 5;
    }

    uint32_t h_active = width;
    uint32_t v_active = height;

    // Pixel clock in 10kHz units
    uint16_t pixel_clock = pixel_clock_khz / 10;

    // Detailed Timing Descriptor format (18 bytes)
    dtd[0] = pixel_clock & 0xFF;
    dtd[1] = (pixel_clock >> 8) & 0xFF;

    dtd[2] = h_active & 0xFF;
    dtd[3] = h_blank & 0xFF;
    dtd[4] = ((h_active >> 8) & 0x0F) << 4 | ((h_blank >> 8) & 0x0F);

    dtd[5] = v_active & 0xFF;
    dtd[6] = v_blank & 0xFF;
    dtd[7] = ((v_active >> 8) & 0x0F) << 4 | ((v_blank >> 8) & 0x0F);

    dtd[8] = h_front & 0xFF;
    dtd[9] = h_sync & 0xFF;
    dtd[10] = ((v_front & 0x0F) << 4) | (v_sync & 0x0F);
    dtd[11] = (((h_front >> 8) & 0x03) << 6) | (((h_sync >> 8) & 0x03) << 4) |
              (((v_front >> 4) & 0x03) << 2) | ((v_sync >> 4) & 0x03);

    // Physical size (approximate based on 27" diagonal for 4K, scaled for others)
    uint32_t h_size_mm = (width * 600) / 3840;  // 600mm for 4K width
    uint32_t v_size_mm = (height * 340) / 2160; // 340mm for 4K height
    dtd[12] = h_size_mm & 0xFF;
    dtd[13] = v_size_mm & 0xFF;
    dtd[14] = ((h_size_mm >> 8) & 0x0F) << 4 | ((v_size_mm >> 8) & 0x0F);

    dtd[15] = 0; // No border
    dtd[16] = 0; // No border
    dtd[17] = 0x1E; // Digital separate sync, positive H and V
  }

  // Cap a requested resolution to one that fits in a classic EDID DTD.
  //
  // The 18-byte Detailed Timing Descriptor encodes h_active / v_active in
  // 12-bit fields (8 bits in one byte + 4 bits packed into another's upper
  // nibble; see create_detailed_timing_descriptor above). Max representable
  // value is 4095 — anything larger gets silently masked.
  //
  // For 5120x2880, the 0x1400 width truncates to 0x400 (1024) in the EDID.
  // cosmic-comp parses 1024x2880 from the malformed EDID, sets its scanout
  // fb to that, and apollo's correctly-sized 5K capture buffer then
  // mismatches efb->base.width inside the kernel — every grab_pixels
  // returns -EINVAL and the client sees black.
  //
  // Until we add a DisplayID extension block (no 12-bit limit), cap to the
  // nearest standard mode that fits. Aspect-preserving where possible.
  // Returns the requested resolution unchanged if it already fits.
  static std::pair<uint32_t, uint32_t> cap_to_displayable(uint32_t w, uint32_t h) {
    if (w <= 4095 && h <= 4095) {
      return {w, h};
    }
    // Common standard modes in descending pixel count. Pick the candidate
    // with the closest aspect ratio to the request.
    static const std::pair<uint32_t, uint32_t> standards[] = {
      {3840, 2160},  // 4K UHD 16:9 (~1.778)
      {3440, 1440},  // UW 21:9 1440p (~2.389)
      {2560, 1440},  // QHD 16:9 (~1.778)
      {2560, 1080},  // UW 21:9 1080p (~2.370)
      {1920, 1200},  // 16:10 1200p (1.600)
      {1920, 1080},  // FHD 16:9
      {1680, 1050},  // 16:10
      {1280, 720},   // 720p 16:9
    };
    const double aspect = static_cast<double>(w) / static_cast<double>(h);
    double best_diff = std::numeric_limits<double>::max();
    std::pair<uint32_t, uint32_t> best = {3840, 2160};
    for (const auto &s : standards) {
      double s_aspect = static_cast<double>(s.first) / static_cast<double>(s.second);
      double diff = std::abs(s_aspect - aspect);
      // Strict <: ties go to the first (higher pixel count) candidate.
      if (diff < best_diff) {
        best_diff = diff;
        best = s;
      }
    }
    return best;
  }

  // Emit a 128-byte EDID extension block containing a DisplayID 1.3
  // section with a single Type I Detailed Timing for the requested
  // resolution. Used when the requested resolution exceeds the classic
  // EDID DTD's 12-bit fields (>4095 wide/tall) so the base EDID can't
  // express it. cosmic-comp parses DisplayID extension blocks (verified
  // empirically: the user's Samsung LC49G95T's native 5120x1440 mode
  // exceeds DTD limits and shows up in cosmic-randr's mode list, which
  // means cosmic-comp picked it up via the monitor's DisplayID
  // extension), so emitting one for the EVDI should unlock true >4K
  // streaming. The Type I Timing's preferred bit is set so DisplayID-
  // aware parsers prefer this mode over the base EDID's capped DTD.
  static void emit_displayid_extension_block(unsigned char *block128,
                                              uint32_t width,
                                              uint32_t height,
                                              uint32_t refresh_hz) {
    std::memset(block128, 0, 128);

    // EDID extension wrapper.
    block128[0] = 0x70;  // DisplayID extension tag

    // DisplayID section header (4 bytes after the wrapper tag).
    block128[1] = 0x13;  // version 1.3
    block128[2] = 23;    // bytes in section: one Type I Timing data block
                         //   (3-byte db header + 20-byte timing payload)
    block128[3] = 0x03;  // primary use case: Desktop productivity display
    block128[4] = 0x00;  // extension count

    // Data block header: Type I Detailed Timings (tag 0x03).
    block128[5] = 0x03;  // data block tag
    block128[6] = 0x00;  // data block revision
    block128[7] = 20;    // payload bytes

    // Type I Timing payload (20 bytes at block128[8..27]).
    // CVT-RB-ish fixed blanking. The virtual display has no physical
    // pixel-clock constraint, so any internally-consistent timing is
    // accepted by cosmic-comp.
    const uint32_t h_blank = 80;
    const uint32_t h_front = 8;
    const uint32_t h_sync  = 32;
    const uint32_t v_blank = 32;
    const uint32_t v_front = 3;
    const uint32_t v_sync  = 8;
    const uint32_t h_total = width + h_blank;
    const uint32_t v_total = height + v_blank;
    const uint64_t pixel_clock_hz =
        static_cast<uint64_t>(h_total) * v_total * refresh_hz;
    const uint32_t pixel_clock_10khz =
        static_cast<uint32_t>(pixel_clock_hz / 10000);

    // Pixel clock in 10kHz units, 3 bytes little-endian.
    block128[8]  = pixel_clock_10khz & 0xFF;
    block128[9]  = (pixel_clock_10khz >> 8) & 0xFF;
    block128[10] = (pixel_clock_10khz >> 16) & 0xFF;
    // Flags: bit 7 = preferred timing.
    block128[11] = 0x80;

    // All horizontal/vertical fields encoded as (value - 1), 16-bit LE.
    auto le16m1 = [](unsigned char *out, uint32_t v) {
      const uint16_t enc = static_cast<uint16_t>(v - 1);
      out[0] = enc & 0xFF;
      out[1] = (enc >> 8) & 0xFF;
    };
    le16m1(&block128[12], width);   // h_active
    le16m1(&block128[14], h_blank);
    le16m1(&block128[16], h_front);
    le16m1(&block128[18], h_sync);
    le16m1(&block128[20], height);  // v_active
    le16m1(&block128[22], v_blank);
    le16m1(&block128[24], v_front);
    le16m1(&block128[26], v_sync);

    // DisplayID section checksum at block128[28]: sum of block128[1..28] = 0 mod 256.
    uint8_t did_sum = 0;
    for (int i = 1; i <= 27; i++) did_sum += block128[i];
    block128[28] = static_cast<uint8_t>(0x100 - did_sum);

    // EDID extension checksum at block128[127]: sum of all 128 bytes = 0 mod 256.
    uint8_t ext_sum = 0;
    for (int i = 0; i <= 126; i++) ext_sum += block128[i];
    block128[127] = static_cast<uint8_t>(0x100 - ext_sum);
  }

  // generate_edid_for_resolution params:
  //   width / height       — the dims the base EDID's DTD will advertise
  //                          (already capped by cap_to_displayable at the
  //                          call sites so it fits the 12-bit DTD field)
  //   refresh_rate         — refresh in Hz for both the DTD and DisplayID
  //   original_width /     — the dims the client actually requested. If
  //   original_height        these exceed width/height (cap fired), a
  //                          DisplayID extension block is appended that
  //                          advertises the original mode for DisplayID-
  //                          aware compositors.
  static unsigned char *generate_edid_for_resolution(uint32_t width, uint32_t height, uint32_t refresh_rate,
                                                      uint32_t original_width = 0, uint32_t original_height = 0) {
    // Up to 2 extension blocks (CEA-861 + DisplayID 1.3) for >4K modes.
    static unsigned char edid[384];
    memset(edid, 0, sizeof(edid));

    // Block 0: Base EDID
    // Header
    edid[0] = 0x00;
    edid[1] = 0xFF; edid[2] = 0xFF; edid[3] = 0xFF;
    edid[4] = 0xFF; edid[5] = 0xFF; edid[6] = 0xFF;
    edid[7] = 0x00;

    // Manufacturer ID: "APL" (Apollo)
    edid[8] = 0x06; edid[9] = 0x4C;

    // Product code
    edid[10] = 0x01; edid[11] = 0x00;

    // Serial number
    edid[12] = 0x01; edid[13] = 0x00; edid[14] = 0x00; edid[15] = 0x00;

    // Week and year of manufacture (week 1, 2024)
    edid[16] = 0x01; edid[17] = 0x22;

    // EDID version 1.4
    edid[18] = 0x01; edid[19] = 0x04;

    // Video input: Digital, 8-bit color, DisplayPort
    edid[20] = 0xB5;

    // Screen size (cm) - approximate for 27"
    edid[21] = 60; // 60 cm wide
    edid[22] = 34; // 34 cm tall

    // Gamma (2.2)
    edid[23] = 0x78;

    // Features: RGB, preferred timing in DTD1
    edid[24] = 0x3A;

    // Chromaticity coordinates (standard sRGB)
    edid[25] = 0xFC; edid[26] = 0x81; edid[27] = 0xA4; edid[28] = 0x55;
    edid[29] = 0x4D; edid[30] = 0x9D; edid[31] = 0x25; edid[32] = 0x12;
    edid[33] = 0x50; edid[34] = 0x54;

    // Established timings
    edid[35] = 0x21; edid[36] = 0x08; edid[37] = 0x00;

    // Standard timings (8 entries, 2 bytes each)
    // We'll add some common resolutions
    edid[38] = 0xD1; edid[39] = 0xC0; // 1920x1080@60
    edid[40] = 0xB3; edid[41] = 0x00; // 1680x1050@60
    edid[42] = 0xA9; edid[43] = 0xC0; // 1600x900@60
    edid[44] = 0x81; edid[45] = 0x80; // 1280x1024@60
    edid[46] = 0x81; edid[47] = 0xC0; // 1280x720@60
    edid[48] = 0x01; edid[49] = 0x01; // Unused
    edid[50] = 0x01; edid[51] = 0x01; // Unused
    edid[52] = 0x01; edid[53] = 0x01; // Unused

    // Detailed Timing Descriptor 1 (preferred timing)
    create_detailed_timing_descriptor(&edid[54], width, height, refresh_rate);

    // Descriptor 2: Display name
    edid[72] = 0x00; edid[73] = 0x00; edid[74] = 0x00; edid[75] = 0xFC; edid[76] = 0x00;
    const char *name = "APOLLO VDISP";
    for (int i = 0; i < 13 && name[i]; i++) {
      edid[77 + i] = name[i];
    }
    edid[89] = '\n';

    // Descriptor 3: Display range limits
    edid[90] = 0x00; edid[91] = 0x00; edid[92] = 0x00; edid[93] = 0xFD; edid[94] = 0x00;
    edid[95] = 0x18; // Min V rate: 24 Hz
    edid[96] = 0x78; // Max V rate: 120 Hz
    edid[97] = 0x0F; // Min H rate: 15 kHz
    edid[98] = 0xA0; // Max H rate: 160 kHz
    edid[99] = 0x78; // Max pixel clock: 1200 MHz (for 4K@120Hz support)
    edid[100] = 0x00; // GTF support
    edid[101] = 0x0A; // Newline padding
    for (int i = 102; i < 108; i++) edid[i] = 0x20; // Space padding

    // Descriptor 4: Dummy/unused
    edid[108] = 0x00; edid[109] = 0x00; edid[110] = 0x00; edid[111] = 0x10; edid[112] = 0x00;
    for (int i = 113; i < 126; i++) edid[i] = 0x20;

    // Extension count: CEA-861 if >1080p, plus DisplayID if the cap fired.
    // DisplayID lets cosmic-comp see the original (uncapped) requested mode
    // for >4K resolutions that the base EDID's classic DTD can't represent.
    bool needs_extension = (width > 1920 || height > 1080);
    bool needs_displayid = (original_width > 0 && original_height > 0 &&
                            (original_width != width || original_height != height));
    uint8_t ext_count = 0;
    if (needs_extension) ext_count++;
    if (needs_displayid) ext_count++;
    edid[126] = ext_count;

    // Calculate checksum for block 0
    calculate_edid_checksum(edid, 128);

    // Block 1: CEA-861 Extension (for 4K support)
    if (needs_extension) {
      edid[128] = 0x02; // CEA extension tag
      edid[129] = 0x03; // Revision 3
      edid[130] = 0x18; // DTD offset (24 bytes for data blocks)
      edid[131] = 0x72; // Native DTDs, YCbCr support

      // Video Data Block
      edid[132] = 0x47; // Video tag (0x40) + length (7)
      edid[133] = 0x90; // VIC 16: 1080p60 (native)
      edid[134] = 0x04; // VIC 4: 720p60
      edid[135] = 0x03; // VIC 3: 480p60
      edid[136] = 0x5F; // VIC 95: 4K@60Hz (VIC 95)
      edid[137] = 0x60; // VIC 96: 4K@60Hz (VIC 96)
      edid[138] = 0x61; // VIC 97: 4K@60Hz (VIC 97)
      edid[139] = 0x65; // VIC 101: 4K@120Hz

      // HDMI Vendor Specific Data Block
      edid[140] = 0x67; // Vendor tag (0x60) + length (7)
      edid[141] = 0x03; // IEEE OUI for HDMI (0x000C03)
      edid[142] = 0x0C;
      edid[143] = 0x00;
      edid[144] = 0x10; // Source physical address
      edid[145] = 0x00;
      edid[146] = 0x00; // Supports AI, DC 48/36/30 bit
      edid[147] = 0x78; // Max TMDS clock / 5 MHz = 600 MHz

      // Detailed Timing Descriptor for 4K if needed
      if (width >= 3840) {
        create_detailed_timing_descriptor(&edid[152], 3840, 2160, 60);
      }

      // Calculate checksum for block 1
      calculate_edid_checksum(&edid[128], 128);
    }

    // Block 2: DisplayID 1.3 extension for >4K modes that don't fit in
    // the classic DTD. Placed at edid[256..383]. Only emitted when the
    // caller signalled that the cap fired (original_w/h > width/height).
    // Slot is fixed at 256 even when no CEA-861 block precedes it; the
    // EDID extension count (edid[126]) controls how many blocks parsers
    // walk. We always populate at the same slot for simplicity; if the
    // base needed no CEA-861 but did need DisplayID, edid[126]=1 and
    // edid[128..255] is zeroed-but-unused — not ideal, but it never
    // happens in practice because the >4K cap implies width > 1920.
    if (needs_displayid) {
      emit_displayid_extension_block(&edid[256],
                                      original_width, original_height,
                                      refresh_rate);
    }

    return edid;
  }

  // Returns the active byte size of the EDID buffer for the given
  // resolution combo. Mirrors the size logic the EDID generator uses so
  // call sites can pass the right size to evdi.connect without rebuilding
  // the EDID. Inputs are post-cap (what generate_edid_for_resolution sees
  // as width/height) plus the original requested dims.
  static unsigned int edid_size_for(uint32_t edid_width, uint32_t edid_height,
                                     uint32_t original_width,
                                     uint32_t original_height) {
    unsigned int size = (edid_width > 1920 || edid_height > 1080) ? 256 : 128;
    if (original_width > 0 && original_height > 0 &&
        (original_width != edid_width || original_height != edid_height)) {
      size = 384;
    }
    return size;
  }

  // ============================================================================
  // Public API Implementation
  // ============================================================================

  // Returns true if any process other than ourselves currently holds an
  // open fd to a DRM device backed by the EVDI driver. Used to gate the
  // destructive /sys/devices/evdi/remove_all write in initVDisplayDriver:
  // if cosmic-comp (or another compositor) still has /dev/dri/cardN open
  // from a previous apollo run, tearing the device out of the kernel
  // pulls cosmic-comp's DRM-master grip with it and the next hotplug
  // hits smithay's `Unable to become drm master, assuming unprivileged
  // mode` fail-soft, after which every modeset apply fails and the
  // compositor is effectively wedged. Skipping remove_all leaves the
  // existing AVAILABLE slot ready for find_or_create_slot to reuse,
  // and the compositor never notices apollo restarted.
  //
  // Generic across hardware: card numbers vary by system (1× iGPU + 1×
  // dGPU + EVDI gives /dev/dri/card3 here, but other configurations
  // land EVDI at a different minor). We enumerate by driver name in
  // sysfs rather than assuming a path.
  static bool evdi_devices_held_by_other_process() {
    std::vector<std::string> evdi_paths;
    std::error_code ec;
    for (auto &entry : fs::directory_iterator("/sys/class/drm", ec)) {
      if (ec) break;
      const std::string name = entry.path().filename().string();
      // Only bare cardN, not cardN-... output subdirs.
      if (name.rfind("card", 0) != 0) continue;
      if (name.find('-') != std::string::npos) continue;
      // Driver symlink target's basename tells us which driver owns it.
      fs::path driver_link = entry.path() / "device" / "driver";
      std::error_code rl_ec;
      fs::path driver_target = fs::read_symlink(driver_link, rl_ec);
      if (rl_ec) continue;
      if (driver_target.filename().string() != "evdi") continue;
      evdi_paths.emplace_back("/dev/dri/" + name);
    }
    if (evdi_paths.empty()) return false;

    const pid_t self = ::getpid();
    for (auto &proc_entry : fs::directory_iterator("/proc", ec)) {
      if (ec) break;
      const std::string pid_name = proc_entry.path().filename().string();
      // PID directories only.
      if (pid_name.empty() || !std::isdigit(static_cast<unsigned char>(pid_name[0]))) {
        continue;
      }
      pid_t pid = 0;
      try { pid = std::stoi(pid_name); } catch (...) { continue; }
      if (pid <= 0 || pid == self) continue;
      std::error_code fd_ec;
      auto fd_iter = fs::directory_iterator(proc_entry.path() / "fd", fd_ec);
      if (fd_ec) continue; // EACCES on processes we can't inspect — skip silently
      for (auto &fd_entry : fd_iter) {
        std::error_code link_ec;
        fs::path target = fs::read_symlink(fd_entry.path(), link_ec);
        if (link_ec) continue;
        const std::string target_str = target.string();
        for (const auto &p : evdi_paths) {
          if (target_str == p) {
            BOOST_LOG(info) << "[VDISPLAY] EVDI device " << p
                            << " is held open by pid " << pid
                            << " — preserving it (skipping remove_all)";
            return true;
          }
        }
      }
    }
    return false;
  }

  DRIVER_STATUS openVDisplayDevice() {
    std::lock_guard<std::mutex> lock(vdisplay_mutex);

    if (driver_status == DRIVER_STATUS::OK) {
      return driver_status;
    }

    BOOST_LOG(info) << "[VDISPLAY] Initializing Linux virtual display driver...";

    // Try to load EVDI library
    evdi_available = load_evdi_library();

    if (evdi_available) {
      // Check if kernel module is loaded
      if (!check_evdi_module_loaded()) {
        BOOST_LOG(warning) << "[VDISPLAY] EVDI library loaded but kernel module not available.";
        BOOST_LOG(warning) << "[VDISPLAY] Falling back to passthrough mode.";
        evdi_available = false;
      }
    }

    if (evdi_available) {
      BOOST_LOG(info) << "[VDISPLAY] EVDI available - real virtual displays supported!";

      // Wipe any leftover EVDI slots from previous apollo runs (and from
      // resolution-change recreates within this run that haven't been
      // cleaned). cosmic-comp keeps fds open to all the EVDI slots it
      // ever saw, so without this they'd accumulate up to the kernel's
      // 16-slot limit before exhausting.
      //
      // BUT: only if no other process still holds an EVDI device open.
      // If apollo restarted (e.g. evdi_grab's "Falling back to apollo
      // restart for monitor restore"), cosmic-comp still has the prior
      // /dev/dri/cardN fd open. Tearing the device out of the kernel
      // makes the next EVDI hotplug land in smithay's "Unable to become
      // drm master, assuming unprivileged mode" branch — after that,
      // every modeset apply fails inside drm.initialize_output and the
      // compositor wedges. Better to keep the slot alive: when previous
      // apollo's evdi_open fd closed, libevdi already flipped the slot
      // to EVDI_AVAILABLE, so find_or_create_slot picks it up cleanly
      // and the next session reuses the same /dev/dri/cardN without
      // any new hotplug churn for the compositor to navigate.
      if (evdi_devices_held_by_other_process()) {
        BOOST_LOG(info) << "[VDISPLAY] Skipping remove_all: another process "
                           "(likely cosmic-comp from a prior apollo run) still "
                           "holds an EVDI device open. Existing AVAILABLE slots "
                           "will be reused by find_or_create_slot.";
      } else if (FILE *f = std::fopen("/sys/devices/evdi/remove_all", "w")) {
        std::fputc('1', f);
        std::fclose(f);
        BOOST_LOG(info) << "[VDISPLAY] Cleared leftover EVDI slots via remove_all";
      } else {
        BOOST_LOG(warning) << "[VDISPLAY] Couldn't write /sys/devices/evdi/remove_all "
                              "(udev rules / video group permissions?) — EVDI slot "
                              "accumulation may eventually exhaust the 16-slot limit";
      }
    } else {
      BOOST_LOG(warning) << "[VDISPLAY] EVDI not available - using passthrough mode.";
      BOOST_LOG(warning) << "[VDISPLAY] The stream will capture the physical display.";
    }

    driver_status = DRIVER_STATUS::OK;
    BOOST_LOG(info) << "[VDISPLAY] Linux virtual display driver initialized successfully.";

    return driver_status;
  }

  void closeVDisplayDevice() {
    std::lock_guard<std::mutex> lock(vdisplay_mutex);

    BOOST_LOG(info) << "[VDISPLAY] Closing Linux virtual display driver...";

    // Stop watchdog thread
    watchdog_running = false;
    if (watchdog_thread.joinable()) {
      watchdog_thread.join();
    }

    virtual_displays.clear();
    capture_states.clear();

    // Tear down the shared EVDI device, if it's still open.
    if (shared_evdi.handle) {
      if (evdi.disconnect) evdi.disconnect(shared_evdi.handle);
      if (evdi.close) evdi.close(shared_evdi.handle);
    }
    if (shared_evdi.drm_fd >= 0) {
      ::close(shared_evdi.drm_fd);
    }
    shared_evdi = {-1, nullptr, -1, 0};

    // Unload EVDI library
    unload_evdi_library();

    driver_status = DRIVER_STATUS::UNKNOWN;
    BOOST_LOG(info) << "[VDISPLAY] Linux virtual display driver closed.";
  }

  int disconnectAllEvdiMonitors() {
    std::lock_guard<std::mutex> lock(vdisplay_mutex);
    int count = 0;
    for (auto &[guid, vdinfo] : virtual_displays) {
      if (vdinfo.active && vdinfo.using_evdi && vdinfo.handle && evdi.disconnect) {
        evdi.disconnect(vdinfo.handle);
        count++;
      }
    }
    // Intentionally NOT clearing virtual_displays / capture_states /
    // persistent_evdi here — apollo's normal session teardown
    // (removeVirtualDisplay) will tidy up its own state. This helper is
    // only responsible for forcing cosmic-comp to see the monitor hot-
    // unplug so the upcoming kdl apply doesn't fight the EVDI plane.
    return count;
  }

  bool startPingThread(std::function<void()> failCb) {
    std::lock_guard<std::mutex> lock(vdisplay_mutex);

    if (watchdog_running) {
      return true;
    }

    watchdog_running = true;

    watchdog_thread = std::thread([failCb = std::move(failCb)]() {
      BOOST_LOG(debug) << "[VDISPLAY] Watchdog thread started.";

      while (watchdog_running) {
        std::this_thread::sleep_for(5s);

        if (!watchdog_running) {
          break;
        }

        std::lock_guard<std::mutex> lock(vdisplay_mutex);

        for (const auto &[guid, vdinfo] : virtual_displays) {
          if (vdinfo.active && vdinfo.using_evdi && vdinfo.handle) {
            // Check EVDI device health
            int ready = evdi.get_event_ready(vdinfo.handle);
            if (ready < 0) {
              BOOST_LOG(error) << "[VDISPLAY] Virtual display " << vdinfo.name << " lost!";
              if (failCb) {
                failCb();
              }
              return;
            }
          }
        }
      }

      BOOST_LOG(debug) << "[VDISPLAY] Watchdog thread stopped.";
    });

    return true;
  }

  bool setRenderAdapterByName(const std::string &adapterName) {
    std::lock_guard<std::mutex> lock(vdisplay_mutex);

    if (adapterName.empty()) {
      BOOST_LOG(debug) << "[VDISPLAY] No specific adapter requested.";
      return true;
    }

    BOOST_LOG(info) << "[VDISPLAY] Adapter hint: " << adapterName;
    // On Linux, we don't need to select specific adapters for EVDI
    return true;
  }

  std::string createVirtualDisplay(
    const char *s_client_uid,
    const char *s_client_name,
    uint32_t width,
    uint32_t height,
    uint32_t fps,
    const uuid_util::uuid_t &guid
  ) {
    std::lock_guard<std::mutex> lock(vdisplay_mutex);

    if (driver_status != DRIVER_STATUS::OK) {
      BOOST_LOG(error) << "[VDISPLAY] Driver not initialized.";
      return "";
    }

    std::string guid_str = guid.string();
    std::string display_name = generate_display_name(guid);

    // Convert fps from mHz to Hz
    uint32_t fps_hz = fps / 1000;

    // Cap to what apollo's classic-DTD EDID can represent. The cap is
    // applied to both the EDID generation AND the vdinfo width/height,
    // so the capture buffer is sized to match cosmic-comp's scanout fb.
    // The streaming config (and thus encoder ctx) keeps the original
    // requested dims — swscale upscales the captured frame on its way
    // to the encoder, so the client still receives video at the
    // resolution it asked for, just upscaled from the capped capture.
    //
    // We also remember the original requested dims so a DisplayID
    // extension can advertise them — DisplayID-aware compositors
    // (cosmic-comp included) will see and offer the original mode in
    // their output mode list. This is a transparent upgrade: parsers
    // that ignore DisplayID still see the capped DTD as before.
    const uint32_t original_width = width;
    const uint32_t original_height = height;
    auto [capped_w, capped_h] = cap_to_displayable(width, height);
    if (capped_w != width || capped_h != height) {
      BOOST_LOG(info) << "[VDISPLAY] Capping requested " << width << "x"
                      << height << " to " << capped_w << "x" << capped_h
                      << " (classic EDID DTD limit is 4095). DisplayID "
                         "extension block will also advertise the original "
                         "mode for DisplayID-aware parsers. Encoder still "
                         "operates at requested dims.";
      width = capped_w;
      height = capped_h;
    }

    BOOST_LOG(info) << "[VDISPLAY] Creating virtual display: " << display_name
                    << " (W: " << width << ", H: " << height << ", FPS: " << fps_hz << ")";
    BOOST_LOG(info) << "[VDISPLAY] Client: " << s_client_name << " (" << s_client_uid << ")";

    VirtualDisplayInfo vdinfo;
    vdinfo.name = display_name;
    vdinfo.guid_str = guid_str;
    // When the DisplayID extension is in play (cap fired), cosmic-comp
    // honors the DisplayID's original mode and sets its EVDI scanout fb
    // to those dims. The capture buffer must match that scanout — verified
    // empirically: kernel evdi log showed `Notifying mode changed: 5120
    // x2880@30` on a 5K request, then grabpix returned -EINVAL for every
    // call because vdinfo (and thus the buffer) was sized to the capped
    // 4K. Use the original dims for vdinfo when DisplayID is emitted.
    const bool using_displayid =
        (original_width != width || original_height != height);
    vdinfo.width  = using_displayid ? original_width  : width;
    vdinfo.height = using_displayid ? original_height : height;
    vdinfo.fps = fps;
    vdinfo.device_index = -1;
    vdinfo.handle = nullptr;
    vdinfo.drm_fd = -1;
    vdinfo.active = true;
    vdinfo.using_evdi = false;

    if (evdi_available) {
      if (shared_evdi.handle == nullptr) {
        // First-ever stream session. Open a kernel-side EVDI device
        // and connect an EDID for this client's requested resolution.
        int device = find_available_evdi_device();
        evdi_handle handle = nullptr;
        if (device >= 0) {
          handle = evdi.open(device);
          if (handle) {
            BOOST_LOG(info) << "[VDISPLAY] Allocated EVDI device at slot " << device;
          } else {
            BOOST_LOG(warning) << "[VDISPLAY] Failed to open EVDI device " << device;
          }
        } else {
          BOOST_LOG(warning) << "[VDISPLAY] No available EVDI device, using passthrough.";
        }
        if (handle) {
          unsigned char *edid = generate_edid_for_resolution(width, height, fps_hz,
                                                              original_width, original_height);
          unsigned int edid_size = edid_size_for(width, height,
                                                  original_width, original_height);
          BOOST_LOG(info) << "[VDISPLAY] Connecting EDID " << width << "x" << height
                          << " (" << edid_size << " bytes"
                          << (edid_size == 384 ? ", with DisplayID for original "
                              + std::to_string(original_width) + "x"
                              + std::to_string(original_height) : "") << ")";
          evdi.connect(handle, edid, edid_size, 0);
          std::string card_path = "/dev/dri/card" + std::to_string(device);
          shared_evdi.device_index = device;
          shared_evdi.handle = handle;
          shared_evdi.drm_fd = ::open(card_path.c_str(), O_RDONLY | O_CLOEXEC);
          shared_evdi.refcount = 1;
        }
      } else if (shared_evdi.refcount == 0) {
        // Existing EVDI device is between streams (no active sessions).
        // The previous client's EDID is disconnected (we did that on
        // last-session-end). Reconnect with this client's EDID — this
        // is the "first session after all clients disconnected" path
        // and DOES update the resolution to match this client. The
        // 200ms sleep gives cosmic-comp time to process the prior
        // unplug before the new plug arrives.
        evdi.disconnect(shared_evdi.handle);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        unsigned char *edid = generate_edid_for_resolution(width, height, fps_hz,
                                                            original_width, original_height);
        unsigned int edid_size = edid_size_for(width, height,
                                                original_width, original_height);
        BOOST_LOG(info) << "[VDISPLAY] Reconnecting EDID " << width << "x" << height
                        << " (new first session)";
        evdi.connect(shared_evdi.handle, edid, edid_size, 0);
        shared_evdi.refcount = 1;
      } else {
        // Another client is already streaming. Join as a viewer at
        // the existing resolution — no EDID change.
        shared_evdi.refcount++;
        BOOST_LOG(info) << "[VDISPLAY] Joining existing EVDI as viewer "
                        << "(refcount " << shared_evdi.refcount
                        << "); client's " << width << "x" << height
                        << " ignored, stream stays at the first session's mode";
      }

      if (shared_evdi.handle) {
        vdinfo.device_index = shared_evdi.device_index;
        vdinfo.handle = shared_evdi.handle;
        vdinfo.drm_fd = shared_evdi.drm_fd;
        vdinfo.using_evdi = true;
      }
    }

    if (!vdinfo.using_evdi) {
      // Passthrough mode - just track the virtual display logically
      BOOST_LOG(info) << "[VDISPLAY] Using passthrough mode (no EVDI).";
      BOOST_LOG(info) << "[VDISPLAY] Stream will capture primary physical display.";
    }

    virtual_displays[guid_str] = vdinfo;

    BOOST_LOG(info) << "[VDISPLAY] Virtual display created successfully: " << display_name;
    BOOST_LOG(info) << "[VDISPLAY] Mode: " << (vdinfo.using_evdi ? "EVDI (real virtual display)" : "Passthrough");

    return display_name;
  }

  bool removeVirtualDisplay(const uuid_util::uuid_t &guid) {
    std::lock_guard<std::mutex> lock(vdisplay_mutex);

    std::string guid_str = guid.string();

    auto it = virtual_displays.find(guid_str);
    if (it == virtual_displays.end()) {
      BOOST_LOG(warning) << "[VDISPLAY] Virtual display not found: " << guid_str;
      return false;
    }

    auto &vdinfo = it->second;
    BOOST_LOG(info) << "[VDISPLAY] Removing virtual display: " << vdinfo.name;

    // Free the per-session capture state first (registered buffer etc.).
    auto cs_it = capture_states.find(guid_str);
    if (cs_it != capture_states.end()) {
      if (vdinfo.handle && evdi.unregister_buffer) {
        evdi.unregister_buffer(vdinfo.handle, cs_it->second->buffer_id);
      }
      capture_states.erase(cs_it);
    }

    virtual_displays.erase(it);

    // Shared-EVDI bookkeeping. Decrement refcount; when it hits 0 we
    // ONLY disconnect the EDID (cosmic-comp drops it from its layout)
    // but keep the underlying kernel device + handle + drm_fd alive
    // for apollo's lifetime. Closing the handle here would race with
    // the still-exiting videoThread inside captureFrame — evdi.close
    // blocks waiting for the in-flight ioctl to settle, and that
    // burns through apollo's session::join 10-second hang detect →
    // SIGABRT → forced restart. The handle gets closed properly at
    // apollo shutdown via closeVDisplayDevice.
    if (vdinfo.using_evdi && vdinfo.handle == shared_evdi.handle) {
      shared_evdi.refcount--;
      BOOST_LOG(info) << "[VDISPLAY] Shared EVDI refcount now " << shared_evdi.refcount;
      if (shared_evdi.refcount <= 0) {
        shared_evdi.refcount = 0;
        if (shared_evdi.handle && evdi.disconnect) {
          BOOST_LOG(info) << "[VDISPLAY] All sessions ended — disconnecting EDID "
                             "(keeping device open for next session)";
          evdi.disconnect(shared_evdi.handle);
        }
      }
    }

    BOOST_LOG(info) << "[VDISPLAY] Virtual display removed successfully.";
    return true;
  }

  int changeDisplaySettings(const char *deviceName, int width, int height, int refresh_rate) {
    std::lock_guard<std::mutex> lock(vdisplay_mutex);

    // Convert from mHz to Hz
    int refresh_hz = refresh_rate / 1000;

    // Same cap as createVirtualDisplay — see comment there. Preserve
    // the originals for the DisplayID extension.
    const uint32_t original_width = static_cast<uint32_t>(width);
    const uint32_t original_height = static_cast<uint32_t>(height);
    auto [capped_w, capped_h] = cap_to_displayable(original_width, original_height);
    if (static_cast<int>(capped_w) != width ||
        static_cast<int>(capped_h) != height) {
      BOOST_LOG(info) << "[VDISPLAY] Capping requested " << width << "x"
                      << height << " to " << capped_w << "x" << capped_h
                      << " (EDID DTD limit). DisplayID extension block "
                         "will also advertise the original mode.";
      width = static_cast<int>(capped_w);
      height = static_cast<int>(capped_h);
    }

    BOOST_LOG(info) << "[VDISPLAY] Changing display settings for " << deviceName
                    << " to " << width << "x" << height << "@" << refresh_hz << "Hz";

    // See createVirtualDisplay — when DisplayID is emitted, vdinfo must
    // carry the ORIGINAL dims so the capture buffer matches cosmic-comp's
    // scanout fb (which goes to the original mode via DisplayID).
    const bool using_displayid =
        (static_cast<uint32_t>(width) != original_width ||
         static_cast<uint32_t>(height) != original_height);
    const int vdinfo_w = using_displayid ? static_cast<int>(original_width)  : width;
    const int vdinfo_h = using_displayid ? static_cast<int>(original_height) : height;
    // Find the virtual display
    for (auto &[guid, vdinfo] : virtual_displays) {
      if (vdinfo.name == deviceName) {
        vdinfo.width = vdinfo_w;
        vdinfo.height = vdinfo_h;
        vdinfo.fps = refresh_rate;

        if (vdinfo.using_evdi && vdinfo.handle) {
          // Reconnect with new EDID for new resolution
          evdi.disconnect(vdinfo.handle);
          unsigned char *edid = generate_edid_for_resolution(width, height, refresh_hz,
                                                              original_width, original_height);
          unsigned int edid_size = edid_size_for(static_cast<uint32_t>(width),
                                                  static_cast<uint32_t>(height),
                                                  original_width, original_height);
          BOOST_LOG(info) << "[VDISPLAY] Reconnecting with " << edid_size << "-byte EDID for " << width << "x" << height;
          evdi.connect(vdinfo.handle, edid, edid_size, 0);
        }

        BOOST_LOG(info) << "[VDISPLAY] Display settings updated successfully.";
        return 0;
      }
    }

    BOOST_LOG(debug) << "[VDISPLAY] Display not found: " << deviceName;
    return 0;
  }

  int changeDisplaySettings2(const char *deviceName, int width, int height, int refresh_rate, bool bApplyIsolated) {
    if (bApplyIsolated) {
      BOOST_LOG(debug) << "[VDISPLAY] Isolated mode is implicit with EVDI.";
    }
    return changeDisplaySettings(deviceName, width, height, refresh_rate);
  }

  std::string getPrimaryDisplay() {
    // Return first connected physical display
    try {
      for (const auto &entry : fs::directory_iterator("/dev/dri")) {
        const auto &path = entry.path();
        std::string filename = path.filename().string();
        if (filename.find("card") == 0 && filename.find("render") == std::string::npos) {
          int fd = ::open(path.c_str(), O_RDWR);
          if (fd >= 0) {
            drmModeRes *res = drmModeGetResources(fd);
            if (res) {
              for (int i = 0; i < res->count_connectors; i++) {
                drmModeConnector *conn = drmModeGetConnector(fd, res->connectors[i]);
                if (conn && conn->connection == DRM_MODE_CONNECTED) {
                  std::string name = "HDMI-A-" + std::to_string(conn->connector_type_id);
                  drmModeFreeConnector(conn);
                  drmModeFreeResources(res);
                  ::close(fd);
                  return name;
                }
                if (conn) drmModeFreeConnector(conn);
              }
              drmModeFreeResources(res);
            }
            ::close(fd);
          }
        }
      }
    } catch (...) {}
    return "";
  }

  bool setPrimaryDisplay(const char *primaryDeviceName) {
    BOOST_LOG(debug) << "[VDISPLAY] setPrimaryDisplay is a no-op on Linux.";
    return true;
  }

  bool getDisplayHDRByName(const char *displayName) {
    BOOST_LOG(debug) << "[VDISPLAY] HDR check for: " << displayName;
    // EVDI doesn't support HDR currently
    return false;
  }

  bool setDisplayHDRByName(const char *displayName, bool enableAdvancedColor) {
    BOOST_LOG(debug) << "[VDISPLAY] HDR setting not supported on Linux/EVDI.";
    return false;
  }

  std::vector<std::string> matchDisplay(const std::string &sMatch) {
    std::vector<std::string> matches;

    std::lock_guard<std::mutex> lock(vdisplay_mutex);

    for (const auto &[guid, vdinfo] : virtual_displays) {
      if (vdinfo.active && vdinfo.name.find(sMatch) != std::string::npos) {
        matches.push_back(vdinfo.name);
      }
    }

    return matches;
  }

  // ============================================================================
  // EVDI-specific functions for KMS integration
  // ============================================================================

  /**
   * @brief Check if a display name is an EVDI virtual display.
   */
  bool isEvdiDisplay(const std::string &displayName) {
    if (!evdi_available) {
      return false;
    }

    std::lock_guard<std::mutex> lock(vdisplay_mutex);
    for (const auto &[guid, vdinfo] : virtual_displays) {
      if (vdinfo.name == displayName && vdinfo.using_evdi) {
        return true;
      }
    }
    return false;
  }

  /**
   * @brief Get the DRM card index for an EVDI display.
   * @return Card index, or -1 if not found.
   */
  int getEvdiCardIndex(const std::string &displayName) {
    std::lock_guard<std::mutex> lock(vdisplay_mutex);
    for (const auto &[guid, vdinfo] : virtual_displays) {
      if (vdinfo.name == displayName && vdinfo.using_evdi) {
        return vdinfo.device_index;
      }
    }
    return -1;
  }

  std::string firstActiveEvdiName() {
    std::lock_guard<std::mutex> lock(vdisplay_mutex);
    for (const auto &[guid, vdinfo] : virtual_displays) {
      if (vdinfo.active && vdinfo.using_evdi) {
        return vdinfo.name;
      }
    }
    return {};
  }

  // ==========================================================================
  // EVDI frame consumer implementation
  // ==========================================================================

  namespace {
    // Lookup helper: returns the GUID string of the named virtual display, or
    // an empty string if not found / not EVDI-backed. Caller must already hold
    // vdisplay_mutex.
    std::string find_guid_by_name_locked(const std::string &displayName) {
      for (const auto &[guid, vdinfo] : virtual_displays) {
        if (vdinfo.name == displayName && vdinfo.using_evdi) {
          return guid;
        }
      }
      return {};
    }

    // EVDI update_ready callback. Fires when the kernel has copied a new frame
    // into our registered buffer. user_data is the CaptureState* for the
    // associated virtual display.
    void on_update_ready(int /*buffer_id*/, void *user_data) {
      auto *state = static_cast<CaptureState *>(user_data);
      if (state) {
        state->update_ready.store(true, std::memory_order_release);
      }
    }

    void on_mode_changed(struct evdi_mode mode, void *user_data) {
      // cosmic-comp set a new mode on the EVDI head. Until this fires after
      // a fresh evdi.connect, the kernel rejects grab_pixels with EINVAL
      // ("Invalid argument") and the buffer stays zeroed — which is what
      // showed up on the client as a black screen on every reconnect.
      auto *state = static_cast<CaptureState *>(user_data);
      if (state) {
        state->mode_width.store(mode.width, std::memory_order_release);
        state->mode_height.store(mode.height, std::memory_order_release);
        state->mode_ready.store(true, std::memory_order_release);
      }
    }

    void on_dpms(int /*dpms_mode*/, void * /*user_data*/) {
    }

    void on_crtc_state(int /*state*/, void * /*user_data*/) {
    }

    // cosmic-comp renders the cursor on a hardware cursor plane that's
    // separate from the primary framebuffer, so grab_pixels never sees it.
    // EVDI exposes the cursor pixels + position via these two callbacks.
    void on_cursor_set(struct evdi_cursor_set cursor_set, void *user_data) {
      auto *state = static_cast<CaptureState *>(user_data);
      if (!state) return;
      std::lock_guard<std::mutex> lock(state->cursor_mutex);
      if (!cursor_set.enabled || cursor_set.buffer == nullptr ||
          cursor_set.width == 0 || cursor_set.height == 0) {
        state->cursor_enabled.store(false, std::memory_order_release);
        return;
      }
      state->cursor_width = static_cast<int>(cursor_set.width);
      state->cursor_height = static_cast<int>(cursor_set.height);
      state->cursor_hot_x = cursor_set.hot_x;
      state->cursor_hot_y = cursor_set.hot_y;
      size_t bytes = static_cast<size_t>(cursor_set.width) * cursor_set.height * 4;
      state->cursor_pixels.assign(bytes, 0);
      std::memcpy(state->cursor_pixels.data(), cursor_set.buffer, bytes);
      state->cursor_enabled.store(true, std::memory_order_release);
    }

    void on_cursor_move(struct evdi_cursor_move cursor_move, void *user_data) {
      auto *state = static_cast<CaptureState *>(user_data);
      if (!state) return;
      state->cursor_pos_x.store(cursor_move.x, std::memory_order_release);
      state->cursor_pos_y.store(cursor_move.y, std::memory_order_release);
    }
  }  // namespace

  bool registerCaptureBuffer(const std::string &displayName) {
    std::lock_guard<std::mutex> lock(vdisplay_mutex);

    auto guid = find_guid_by_name_locked(displayName);
    if (guid.empty()) {
      BOOST_LOG(warning) << "[VDISPLAY] registerCaptureBuffer: display "
                         << displayName << " not found or not EVDI";
      return false;
    }

    auto &vdinfo = virtual_displays[guid];
    if (!vdinfo.using_evdi || !vdinfo.handle) {
      BOOST_LOG(warning) << "[VDISPLAY] registerCaptureBuffer: no EVDI handle for "
                         << displayName;
      return false;
    }

    if (capture_states.count(guid)) {
      // Already registered. Treat as success — caller may have been retrying.
      return true;
    }

    auto state = std::make_unique<CaptureState>();
    state->width = static_cast<int>(vdinfo.width);
    state->height = static_cast<int>(vdinfo.height);
    state->stride = state->width * 4;  // BGRX/BGRA, 4 bytes per pixel
    state->buffer.assign(static_cast<size_t>(state->stride) * state->height, 0);
    state->dirty_rects.assign(16, evdi_rect {0, 0, 0, 0});
    state->buffer_id = 1;
    state->update_ready.store(false, std::memory_order_release);
    // Assume the mode is already settled on the EVDI device. evdi.connect()
    // fires mode_changed once per cosmic-comp hotplug, but Apollo creates
    // multiple display_t / CaptureState instances per stream session (one
    // per encoder probe iteration) without a fresh connect — so the event
    // never re-fires for the 2nd+ buffer registration, captureFrame would
    // wait forever, and width/height collapse to 0 in the probe result,
    // producing swscaler srcw=0 errors and black frames on the client.
    // Marking mode_ready true is safe: if the mode actually isn't settled,
    // grab_pixels returns EINVAL and the watchdog/retry handles it.
    state->mode_ready.store(true, std::memory_order_release);
    state->mode_width.store(state->width, std::memory_order_release);
    state->mode_height.store(state->height, std::memory_order_release);

    evdi_buffer ebuf {};
    ebuf.id = state->buffer_id;
    ebuf.buffer = state->buffer.data();
    ebuf.width = state->width;
    ebuf.height = state->height;
    ebuf.stride = state->stride;
    ebuf.rects = state->dirty_rects.data();
    ebuf.rect_count = static_cast<int>(state->dirty_rects.size());

    evdi.register_buffer(vdinfo.handle, ebuf);

    // Ask EVDI to deliver cursor events so we can composite the cursor onto
    // the captured framebuffer. cosmic-comp paints the cursor on a separate
    // hardware plane that grab_pixels never sees; without this the stream
    // shows the desktop but no pointer.
    if (evdi.enable_cursor_events) {
      evdi.enable_cursor_events(vdinfo.handle, true);
    }

    BOOST_LOG(info) << "[VDISPLAY] Registered capture buffer for " << displayName
                    << " (" << state->width << "x" << state->height
                    << ", " << state->buffer.size() << " bytes)";

    capture_states.emplace(guid, std::move(state));
    return true;
  }

  void blendCursor(const std::string &displayName,
                    uint8_t *dst,
                    int dst_width,
                    int dst_height,
                    int dst_stride) {
    if (!dst || dst_width <= 0 || dst_height <= 0) return;

    CaptureState *state = nullptr;
    {
      std::lock_guard<std::mutex> lock(vdisplay_mutex);
      auto guid = find_guid_by_name_locked(displayName);
      if (guid.empty()) return;
      auto cs_it = capture_states.find(guid);
      if (cs_it == capture_states.end()) return;
      state = cs_it->second.get();
    }

    if (!state->cursor_enabled.load(std::memory_order_acquire)) return;

    std::lock_guard<std::mutex> cursor_lock(state->cursor_mutex);
    if (state->cursor_pixels.empty() ||
        state->cursor_width <= 0 ||
        state->cursor_height <= 0) {
      return;
    }

    int cursor_x = state->cursor_pos_x.load(std::memory_order_acquire) - state->cursor_hot_x;
    int cursor_y = state->cursor_pos_y.load(std::memory_order_acquire) - state->cursor_hot_y;

    const uint8_t *cursor = state->cursor_pixels.data();
    const int cw = state->cursor_width;
    const int ch = state->cursor_height;

    // EVDI cursor pixels are in the same byte order as the framebuffer
    // (BGRA after our R/B swap on copy). The cursor buffer is pre-multiplied
    // alpha; standard "over" blend.
    for (int row = 0; row < ch; ++row) {
      int dy = cursor_y + row;
      if (dy < 0 || dy >= dst_height) continue;
      for (int col = 0; col < cw; ++col) {
        int dx = cursor_x + col;
        if (dx < 0 || dx >= dst_width) continue;

        const uint8_t *src_px = cursor + (row * cw + col) * 4;
        uint8_t *dst_px = dst + dy * dst_stride + dx * 4;
        uint8_t a = src_px[3];
        if (a == 0) continue;
        // EVDI cursor is RGBA in memory; we want BGRA. Swap R<->B as we blend.
        uint8_t sr = src_px[0];
        uint8_t sg = src_px[1];
        uint8_t sb = src_px[2];
        if (a == 255) {
          dst_px[0] = sb;
          dst_px[1] = sg;
          dst_px[2] = sr;
        } else {
          // Standard over blend: dst = src + dst * (1 - src_alpha).
          // src is already premultiplied per EVDI cursor convention.
          uint16_t inv_a = 255 - a;
          dst_px[0] = static_cast<uint8_t>(sb + (dst_px[0] * inv_a) / 255);
          dst_px[1] = static_cast<uint8_t>(sg + (dst_px[1] * inv_a) / 255);
          dst_px[2] = static_cast<uint8_t>(sr + (dst_px[2] * inv_a) / 255);
        }
      }
    }
  }

  void resetModeReady(const std::string &displayName) {
    std::lock_guard<std::mutex> lock(vdisplay_mutex);
    auto guid = find_guid_by_name_locked(displayName);
    if (guid.empty()) return;
    auto cs_it = capture_states.find(guid);
    if (cs_it == capture_states.end()) return;
    cs_it->second->mode_ready.store(false, std::memory_order_release);
    cs_it->second->update_ready.store(false, std::memory_order_release);
  }

  void unregisterCaptureBuffer(const std::string &displayName) {
    std::lock_guard<std::mutex> lock(vdisplay_mutex);

    auto guid = find_guid_by_name_locked(displayName);
    if (guid.empty()) {
      return;
    }

    auto cs_it = capture_states.find(guid);
    if (cs_it == capture_states.end()) {
      return;
    }

    auto &vdinfo = virtual_displays[guid];
    if (vdinfo.handle && evdi.unregister_buffer) {
      evdi.unregister_buffer(vdinfo.handle, cs_it->second->buffer_id);
    }
    capture_states.erase(cs_it);

    BOOST_LOG(info) << "[VDISPLAY] Unregistered capture buffer for " << displayName;
  }

  int captureFrame(const std::string &displayName, CapturedFrame *frame, int timeout_ms) {
    if (!frame) {
      return -1;
    }

    // Take the EVDI handle and capture state without holding vdisplay_mutex
    // across the blocking poll() — the watchdog and frame loop must not
    // deadlock against teardown.
    evdi_handle handle = nullptr;
    CaptureState *state = nullptr;
    {
      std::lock_guard<std::mutex> lock(vdisplay_mutex);
      auto guid = find_guid_by_name_locked(displayName);
      if (guid.empty()) {
        return -1;
      }
      auto cs_it = capture_states.find(guid);
      if (cs_it == capture_states.end()) {
        return -1;
      }
      auto &vdinfo = virtual_displays[guid];
      if (!vdinfo.handle) {
        return -1;
      }
      handle = vdinfo.handle;
      state = cs_it->second.get();
    }

    std::lock_guard<std::mutex> grab_lock(state->grab_mutex);

    // If we haven't seen a mode_changed event yet after the last evdi.connect,
    // the kernel will reject grab_pixels with EINVAL. Drain the event queue
    // first (handle_events fires on_mode_changed → flips mode_ready).
    if (!state->mode_ready.load(std::memory_order_acquire)) {
      int fd = evdi.get_event_ready(handle);
      if (fd >= 0) {
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_ms);
        while (!state->mode_ready.load(std::memory_order_acquire)) {
          auto now = std::chrono::steady_clock::now();
          if (now >= deadline) {
            return 1;  // timeout — capture loop will retry
          }
          int remaining_ms = static_cast<int>(
              std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
          struct pollfd pfd {};
          pfd.fd = fd;
          pfd.events = POLLIN;
          int rc = poll(&pfd, 1, remaining_ms);
          if (rc < 0) {
            if (errno == EINTR) continue;
            return -1;
          }
          if (rc == 0) return 1;

          evdi_event_context ctx {};
          ctx.dpms_handler = on_dpms;
          ctx.mode_changed_handler = on_mode_changed;
          ctx.update_ready_handler = on_update_ready;
          ctx.crtc_state_handler = on_crtc_state;
          ctx.cursor_set_handler = on_cursor_set;
          ctx.cursor_move_handler = on_cursor_move;
          ctx.user_data = state;
          evdi.handle_events(handle, &ctx);
        }
        // Mode is now known. If it differs from what we registered the
        // buffer for, re-register at the new dimensions.
        int new_w = state->mode_width.load(std::memory_order_acquire);
        int new_h = state->mode_height.load(std::memory_order_acquire);
        if (new_w > 0 && new_h > 0 && (new_w != state->width || new_h != state->height)) {
          BOOST_LOG(info) << "[VDISPLAY] Mode changed to " << new_w << "x" << new_h
                          << " (was " << state->width << "x" << state->height
                          << "), re-registering buffer";
          if (evdi.unregister_buffer) {
            evdi.unregister_buffer(handle, state->buffer_id);
          }
          state->width = new_w;
          state->height = new_h;
          state->stride = new_w * 4;
          state->buffer.assign(static_cast<size_t>(state->stride) * state->height, 0);
          evdi_buffer ebuf {};
          ebuf.id = state->buffer_id;
          ebuf.buffer = state->buffer.data();
          ebuf.width = state->width;
          ebuf.height = state->height;
          ebuf.stride = state->stride;
          ebuf.rects = state->dirty_rects.data();
          ebuf.rect_count = static_cast<int>(state->dirty_rects.size());
          evdi.register_buffer(handle, ebuf);
        }
      }
    }

    // request_update can return true (buffer already updated synchronously) or
    // false (async — wait for update_ready_handler).
    state->update_ready.store(false, std::memory_order_release);

    bool ready = evdi.request_update(handle, state->buffer_id);
    if (!ready) {
      // Block on the EVDI event fd until either the update arrives or we time out.
      // evdi_selectable is typedef'd to int in upstream libevdi headers.
      int fd = evdi.get_event_ready(handle);
      if (fd < 0) {
        return -1;
      }

      auto deadline = std::chrono::steady_clock::now() +
                      std::chrono::milliseconds(timeout_ms);

      while (!state->update_ready.load(std::memory_order_acquire)) {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
          return 1;  // timeout
        }
        int remaining_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());

        struct pollfd pfd {};
        pfd.fd = fd;
        pfd.events = POLLIN;
        int rc = poll(&pfd, 1, remaining_ms);
        if (rc < 0) {
          if (errno == EINTR) {
            continue;
          }
          return -1;
        }
        if (rc == 0) {
          return 1;
        }
        // Drain events; this may invoke on_update_ready -> sets update_ready=true.
        evdi_event_context ctx {};
        ctx.dpms_handler = on_dpms;
        ctx.mode_changed_handler = on_mode_changed;
        ctx.update_ready_handler = on_update_ready;
        ctx.crtc_state_handler = on_crtc_state;
        ctx.cursor_set_handler = on_cursor_set;
        ctx.cursor_move_handler = on_cursor_move;
        ctx.ddcci_data_handler = nullptr;
        ctx.user_data = state;
        evdi.handle_events(handle, &ctx);
      }
    }

    // Tell libevdi to fill in dirty rects and update the buffer mapping.
    int num_rects = static_cast<int>(state->dirty_rects.size());
    evdi.grab_pixels(handle, state->dirty_rects.data(), &num_rects);

    timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);

    frame->data = state->buffer.data();
    frame->width = state->width;
    frame->height = state->height;
    frame->stride = state->stride;
    frame->timestamp_ns = static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ull +
                          static_cast<uint64_t>(ts.tv_nsec);
    frame->dirty_rect_count = num_rects;

    return 0;
  }

}  // namespace VDISPLAY
