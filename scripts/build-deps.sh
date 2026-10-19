#!/usr/bin/env bash
# Build vendored static third-party libraries for the guest toolchain (musl).
# usage: scripts/build-deps.sh zlib| mbedtls|busybox|quickjs|stb
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
  quickjs)
    # QuickJS (MIT, Fabrice Bellard) - Safari's JavaScript engine
    d="$B/quickjs"
    v=2025-04-26
    mkdir -p "$d"
    if [ ! -f "$d/libquickjs.a" ]; then
      if [ ! -d "$d/quickjs-$v" ]; then
        curl -fsSL -o "$d/quickjs-$v.tar.xz" \
          "https://bellard.org/quickjs/quickjs-$v.tar.xz"
        echo "2f20074c25166ef6f781f381c50d57b502cb85d470d639abccebbef7954c83bf  $d/quickjs-$v.tar.xz" \
          | sha256sum -c - >/dev/null
        tar -C "$d" -xf "$d/quickjs-$v.tar.xz"
      fi
      make -C "$d/quickjs-$v" CC=musl-gcc -j"$(nproc)" libquickjs.a >/dev/null
      cp "$d/quickjs-$v/libquickjs.a" "$d/"
    fi
    test -f "$d/libquickjs.a"
    ;;
  stb)
    # stb_image (public domain, Sean Barrett) - Safari's image decoder.
    # Long-standing dep of this repo, fetched pinned; scalable text and SVG
    # are handled by our own from-scratch code (k93font.c / k93svg.c).
    # URL pinned by commit: the old v2.30 tag URL now 404s upstream.
    d="$B/stb"
    mkdir -p "$d"
    if [ ! -f "$d/stb_image.h" ]; then
      curl -fsSL -o "$d/stb_image.h" \
        "https://raw.githubusercontent.com/nothings/stb/2c980bb59875b0d32144a71867fbdebb2f77cd20/stb_image.h"
    fi
    echo "594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3  $d/stb_image.h" \
      | sha256sum -c - >/dev/null
    ;;
  *)
    echo "usage: $0 zlib|mbedtls|busybox|quickjs|stb" >&2
    exit 2
    ;;
esac
echo "deps: $1 ok"
