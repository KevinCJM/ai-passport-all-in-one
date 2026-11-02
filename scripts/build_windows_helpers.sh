#!/usr/bin/env bash
# Windows helpers for the assistant, built in MSYS2 UCRT64 (see .github/workflows/windows-assistant.yml):
# passport-ppp.exe (PPP gateway + libslirp), libopus-0.dll (语音输入's encoder), a minimal static ffmpeg.exe
# (radio/podcast playback, the same features as the Mac build) and the DLLs and licences they need.
set -euo pipefail
cd "$(dirname "$0")/.."
OUT=experiments/passport-connectivity-tools/bin; WORK=experiments/passport-assistant-build/windows-helpers
mkdir -p "$OUT/licenses" "$WORK"

g++ -std=c++20 -O2 scripts/passport_ppp_gateway.cpp $(pkg-config --cflags slirp) -o "$OUT/passport-ppp.exe" $(pkg-config --libs slirp) -lws2_32 -liphlpapi
cp /ucrt64/bin/libopus-0.dll "$OUT/"
# Only DLLs from the UCRT64 prefix; system DLLs come with Windows.
for file in "$OUT/passport-ppp.exe" "$OUT/libopus-0.dll"; do
  ldd "$file" | awk '$3 ~ /^\/ucrt64\/bin\// {print $3}' | while read -r dll; do cp -n "$dll" "$OUT/"; done
done
"$OUT/passport-ppp.exe" --self-test
for package in libslirp glib2 opus gettext-runtime libiconv pcre2 libwinpthread gcc-libs; do
  [ -d "/ucrt64/share/licenses/$package" ] && cp -r "/ucrt64/share/licenses/$package" "$OUT/licenses/"
done

if [ ! -f "$OUT/ffmpeg.exe" ]; then
  # LAME from source, static, like the Mac build (MSYS2's package links only as a DLL).
  PREFIX="$PWD/$WORK/prefix"
  lame="$WORK/lame-3.100.tar.gz"
  [ -f "$lame" ] || curl -fsSL -o "$lame" https://downloads.sourceforge.net/project/lame/lame/3.100/lame-3.100.tar.gz
  echo "ddfe36cab873794038ae2c1210557ad34857a4b6bdc515785d1da9e175b1da1e  $lame" | sha256sum -c -
  tar -xf "$lame" -C "$WORK"
  (cd "$WORK/lame-3.100" && ./configure --prefix="$PREFIX" --disable-shared --enable-static --disable-frontend && make -j4 && make install)
  cp "$WORK/lame-3.100/COPYING" "$OUT/licenses/lame-COPYING"
  archive="$WORK/ffmpeg-7.1.2.tar.xz"
  [ -f "$archive" ] || curl -fsSL -o "$archive" https://ffmpeg.org/releases/ffmpeg-7.1.2.tar.xz
  echo "089bc60fb59d6aecc5d994ff530fd0dcb3ee39aa55867849a2bbc4e555f9c304  $archive" | sha256sum -c -
  tar -xf "$archive" -C "$WORK"
  (cd "$WORK/ffmpeg-7.1.2" && ./configure --disable-autodetect --disable-doc --disable-debug --disable-shared --enable-static \
     --disable-everything --disable-x86asm --enable-w32threads --enable-ffmpeg --enable-schannel --enable-libmp3lame \
     --enable-protocol=file,pipe,http,https,tcp,tls,httpproxy --enable-demuxer=mp3,mov,aac,ogg,wav,flac,matroska \
     --enable-decoder=mp3,mp3float,aac,flac,opus,vorbis,pcm_s16le --enable-encoder=libmp3lame,pcm_s16le \
     --enable-parser=mpegaudio,aac,flac,opus,vorbis --enable-muxer=mp3,pcm_s16le --enable-filter=aresample,anull,aformat \
     --extra-cflags="-I$PREFIX/include" --extra-ldflags="-static -L$PREFIX/lib" --pkg-config-flags=--static \
     || { tail -40 ffbuild/config.log; exit 1; }
   make -j4)
  cp "$WORK/ffmpeg-7.1.2/ffmpeg.exe" "$OUT/"
  cp "$WORK/ffmpeg-7.1.2/COPYING.LGPLv2.1" "$OUT/licenses/ffmpeg-COPYING.LGPLv2.1"
fi
"$OUT/ffmpeg.exe" -hide_banner -version | head -1
ls -l "$OUT"
