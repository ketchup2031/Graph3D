#!/usr/bin/env bash
set -euo pipefail
export PATH=/ucrt64/bin:/usr/bin:$PATH
archive=$1
work=$2
if [[ $(sha256sum "$archive" | cut -d ' ' -f 1) != 8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e ]]; then
    echo 'Unexpected FFmpeg source checksum.' >&2
    exit 1
fi
tar -xf "$archive" -C "$work"
cd "$work/ffmpeg-9.0.2"
./configure --target-os=mingw32 --arch=x86_64 --cc=gcc --enable-shared --disable-static \
    --disable-autodetect --disable-programs --disable-doc --disable-asm --disable-debug \
    --disable-everything --disable-avdevice --disable-avfilter --disable-network
make -j "${3:-3}"
