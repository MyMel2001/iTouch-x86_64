#!/bin/sh
# Full clean build: wipe build/ (incl. vendored-dep objects) and rebuild
# the bootable disk image. Extra args are passed to make (e.g. ./build.sh run).
set -e
cd "$(dirname "$0")"
rm -rf build
make disk "$@"
