#!/bin/sh
# Builds dsound.dll (32-bit) next to this script. Needs mingw-w64 (i686-w64-mingw32-gcc and -windres).
# The version comes from CMSPEED_VERSION if set, otherwise from tools/version.sh.
set -e
cd "$(dirname "$0")"
version=${CMSPEED_VERSION:-$(tools/version.sh)}
case $version in
    [0-9]*.[0-9]*.[0-9]*) ;;
    *) echo "bad version: $version" >&2; exit 1 ;;
esac

# The numeric version is major,minor,patch,N: N is the dev build count for 1.2.3-dev.N, and 0 for a release.
core=${version%%[-+]*}
major=${core%%.*}
rest=${core#*.}
minor=${rest%%.*}
patch=${rest#*.}
build=0
flags=0
case $version in
    *-*)
        flags=VS_FF_PRERELEASE
        pre=${version#*-}
        pre=${pre%%+*}
        case ${pre##*.} in ''|*[!0-9]*) ;; *) build=${pre##*.} ;; esac
        ;;
esac

mkdir -p build
cat > build/version.h <<EOF
#define CMSPEED_VERSION_STR "$version"
#define CMSPEED_VERSION_NUM $major,$minor,$patch,$build
#define CMSPEED_FILEFLAGS $flags
EOF
i686-w64-mingw32-windres -I build -O coff -o build/version.o src/version.rc
i686-w64-mingw32-gcc -O2 -Wall -Wextra -Wno-unused-parameter -shared -static-libgcc \
    -o dsound.dll src/cmspeed.c src/nav.c src/hist.c src/prefetch.c src/scan.c src/par.c src/tls.c src/dates.c \
    src/dsound.def build/version.o -lgdi32
echo "built dsound.dll $version"
