#!/usr/bin/env python3
"""
Automated setup for Spotify Wii U.

Runs librespot, waits for you to authenticate via the Spotify app,
converts the saved credentials to the binary format the homebrew expects,
and optionally copies the result to your Wii U SD card.
"""

from __future__ import annotations

import base64
import json
import os
import pathlib
import platform
import shutil
import subprocess
import sys
import tempfile
import threading
import time

# ── Helpers ───────────────────────────────────────────────────────────────────

DEVICE_NAME = "wii-u-setup"
AUTH_TIMEOUT = 120  # seconds to wait for Spotify authentication

AUTH_TYPE_NAMES = {
    0: "AUTHENTICATION_USER_PASS",
    1: "AUTHENTICATION_STORED_SPOTIFY_CREDENTIALS",
    2: "AUTHENTICATION_STORED_FACEBOOK_CREDENTIALS",
    3: "AUTHENTICATION_SPOTIFY_TOKEN",
    4: "AUTHENTICATION_FACEBOOK_TOKEN",
}
TYPE_STR_TO_INT = {v: k for k, v in AUTH_TYPE_NAMES.items()}


def _sep(char="─", width=60):
    print(char * width)


def _step(n, text):
    _sep()
    print(f"  Step {n}: {text}")
    _sep()


def _ok(msg):
    print(f"  ✓ {msg}")


def _info(msg):
    print(f"  · {msg}")


def _warn(msg):
    print(f"  ! {msg}")


def _fail(msg):
    print(f"\n  ERROR: {msg}")
    sys.exit(1)


# ── librespot discovery ────────────────────────────────────────────────────────

def find_librespot() -> pathlib.Path | None:
    """Search PATH and the tools/ directory next to this script."""
    exe = "librespot.exe" if platform.system() == "Windows" else "librespot"

    # 1. Same directory as this script (tools/)
    local = pathlib.Path(__file__).parent / exe
    if local.exists():
        return local

    # 2. PATH
    found = shutil.which(exe)
    if found:
        return pathlib.Path(found)

    return None


# ── Credential conversion ─────────────────────────────────────────────────────

def convert_credentials(creds_json: pathlib.Path, out: pathlib.Path) -> str:
    """Convert librespot credentials.json → binary blob. Returns username."""
    with open(creds_json) as f:
        data = json.load(f)

    username = data.get("username", "<unknown>")

    if "auth_data" in data:
        auth_type_int = int(data.get("auth_type", 1))
        creds_b64 = data["auth_data"]
    elif "credentials" in data:
        raw_type = data.get("type", "AUTHENTICATION_STORED_SPOTIFY_CREDENTIALS")
        auth_type_int = TYPE_STR_TO_INT.get(raw_type, 1)
        creds_b64 = data["credentials"]
    else:
        _fail("credentials.json has no 'auth_data' or 'credentials' field")

    blob = base64.urlsafe_b64decode(creds_b64 + "==")
    out.write_bytes(bytes([auth_type_int]) + blob)

    _ok(f"username  : {username}")
    _ok(f"auth_type : {auth_type_int} ({AUTH_TYPE_NAMES.get(auth_type_int, 'unknown')})")
    _ok(f"blob size : {len(blob)} bytes")
    return username


# ── SD card detection ─────────────────────────────────────────────────────────

def find_sd_card() -> pathlib.Path | None:
    """Try to locate a mounted SD card across Windows / Linux / macOS."""
    system = platform.system()

    if system == "Windows":
        import string
        for letter in string.ascii_uppercase:
            root = pathlib.Path(f"{letter}:\\")
            if root.exists():
                # Prefer drives that already have the homebrew marker
                if (root / "wiiu").exists() or (root / "WIIU").exists():
                    return root
        # Fall back to any removable-looking drive (skip C:\)
        for letter in string.ascii_uppercase:
            if letter == "C":
                continue
            root = pathlib.Path(f"{letter}:\\")
            if root.exists():
                return root

    elif system == "Darwin":
        for vol in pathlib.Path("/Volumes").iterdir():
            if (vol / "wiiu").exists() or (vol / "WIIU").exists():
                return vol
        # Any external volume that isn't Macintosh HD
        for vol in pathlib.Path("/Volumes").iterdir():
            if vol.name not in ("Macintosh HD",) and vol.is_dir():
                return vol

    else:  # Linux
        uid = os.getuid()
        user = os.environ.get("USER", "")
        candidates = []
        for base in (
            pathlib.Path(f"/media/{user}"),
            pathlib.Path("/media"),
            pathlib.Path("/mnt"),
            pathlib.Path("/run/media") / user,
        ):
            if base.exists():
                for vol in base.iterdir():
                    candidates.append(vol)
        for vol in candidates:
            if (vol / "wiiu").exists() or (vol / "WIIU").exists():
                return vol
        if candidates:
            return candidates[0]

    return None


# ── Main ──────────────────────────────────────────────────────────────────────

