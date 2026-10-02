#!/usr/bin/env bash
set -euxo pipefail
: "${OPENORBIS:?Set OPENORBIS to the PS4 SDK path}"
work=/tmp/gmca-ps4-libmpv
rm -rf "$work"
mkdir -p "$work"
cp scripts/ps4/libmpv/mpv.patch "$work/"
cp scripts/ps4/libmpv/gmca-bitmap-subs.patch "$work/"

python3 - <<'PY'
import hashlib
import pathlib
import urllib.request

url = "https://github.com/mpv-player/mpv/archive/v0.36.0.tar.gz"
out = pathlib.Path("/tmp/gmca-ps4-libmpv/libmpv-0.36.0.tar.gz")
with urllib.request.urlopen(url, timeout=60) as src, out.open("wb") as dst:
    while True:
        chunk = src.read(1024 * 1024)
        if not chunk:
            break
        dst.write(chunk)
digest = hashlib.sha256(out.read_bytes()).hexdigest()
expected = "29abc44f8ebee013bb2f9fe14d80b30db19b534c679056e4851ceadf5a5e8bf6"
if digest != expected:
    raise SystemExit(f"mpv source SHA256 mismatch: {digest}")
print(f"mpv source SHA256: {digest}")
PY

cd "$work"
tar -xzf libmpv-0.36.0.tar.gz
cd mpv-0.36.0
patch -Np1 -i ../mpv.patch
patch -Np1 -i ../gmca-bitmap-subs.patch
./bootstrap.py
source "$OPENORBIS/ps4vars.sh"
TARGET=x86_64 ./waf configure --prefix="$OPENORBIS/usr" \
  --disable-libmpv-shared --enable-libmpv-static --disable-cplayer \
  --disable-iconv --disable-jpeg --disable-libavdevice \
  --enable-sdl2 --enable-sdl2-audio --disable-sdl2-gamepad --disable-sdl2-video
./waf build -j2
./waf install

