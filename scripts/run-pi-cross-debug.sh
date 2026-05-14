#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
remote_host="${COCKSCREEN_PI_HOST:-atom@cockscreen}"
remote_root="${COCKSCREEN_PI_ROOT:-/home/atom/cockscreen}"
build_preset="${1:-cross-pi-zero2w-debug}"
binary_rel="${2:-out/build/${build_preset}/cockscreen}"

cd "$repo_root"

echo "[pi] cross build: $build_preset"
cmake --preset "$build_preset"
cmake --build --preset "$build_preset"

echo "[pi] sync workspace: $remote_host:$remote_root"
ssh "$remote_host" "mkdir -p '$remote_root'"
rsync -az --delete \
    --exclude .git \
    --exclude out \
    --exclude build \
    --exclude .cache \
    --exclude .vscode/ipch \
    --exclude tmp \
    --exclude pi \
    --exclude oe-logs \
    --exclude oe-workdir \
    --exclude resources/videos \
    --exclude distro/build \
    --exclude distro/bitbake \
    --exclude distro/oe-core \
    --exclude distro/meta-openembedded \
    --exclude distro/meta-qt6 \
    --exclude distro/sources \
    "$repo_root/" "$remote_host:$remote_root/"

echo "[pi] upload binary: $binary_rel"
ssh "$remote_host" "mkdir -p '$remote_root/$(dirname "$binary_rel")'"
rsync -az "$repo_root/$binary_rel" "$remote_host:$remote_root/$binary_rel"

echo "[pi] run"
exec stdbuf -oL -eL ssh -t -o ServerAliveInterval=30 -o ServerAliveCountMax=120 "$remote_host" \
    "cd '$remote_root' || exit 1; pkill -x cockscreen || true; './$binary_rel' --scene-file scenes/pizero-linux.scene.jsonc --enable-web-server http://0.0.0.0:8080"
