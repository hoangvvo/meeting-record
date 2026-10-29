#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${MREC_LINUX_BUILD:-$ROOT/native/build/linux}"
mkdir -p "$BUILD"
cd "$ROOT"
cc -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -fPIC -Inative/include -Inative/common -c native/common/tracks.c -o "$BUILD/tracks.o" -pthread
cc -std=c11 -Wall -Wextra -Werror -fPIC -Inative/include -Inative/common -c native/common/meeting-catalog.c -o "$BUILD/catalog.o"
for name in tracks meeting_catalog; do
  cc -std=c11 -D_POSIX_C_SOURCE=200809L -Inative/include -Inative/common "native/tests/${name}_test.c" "$BUILD/tracks.o" "$BUILD/catalog.o" -pthread -o "$BUILD/$name"
  "$BUILD/$name"
done
for name in timeline_mixer tracks_concurrency linux_clock; do
  g++ -std=c++17 -Wall -Wextra -Werror -Inative/include -Inative/common -Inative/windows "native/tests/${name}_test.cpp" "$BUILD/tracks.o" -pthread -o "$BUILD/$name"
  "$BUILD/$name"
done
g++ -std=c++17 -Wall -Wextra -Werror native/tests/linux_tone.cpp $(pkg-config --cflags --libs libpulse-simple) -pthread -o "$BUILD/zoom"
g++ -std=c++17 -Wall -Wextra -Werror -fPIC -shared -Inative/include -Inative/common native/linux/{Capture,MeetingDetector,Accessibility}.cpp "$BUILD/tracks.o" "$BUILD/catalog.o" $(pkg-config --cflags --libs libpulse atspi-2) -pthread -o "$BUILD/libmeetingrecord.so"
g++ -std=c++17 -Inative/include native/examples/linux-diagnose.cpp -L"$BUILD" -lmeetingrecord -Wl,-rpath,"$BUILD" -o "$BUILD/diagnose"
