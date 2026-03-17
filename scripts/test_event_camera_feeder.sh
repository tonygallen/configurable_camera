#!/bin/bash
# test_event_camera_feeder.sh — standalone test for the event camera frame feeder.
#
# Runs the feeder script and pipes its raw RGB output through a local
# gst-launch-1.0 pipeline so you can verify:
#   1. The feeder starts and produces frames (real camera or test pattern).
#   2. The GStreamer encode path (nvvidconv + nvv4l2h264enc) accepts the frames.
#   3. The rendered video looks correct (opens a preview window on the Jetson).
#
# Usage (run from the repository root):
#   ./scripts/test_event_camera_feeder.sh
#   ./scripts/test_event_camera_feeder.sh --width 640 --height 480 --fps 30
#   ./scripts/test_event_camera_feeder.sh --test-pattern   # no camera required
#
# With --test-pattern the feeder is bypassed entirely and a grey-ramp is
# generated in-process, so you can verify the GStreamer pipeline works even
# without a connected event camera or a faery installation.

set -euo pipefail

WIDTH=1280
HEIGHT=720
FPS=30
TEST_PATTERN=0
EXTRA_ARGS=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --width)  WIDTH="$2";  shift 2 ;;
        --height) HEIGHT="$2"; shift 2 ;;
        --fps)    FPS="$2";    shift 2 ;;
        --test-pattern) TEST_PATTERN=1; shift ;;
        *) EXTRA_ARGS+=("$1"); shift ;;
    esac
done

FRAME_SIZE=$(( WIDTH * HEIGHT * 3 ))
FIFO="$(mktemp -u /tmp/event_camera_feeder_XXXXXX.fifo)"
mkfifo "${FIFO}"

echo "=== Event camera feeder test ==="
echo "  Resolution : ${WIDTH}x${HEIGHT} @ ${FPS} fps"
echo "  Frame size : ${FRAME_SIZE} bytes"
if [[ $TEST_PATTERN -eq 1 ]]; then
    echo "  Mode       : test pattern (no camera)"
else
    echo "  Mode       : real camera (faery + neuromorphic_drivers)"
fi
echo ""
echo "Press Ctrl-C to stop."
echo ""

cleanup() {
    echo ""
    echo "Stopping..."
    # Kill the feeder if still running
    kill "${FEEDER_PID:-}" 2>/dev/null || true
    wait "${FEEDER_PID:-}" 2>/dev/null || true
    rm -f "${FIFO}"
}
trap cleanup EXIT INT TERM

if [[ $TEST_PATTERN -eq 1 ]]; then
    # Generate a grey-ramp test pattern without the feeder script.
    python3 -c "
import sys, time
frame_num = 0
fps = ${FPS}
frame_size = ${FRAME_SIZE}
interval = 1.0 / fps
while True:
    grey = frame_num % 256
    sys.stdout.buffer.write(bytes([grey] * frame_size))
    sys.stdout.buffer.flush()
    frame_num += 1
    time.sleep(interval)
" > "${FIFO}" &
    FEEDER_PID=$!
else
    python3 scripts/event_camera_frame_feeder.py \
        --width "${WIDTH}" \
        --height "${HEIGHT}" \
        --frame-rate "${FPS}" \
        ${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"} > "${FIFO}" &
    FEEDER_PID=$!
fi

# Read the FIFO with gst-launch-1.0 using fdsrc.
# Pipeline notes:
#   fdsrc       — raw byte source from the FIFO
#   rawvideoparse — frames the byte stream into properly-described video buffers
#                   (without this, nvvidconv cannot link: it sees raw bytes, not video frames)
#   videoconvert — CPU-side RGB → I420 conversion (nvvidconv on Jetson does not
#                  accept RGB directly when the source is fdsrc)
#   nvvidconv   — moves frames from system memory into NVMM memory
exec 3< "${FIFO}"
gst-launch-1.0 \
    fdsrc fd=3 blocksize="${FRAME_SIZE}" \
    ! rawvideoparse format=rgb width="${WIDTH}" height="${HEIGHT}" framerate="${FPS}/1" \
    ! videoconvert \
    ! "video/x-raw,format=I420" \
    ! nvvidconv \
    ! "video/x-raw(memory:NVMM),format=I420" \
    ! nvv4l2h264enc bitrate=5000000 \
    ! h264parse \
    ! avdec_h264 \
    ! xvimagesink sync=false
exec 3<&-
