#!/usr/bin/env bash
#
# Build this ZMK config locally in Docker, mirroring what
# .github/workflows/build.yml does in CI.
#
# Usage:
#   scripts/zmk-build.sh "corne_right nice_view_adapter nice_view"
#   scripts/zmk-build.sh "settings_reset"
#   BOARD=nice_nano_v2 scripts/zmk-build.sh "corne_left nice_view_adapter nice_view"
#
# Artifacts land in build-out/<slug>/ : zmk.uf2, .config, zephyr.dts, build.log
#
# The west workspace lives in a persistent Docker volume so only the first
# run pays the download cost. To start clean: docker volume rm zmk-corne-west
#
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE="zmkfirmware/zmk-build-arm:stable"
VOLUME="zmk-corne-west"

BOARD="${BOARD:-nice_nano_v2}"
SHIELD="${1:-}"
SLUG="$(echo "${SHIELD:-$BOARD}" | tr ' ' '-')"
OUT="${REPO_ROOT}/build-out/${SLUG}"

rm -rf "$OUT"
mkdir -p "$OUT"

# `set -e` would abort on a failed build before PIPESTATUS could be read,
# so the pipeline runs with errexit off and the status is captured by hand.
set +e
docker run --rm \
  -v "${REPO_ROOT}:/repo:ro" \
  -v "${VOLUME}:/ws" \
  -v "${OUT}:/out" \
  -e BOARD="$BOARD" \
  -e SHIELD="$SHIELD" \
  "$IMAGE" \
  bash -euo pipefail -c '
    rm -rf /ws/config
    mkdir -p /ws/config
    cp -R /repo/config/. /ws/config/

    cd /ws
    [ -d .west ] || west init -l config
    west update --fetch-opt=--filter=tree:0
    west zephyr-export

    rm -rf /ws/build
    EXTRA=()
    if [ -n "${SHIELD}" ]; then EXTRA+=(-DSHIELD="${SHIELD}"); fi

    west build -s zmk/app -d /ws/build -b "${BOARD}" -- \
      -DZMK_CONFIG=/ws/config \
      -DZMK_EXTRA_MODULES=/repo \
      ${EXTRA[@]+"${EXTRA[@]}"}
  ' 2>&1 | tee "${OUT}/build.log"

# tee swallows the exit status; recover it from PIPESTATUS before re-arming errexit.
status="${PIPESTATUS[0]}"
set -e

# Copy artifacts out even on failure — .config and zephyr.dts are often the
# most useful things to inspect when a build breaks.
docker run --rm -v "${VOLUME}:/ws" -v "${OUT}:/out" "$IMAGE" bash -c '
  cp /ws/build/zephyr/zmk.uf2    /out/ 2>/dev/null || true
  cp /ws/build/zephyr/.config    /out/ 2>/dev/null || true
  cp /ws/build/zephyr/zephyr.dts /out/ 2>/dev/null || true
' || true

echo
echo "Artifacts in build-out/${SLUG}/:"
ls -la "$OUT"
exit "$status"
