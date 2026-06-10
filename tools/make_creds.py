#!/usr/bin/env python3
"""
Convert a librespot credentials.json to the binary format read by spotify-wii-u.

Step 1 — get librespot
----------------------
  Prebuilt binaries (Windows, Linux, macOS) are available as a release in this repo:
    https://github.com/Happynico7504/spotify-wiiu/releases/tag/librespot-tools

  Alternatively, build from source:
    Windows / Linux:  cargo install librespot
    Linux (packaged): sudo apt install librespot
                   or sudo pacman -S librespot

Step 2 — authenticate once and save credentials
------------------------------------------------
  Windows (PowerShell / cmd):
    librespot.exe --name "wii-u-setup" --cache %TEMP%\\librespot-cache

    Credentials file: %TEMP%\\librespot-cache\\credentials.json

  Linux:
    librespot --name "wii-u-setup" --cache /tmp/librespot-cache

    Credentials file: /tmp/librespot-cache/credentials.json

  librespot will appear as a Spotify Connect device called "wii-u-setup".
  Open any Spotify app, select it as your playback device, and librespot will
  authenticate automatically. Kill it with Ctrl+C once it prints
  "Authenticated as ..."  (audio output errors are harmless — ignore them).

Step 3 — convert
----------------
  Windows:
    python tools\\make_creds.py %TEMP%\\librespot-cache\\credentials.json

  Linux:
    python3 tools/make_creds.py /tmp/librespot-cache/credentials.json

Step 4 — copy to SD card
------------------------
  Copy the resulting spotify_saved_creds.bin to the root of the Wii U SD card.
  The homebrew picks it up automatically on the next Spotify Connect.
  You can delete spotify_password.txt and spotify_email.txt from the SD card.

File format written
-------------------
  [1 byte]  auth_type  (matches Spotify's AuthenticationType proto enum)
  [N bytes] raw auth_data blob (base64-decoded from credentials.json)
"""

import json
import base64
import sys
import pathlib

AUTH_TYPE_NAMES = {
    0: "AUTHENTICATION_USER_PASS",
    1: "AUTHENTICATION_STORED_SPOTIFY_CREDENTIALS",
    2: "AUTHENTICATION_STORED_FACEBOOK_CREDENTIALS",
    3: "AUTHENTICATION_SPOTIFY_TOKEN",
    4: "AUTHENTICATION_FACEBOOK_TOKEN",
}

TYPE_STR_TO_INT = {v: k for k, v in AUTH_TYPE_NAMES.items()}


def main():
    if len(sys.argv) != 2:
        print("Usage: python3 tools/make_creds.py <path/to/credentials.json>")
        sys.exit(1)

    src = pathlib.Path(sys.argv[1])
    if not src.exists():
        print(f"Error: {src} not found")
        sys.exit(1)

    with open(src) as f:
        data = json.load(f)

    username = data.get("username", "<unknown>")

    # New librespot format: auth_type (int) + auth_data (base64)
    # Old librespot format: type (string) + credentials (base64)
    if "auth_data" in data:
        auth_type_int = int(data.get("auth_type", 1))
        creds_b64 = data["auth_data"]
    elif "credentials" in data:
        raw_type = data.get("type", "AUTHENTICATION_STORED_SPOTIFY_CREDENTIALS")
        auth_type_int = TYPE_STR_TO_INT.get(raw_type, 1)
        creds_b64 = data["credentials"]
    else:
        print("Error: no 'auth_data' or 'credentials' field in JSON")
        sys.exit(1)

    # librespot uses URL-safe base64 (- and _ instead of + and /)
    blob = base64.urlsafe_b64decode(creds_b64 + "==")

    out = pathlib.Path("spotify_saved_creds.bin")
    with open(out, "wb") as f:
        f.write(bytes([auth_type_int]) + blob)

    print(f"username  : {username}")
    print(f"auth_type : {auth_type_int} ({AUTH_TYPE_NAMES.get(auth_type_int, 'unknown')})")
    print(f"blob len  : {len(blob)} bytes")
    print(f"written   : {out}  ({len(blob) + 1} bytes total)")
    print()
    print("Next step: copy spotify_saved_creds.bin to the root of the Wii U SD card.")


if __name__ == "__main__":
    main()
