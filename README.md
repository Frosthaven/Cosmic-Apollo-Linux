# Cosmic-Apollo-Linux

> [!NOTE]
> This is an AI assisted personal fork intended for personal use, but made
> public for all who are interested.

This is a fork of [Mr0z59/Apollo-Linux](https://github.com/MrOz59/Apollo-Linux) (which itself forks [ClassicOldSong/Apollo](https://github.com/ClassicOldSong/Apollo)) tuned specifically for **COSMIC** running on Wayland. You stream to clients with **[Moonlight](https://moonlight-stream.org/)** (any platform) or **[Artemis](https://github.com/ClassicOldSong/moonlight-android)** (Android, fork of Moonlight with extra features).

This fork exists as a way to provide support for multi-client game and desktop
streaming over virtualized displays under the Cosmic desktop. This integrates
seamlessly with Cosmic's compositor and misc tooling.

## What you get

- **A real virtual display** - Using `evdi`, a real and dedicated virtual
  display is used to present your computer's desktop or application.
- **Automatic Monitor Swapping** - When connecting remotely, sound and video is
  sent through the virtual display. Primary monitor(s) are disabled. When all
  clients disconnect, the monitors are restored to what they were prior.
- **Tiling automatically off during streams** - COSMIC's auto-tile is great for desktop work but makes fullscreen games a challenge. The fork toggles it off when you connect, back on when you disconnect (if it was on to start with).
- **Multi-device sharing** - connect from multiple devices at the same time. First device decides the resolution; the second joins as a viewer. When everyone disconnects, things reset for the next "first."
- **Quick reconnect** - disconnecting and reconnecting from the same device is sub-second; the virtual display stays warm.
- **Self-healing** - if apollo crashes mid-stream, the next start automatically turns your physical monitors back on, restores audio, re-enables tiling.
- **Disconnect vs Quit Session do the right thing** - Disconnect leaves the app paused so you can resume; Quit Session actually closes it.
- **Per-client display scale memory** - whatever cosmic-comp scale you set on the virtual display while streaming is remembered against the connecting client. Reconnect from the same client and the virtual display comes back at that scale. Brand-new clients start at 100% for a deterministic baseline. Only the first/sole client of a session sets the scale; followers inherit.

## Requirements

- **A Linux distro that runs COSMIC on Wayland.** Pre-built packages are produced for Debian/Ubuntu/Pop!_OS (`.deb`), Fedora (`.rpm`), Arch (`.tar.gz`), and a distro-agnostic `.AppImage`. Building from source also works.
- **EVDI** kernel module + library - install `evdi-dkms` and `libevdi` from your package manager.
- **A modern GPU** for hardware encoding (any NVIDIA, AMD VAAPI-capable, or Intel UHD).

## Installing

Pre-built `.deb`, `.rpm`, Arch tarball, and `.AppImage` artifacts are on the [GitHub Releases page](https://github.com/Frosthaven/Cosmic-Apollo-Linux/releases). Build-from-source instructions below if you'd rather track master.

```bash
# Install build + runtime deps (Arch / CachyOS)
sudo pacman -S --needed \
  cmake ninja gcc pkgconf boost openssl ffmpeg \
  libcap libnotify libdrm libevdev libpulse opus \
  miniupnpc nlohmann-json wayland evdi-dkms libevdi

# Clone + build
git clone https://github.com/Frosthaven/Cosmic-Apollo-Linux
cd Cosmic-Apollo-Linux
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DUSE_STANDALONE_ASIO=ON
cmake --build build --target sunshine -j$(nproc)

# Install
sudo install -Dm755 build/sunshine /usr/bin/apollo
sudo setcap cap_sys_admin+p /usr/bin/apollo
```

### Give yourself permission to use EVDI

Once, set up udev + tmpfiles entries so apollo can manage virtual displays without `sudo`:

```bash
sudo tee /etc/udev/rules.d/99-evdi.rules > /dev/null <<'EOF'
KERNEL=="evdi*", MODE="0660", GROUP="video"
EOF

sudo tee /etc/tmpfiles.d/evdi-permissions.conf > /dev/null <<'EOF'
z /sys/devices/evdi/add 0220 root video -
z /sys/devices/evdi/remove_all 0220 root video -
EOF

sudo udevadm control --reload-rules && sudo udevadm trigger
sudo systemd-tmpfiles --create
sudo modprobe evdi

# Make sure you're in the video group
sudo usermod -aG video $USER
# Log out and back in for the group change to take effect
```

### Run apollo on login

Create `~/.config/systemd/user/apollo.service`:

```ini
[Unit]
Description=Apollo streaming host
After=cosmic-session.target graphical-session.target
PartOf=graphical-session.target

[Service]
Type=simple
ExecStartPre=/bin/sleep 5
ExecStartPre=/bin/sh -c 'until [ -n "$WAYLAND_DISPLAY" ]; do sleep 0.5; done'
ExecStart=/usr/bin/apollo
Restart=on-failure
RestartSec=2

[Install]
WantedBy=graphical-session.target
```

```bash
systemctl --user daemon-reload
systemctl --user enable --now apollo.service
```

## First-time setup

1. **Open the web UI** at <https://localhost:47990> in your browser. Set a username and password.
2. **Enable headless mode**: edit `~/.config/sunshine/sunshine.conf` and add:
   ```
   headless_mode = on
   isolated_virtual_display_option = on
   ```
   Restart apollo: `systemctl --user restart apollo.service`.
3. **Install a client** on your other device:
   - Phone/tablet (Android): [Artemis](https://github.com/ClassicOldSong/moonlight-android/releases) - recommended for extra features.
   - Anything else: [Moonlight](https://moonlight-stream.org/).
4. **Pair**: open the client, point it at your host's IP, enter the 4-digit PIN that the web UI shows.
5. **Connect** and pick "Desktop" - your virtual display starts streaming.

## Suggested config tweaks

### Disconnect-as-Quit for the Desktop entry

If you want hitting "Disconnect" on your client to fully end the session (mobile shows "Start" instead of "Resume"), edit `~/.config/sunshine/apps.json` and add `"terminate-on-pause": true` to the Desktop entry:

```json
{
  "name": "Desktop",
  "image-path": "desktop.png",
  "terminate-on-pause": true,
  ...
}
```

If you prefer being able to resume a paused session later, leave it off (the default).

### Steam Big Picture

Kills any running Steam, opens Big Picture detached, and closes Steam again on Quit Session. The kill-then-relaunch is what gets apollo's `SUNSHINE_CLIENT_*` env vars into the game's process tree (Steam only inherits them at launch).

```json
{
  "detached": [
    "setsid steam steam://open/bigpicture"
  ],
  "image-path": "steam.png",
  "name": "Steam Big Picture",
  "prep-cmd": [
    {
      "do": "sh -c \"pkill -TERM -f [/]Steam/ ; sleep 3 ; exit 0\"",
      "undo": "sh -c \"pkill -TERM -f [/]Steam/ ; exit 0\""
    }
  ]
}
```

> Gotchas if you write your own prep-cmd lines: apollo's parser only respects **double-quoted** shell commands, and `pkill -f` patterns matching `/Steam/` will match the command line that's running pkill itself - `[/]Steam/` is a regex trick that avoids that self-match.

### Dynamic resolution for games launched through gamescope

Replace hardcoded `-W/-H/-r` in your gamescope Launch Options with `${SUNSHINE_CLIENT_*}` fallbacks. The game renders at the connecting client's exact resolution and framerate when streaming, and falls back to your native mode otherwise. Steam must be launched **by apollo** (not already open) for the env vars to reach gamescope - the Steam Big Picture entry above handles that.

Example (Cyberpunk 2077) Launch Options:

```
gamescope --adaptive-sync --backend wayland --force-grab-cursor -W ${SUNSHINE_CLIENT_WIDTH:-5120} -H ${SUNSHINE_CLIENT_HEIGHT:-1440} -r ${SUNSHINE_CLIENT_FPS:-120} -f -- env __GL_SHADER_DISK_CACHE_SKIP_CLEANUP=1 env VKD3D_DISABLE_EXTENSIONS=VK_NV_low_latency2 env PROTON_DLSS_UPGRADE=1 env PROTON_ENABLE_WAYLAND=1 %command% -noDirectStorage
```

## How streaming behaves

### Single client

| You do this on the client | This happens on your host |
|---|---|
| **Connect** | A virtual screen appears at the client's requested resolution + refresh rate. Your physical monitors turn off. COSMIC's auto-tile is disabled for the duration of the stream. Your current audio sink is recorded so it can be restored on disconnect. |
| **Disconnect** (close client / press back / kill the app) | Physical monitors come back on at their exact original mode. Auto-tile returns to its prior value. Audio sink is restored. The streaming app stays paused so you can resume it later - *unless* the app has `terminate-on-pause: true`, in which case the session fully ends and the app's `undo` runs. |
| **Resume** (start the same paused session again) | Re-uses the existing virtual display; reconnect is sub-second. |
| **Quit Session** | Always fully ends the session: runs the app's `prep-cmd.undo` (e.g. kills Steam) and reverts display configuration. |

### Multiple clients

The first client to connect sets the resolution/framerate. Anyone who connects afterwards joins as a viewer at whatever the first one set.

| Scenario | What happens |
|---|---|
| Client B connects while Client A is already streaming | B joins A's stream at A's resolution/framerate. No re-launch of the app, no EVDI reset. Both see the same content. |
| Client A or B hits **Disconnect** | Only the requester drops. The session stays active for everyone else - app keeps running, physical monitors stay off, tiling stays disabled. |
| Client A or B hits **Quit Session** | The session fully ends for everyone. All clients are dropped, the app's `prep-cmd.undo` runs (e.g. kills Steam), physical monitors come back on at their original mode, auto-tile and audio sink are restored. |
| Last connected client disconnects | Everything resets: physicals back on at their original mode, EVDI torn down, auto-tile restored, audio sink restored. Next "first" client gets a fresh setup. |

### Self-healing on crash

If apollo crashes mid-stream (kernel panic, OOM, SIGSEGV, force-quit, you-name-it), the next start reads state files in `~/.cache/apollo/` and reverses everything: re-enables physical outputs at their original modes, restores auto-tile, switches back to your original audio sink, clears the recovery state. You don't have to remember what state to put your desk back in.

### If something gets stuck

```bash
# Restart apollo cleanly - kicks all connected clients, resets the virtual display
systemctl --user restart apollo.service
```

That's the universal "I want to start over" button. apollo's startup recovery turns your physical monitors back on and restores audio/tiling automatically.

## Troubleshooting

**The client just shows me my actual desktop, not a separate virtual screen.**
Make sure `headless_mode = on` is in `~/.config/sunshine/sunshine.conf` and restart apollo.

**A game window is tiled / not full-screen.**
COSMIC's tile-on-map is interfering. Cosmic-Apollo-Linux should be auto-disabling it for the duration of every stream - restart apollo and try again. If the issue persists, check `~/.config/cosmic/com.system76.CosmicComp/v1/autotile` while you're connected; it should say `false`.

**My monitor came back at the wrong resolution.**
The fork records your exact mode (resolution + refresh) before each stream and restores it after. If you still hit this, it usually means the recovery state file was wiped before it ran - a `systemctl --user restart apollo.service` does a clean recovery.

**Black screen on the client.**
Most often the result of a stuck EVDI device after a crash. A `systemctl --user restart apollo.service` fixes it; the startup recovery sequence rebuilds the virtual display from scratch.

**"Failed to start" when launching an app.**
Check `journalctl --user -u apollo.service` for the actual error. If it mentions `prep-cmd failed with code [2]`, your apps.json has a quoting bug - apollo only respects double quotes in shell commands, not single quotes.

**Apollo crashed and now my desk is messed up.**
Just `systemctl --user restart apollo.service`. The startup recovery reads state files in `~/.cache/apollo/` and puts everything back: monitor modes, audio sink, tiling setting, output enable/disable.

## Credits

- **[Apollo](https://github.com/ClassicOldSong/Apollo)** by [ClassicOldSong](https://github.com/ClassicOldSong) - the upstream Sunshine fork with the Artemis features (better encoder tuning, permission system, virtual-display protocol).
- **[Apollo-Linux](https://github.com/MrOz59/Apollo-Linux)** by [MrOz59](https://github.com/MrOz59) - added the original EVDI virtual-display path on Linux. Cosmic-Apollo-Linux forks from this.
- **[EVDI](https://github.com/DisplayLink/evdi)** by DisplayLink - the kernel module that makes virtual displays possible.
- **[Moonlight](https://moonlight-stream.org/)** - the streaming protocol and reference client.

This fork's job is to make the above three play nicely with COSMIC on Wayland.

## License

GPLv3 (inherited from Apollo upstream).
