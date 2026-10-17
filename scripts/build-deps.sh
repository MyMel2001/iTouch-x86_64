#!/usr/bin/env bash
# Build vendored static third-party libraries for the guest toolchain (musl).
# usage: scripts/build-deps.sh zlib| mbedtls|busybox
set -euo pipefail
TOP="$(cd "$(dirname "$0")/.." && pwd)"
B="$TOP/third_party/_build"
case "${1:-}" in
  zlib)
    cd "$B/zlib"
    if [ ! -f libz.a ]; then
      CHOST=x86_64-linux-musl CFLAGS="-Os -static" ./configure --static >/dev/null
      make -j"$(nproc)" >/dev/null
    fi
    test -f libz.a
    ;;
  mbedtls)
    cd "$B/mbedtls"
    [ -d framework/.git ] || git submodule update --init --depth 1 >/dev/null 2>&1 || true
    cmake -B build -DCMAKE_BUILD_TYPE=Static -DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF \
          -DCMAKE_C_COMPILER=musl-gcc >/dev/null
    cmake --build build -j"$(nproc)" >/dev/null
    test -f build/library/libmbedtls.a
    ;;
  busybox)
    cd "$B/busybox"
    if [ ! -x busybox ]; then
      { yes "" || true; } | make defconfig >/dev/null
      sed -i 's/^# CONFIG_STATIC is not set/CONFIG_STATIC=y/;
              s/^CONFIG_TC=y/# CONFIG_TC is not set/;
              s/^CONFIG_TC_INGRESS=y/# CONFIG_TC_INGRESS is not set/;' .config
      # musl lacks kernel UAPI headers; reuse the build tree's copies (+ mtd)
      [ -e "$TOP/build/uapi/mtd" ] || ln -sfn /usr/include/mtd "$TOP/build/uapi/mtd"
      CPATH="$TOP/build/uapi" make CC=musl-gcc -j"$(nproc)" >/dev/null
    fi
    test -x busybox
    ;;
  *)
    echo "usage: $0 zlib|mbedtls|busybox" >&2
    exit 2
    ;;
esac
echo "deps: $1 ok"
