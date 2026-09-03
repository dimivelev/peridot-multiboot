#!/usr/bin/env bash
# fetch-android-tools.sh — install AOSP mkbootimg/unpack_bootimg/avbtool into ~/.local/bin
# (python scripts from AOSP, not on PyPI; mkbootimg needs its gki/ package)
set -euo pipefail
BIN="${HOME}/.local/bin"
mkdir -p "$BIN"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

echo "[*] fetching mkbootimg repo archive (mkbootimg.py + unpack_bootimg.py + gki/)"
curl -fsSL --retry 2 \
  "https://android.googlesource.com/platform/system/tools/mkbootimg/+archive/refs/heads/master.tar.gz" \
  -o "$WORK/mkbootimg.tar.gz"
tar -xf "$WORK/mkbootimg.tar.gz" -C "$WORK"
install -m 755 "$WORK/mkbootimg.py"      "$BIN/mkbootimg"
install -m 755 "$WORK/unpack_bootimg.py" "$BIN/unpack_bootimg"
cp -r "$WORK/gki" "$BIN/gki"             # python package imported by mkbootimg.py

echo "[*] fetching avbtool"
curl -fsSL --retry 2 \
  "https://android.googlesource.com/platform/external/avb/+/refs/heads/main/avbtool.py?format=TEXT" \
  | base64 -d > "$BIN/avbtool"
chmod +x "$BIN/avbtool"

# prepend to PATH for subsequent steps (GITHUB_PATH prepends; GITHUB_ENV would REPLACE)
echo "$BIN" >> "$GITHUB_PATH" 2>/dev/null || export PATH="$BIN:$PATH"
"$BIN/mkbootimg" --help >/dev/null 2>&1 && echo "[+] mkbootimg OK"
"$BIN/unpack_bootimg" --help >/dev/null 2>&1 && echo "[+] unpack_bootimg OK"
"$BIN/avbtool" version >/dev/null 2>&1 && echo "[+] avbtool OK"
