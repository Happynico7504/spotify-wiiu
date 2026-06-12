# Spotify Wii U

An unofficial [Spotify Connect](https://www.spotify.com/connect/) client for the Nintendo Wii U, running under the [Aroma](https://aroma.foryour.cafe/) homebrew environment.

> **Requires a Spotify Premium account.**
> Not affiliated with or endorsed by Spotify AB.

## Features

- **Spotify Connect** — select "Wii U" as your playback device from any Spotify app; control playback from your phone, PC, or directly on the console
- **Full playlist context** — skips and auto-advance work across entire playlists and albums, not just the visible window
- **Shuffle & repeat** — toggle on the console or from any connected Spotify app; state syncs both ways
- **Album art & track info** — fetched automatically for the current track
- **Progress bar** with real-time position
- **Audio Crystalizer** — harmonic-enhancement effect with adjustable strength
- **Spectrum visualizer**
- **GamePad touchscreen** — tap album art to play/pause, swipe left/right to skip, drag the progress bar to seek
- **Multi-controller support** — GamePad, Wii Remote, and Pro Controller all work
- **Roséverse integration** — shows community posts for the current track; `Stick R` opens the post directly in Miiverse, `Stick L` lets you post a reaction
- **Cache-sweep plugin** — optional Aroma plugin that automatically purges stale audio cache at boot and on a configurable interval, keeping your SD card tidy

## Controls

### GamePad (VPAD)

| Button / Gesture | Action |
|-----------------|--------|
| `+` / `-` | Volume +5 / −5 |
| `R` / `L` | Next / Previous track |
| `ZR` / `ZL` | Seek +5 s / −5 s |
| `A` | Play / Pause |
| `X` | Toggle shuffle |
| `Y` | Toggle repeat (off → context → track) |
| `↑` / `↓` | Crystalizer on / off |
| `←` / `→` | Crystalizer strength −1 / +1 |
| `B` | Show / hide controls overlay |
| `Stick R` | Open current post in Roséverse overlay |
| `Stick L` | Post to Roséverse |
| Tap album art | Play / Pause |
| Swipe left / right | Next / Previous track |
| Drag progress bar | Seek |

### Wii Remote

| Button | Action |
|--------|--------|
| `+` / `-` | Volume +5 / −5 |
| `↑` / `↓` | Next / Previous track |
| `←` / `→` | Seek +5 s / −5 s |
| `A` | Play / Pause |
| `1` | Toggle shuffle |
| `2` | Toggle repeat |
| `B` | Show / hide controls overlay |

### Pro Controller

| Button | Action |
|--------|--------|
| `+` / `-` | Volume +5 / −5 |
| `R` / `L` | Next / Previous track |
| `ZR` / `ZL` | Seek +5 s / −5 s |
| `A` | Play / Pause |
| `X` | Toggle shuffle |
| `Y` | Toggle repeat |
| `↑` / `↓` | Crystalizer on / off |
| `←` / `→` | Crystalizer strength −1 / +1 |
| `B` | Show / hide controls overlay |
| `Stick R` | Open current post in Roséverse overlay |
| `Stick L` | Post to Roséverse |

## Setup

### 1. Get credentials

#### Easy — one download (recommended)

Grab the bundle for your platform from the [latest bundle release](../../releases/tag/bundle-latest):

| Platform | File |
|----------|------|
| Windows | `spotify-wiiu-setup-windows-x64.zip` |
| Linux | `spotify-wiiu-setup-linux-x64.tar.gz` |
| macOS (Apple Silicon) | `spotify-wiiu-setup-macos-arm64.tar.gz` |

Extract the archive, then run the setup tool:

```sh
# Linux / macOS — mark both files executable first
chmod +x librespot spotify-wiiu-setup
./spotify-wiiu-setup
```

```
# Windows
spotify-wiiu-setup.exe
```

The tool launches librespot, waits for you to select **"wii-u-setup"** in any Spotify app, converts the credentials, and asks whether to download the app and the cache-sweep plugin. All files are saved to an `sd-files/` folder next to the setup tool — if your SD card is mounted, the tool copies them to the right locations automatically.

#### Manual — power users

<details>
<summary>Expand manual steps</summary>

Spotify Wii U authenticates using a stored-credentials blob produced by [librespot](https://github.com/librespot-org/librespot).

**Get librespot**

Prebuilt binaries are in the [`librespot-tools` release](../../releases/tag/librespot-tools), or install it yourself:

```sh
# Linux (Arch)
sudo pacman -S librespot

# Linux (Debian/Ubuntu)
sudo apt install librespot

# Any platform via Cargo
cargo install librespot
```

**Authenticate once to save credentials**

```sh
# Windows (PowerShell)
librespot.exe --name "wii-u-setup" --cache %TEMP%\librespot-cache

# Linux / macOS
librespot --name "wii-u-setup" --cache /tmp/librespot-cache
```

Open any Spotify app, select **"wii-u-setup"** as your playback device.
librespot will print `Authenticated as ...` — press `Ctrl+C` to stop it.

**Convert the credentials**

```sh
# Windows
python tools\make_creds.py %TEMP%\librespot-cache\credentials.json

# Linux / macOS
python3 tools/make_creds.py /tmp/librespot-cache/credentials.json
```

This writes `spotify_saved_creds.bin` in the current directory.

</details>

### 2. Copy to SD card

The setup tool saves all files to `sd-files/` next to itself and copies them automatically if your SD card is mounted. If you need to copy manually:

| File | SD card destination |
|------|-------------------|
| `sd-files/spotify_saved_creds.bin` | `SD:/spotify_saved_creds.bin` |
| `sd-files/spotify-wiiu.wuhb` | `SD:/wiiu/apps/spotify-wiiu/spotify-wiiu.wuhb` |
| `sd-files/spotify-cache-sweep.wps` | `SD:/wiiu/environments/<env>/plugins/spotify-cache-sweep.wps` |

> The setup tool can do this step automatically if your SD card is mounted.

### 3. Install the cache-sweep plugin (optional)

The cache-sweep plugin runs at boot under Aroma and automatically purges Spotify audio cache entries older than 3 days, keeping SD card usage in check. The setup tool installs it automatically; to install manually, copy `spotify-cache-sweep.wps` to:

```
SD:/wiiu/environments/<your-aroma-env>/plugins/spotify-cache-sweep.wps
```

Once installed, its sweep interval (default: 60 minutes) and enable/disable toggle are configurable from the **Aroma Config Menu** (`L` + `↓` + `SELECT` on the GamePad).

### 4. Launch

1. Insert the SD card and power on the Wii U
2. Open the **Aroma Homebrew Launcher**
3. Select **Spotify Wii U**
4. On any Spotify app, open the device list and select **"Wii U"**

## Building from source

### Prerequisites

- [devkitPro](https://devkitpro.org/wiki/Getting_Started) with the Wii U dev package:
  ```sh
  dkp-pacman -S wiiu-dev
  ```
- **For the cache-sweep plugin only:** [WiiUPluginSystem](https://github.com/wiiu-env/WiiUPluginSystem) (WUPS SDK) — not in dkp-pacman, build from source:
  ```sh
  git clone --depth 1 https://github.com/wiiu-env/WiiUPluginSystem.git /tmp/wups
  cd /tmp/wups && make install
  ```

### Build

```sh
make                          # produces spotify-wiiu.wuhb
make -C plugins/cache-sweep  # produces plugins/cache-sweep/spotify-cache-sweep.wps
```

### CI / Docker

A `Dockerfile` is provided at `.github/Dockerfile` with all dependencies pre-installed (including the WUPS SDK). The GitHub Actions workflow builds both the WUHB and the plugin on every push and publishes them in releases.

## Project layout

```
src/
  connect/    # AP handshake, Spirc/Connect state, audio pipeline, player
  discovery/  # Zeroconf / mDNS (makes the device visible to Spotify apps)
  ui/         # SDL2 display, spectrum visualiser, font baking
  spotify/    # Shared utilities (HTTP mutex)
  olv/        # Roséverse / Miiverse (nn_olv) integration
plugins/
  cache-sweep/  # Aroma WUPS plugin — boot-time audio cache GC with config menu
vendor/       # cJSON, stb_image
tools/
  setup/      # Native setup tool (Rust) — credentials, SD copy, WUHB + plugin install
  setup.py    # Python equivalent for power users
  make_creds.py  # Convert librespot credentials.json → spotify_saved_creds.bin
meta/         # Wii U app metadata (meta.xml, icon.png)
content/      # Bundled assets (font)
.github/
  Dockerfile  # Pre-baked builder image (devkitPPC + WUT + WUPS SDK + Rust/musl)
  workflows/  # CI: wuhb + plugin build, setup tool, platform bundles, Docker image
```

## License

[MIT](LICENSE)
