#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
remote_host="${COCKSCREEN_PI_HOST:-atom@cockscreen}"
binary_path="${1:-./out/build/cross-pi-zero2w-debug/cockscreen}"
scene_path="${2:-scenes/pizero-linux.scene.jsonc}"
signal_lock_seconds="${3:-4}"
run_timeout_seconds="${4:-18}"

if [[ "$signal_lock_seconds" =~ ^[0-9]+$ ]] && [[ "$run_timeout_seconds" =~ ^[0-9]+$ ]]; then
	if ((run_timeout_seconds <= signal_lock_seconds)); then
		echo "[pi-smoke] run timeout must be greater than signal lock delay"
		exit 1
	fi
else
	echo "[pi-smoke] signal lock and timeout arguments must be integers"
	exit 1
fi

stamp="$(date +%Y%m%d-%H%M%S)"
local_output_dir="$repo_root/tmp"
local_snapshot="$local_output_dir/pi-qt-shader-smoke-$stamp.png"
remote_scene="/tmp/cockscreen-pi-qt-shader-smoke.scene.jsonc"
remote_api="/tmp/cockscreen-pi-qt-shader-smoke.api.json"
remote_log="/tmp/cockscreen-pi-qt-shader-smoke.log"
remote_snapshot="/tmp/cockscreen-pi-qt-shader-smoke.png"

mkdir -p "$local_output_dir"

echo "[pi-smoke] host: $remote_host"
echo "[pi-smoke] binary: $binary_path"
echo "[pi-smoke] scene: $scene_path"
echo "[pi-smoke] signal lock wait: ${signal_lock_seconds}s"
echo "[pi-smoke] run timeout: ${run_timeout_seconds}s"

remote_output="$(ssh -o BatchMode=yes -o StrictHostKeyChecking=no "$remote_host" "cd /home/atom/cockscreen && \
python3 - <<'PY' '$scene_path' '$remote_scene'
from pathlib import Path
import sys

source = Path(sys.argv[1])
target = Path(sys.argv[2])
text = source.read_text()
if '\"render_path\": \"v4l2-dmabuf-egl\"' in text:
    text = text.replace('\"render_path\": \"v4l2-dmabuf-egl\"', '\"render_path\": \"qt-shader\"', 1)
target.write_text(text)
print(target)
PY
sudo pkill -9 -x cockscreen || true
rm -f '$remote_api' '$remote_log' '$remote_snapshot'
timeout '$run_timeout_seconds's stdbuf -oL -eL sudo '$binary_path' --scene-file '$remote_scene' --enable-web-server http://127.0.0.1:8080 > '$remote_log' 2>&1 &
pid=\$!
api_ready=0
for _ in \$(seq 1 60); do
    if curl -fsS http://127.0.0.1:8080/api/state > '$remote_api' 2>/dev/null; then
        api_ready=1
        break
    fi
    sleep 0.2
done
echo API_READY=\$api_ready
if [[ \$api_ready -eq 1 ]]; then
    sleep '$signal_lock_seconds'
    if curl -fsS http://127.0.0.1:8080/api/video-frame.png -o '$remote_snapshot'; then
        echo FRAME_READY=1
    else
        echo FRAME_READY=0
    fi
else
    echo FRAME_READY=0
fi
wait \"\$pid\"
app_rc=\$?
echo APP_RC=\$app_rc
echo --- API ---
cat '$remote_api' 2>/dev/null || true
echo
echo --- LOG ---
grep -E 'Web control|Waiting for video signal lock|Analog input appears blank|No raw video frames yet|capture|video|camera|v4l2' '$remote_log' || true
echo --- SNAPSHOT ---
ls -l '$remote_snapshot' 2>/dev/null || true
")"

printf '%s\n' "$remote_output"

if ! grep -q '^API_READY=1$' <<<"$remote_output"; then
	echo "[pi-smoke] API did not become ready"
	exit 1
fi

if ! grep -q '^FRAME_READY=1$' <<<"$remote_output"; then
	echo "[pi-smoke] delayed frame capture failed"
	exit 1
fi

app_rc="$(grep '^APP_RC=' <<<"$remote_output" | tail -n 1 | cut -d= -f2)"
if [[ -z "$app_rc" ]]; then
	echo "[pi-smoke] remote app exit code was not reported"
	exit 1
fi

if [[ "$app_rc" != "0" && "$app_rc" != "124" ]]; then
	echo "[pi-smoke] unexpected app exit code: $app_rc"
	exit 1
fi

scp -o BatchMode=yes -o StrictHostKeyChecking=no "$remote_host:$remote_snapshot" "$local_snapshot" >/dev/null
echo "[pi-smoke] snapshot: $local_snapshot"
