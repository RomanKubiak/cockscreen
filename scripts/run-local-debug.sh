#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
binary="$repo_root/out/build/local-x86_64-debug/cockscreen"

existing_pids="$(pgrep -x cockscreen || true)"
if [[ -n "$existing_pids" ]]; then
	kill $existing_pids 2>/dev/null || true
fi

exec "$binary" "$@"
