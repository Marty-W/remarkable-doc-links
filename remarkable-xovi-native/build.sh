#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
DIST_DIR="$ROOT_DIR/dist"
HOST_XOVI_REPO=${XOVI_REPO_HOST:-/tmp/pi-github-repos/asivery-xovi}
mkdir -p "$DIST_DIR"

DOCKER_ARGS=(--rm -v "$ROOT_DIR:/build")
if [ -d "$HOST_XOVI_REPO/.git" ]; then
  DOCKER_ARGS+=( -v "$HOST_XOVI_REPO:/xovi-ro:ro" )
fi

docker run "${DOCKER_ARGS[@]}" eeems/remarkable-toolchain:latest-rmpp bash -lc '
  set -euo pipefail
  cd /build
  . /opt/codex/*/*/environment-setup-*
  if [ -d /xovi-ro/.git ]; then
    export XOVI_REPO=/xovi-ro
  else
    export XOVI_REPO=/tmp/xovi
    if [ ! -d "$XOVI_REPO/.git" ]; then
      git clone --depth=1 https://github.com/asivery/xovi "$XOVI_REPO"
    fi
  fi
  python3 "$XOVI_REPO/util/xovigen.py" -o xovi.cpp -H xovi.h remarkable-xovi-native.xovi
  qmake .
  make -j"$(nproc)"
  cp remarkable-xovi-native.so dist/remarkable-xovi-native-aarch64.so
  make clean || true
  rm -f remarkable-xovi-native.so Makefile .qmake.stash xovi.cpp xovi.h moc_predefs.h
'

echo "Built: $DIST_DIR/remarkable-xovi-native-aarch64.so"
