#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKSPACE_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
WORKSPACE_TMP_DIR="${WORKSPACE_TMP_DIR:-${WORKSPACE_DIR}/tmp}"
ROOT_DIR="${1:-/work/pizero}"
TOOLCHAIN_VERSION="${TOOLCHAIN_VERSION:-15.2.rel1}"
TOOLCHAIN_ARCHIVE="arm-gnu-toolchain-${TOOLCHAIN_VERSION}-x86_64-aarch64-none-linux-gnu.tar.xz"
TOOLCHAIN_URL="https://developer.arm.com/-/media/Files/downloads/gnu/${TOOLCHAIN_VERSION}/binrel/${TOOLCHAIN_ARCHIVE}"
TOOLCHAIN_EXTRACTED_DIR_NAME="${TOOLCHAIN_ARCHIVE%.tar.xz}"
TOOLCHAIN_DIR="${ROOT_DIR}/toolchains/${TOOLCHAIN_EXTRACTED_DIR_NAME}"
TOOLCHAIN_LINK="${ROOT_DIR}/toolchains/current"
DOWNLOAD_DIR="${ROOT_DIR}/downloads"
SYSROOT_DIR="${ROOT_DIR}/sysroot"
STAGE_DIR="${ROOT_DIR}/stage"
DEBIAN_SUITE="${DEBIAN_SUITE:-trixie}"
DEBIAN_MIRROR="${DEBIAN_MIRROR:-http://deb.debian.org/debian}"
DEBIAN_COMPONENTS="${DEBIAN_COMPONENTS:-main}"
DEBOOTSTRAP_CACHE_DIR="${ROOT_DIR}/cache/debootstrap/${DEBIAN_SUITE}"
PACKAGE_EXTRACT_STAMP="${SYSROOT_DIR}/.cockscreen-package-extract-stamp"

SYSROOT_PACKAGES=(
	build-essential
	cmake
	ninja-build
	pkg-config
	qt6-base-dev
	qt6-multimedia-dev
	libasound2-dev
	libegl1-mesa-dev
	libgl-dev
	libgles2-mesa-dev
	libdrm-dev
	libgbm-dev
	libxcb-xkb-dev
	libxkbcommon-dev
	libxkbcommon-x11-dev
)

require_command() {
	if ! command -v "$1" >/dev/null 2>&1; then
		echo "Missing required host tool: $1" >&2
		exit 1
	fi
}

run_root() {
	if [[ "${EUID}" -eq 0 ]]; then
		"$@"
		return
	fi

	if ! command -v sudo >/dev/null 2>&1; then
		echo "This script needs root access for ${ROOT_DIR} and debootstrap. Install sudo or run as root." >&2
		exit 1
	fi

	sudo env TMPDIR="${TMPDIR}" TMP="${TMP}" TEMP="${TEMP}" "$@"
}

join_by_comma() {
	local first="true"
	for item in "$@"; do
		if [[ "${first}" == "true" ]]; then
			printf "%s" "${item}"
			first="false"
		else
			printf ",%s" "${item}"
		fi
	done
}

DESIRED_PACKAGE_LIST="$(join_by_comma "${SYSROOT_PACKAGES[@]}")"

mkdir -p "${WORKSPACE_TMP_DIR}"
export TMPDIR="${WORKSPACE_TMP_DIR}"
export TMP="${WORKSPACE_TMP_DIR}"
export TEMP="${WORKSPACE_TMP_DIR}"

require_command curl
require_command tar
require_command debootstrap
require_command dpkg-deb

run_root mkdir -p "${ROOT_DIR}" "${ROOT_DIR}/toolchains" "${DOWNLOAD_DIR}" "${DEBOOTSTRAP_CACHE_DIR}" "${STAGE_DIR}"

if [[ ! -f "${DOWNLOAD_DIR}/${TOOLCHAIN_ARCHIVE}" ]]; then
	echo "Downloading Arm GNU Toolchain ${TOOLCHAIN_VERSION} to ${DOWNLOAD_DIR}" >&2
	run_root curl --fail --location --output "${DOWNLOAD_DIR}/${TOOLCHAIN_ARCHIVE}" "${TOOLCHAIN_URL}"
else
	echo "Reusing cached toolchain archive ${DOWNLOAD_DIR}/${TOOLCHAIN_ARCHIVE}" >&2
fi

