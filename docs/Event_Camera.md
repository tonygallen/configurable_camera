# Event Camera Quickstart

This guide covers everything needed to stream an event camera (e.g. DVXplorer, EVK4) over RTSP using `configurable_camera` on an NVIDIA Jetson.  No Basler/Pylon hardware or software is required.

## Prerequisites

- NVIDIA Jetson with GStreamer already installed (standard on JetPack)
- Event camera connected over USB
- [`faery`](https://github.com/neuromorphic-paris/faery) Python library installed
- `configurable_camera` repository cloned to the Jetson

## 1. Install dependencies

```bash
sudo apt update
sudo apt install libgstrtspserver-1.0-0 libgstrtspserver-1.0-dev screen
```

> The Basler Pylon SDK and `gst-plugin-pylon` are **not** needed for event camera mode.

## 2. Build the main binary

Run from the repository root:

```bash
make main
```

> `sensor_setup` is for Basler sensors only — you do not need to build or run it.

## 3. Create the recording storage directory

The program writes startup metadata to a subdirectory of `"Raw Storage"` on every launch (even when raw recording is disabled).  Create that directory before running:

```bash
sudo mkdir -p /data/rec
sudo chown $USER /data/rec
```

If you want to use a different path, change `"Raw Storage"` in `config.json` (see step 4) and create that directory instead.

## 4. Configure `config.json`

Copy the example event camera config to the repository root and open it for editing:

```bash
cp configs/event_camera_config.json config.json
nano config.json   # or your preferred editor
```

### Fields to update

| Field | What to set |
|---|---|
| `"Name"` | An identifier for this camera (used in the RTSP mount point) |
| `"Control IP"` | Network interface for status broadcast and UDP control — use the interface name (e.g. `"eth0"`) or the Jetson's IP address |
| `"RTSP Address"` | Network interface the RTSP server listens on — use the interface name (e.g. `"eth0"`) or the Jetson's IP address |
| `"Raw Storage"` | Path to the metadata/recording directory (default: `"/data/rec"`) |
| `"Event Camera Width"` | Frame width in pixels (default: `640`) |
| `"Event Camera Height"` | Frame height in pixels (default: `480`) |
| `"Event Camera Frame Rate"` | Frames per second (default: `30`) |

### Event camera config fields reference

```json
"Sensor Type":              "event_camera",
"Event Camera Width":       640,
"Event Camera Height":      480,
"Event Camera Frame Rate":  30
```

Setting `"Sensor Type"` to `"event_camera"` is what tells the program to use the
`appsrc`-based pipeline and the faery frame feeder instead of the Basler pipeline.

### Minimal example

```json
{
    "Metadata": {
        "Name":          "evk4",
        "Location":      "lab",
        "Range":         5,
        "Yaw":           0,
        "Elevated":      false,
        "Lens Name":     "None",
        "Focal Length":  0,
        "Collection ID": "test"
    },
    "Control Data": {
        "Record Raw":              false,
        "Raw Storage":             "/data/rec",
        "Control IP":              "eth0",
        "Control Port":            9000,
        "Status Broadcast Port":   9001,
        "Raw Time Limit":          600,
        "RTSP Address":            "eth0",
        "Sensor Type":             "event_camera",
        "Event Camera Width":      640,
        "Event Camera Height":     480,
        "Event Camera Frame Rate": 30,
        "Hardware Scripts":        []
    }
}
```

> **Note:** `"Control IP"` and `"RTSP Address"` accept either a network interface name
> (e.g. `"eth0"`) or a literal IPv4 address.  Using an interface name is recommended —
> the program will resolve it to the current IP at startup.

## 5. Run the program

**The program must be run from the repository root** so that it can find
`pipeline_event_camera.txt` and `scripts/event_camera_frame_feeder.py` via relative paths.

```bash
./main
# or, to keep it running in a detached screen session:
./run.sh
```

On startup you should see output similar to:

```
[timestamp] Starting camera program.
[timestamp] Successfully read config file, verify values are correct:
 ----- Configurable Camera Settings -----
...
[timestamp] RTSP server ready at rtsp://<ip>:8554/<name>_event_camera
[timestamp] Event camera source set up (PID: <pid>).
```

## 6. Connect an RTSP client

The RTSP stream URL follows the pattern:

```
rtsp://<jetson-ip>:8554/<config-name>_event_camera
```

Using the minimal example above (name `evk4`, Jetson at `192.168.1.100`):

```
rtsp://192.168.1.100:8554/evk4_event_camera
```

You can test the stream with VLC or GStreamer on another machine:

```bash
# VLC
vlc rtsp://192.168.1.100:8554/evk4_event_camera

# GStreamer
gst-launch-1.0 rtspsrc location=rtsp://192.168.1.100:8554/evk4_event_camera latency=0 ! decodebin ! autovideosink
```

> **Note for Jetsons:** `autovideosink` may not work; use `xvimagesink` instead.

## 7. Stop the stream

In the terminal (or screen session):

```
Ctrl-C
```

Or from another shell:

```bash
screen -r camera   # attach to the screen session
# then Ctrl-C
```

## Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| `Failed to create recording directory` | `"Raw Storage"` directory does not exist | `mkdir -p /data/rec` |
| `Failed to copy startup config to metadata file` | Recording directory does not exist | `mkdir -p /data/rec` |
| `Failed to spawn faery frame feeder` | `python3` not in PATH, or `scripts/event_camera_frame_feeder.py` not found | Run `./main` from the repo root; confirm `which python3` works |
| `Event camera source setup failed` | faery library not installed or camera not detected | Check `python3 scripts/event_camera_frame_feeder.py --help` runs; confirm the event camera is connected |
| RTSP stream shows grey ramp pattern | faery is not installed (feeder uses fallback test pattern) | Install faery: see [faery documentation](https://github.com/neuromorphic-paris/faery) |
| No stream / client cannot connect | RTSP server bound to wrong address | Check `"RTSP Address"` in `config.json` is the interface facing the client |

## How it works

When `"Sensor Type"` is `"event_camera"`:

1. `main` loads `pipeline_event_camera.txt` instead of `pipeline.txt` and formats it with the configured width, height, and frame rate.
2. The GStreamer pipeline uses an `appsrc` element as the video source instead of `pylonsrc`.
3. When an RTSP client connects, `setup_event_camera_source()` spawns `scripts/event_camera_frame_feeder.py` as a subprocess and reads raw RGB frames from its stdout via a GLib `GIOChannel`.
4. Each frame is wrapped in a `GstBuffer` and pushed into the `appsrc`, which drives the rest of the encode/stream pipeline.
5. When the RTSP client disconnects, the feeder subprocess is terminated and resources are released.
