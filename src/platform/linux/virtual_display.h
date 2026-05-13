/**
 * @file src/platform/linux/virtual_display.h
 * @brief Virtual display declarations for Linux.
 */
#pragma once

// standard includes
#include <functional>
#include <string>
#include <vector>

// local includes
#include "src/uuid.h"

namespace VDISPLAY {

  /**
   * @brief Status of the virtual display driver.
   */
  enum class DRIVER_STATUS {
    UNKNOWN = 1,  ///< Driver status unknown
    OK = 0,  ///< Driver is operational
    FAILED = -1,  ///< Driver failed to initialize
    VERSION_INCOMPATIBLE = -2,  ///< Driver version incompatible
    WATCHDOG_FAILED = -3,  ///< Driver watchdog failed
    NOT_SUPPORTED = -4  ///< Virtual display not supported on this system
  };

  /**
   * @brief Initialize the virtual display driver.
   * @return DRIVER_STATUS indicating the result of initialization.
   */
  DRIVER_STATUS openVDisplayDevice();

  /**
   * @brief Close the virtual display driver.
   */
  void closeVDisplayDevice();

  /**
   * @brief Force a hot-unplug of every currently-active EVDI monitor by
   *        calling evdi.disconnect on each handle. Leaves the kernel-side
   *        EVDI device(s) open so the next session can reuse them — only
   *        the EDID is detached. Used by the disconnect-time restore path
   *        to break the cosmic-comp render-scheduler hold on the EVDI
   *        plane *before* issuing the kdl apply, so the apply doesn't
   *        deadlock on backend.lock().
   * @return Number of monitors disconnected.
   */
  int disconnectAllEvdiMonitors();

  /**
   * @brief Start a ping thread to keep the virtual display alive.
   * @param failCb Callback to invoke if the watchdog fails.
   * @return true if the ping thread was started successfully, false otherwise.
   */
  bool startPingThread(std::function<void()> failCb);

  /**
   * @brief Set the render adapter by name.
   * @param adapterName The name of the adapter to use for rendering.
   * @return true if the adapter was set successfully, false otherwise.
   */
  bool setRenderAdapterByName(const std::string &adapterName);

  /**
   * @brief Create a virtual display.
   * @param s_client_uid The unique identifier of the client.
   * @param s_client_name The name of the client.
   * @param width The width of the virtual display.
   * @param height The height of the virtual display.
   * @param fps The refresh rate of the virtual display (in mHz).
   * @param guid The GUID for the virtual display.
   * @return The name of the created virtual display, or empty string on failure.
   */
  std::string createVirtualDisplay(
    const char *s_client_uid,
    const char *s_client_name,
    uint32_t width,
    uint32_t height,
    uint32_t fps,
    const uuid_util::uuid_t &guid
  );

  /**
   * @brief Remove a virtual display.
   * @param guid The GUID of the virtual display to remove.
   * @return true if the virtual display was removed successfully, false otherwise.
   */
  bool removeVirtualDisplay(const uuid_util::uuid_t &guid);

  /**
   * @brief Change the display settings of a virtual display.
   * @param deviceName The name of the virtual display.
   * @param width The new width.
   * @param height The new height.
   * @param refresh_rate The new refresh rate (in mHz).
   * @return 0 on success, non-zero on failure.
   */
  int changeDisplaySettings(const char *deviceName, int width, int height, int refresh_rate);

  /**
   * @brief Change the display settings with isolated display option.
   * @param deviceName The name of the virtual display.
   * @param width The new width.
   * @param height The new height.
   * @param refresh_rate The new refresh rate (in mHz).
   * @param bApplyIsolated Whether to apply isolated display settings.
   * @return 0 on success, non-zero on failure.
   */
  int changeDisplaySettings2(const char *deviceName, int width, int height, int refresh_rate, bool bApplyIsolated = false);

  /**
   * @brief Get the primary display name.
   * @return The name of the primary display.
   */
  std::string getPrimaryDisplay();

  /**
   * @brief Set the primary display by name.
   * @param primaryDeviceName The name of the display to set as primary.
   * @return true if the primary display was set successfully, false otherwise.
   */
  bool setPrimaryDisplay(const char *primaryDeviceName);

  /**
   * @brief Get the HDR status of a display by name.
   * @param displayName The name of the display.
   * @return true if HDR is enabled, false otherwise.
   */
  bool getDisplayHDRByName(const char *displayName);