if [[ ! -x "${TOOLCHAIN_DIR}/bin/aarch64-none-linux-gnu-g++" ]]; then
	echo "Extracting Arm GNU Toolchain into ${TOOLCHAIN_DIR}" >&2
	run_root rm -rf "${TOOLCHAIN_DIR}"
	run_root tar -xJf "${DOWNLOAD_DIR}/${TOOLCHAIN_ARCHIVE}" -C "${ROOT_DIR}/toolchains"
else
	echo "Reusing extracted toolchain ${TOOLCHAIN_DIR}" >&2
fi

run_root ln -sfn "${TOOLCHAIN_DIR}" "${TOOLCHAIN_LINK}"

if [[ ! -f "${SYSROOT_DIR}/.cockscreen-sysroot-stamp" ]] ||
	! grep -Fqx "suite=${DEBIAN_SUITE}" "${SYSROOT_DIR}/.cockscreen-sysroot-stamp" ||
	! grep -Fqx "mirror=${DEBIAN_MIRROR}" "${SYSROOT_DIR}/.cockscreen-sysroot-stamp" ||
	! grep -Fqx "components=${DEBIAN_COMPONENTS}" "${SYSROOT_DIR}/.cockscreen-sysroot-stamp" ||
	! grep -Fqx "cache_dir=${DEBOOTSTRAP_CACHE_DIR}" "${SYSROOT_DIR}/.cockscreen-sysroot-stamp" ||
	! grep -Fqx "toolchain=${TOOLCHAIN_VERSION}" "${SYSROOT_DIR}/.cockscreen-sysroot-stamp" ||
	! grep -Fqx "packages=${DESIRED_PACKAGE_LIST}" "${SYSROOT_DIR}/.cockscreen-sysroot-stamp"; then
	echo "Bootstrapping Debian ${DEBIAN_SUITE} arm64 sysroot into ${SYSROOT_DIR}" >&2
	run_root rm -rf "${SYSROOT_DIR}"
	run_root debootstrap \
		--arch=arm64 \
		--foreign \
		--variant=minbase \
		--components="${DEBIAN_COMPONENTS}" \
		--include="${DESIRED_PACKAGE_LIST}" \
		--cache-dir="${DEBOOTSTRAP_CACHE_DIR}" \
		"${DEBIAN_SUITE}" \
		"${SYSROOT_DIR}" \
		"${DEBIAN_MIRROR}"

	cat <<EOF | run_root tee "${SYSROOT_DIR}/.cockscreen-sysroot-stamp" >/dev/null
suite=${DEBIAN_SUITE}
mirror=${DEBIAN_MIRROR}
components=${DEBIAN_COMPONENTS}
cache_dir=${DEBOOTSTRAP_CACHE_DIR}
toolchain=${TOOLCHAIN_VERSION}
packages=${DESIRED_PACKAGE_LIST}
EOF
else
	echo "Reusing existing sysroot ${SYSROOT_DIR}" >&2
fi

if [[ ! -f "${PACKAGE_EXTRACT_STAMP}" ]]; then
	echo "Extracting cached Debian packages into ${SYSROOT_DIR}" >&2
	mapfile -t deb_packages < <(find "${DEBOOTSTRAP_CACHE_DIR}" -maxdepth 1 -type f -name '*.deb' | sort)
	if [[ "${#deb_packages[@]}" -eq 0 ]]; then
		echo "No cached Debian packages were found in ${DEBOOTSTRAP_CACHE_DIR}" >&2
		exit 1
	fi

	for deb_package in "${deb_packages[@]}"; do
		if [[ "$(basename "${deb_package}")" == usr-is-merged_* ]]; then
			continue
		fi
		run_root dpkg-deb -x "${deb_package}" "${SYSROOT_DIR}"
	done

	cat <<EOF | run_root tee "${PACKAGE_EXTRACT_STAMP}" >/dev/null
toolchain=${TOOLCHAIN_VERSION}
package_count=${#deb_packages[@]}
suite=${DEBIAN_SUITE}
mirror=${DEBIAN_MIRROR}
cache_dir=${DEBOOTSTRAP_CACHE_DIR}
EOF
else
	echo "Reusing extracted package payloads in ${SYSROOT_DIR}" >&2
fi

cat <<EOF
Pi Zero 2 W cross-compile assets are ready.

Root: ${ROOT_DIR}
Toolchain: ${TOOLCHAIN_LINK}
Sysroot: ${SYSROOT_DIR}
debootstrap cache: ${DEBOOTSTRAP_CACHE_DIR}
Temporary files: ${WORKSPACE_TMP_DIR}

Configure with:
  cmake --preset cross-pi-zero2w-debug

Build with:
  cmake --build --preset cross-pi-zero2w-debug
EOF