def main():
    print()
    print("  ╔══════════════════════════════════════════════╗")
    print("  ║        Spotify Wii U — Setup Tool           ║")
    print("  ╚══════════════════════════════════════════════╝")
    print()

    if sys.version_info < (3, 9):
        _fail(f"Python 3.9+ required (you have {sys.version})")

    # ── Step 1: Find librespot ─────────────────────────────────────────────────
    _step(1, "Locating librespot")

    librespot = find_librespot()
    if not librespot:
        _fail(
            "librespot not found.\n\n"
            "  Option A: Place the librespot binary in the same folder as\n"
            "            this script (tools/) and run setup.py again.\n\n"
            "  Option B: Install librespot so it's on your PATH:\n"
            "              Windows/Linux/macOS: cargo install librespot\n"
            "              Linux (apt):         sudo apt install librespot\n"
            "              Linux (pacman):      sudo pacman -S librespot\n\n"
            "  Prebuilt binaries are attached to the librespot-tools release\n"
            "  on the GitHub repository."
        )

    _ok(f"Found librespot at: {librespot}")

    # ── Step 2: Run librespot and wait for auth ────────────────────────────────
    _step(2, "Authenticating with Spotify")

    cache_dir = pathlib.Path(tempfile.mkdtemp(prefix="librespot-cache-"))
    creds_file = cache_dir / "credentials.json"

    _info(f"Cache dir : {cache_dir}")
    _info(f"Device name: {DEVICE_NAME}")
    print()
    print("  ┌─────────────────────────────────────────────────────┐")
    print("  │  Open the Spotify app on any device, go to the     │")
    print("  │  device list, and select:  wii-u-setup             │")
    print("  │                                                     │")
    print(f"  │  Waiting up to {AUTH_TIMEOUT}s for authentication...        │")
    print("  └─────────────────────────────────────────────────────┘")
    print()

    authenticated_event = threading.Event()
    authenticated_as: list[str] = []

    cmd = [str(librespot), "--name", DEVICE_NAME, "--cache", str(cache_dir)]
    # Suppress audio output errors — we don't need playback
    cmd += ["--backend", "pipe"]

    try:
        proc = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
        )
    except PermissionError:
        _fail(
            f"Cannot execute {librespot} — check file permissions.\n"
            f"  Linux/macOS: chmod +x {librespot}"
        )

    def _reader():
        for line in proc.stdout:
            line = line.rstrip()
            if line:
                print(f"  [librespot] {line}")
            low = line.lower()
            if "authenticated as" in low or "authenticated" in low:
                # grab the username from the line if possible
                parts = line.split()
                idx = next((i for i, w in enumerate(parts) if "authenticated" in w.lower()), -1)
                if idx >= 0 and idx + 2 < len(parts):
                    authenticated_as.append(parts[idx + 2])
                authenticated_event.set()

    reader_thread = threading.Thread(target=_reader, daemon=True)
    reader_thread.start()

    # Poll: either the event fires OR the credentials file appears
    deadline = time.monotonic() + AUTH_TIMEOUT
    while time.monotonic() < deadline:
        if authenticated_event.is_set():
            break
        if creds_file.exists():
            # credentials.json written — auth succeeded even if we missed the log line
            time.sleep(0.5)  # let librespot finish writing
            break
        time.sleep(0.5)
    else:
        proc.terminate()
        _fail(
            f"Timed out after {AUTH_TIMEOUT}s without authentication.\n"
            "  Make sure the Wii U is visible in the Spotify device list\n"
            "  and that your account is Premium."
        )

    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()

    print()
    _ok("librespot stopped")

    if not creds_file.exists():
        _fail(
            f"credentials.json not found at {creds_file}\n"
            "  Authentication may not have completed. Try again."
        )

    # ── Step 3: Convert credentials ───────────────────────────────────────────
    _step(3, "Converting credentials")

    out_file = pathlib.Path("spotify_saved_creds.bin")
    convert_credentials(creds_file, out_file)
    _ok(f"Written   : {out_file.resolve()}")

    # ── Step 4: Copy to SD card ────────────────────────────────────────────────
    _step(4, "Copying to SD card")

    sd = find_sd_card()
    if sd:
        _info(f"Detected SD card at: {sd}")
        dest = sd / "spotify_saved_creds.bin"
        answer = input("  Copy spotify_saved_creds.bin to SD card? [Y/n]: ").strip().lower()
        if answer in ("", "y", "yes"):
            shutil.copy2(out_file, dest)
            _ok(f"Copied to {dest}")
        else:
            _info("Skipped — copy manually when ready.")
    else:
        _warn("No SD card detected.")
        _info(f"Copy manually: {out_file.resolve()}  →  SD:/spotify_saved_creds.bin")

    # ── Done ───────────────────────────────────────────────────────────────────
    _sep("═")
    print()
    print("  Setup complete!")
    print()
    print("  Next steps:")
    print("    1. Insert the SD card in your Wii U")
    print("    2. Open the Aroma Homebrew Launcher")
    print("    3. Launch Spotify Wii U")
    print("    4. Select 'Wii U' from any Spotify app's device list")
    print()
    _sep("═")

    # Clean up temp cache
    shutil.rmtree(cache_dir, ignore_errors=True)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\n\n  Interrupted — exiting.")
        sys.exit(1)
