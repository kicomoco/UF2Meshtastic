#!/usr/bin/env bash
# Fetches the build dependencies into .deps/ (toolchain, pico-sdk, nanopb,
# meshtastic protobufs, python venv) and regenerates the protobuf C sources.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
D="$ROOT/.deps"
mkdir -p "$D"
cd "$D"

TC_VER=14.2.rel1
case "$(uname -s)-$(uname -m)" in
  Darwin-arm64)  TC_HOST=darwin-arm64 ;;
  Darwin-x86_64) TC_HOST=darwin-x86_64 ;;
  Linux-x86_64)  TC_HOST=x86_64 ;;
  Linux-aarch64) TC_HOST=aarch64 ;;
  *) echo "Unsupported host; install arm-none-eabi-gcc yourself"; TC_HOST="" ;;
esac
if [ -n "$TC_HOST" ] && [ ! -d "arm-gnu-toolchain-$TC_VER-$TC_HOST-arm-none-eabi" ]; then
  curl -L -o tc.tar.xz "https://developer.arm.com/-/media/Files/downloads/gnu/$TC_VER/binrel/arm-gnu-toolchain-$TC_VER-$TC_HOST-arm-none-eabi.tar.xz"
  tar xf tc.tar.xz && rm tc.tar.xz
fi

[ -d pico-sdk ] || git clone --depth 1 -b 2.2.0 --recurse-submodules --shallow-submodules https://github.com/raspberrypi/pico-sdk.git
[ -d nanopb ]   || git clone --depth 1 -b nanopb-0.4.9.1 https://github.com/nanopb/nanopb.git
[ -d protobufs ] || git clone --depth 1 https://github.com/meshtastic/protobufs.git
[ -d venv ] || { python3 -m venv venv && venv/bin/pip install -q protobuf grpcio-tools; }

"$ROOT/tools/gen_protos.sh"
echo
echo "Done. Build with:"
echo "  export PICO_TOOLCHAIN_PATH=\$PWD/.deps/arm-gnu-toolchain-$TC_VER-$TC_HOST-arm-none-eabi"
echo "  export PATH=\$PICO_TOOLCHAIN_PATH/bin:\$PATH"
echo "  cmake -B build -DCMAKE_BUILD_TYPE=Release && make -C build -j8"
