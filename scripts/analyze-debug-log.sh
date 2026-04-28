#!/usr/bin/env bash
# Analyze a cockscreen debug run log and print a structured summary.
# Usage: analyze-debug-log.sh [log_file]
#   log_file: path to the log (default: ../tmp/debug-run.log)

log="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/tmp/debug-run.log}"

if [[ ! -f "$log" ]]; then
	echo "Log file not found: $log"
	exit 1
fi

echo "Log: $log  ($(wc -l <"$log") lines)"
echo ""

# ── Helpers ──────────────────────────────────────────────────────────────────
count() { grep -c "$1" "$log" 2>/dev/null || echo 0; }
first() { grep -m1 "$1" "$log" 2>/dev/null || true; }
exists() { grep -q "$1" "$log" 2>/dev/null; }

pass() { printf "  [PASS] %s\n" "$*"; }
fail() { printf "  [FAIL] %s\n" "$*"; }
info() { printf "  [INFO] %s\n" "$*"; }
warn() { printf "  [WARN] %s\n" "$*"; }

issues=0

# ── Loopback pipeline startup ─────────────────────────────────────────────────
echo "--- Loopback Pipeline ---"
if exists "\[LoopbackPipeline\] receiver cmd:"; then
	pass "Receiver pipeline started"
else
	fail "Receiver pipeline never started"
	((issues++))
fi

if exists "\[LoopbackPipeline\] sender cmd:"; then
	pass "Sender pipeline started"
else
	fail "Sender pipeline never started"
	((issues++))
fi

if exists "\[LoopbackPipeline\] cleared stale format"; then
	info "Stale format cleared on loopback device"
fi

if exists "\[LoopbackPipeline\] device ready:"; then
	device_ready_line="$(first '\[LoopbackPipeline\] device ready:')"
	pass "Device ready: ${device_ready_line#*device ready: }"
else
	fail "Device never became ready (wait_for_device_ready timed out?)"
	((issues++))
fi

# ── STREAMON ─────────────────────────────────────────────────────────────────
echo ""
echo "--- VIDIOC_STREAMON ---"
streamon_retries=$(count "STREAMON not ready (EIO)")
streamon_ok=$(count "STREAMON ok")
streamon_fail=$(count "STREAMON failed")

if [[ "$streamon_ok" -gt 0 ]]; then
	pass "STREAMON succeeded (after ${streamon_retries} EIO retries)"
elif [[ "$streamon_fail" -gt 0 ]]; then
	fail "STREAMON failed permanently"
	((issues++))
else
	warn "STREAMON outcome not found in log"
fi

# ── Frame delivery ────────────────────────────────────────────────────────────
echo ""
echo "--- Frame Delivery ---"
first_frame_line="$(first '\[LoopbackCapture\] frame 1 ')"
if [[ -n "$first_frame_line" ]]; then
	pass "First frame: ${first_frame_line#*\] }"
else
	fail "No frames delivered to LoopbackCapture"
	((issues++))
fi

# Count distinct frame log lines (logged at 1,2,3 then every 60)
frame_log_count=$(count "\[LoopbackCapture\] frame [0-9]")
if [[ "$frame_log_count" -gt 0 ]]; then
	last_frame_line="$(grep '\[LoopbackCapture\] frame [0-9]' "$log" | tail -1)"
	info "Last frame log: ${last_frame_line#*\] }"
fi

dqbuf_errors=$(count "VIDIOC_DQBUF failed")
if [[ "$dqbuf_errors" -gt 0 ]]; then
	warn "DQBUF errors: ${dqbuf_errors}"
fi

# ── Sender / receiver lifecycle ───────────────────────────────────────────────
echo ""
echo "--- Sender / Receiver Lifecycle ---"
sender_exits=$(count "\[LoopbackPipeline\] sender exited")
sender_restarts=$(count "\[LoopbackPipeline\] sender.*restarting")
receiver_exits=$(count "\[LoopbackPipeline\] receiver exited")
receiver_restarts=$(count "\[LoopbackPipeline\] restarting receiver")

if [[ "$sender_exits" -gt 0 ]]; then
	info "Sender exited ${sender_exits} time(s)"
fi
if [[ "$sender_restarts" -gt 0 ]]; then
	pass "Sender auto-restarted ${sender_restarts} time(s)"
else
	if [[ "$sender_exits" -gt 0 ]]; then
		warn "Sender exited but no restart logged"
	fi
fi
if [[ "$receiver_exits" -gt 0 ]]; then
	info "Receiver exited ${receiver_exits} time(s)"
fi
if [[ "$receiver_restarts" -gt 0 ]]; then
	pass "Receiver auto-restarted ${receiver_restarts} time(s)"
fi

# GST prerolled / playing state
if exists "Pipeline is PREROLLED"; then pass "GStreamer pipeline reached PREROLLED"; fi
if exists "Setting pipeline to PLAYING"; then pass "GStreamer pipeline set to PLAYING"; fi
if exists "New clock:"; then pass "GStreamer pipeline has active clock (playing)"; fi

# ── Errors ────────────────────────────────────────────────────────────────────
echo ""
echo "--- Errors & Warnings ---"
gst_errors=$(grep -c "ERROR\|CRITICAL" "$log" 2>/dev/null || echo 0)
if [[ "$gst_errors" -gt 0 ]]; then
	warn "${gst_errors} GStreamer ERROR/CRITICAL line(s):"
	grep "ERROR\|CRITICAL" "$log" | head -5 | while IFS= read -r line; do
		printf "       %s\n" "$line"
	done
	((issues++))
fi

if exists "EBUSY\|Device or resource busy"; then
	fail "Device busy (EBUSY) — run: sudo fuser -k /dev/video10"
	((issues++))
fi

if exists "LoopbackCapture.*failed"; then
	fail "LoopbackCapture reported a failure:"
	grep "LoopbackCapture.*failed" "$log" | head -3 | while IFS= read -r line; do
		printf "       %s\n" "$line"
	done
	((issues++))
fi

# ── Summary ───────────────────────────────────────────────────────────────────
echo ""
echo "================================================================"
if [[ "$issues" -eq 0 ]]; then
	echo " Result: OK — no issues detected"
else
	echo " Result: ${issues} issue(s) detected (see [FAIL] entries above)"
fi
echo "================================================================"