  /**
   * @brief Set the HDR status of a display by name.
   * @param displayName The name of the display.
   * @param enableAdvancedColor Whether to enable HDR.
   * @return true if the HDR status was set successfully, false otherwise.
   */
  bool setDisplayHDRByName(const char *displayName, bool enableAdvancedColor);

  /**
   * @brief Match displays by a given pattern.
   * @param sMatch The pattern to match.
   * @return A vector of matching display names.
   */
  std::vector<std::string> matchDisplay(const std::string &sMatch);

  /**
   * @brief Check if a display is an EVDI virtual display.
   * @param displayName The name of the display to check.
   * @return true if the display is an EVDI virtual display, false otherwise.
   */
  bool isEvdiDisplay(const std::string &displayName);

  /**
   * @brief Return the name of any currently-active EVDI virtual display, or
   *        an empty string if none exists. Used to route encoder-probe
   *        calls (which pass an empty display_name) into the EVDI capture
   *        backend instead of letting them fall through to KMS — KMS opens
   *        the physical DRM cards and contends with cosmic-comp's page
   *        flips, causing the user's monitors to go dark with EBUSY.
   */
  std::string firstActiveEvdiName();

  /**
   * @brief Get the DRM card index for an EVDI display.
   * @param displayName The name of the EVDI display.
   * @return The card index, or -1 if not found or not an EVDI display.
   */
  int getEvdiCardIndex(const std::string &displayName);

  // ==========================================================================
  // EVDI frame consumer API
  //
  // After createVirtualDisplay() returns a name for an EVDI-backed display,
  // a capture client should call registerCaptureBuffer() once and then
  // captureFrame() in a loop. captureFrame blocks until either a new frame is
  // ready (kernel copies the rendered framebuffer into the registered buffer)
  // or the timeout expires.
  //
  // Without these calls, cosmic-comp's page flips on the EVDI card stall in
  // the kernel waiting for a consumer and fail with EBUSY, which is the
  // failure mode the upstream MrOz59 fork ships with.
  // ==========================================================================

  /**
   * @brief Allocate a capture buffer for the named virtual display and
   *        register it with the EVDI kernel module. Must be called exactly
   *        once per virtual display, after createVirtualDisplay().
   * @param displayName The name returned by createVirtualDisplay().
   * @return true on success.
   */
  bool registerCaptureBuffer(const std::string &displayName);

  /**
   * @brief Unregister and free the capture buffer. Called automatically on
   *        removeVirtualDisplay() but exposed for explicit teardown.
   * @param displayName The name of the virtual display.
   */
  void unregisterCaptureBuffer(const std::string &displayName);

  struct CapturedFrame {
    const uint8_t *data;  ///< BGRX/BGRA pixel data, owned by VDISPLAY
    int width;
    int height;
    int stride;
    uint64_t timestamp_ns;  ///< CLOCK_MONOTONIC timestamp at frame ready
    int dirty_rect_count;
  };

  /**
   * @brief Request a frame update from EVDI and block until it is ready.
   * @param displayName The name of the virtual display.
   * @param frame Output: populated with the latest framebuffer pointer.
   * @param timeout_ms Maximum time to wait for the next frame.
   * @retval 0 frame populated with new contents
   * @retval 1 timeout (no new frame arrived in time)
   * @retval -1 error (display gone, EVDI failure, etc.)
   */
  int captureFrame(const std::string &displayName, CapturedFrame *frame, int timeout_ms);

  /**
   * @brief Reset the capture state so the next captureFrame call blocks
   *        until a fresh mode_changed event arrives. Call this immediately
   *        before triggering a compositor-driven mode change (e.g.
   *        cosmic-randr mode ...) so the consumer doesn't race the
   *        kernel's mode-set IOCTL.
   * @param displayName The name of the virtual display.
   */
  void resetModeReady(const std::string &displayName);

  /**
   * @brief Composite the latest cursor (as delivered by EVDI cursor_set /
   *        cursor_move events) onto the given BGRA buffer. EVDI doesn't
   *        bake the hardware cursor into the primary framebuffer, so the
   *        capture backend must paint it manually after grab_pixels.
   * @param displayName The name of the virtual display.
   * @param dst        Target BGRA buffer (must be width*height*4 bytes).
   * @param dst_width  Width of the target buffer.
   * @param dst_height Height of the target buffer.
   * @param dst_stride Row stride in bytes (typically dst_width*4).
   */
  void blendCursor(const std::string &displayName,
                    uint8_t *dst,
                    int dst_width,
                    int dst_height,
                    int dst_stride);

}  // namespace VDISPLAY
