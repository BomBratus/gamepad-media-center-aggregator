#!/usr/bin/env bash
set -euxo pipefail
: "${OPENORBIS:?Set OPENORBIS to the PS4 SDK path}"
work=/tmp/gmca-ps4-libmpv
rm -rf "$work"
mkdir -p "$work"
cp scripts/ps4/libmpv/mpv.patch "$work/"
cp scripts/ps4/libmpv/gmca-bitmap-subs.patch "$work/"

# curl, not Python urllib: the pacbrew container's setup-python build cannot
# import ssl, so an https urlopen fails with "unknown url type: https".
curl --fail --location --retry 3 \
    "https://github.com/mpv-player/mpv/archive/v0.36.0.tar.gz" \
    --output "$work/libmpv-0.36.0.tar.gz"
expected=29abc44f8ebee013bb2f9fe14d80b30db19b534c679056e4851ceadf5a5e8bf6
digest="$(sha256sum "$work/libmpv-0.36.0.tar.gz" | awk '{print $1}')"
if [[ "$digest" != "$expected" ]]; then
    echo "mpv source SHA256 mismatch: $digest" >&2
    exit 1
fi
echo "mpv source SHA256: $digest"

# waf is not in the release tarball and bootstrap.py fetches it with urlopen.
# Fetch it here with the same SHA-256 bootstrap.py verifies, so bootstrap.py
# finds it and skips its own download.
curl --fail --location --retry 3 "https://waf.io/waf-2.0.25" --output "$work/waf-2.0.25"
waf_expected=21199cd220ccf60434133e1fd2ab8c8e5217c3799199c82722543970dc8e38d5
waf_digest="$(sha256sum "$work/waf-2.0.25" | awk '{print $1}')"
if [[ "$waf_digest" != "$waf_expected" ]]; then
    echo "waf SHA256 mismatch: $waf_digest" >&2
    exit 1
fi
echo "waf SHA256: $waf_digest"

cd "$work"
tar -xzf libmpv-0.36.0.tar.gz
cd mpv-0.36.0
patch -Np1 -i ../mpv.patch
patch -Np1 -i ../gmca-bitmap-subs.patch
# bootstrap.py rewrites the interpreter line when it downloads waf; match that
# so ./waf runs under python3.
sed -i '1s|^#!/usr/bin/env python$|#!/usr/bin/env python3|' ../waf-2.0.25
install -m 755 ../waf-2.0.25 waf
./bootstrap.py
source "$OPENORBIS/ps4vars.sh"
TARGET=x86_64 ./waf configure --prefix="$OPENORBIS/usr" \
  --disable-libmpv-shared --enable-libmpv-static --disable-cplayer \
  --disable-iconv --disable-jpeg --disable-libavdevice \
  --enable-sdl2 --enable-sdl2-audio --disable-sdl2-gamepad --disable-sdl2-video
./waf build -j2
./waf install

