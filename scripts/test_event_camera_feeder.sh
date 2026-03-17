#!/bin/bash
# test_event_camera_feeder.sh — standalone test for the event camera frame feeder.
#
# Runs the feeder script and pipes its raw RGB output through a local
# gst-launch-1.0 pipeline so you can verify:
#   1. The feeder starts and produces frames (real camera or test pattern).
#   2. The GStreamer encode path (rawvideoparse → videoconvert → nvvidconv →
#      nvv4l2h264enc) accepts the frames.
#   3. The encoded output is written to an .mp4 file you can inspect.
#
# Usage (run from the repository root):
#   ./scripts/test_event_camera_feeder.sh
#   ./scripts/test_event_camera_feeder.sh --test-pattern        # no camera required
#   ./scripts/test_event_camera_feeder.sh --duration 5          # record 5 seconds
#   ./scripts/test_event_camera_feeder.sh --display             # show live preview (requires $DISPLAY / nv3dsink)
#   ./scripts/test_event_camera_feeder.sh --width 640 --height 480 --fps 30
#
# Note on rawvideoparse / videoconvert in this script vs. pipeline_event_camera.txt:
#   This test script reads from a plain FIFO via fdsrc, which has no GStreamer caps
#   metadata — rawvideoparse is required to describe the frame format.
#   The RTSP pipeline (pipeline_event_camera.txt) uses appsrc with explicit
#   caps=video/x-raw,format=RGB,... so nvvidconv already knows the buffer format
#   and rawvideoparse + videoconvert are NOT needed there.

set -euo pipefail

WIDTH=1280
HEIGHT=720
FPS=30
DURATION=10
TEST_PATTERN=0
DISPLAY_MODE=0
EXTRA_ARGS=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --width)    WIDTH="$2";    shift 2 ;;
        --height)   HEIGHT="$2";   shift 2 ;;
        --fps)      FPS="$2";      shift 2 ;;
        --duration) DURATION="$2"; shift 2 ;;
        --test-pattern) TEST_PATTERN=1; shift ;;
        --display)  DISPLAY_MODE=1; shift ;;
        *) EXTRA_ARGS+=("$1"); shift ;;
    esac
done

FRAME_SIZE=$(( WIDTH * HEIGHT * 3 ))
NUM_BUFFERS=$(( FPS * DURATION ))
TIMESTAMP="$(date +%Y%m%d-%H%M%S)"
OUT_FILE="/tmp/event_camera_test_${TIMESTAMP}.mp4"
FIFO="$(mktemp -u /tmp/event_camera_feeder_XXXXXX.fifo)"
mkfifo "${FIFO}"

echo "=== Event camera feeder test ==="
echo "  Resolution : ${WIDTH}x${HEIGHT} @ ${FPS} fps"
echo "  Frame size : ${FRAME_SIZE} bytes"
echo "  Duration   : ${DURATION} s (${NUM_BUFFERS} frames)"
if [[ $TEST_PATTERN -eq 1 ]]; then
    echo "  Mode       : test pattern (no camera)"
else
    echo "  Mode       : real camera (faery + neuromorphic_drivers)"
fi
if [[ $DISPLAY_MODE -eq 1 ]]; then
    echo "  Output     : live preview (nv3dsink)"
else
    echo "  Output     : ${OUT_FILE}"
fi
echo ""

cleanup() {
    echo ""
    echo "Stopping..."
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
#   fdsrc          — raw byte source from the FIFO (no caps metadata)
#   rawvideoparse  — frames the byte stream into properly-described video buffers;
#                    without this, nvvidconv sees raw bytes and refuses to link
#   videoconvert   — CPU-side RGB → I420; nvvidconv on Jetson does not accept RGB
#                    from non-hardware-allocated buffers (fdsrc / rawvideoparse)
#   nvvidconv      — moves frames from system memory into NVMM memory for the encoder
#
# num-buffers=N causes gst-launch-1.0 to send EOS after N frames so the output
# file is properly finalised without needing a Ctrl-C.
exec 3< "${FIFO}"

if [[ $DISPLAY_MODE -eq 1 ]]; then
    # Live preview — requires a display (nv3dsink, Jetson with attached screen).
    gst-launch-1.0 \
        fdsrc fd=3 blocksize="${FRAME_SIZE}" num-buffers="${NUM_BUFFERS}" \
        ! rawvideoparse format=rgb width="${WIDTH}" height="${HEIGHT}" framerate="${FPS}/1" \
        ! videoconvert \
        ! "video/x-raw,format=I420" \
        ! nvvidconv \
        ! "video/x-raw(memory:NVMM),format=I420" \
        ! nv3dsink sync=false
else
    # Headless: encode and write to an mp4 file.
    gst-launch-1.0 \
        fdsrc fd=3 blocksize="${FRAME_SIZE}" num-buffers="${NUM_BUFFERS}" \
        ! rawvideoparse format=rgb width="${WIDTH}" height="${HEIGHT}" framerate="${FPS}/1" \
        ! videoconvert \
        ! "video/x-raw,format=I420" \
        ! nvvidconv \
        ! "video/x-raw(memory:NVMM),format=I420" \
        ! nvv4l2h264enc bitrate=5000000 \
        ! h264parse \
        ! mp4mux \
        ! filesink location="${OUT_FILE}"
    echo ""
    echo "Output written to: ${OUT_FILE}"
    echo "File size: $(du -h "${OUT_FILE}" | cut -f1)"
fi

exec 3<&-
