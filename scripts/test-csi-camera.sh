#!/usr/bin/env bash
# Test CSI camera detection and capture on a connected Pi.
#
# Detects which sensor (OV5647 or IMX219) is currently attached via the media
# controller, runs format-negotiation and frame-capture tests against the
# unicam node, and verifies the bcm2835-isp pipeline nodes are present.
#
# Usage (run from dev machine):
#   scripts/test-csi-camera.sh [user@host]
#
# Or run directly on the Pi:
#   scripts/test-csi-camera.sh
#
# Requirements on the Pi: v4l2-ctl, media-ctl (v4l-utils)
# Returns: 0 if all tests pass, 1 if any fail.

set -euo pipefail

REMOTE="${1:-}"
UNICAM_DEV="/dev/video2"
MEDIA_DEV="/dev/media0"

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

pass=0
fail=0
skip=0

ok()   { echo "  PASS  $*"; pass=$(( pass + 1 )); }
nok()  { echo "  FAIL  $*"; fail=$(( fail + 1 )); }
skip() { echo "  SKIP  $*"; skip=$(( skip + 1 )); }
head() { echo; echo "── $* ──"; }

run() {
    if [[ -n "$REMOTE" ]]; then
        ssh "$REMOTE" "$@"
    else
        bash -c "$@"
    fi
}

# ---------------------------------------------------------------------------
# 1. Device presence
# ---------------------------------------------------------------------------
head "Device presence"

if run "test -e $UNICAM_DEV" 2>/dev/null; then
    ok "$UNICAM_DEV exists"
else
    nok "$UNICAM_DEV missing — is unicam loaded?"
    exit 1
fi

if run "test -e $MEDIA_DEV" 2>/dev/null; then
    ok "$MEDIA_DEV exists"
else
    nok "$MEDIA_DEV missing — media controller not available"
    exit 1
fi

# ---------------------------------------------------------------------------
# 2. Sensor detection via media controller
# ---------------------------------------------------------------------------
head "Sensor detection"

topo=$(run "media-ctl -d $MEDIA_DEV --print-topology 2>&1")

sensor_name=""
sensor_format=""
sensor_res=""

if echo "$topo" | grep -q "imx219"; then
    sensor_name="IMX219"
    sensor_format="RG10"        # V4L2_PIX_FMT_SRGGB10
    # Resolutions supported by IMX219 via unicam (in order of preference for tests)
    sensor_resolutions="1280x720 1640x1232 1920x1080"
    sensor_mbus_code="0x300f"   # MEDIA_BUS_FMT_SRGGB10_1X10
    ok "IMX219 detected (Pi Camera v2)"
elif echo "$topo" | grep -q "ov5647"; then
    sensor_name="OV5647"
    sensor_format="GB10"        # V4L2_PIX_FMT_SGBRG10
    sensor_resolutions="640x480"
    sensor_mbus_code="0x300e"   # MEDIA_BUS_FMT_SGBRG10_1X10
    ok "OV5647 detected (Pi Camera v1)"
else
    nok "No recognised CSI sensor in media topology (expected ov5647 or imx219)"
    echo "$topo" | grep "entity" | head -10
    exit 1
fi

# Show the subdev entity line and current negotiated format
entity_line=$(echo "$topo" | { grep -E "ov5647|imx219" || true; } | awk 'NR==1' | xargs)
current_fmt=$(echo "$topo" | { grep "fmt:" || true; } | awk 'NR==1' | xargs)
echo "       entity: $entity_line"
echo "       format: $current_fmt"

# ---------------------------------------------------------------------------
# 3. bcm2835-isp pipeline nodes
# ---------------------------------------------------------------------------
head "ISP pipeline nodes"

# Scan for bcm2835-isp input (VIDEO_OUTPUT) and output (VIDEO_CAPTURE) nodes.
# v4l2-ctl --info prints "Device Caps : 0x04200002" for output, 0x...01 for capture.
isp_scan=$(run "
    for n in \$(seq 0 31); do
        dev=/dev/video\$n
        info=\$(v4l2-ctl -d \$dev --info 2>/dev/null) || continue
        echo \"\$info\" | grep -q 'bcm2835-isp' || continue
        caps=\$(echo \"\$info\" | awk '/Device Caps/{print \$NF}')
        echo \"\$dev \$caps\"
    done
" 2>/dev/null || true)

isp_input=""
isp_output=""
while read -r dev caps; do
    [[ -z "$dev" || -z "$caps" ]] && continue
    caps_dec=$(( caps ))
    if (( (caps_dec & 2) != 0 )) && [[ -z "$isp_input" ]]; then
        isp_input="$dev"
    elif (( (caps_dec & 1) != 0 )) && [[ -z "$isp_output" ]]; then
        isp_output="$dev"
    fi
done <<< "$isp_scan"

if [[ -n "$isp_input" ]]; then
    ok "ISP input  node: $isp_input"
else
    nok "ISP input node not found (bcm2835-isp VIDEO_OUTPUT)"
fi

if [[ -n "$isp_output" ]]; then
    ok "ISP output node: $isp_output"
else
    nok "ISP output node not found (bcm2835-isp VIDEO_CAPTURE)"
fi

# ---------------------------------------------------------------------------
# 4. Format negotiation and frame capture per resolution
# ---------------------------------------------------------------------------
head "Frame capture tests ($sensor_name — $sensor_format)"

# Kill anything that might be holding the device
run "fuser -k $UNICAM_DEV 2>/dev/null || true" 2>/dev/null || true
sleep 0.5

for res in $sensor_resolutions; do
    w=${res%x*}
    h=${res#*x}
    expected_per_frame=$(( w * h * 2 ))
    tmpf="/tmp/csi_test_${res}.raw"

    run "v4l2-ctl -d $UNICAM_DEV \
        --set-fmt-video=width=$w,height=$h,pixelformat=$sensor_format \
        --stream-mmap --stream-count=5 --stream-to=$tmpf 2>/dev/null" || true

    if run "test -f $tmpf" 2>/dev/null; then
        got=$(run "wc -c < $tmpf")
        got=$(echo "$got" | tr -d ' ')
        run "rm -f $tmpf" 2>/dev/null || true

        if [[ "$got" -eq 0 ]]; then
            nok "$res  no data written (0 bytes)"
        elif (( got % expected_per_frame == 0 )); then
            frames=$(( got / expected_per_frame ))
            ok "$res  $sensor_format  ${frames} frames × ${expected_per_frame}B (stride=$(( w * 2 )))"
        else
            nok "$res  unexpected size: got=${got}, expected multiple of ${expected_per_frame}"
        fi
    else
        nok "$res  capture failed — no output file"
    fi
done

# ---------------------------------------------------------------------------
# 5. Summary
# ---------------------------------------------------------------------------
echo
echo "══════════════════════════════════════"
total=$(( pass + fail + skip ))
echo " $sensor_name on $UNICAM_DEV"
echo " PASS=$pass  FAIL=$fail  SKIP=$skip  TOTAL=$total"
echo "══════════════════════════════════════"

[[ "$fail" -eq 0 ]]
