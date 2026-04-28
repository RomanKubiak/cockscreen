#!/usr/bin/env bash
# Run cockscreen with verbose debug output for a fixed duration, then analyze.
# Usage: run-test.sh [duration_seconds]
#   duration_seconds: how long to let the app run before stopping (default: 45)

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
binary="$repo_root/out/build/local-x86_64-debug/cockscreen"
log_file="$repo_root/tmp/debug-run.log"
duration="${1:-45}"

if [[ ! -x "$binary" ]]; then
	echo "[run-test] Binary not found: $binary"
	echo "[run-test] Build first with: cmake --build --preset local-x86_64-debug"
	exit 1
fi

# Kill any existing instance of the same binary.
binary_pattern="^${binary//\//\\/}( |$)"
existing_pids="$(pgrep -f -- "$binary_pattern" || true)"
if [[ -n "$existing_pids" ]]; then
	echo "[run-test] Killing existing instance(s): $existing_pids"
	kill $existing_pids 2>/dev/null || true
	sleep 0.5
fi

# Kill any orphaned gst-launch processes (they survive cockscreen kills because
# they run with setsid and therefore aren't in cockscreen's process group).
orphan_gst="$(pgrep -f 'gst-launch.*v4l2sink\|gst-launch.*udpsink\|gst-launch.*udpsrc' || true)"
if [[ -n "$orphan_gst" ]]; then
	echo "[run-test] Killing orphaned gst-launch process(es): $orphan_gst"
	sudo kill $orphan_gst 2>/dev/null || true
	sleep 0.3
fi

# Release any process still holding /dev/video10.
sudo fuser -k /dev/video10 2>/dev/null || true
sleep 0.2

mkdir -p "$(dirname "$log_file")"
echo "[run-test] Running for ${duration}s → $log_file"
echo "[run-test] $(date '+%F %T')" >"$log_file"

# Run under sudo with a timeout; tee output so it's visible in the VS Code terminal
# as well as captured to the log file.  'timeout' sends SIGTERM after N seconds.
# The '|| true' prevents set -e from aborting when timeout terminates the process.
sudo timeout "$duration" stdbuf -oL -eL "$binary" -d 2>&1 | tee -a "$log_file" || true

echo ""
echo "================================================================"
echo " Analysis"
echo "================================================================"
"$repo_root/scripts/analyze-debug-log.sh" "$log_file"
