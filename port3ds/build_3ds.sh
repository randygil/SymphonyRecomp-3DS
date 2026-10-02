#!/usr/bin/env bash
# Builds the Nintendo 3DS port (old 3DS compatible).
#   requirements: devkitPro (devkitARM + libctru), .NET 10 SDK, python 3
#   disc image:   ../disc/Castlevania - Symphony of the Night (USA).cue + bins
# usage: ./build_3ds.sh [pc]      ("pc" also builds the SDL2 test host)
set -e
cd "$(dirname "$0")"

DOTNET=${DOTNET:-dotnet}
if ! "$DOTNET" --list-sdks 2>/dev/null | grep -q '^10\.'; then
    DOTNET="$HOME/.dotnet/dotnet"   # user-local install (dotnet-install script)
    export DOTNET_ROOT="$HOME/.dotnet"
fi

echo "== building cgen"
"$DOTNET" build cgen/CGen.csproj -c Release -v q -nologo

echo "== recompiling the game to C"
"$DOTNET" cgen/bin/Release/net10.0/cgen.dll ../config/sotn.json build/gen

mkdir -p build/tmp
TMPDIR_WIN=$( (cygpath -m "$PWD/build/tmp") 2>/dev/null || echo "$PWD/build/tmp")
JOBS=$(nproc 2>/dev/null || echo 4)

echo "== building 3dsx"
make -j"$JOBS" TMP="$TMPDIR_WIN"
python tools/check3dsx.py build/3ds/SymphonyRecomp.3dsx

if [ "$1" = "pc" ]; then
    echo "== building PC test host"
    PATH=/c/msys64/mingw64/bin:$PATH make -f Makefile.pc -j"$JOBS" TMP="$TMPDIR_WIN"
fi

echo "done: build/3ds/SymphonyRecomp.3dsx"
