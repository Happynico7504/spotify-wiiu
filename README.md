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
- **Multi-controller support** — GamePad, Wii Remote, and Pro Controller all work

## Controls

### GamePad (VPAD)

| Button | Action |
|--------|--------|
| `+` / `-` | Volume +5 / −5 |
| `R` / `L` | Next / Previous track |
| `ZR` / `ZL` | Seek +5 s / −5 s |
| `A` | Play / Pause |
| `X` | Toggle shuffle |
| `Y` | Toggle repeat (off → context → track) |
| `↑` / `↓` | Crystalizer on / off |
| `←` / `→` | Crystalizer strength −1 / +1 |
| `B` | Show / hide controls overlay |

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

## Setup

### 1. Get credentials

#### Easy — one download (recommended)

Grab the bundle for your platform from the [latest bundle release](../../releases/tag/bundle-v1.0.5):

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

The tool launches librespot, waits for you to select **"wii-u-setup"** in any Spotify app, converts the credentials, and offers to copy them to your SD card — all in one step.

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

Copy `spotify_saved_creds.bin` to the **root** of your Wii U SD card:

```
SD:/spotify_saved_creds.bin
```

> The setup tool can do this step automatically if your SD card is mounted.

### 3. Launch

1. Insert the SD card and power on the Wii U
2. Open the **Aroma Homebrew Launcher**
3. Select **Spotify Wii U**
4. On any Spotify app, open the device list and select **"Wii U"**

## Building from source

### Prerequisites

- [devkitPro](https://devkitpro.org/wiki/Getting_Started) with devkitPPC and the following portlibs:
  ```sh
  dkp-pacman -S wut wiiu-sdl2 wiiu-sdl2_ttf wiiu-curl wiiu-mbedtls
  ```
- [Tremor](https://xiph.org/tremor/) (libvorbisidec) — either as a devkitPro portlib or the Makefile will build it from `vendor/tremor/`

### Build

```sh
make
```

This produces `spotify-wiiu.wuhb` (the installable package).

### CI / Docker

A `Dockerfile` is provided at `.github/Dockerfile` with all dependencies pre-installed. The GitHub Actions workflow uses it to build on every push.

## Project layout

```
src/
  connect/    # AP handshake, Spirc/Connect state, audio pipeline, player
  discovery/  # Zeroconf / mDNS (makes the device visible to Spotify apps)
  ui/         # SDL2 display, spectrum visualiser, font baking
  spotify/    # Shared utilities (HTTP mutex)
  olv/        # Roséverse / Miiverse (nn_olv) integration
vendor/       # cJSON, stb_image
tools/
  setup/      # Native setup tool (Rust) — guides users through credential setup
  setup.py    # Python equivalent for power users
  make_creds.py  # Convert librespot credentials.json → spotify_saved_creds.bin
meta/         # Wii U app metadata (meta.xml, icon.png)
content/      # Bundled assets (font)
.github/
  Dockerfile  # Pre-baked builder image (devkitPPC + WUT + Rust/musl)
  workflows/  # CI: wuhb build, setup tool, platform bundles, Docker image
```

## License

[MIT](LICENSE)
