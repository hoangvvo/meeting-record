#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${1:?voice build directory required}"
TARGET="${2:-}"
mkdir -p "$BUILD"
flags=(-O2 -fPIC)
if [[ -n "$TARGET" ]]; then flags+=(-target "$TARGET"); fi
if [[ -n "${MREC_SANITIZERS:-}" ]]; then flags+=(-fsanitize="$MREC_SANITIZERS" -fno-omit-frame-pointer -g); fi
includes=(-I"$ROOT/native/third_party/speexdsp" -I"$ROOT/native/third_party/speexdsp/include")
for source in mdf preprocess filterbank fftwrap kiss_fft kiss_fftr; do
  "${CC:-cc}" "${flags[@]}" -std=c11 -DHAVE_CONFIG_H "${includes[@]}" \
    -c "$ROOT/native/third_party/speexdsp/libspeexdsp/$source.c" -o "$BUILD/speex-$source.o"
done
"${CXX:-c++}" "${flags[@]}" -std=c++17 -Wall -Wextra -Werror "${includes[@]}" \
  -c "$ROOT/native/common/voice-processing.cpp" -o "$BUILD/voice-processing.o"
"${AR:-ar}" rcs "$BUILD/libmeetingrecord_voice.a" "$BUILD"/*.o
