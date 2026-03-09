#!/usr/bin/env python3
"""
Event camera frame feeder script.

Reads frames from an event camera using the faery library and writes raw RGB
bytes to stdout for consumption by event_camera_source.c via a GIOChannel.

If faery is not available, a simple test pattern is emitted instead so the
pipeline can be exercised without hardware.
"""

import argparse
import sys
import time
import neuromorphic_drivers as nd


def main():
    parser = argparse.ArgumentParser(description="Event camera frame feeder")
    parser.add_argument("--width", type=int, default=640, help="Frame width in pixels")
    parser.add_argument("--height", type=int, default=480, help="Frame height in pixels")
    parser.add_argument("--frame-rate", type=int, default=30, help="Target frame rate (Hz)")
    parser.add_argument("--driver", type=str, default="prophesee", help="Camera driver name")
    parser.add_argument("--manufacturer", type=str, default="prophesee", help="Camera manufacturer")
    parser.add_argument("--colormap", type=str, default="magma", help="Colormap for rendering")
    parser.add_argument("--tau", type=float, default=0.1, help="Time constant for decay (seconds)")
    parser.add_argument("--decay", type=str, default="exponential", help="Decay type")
    parser.add_argument("--diff_on", type=int, default=200, help="Bias diff on")
    parser.add_argument("--diff_off", type=int, default=150, help="Bias diff off")
    args = parser.parse_args()

    try:
        import faery
        
        nd_config = nd.prophesee_evk4.Configuration()
        nd_config.biases.diff_off = args.diff_off
        nd_config.biases.diff_on = args.diff_on

        stream = (
            faery.events_stream_from_camera(
                driver="NeuromorphicDrivers",
                nd_configuration=nd_config,
            )
            .regularize(frequency_hz=args.frame_rate)
            .render(
                colormap=args.colormap,
                tau=args.tau,
                decay=args.decay,
            )
        )

        for frame in stream:
            sys.stdout.buffer.write(frame.pixels[:, :, :3].tobytes())
            sys.stdout.buffer.flush()

    except ImportError:
        # faery not available: emit a simple test pattern so the pipeline can
        # still be exercised without camera hardware or the faery library.
        frame_size = args.width * args.height * 3
        frame_interval = 1.0 / args.frame_rate
        frame_num = 0
        while True:
            # Cycle through a basic grey ramp so frames are visually distinct
            grey = frame_num % 256
            data = bytes([grey] * frame_size)
            sys.stdout.buffer.write(data)
            sys.stdout.buffer.flush()
            frame_num += 1
            time.sleep(frame_interval)


if __name__ == "__main__":
    main()
