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
#   ./scripts/test_event_camera_feeder.sh --display             # live preview (needs $DISPLAY / nv3dsink)
#   ./scripts/test_event_camera_feeder.sh --width 640 --height 480 --fps 30
#
# If real-camera mode exits immediately, check the feeder stderr output printed
# above the GStreamer output — it will show the Python traceback.
#
# Note on rawvideoparse / videoconvert in this script vs. pipeline_event_camera.txt:
#   This test script reads from a plain FIFO via fdsrc, which has no GStreamer caps
#   metadata — rawvideoparse is required to describe the frame format.
#   The RTSP pipeline (pipeline_event_camera.txt) uses appsrc with explicit
#   caps=video/x-raw,format=RGB,... so nvvidconv already knows the buffer format
#   and rawvideoparse + videoconvert are NOT needed there.
#
# Note on duration / EOS:
#   We do NOT use num-buffers on fdsrc.  A Linux FIFO delivers data in kernel pipe
#   buffer chunks (~64 KB), so fdsrc.read() returns far fewer bytes than blocksize
#   on each call.  num-buffers counts read() calls, not complete video frames, so
#   300 "buffers" would terminate after only ~7 frames.  Instead we run gst-launch
#   in the background and send it SIGINT after ${DURATION} seconds.  gst-launch
#   handles SIGINT by pushing EOS through the pipeline, which causes mp4mux to
#   write the file trailer and finalise the output correctly.

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
TIMESTAMP="$(date +%Y%m%d-%H%M%S)"
OUT_FILE="/tmp/event_camera_test_${TIMESTAMP}.mp4"
FIFO="$(mktemp -u /tmp/event_camera_feeder_XXXXXX.fifo)"
mkfifo "${FIFO}"

echo "=== Event camera feeder test ==="
echo "  Resolution : ${WIDTH}x${HEIGHT} @ ${FPS} fps"
echo "  Frame size : ${FRAME_SIZE} bytes"
echo "  Duration   : ${DURATION} s"
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

GST_PID=""
FEEDER_PID=""

cleanup() {
    echo ""
    echo "Stopping..."
    # Send SIGINT to gst-launch for graceful EOS / file finalisation.
    kill -INT "${GST_PID:-}" 2>/dev/null || true
    wait "${GST_PID:-}" 2>/dev/null || true
    # Close the FIFO read end so the feeder gets SIGPIPE on its next write.
    exec 3<&- 2>/dev/null || true
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

# Open the FIFO read end on fd 3.
exec 3< "${FIFO}"

# Pipeline notes:
#   fdsrc         — raw byte source from the FIFO (no caps metadata)
#   rawvideoparse — re-frames the byte stream into video buffers with proper caps;
#                   without this, downstream elements see raw bytes with no format info
#   videoconvert  — CPU-side RGB → I420; nvvidconv on Jetson does not accept RGB
#                   directly from non-hardware-allocated buffers
#   nvvidconv     — copies frames from system memory into NVMM memory for the encoder
if [[ $DISPLAY_MODE -eq 1 ]]; then
    gst-launch-1.0 \
        fdsrc fd=3 blocksize="${FRAME_SIZE}" \
        ! rawvideoparse format=rgb width="${WIDTH}" height="${HEIGHT}" framerate="${FPS}/1" \
        ! videoconvert \
        ! "video/x-raw,format=I420" \
        ! nvvidconv \
        ! "video/x-raw(memory:NVMM),format=I420" \
        ! nv3dsink sync=false &
else
    gst-launch-1.0 \
        fdsrc fd=3 blocksize="${FRAME_SIZE}" \
        ! rawvideoparse format=rgb width="${WIDTH}" height="${HEIGHT}" framerate="${FPS}/1" \
        ! videoconvert \
        ! "video/x-raw,format=I420" \
        ! nvvidconv \
        ! "video/x-raw(memory:NVMM),format=I420" \
        ! nvv4l2h264enc bitrate=5000000 \
        ! h264parse \
        ! mp4mux \
        ! filesink location="${OUT_FILE}" &
fi
GST_PID=$!

# Wait for DURATION seconds then send SIGINT to gst-launch.
# SIGINT causes gst-launch to push EOS through the pipeline so mp4mux writes
# its file trailer and the output is a valid, playable mp4.
sleep "${DURATION}"
echo ""
echo "Duration reached — sending EOS to pipeline..."
kill -INT "${GST_PID}" 2>/dev/null || true
wait "${GST_PID}" 2>/dev/null || true
GST_PID=""

if [[ $DISPLAY_MODE -eq 0 ]]; then
    echo "Output written to: ${OUT_FILE}"
    if [[ -f "${OUT_FILE}" ]]; then
        echo "File size: $(du -h "${OUT_FILE}" | cut -f1)"
    else
        echo "WARNING: output file not found — pipeline may have failed"
    fi
fi
