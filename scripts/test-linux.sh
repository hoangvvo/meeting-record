#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${MREC_LINUX_BUILD:-$ROOT/native/build/linux}"
mkdir -p "$BUILD"
cd "$ROOT"
flags=()
if [[ -n "${MREC_SANITIZERS:-}" ]]; then
  flags+=(-fsanitize="$MREC_SANITIZERS" -fno-omit-frame-pointer -g)
fi
"${CC:-cc}" "${flags[@]}" -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -fPIC -Inative/include -Inative/common -c native/common/tracks.c -o "$BUILD/tracks.o" -pthread
"${CC:-cc}" "${flags[@]}" -std=c11 -Wall -Wextra -Werror -fPIC -Inative/include -Inative/common -c native/common/meeting-catalog.c -o "$BUILD/catalog.o"
for name in tracks meeting_catalog; do
  "${CC:-cc}" "${flags[@]}" -std=c11 -D_POSIX_C_SOURCE=200809L -Inative/include -Inative/common "native/tests/${name}_test.c" "$BUILD/tracks.o" "$BUILD/catalog.o" -pthread -o "$BUILD/$name"
  "$BUILD/$name"
done
for name in timeline_mixer tracks_concurrency linux_clock; do
  "${CXX:-g++}" "${flags[@]}" -std=c++17 -Wall -Wextra -Werror -Inative/include -Inative/common -Inative/windows "native/tests/${name}_test.cpp" "$BUILD/tracks.o" -pthread -o "$BUILD/$name"
  "$BUILD/$name"
done
"${CXX:-g++}" "${flags[@]}" -std=c++17 -Wall -Wextra -Werror native/tests/linux_tone.cpp $(pkg-config --cflags --libs libpulse-simple) -pthread -o "$BUILD/zoom"
"${CXX:-g++}" "${flags[@]}" -std=c++17 -Wall -Wextra -Werror -fPIC -shared -Wl,-z,defs -Inative/include -Inative/common native/linux/{Capture,MeetingDetector,Accessibility}.cpp "$BUILD/tracks.o" "$BUILD/catalog.o" $(pkg-config --cflags --libs libpulse atspi-2 gobject-2.0) -pthread -o "$BUILD/libmeetingrecord.so"
"${CXX:-g++}" "${flags[@]}" -std=c++17 -Inative/include native/examples/linux-diagnose.cpp -L"$BUILD" -lmeetingrecord -Wl,-rpath,"$BUILD" -o "$BUILD/diagnose"
"${CXX:-g++}" "${flags[@]}" -std=c++17 -Wall -Wextra -Werror -Inative/include native/tests/linux_accessibility_context_test.cpp -L"$BUILD" -lmeetingrecord $(pkg-config --cflags --libs atspi-2 gobject-2.0) -pthread -Wl,-rpath,"$BUILD" -o "$BUILD/accessibility_context"
