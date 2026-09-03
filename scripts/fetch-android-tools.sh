#!/usr/bin/env bash
# fetch-android-tools.sh — install AOSP mkbootimg/unpack_bootimg/avbtool into ~/.local/bin
# (these are python scripts from AOSP, not on PyPI)
set -euo pipefail
BIN="${HOME}/.local/bin"
mkdir -p "$BIN"

fetch() { # name repo-path
  echo "[*] fetching $1"
  curl -fsSL "https://android.googlesource.com/platform/system/tools/mkbootimg/+/refs/heads/master/$2?format=TEXT" \
    | base64 -d > "$BIN/$1"
  chmod +x "$BIN/$1"
}

fetch mkbootimg mkbootimg.py
fetch unpack_bootimg unpack_bootimg.py

echo "[*] fetching avbtool"
curl -fsSL "https://android.googlesource.com/platform/external/avb/+/refs/heads/main/avbtool.py?format=TEXT" \
  | base64 -d > "$BIN/avbtool"
chmod +x "$BIN/avbtool"

# prepend to PATH for subsequent steps (GITHUB_PATH prepends; GITHUB_ENV would REPLACE)
echo "$BIN" >> "$GITHUB_PATH" 2>/dev/null || export PATH="$BIN:$PATH"
"$BIN/mkbootimg" --help >/dev/null 2>&1 && echo "[+] mkbootimg OK"
"$BIN/unpack_bootimg" --help >/dev/null 2>&1 && echo "[+] unpack_bootimg OK"
"$BIN/avbtool" version >/dev/null 2>&1 && echo "[+] avbtool OK"
