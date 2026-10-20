#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Synthetic audio only. Requires ffmpeg with libmp3lame and libvorbis.
set -eu
cd -- "$(dirname -- "$0")"
ffmpeg -v error -f lavfi -i 'aevalsrc=if(between(t\,3\,5)\,0\,0.4*sin(2*PI*440*t))|-if(between(t\,3\,5)\,0\,0.4*sin(2*PI*440*t)):s=44100:d=8' \
    -c:a libmp3lame -b:a 96k -write_xing 0 -id3v2_version 0 -y stereo-gap.mp3
ffmpeg -v error -f lavfi -i 'sine=frequency=330:sample_rate=44100:duration=8' \
    -c:a libmp3lame -b:a 64k -write_xing 0 -id3v2_version 0 -y mono-tone.mp3
ffmpeg -v error -f lavfi -i 'sine=frequency=330:sample_rate=48000:duration=8' \
    -c:a libvorbis -q:a 3 -metadata artist='Björk' -metadata title='Grüße 東京' -y vorbis.ogg
ffmpeg -v error -f lavfi -i 'sine=frequency=330:sample_rate=44100:duration=8' \
    -c:a aac -b:a 128k -f adts -y tone.aac
ffmpeg -v error -f lavfi -i 'sine=frequency=330:sample_rate=48000:duration=1' \
    -c:a libopus -y unsupported-opus.ogg
