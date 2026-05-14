#!/bin/sh
set -eu

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${ROOT_DIR}/build"

for dir in \
  "${ROOT_DIR}/oe-core" \
  "${ROOT_DIR}/bitbake" \
  "${ROOT_DIR}/meta-openembedded" \
  "${ROOT_DIR}/meta-raspberrypi" \
  "${ROOT_DIR}/meta-qt6" \
  "${ROOT_DIR}/meta-cockscreen" \
  "${ROOT_DIR}/sources/pffft" \
  "${ROOT_DIR}/sources/fbcp-ili9341"
do
  if [ ! -d "$dir" ]; then
    echo "Missing required directory: $dir" >&2
    exit 1
  fi
done

"${ROOT_DIR}/scripts/apply-oe-core-hotfixes.py"

export TEMPLATECONF="${ROOT_DIR}/meta-cockscreen/conf/templates/cockscreen"
# oe-init-build-env expects some shell variables to be unset, so source it
# without nounset and then restore strict mode for the actual build step.
cd "${ROOT_DIR}/oe-core"
set -- "$BUILD_DIR" "${ROOT_DIR}/bitbake"
set +u
# shellcheck disable=SC1091
. ./oe-init-build-env >/dev/null
set -u

bitbake cockscreen-image
