#include <errno.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <glib.h>
#include <gio/gio.h>
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>

#include "log.h"
#include "config.h"
#include "pipeline.h"
#include "event_camera_source.h"

#define FEEDER_SCRIPT "scripts/event_camera_frame_feeder.py"
#define DEFAULT_EVENT_CAMERA_WIDTH  640
#define DEFAULT_EVENT_CAMERA_HEIGHT 480
#define DEFAULT_EVENT_CAMERA_FRAMERATE 30

/* Push one black (all-zeros) frame to keep the RTSP pipeline warm while the
 * Python feeder subprocess is starting up.  Cancelled as soon as the first
 * real frame arrives via the G_IO_IN watch.
 */
static gboolean push_priming_frame(gpointer data) {
    PipelineData *pipeline_data = data;
    ControlData *control_data = pipeline_data->control_data;

    int width = control_data->eventCameraWidth > 0
        ? control_data->eventCameraWidth : DEFAULT_EVENT_CAMERA_WIDTH;
    int height = control_data->eventCameraHeight > 0
        ? control_data->eventCameraHeight : DEFAULT_EVENT_CAMERA_HEIGHT;
    int frame_rate = control_data->eventCameraFrameRate > 0
        ? control_data->eventCameraFrameRate : DEFAULT_EVENT_CAMERA_FRAMERATE;

    gsize frame_size = (gsize)(width * height * 3);

    guint8 *black = g_malloc0(frame_size);  // all zeros = black
    if (black == NULL) {
        // Allocation failure is extremely unlikely; log once and keep trying.
        timestamp_prefix_err();
        g_printerr("Failed to allocate priming frame buffer.\n");
        return G_SOURCE_CONTINUE;
    }

    GstBuffer *buf = gst_buffer_new_wrapped(black, frame_size);
    if (buf == NULL) {
        g_free(black);
        timestamp_prefix_err();
        g_printerr("Failed to wrap priming frame buffer.\n");
        return G_SOURCE_CONTINUE;
    }

    GST_BUFFER_PTS(buf)      = pipeline_data->faery_pts;
    GST_BUFFER_DURATION(buf) = GST_SECOND / frame_rate;
    pipeline_data->faery_pts += GST_SECOND / frame_rate;

    GstFlowReturn ret = gst_app_src_push_buffer(
        GST_APP_SRC(pipeline_data->source), buf);
    if (ret == GST_FLOW_FLUSHING || ret == GST_FLOW_EOS) {
        // Pipeline is shutting down; stop the timer.
        pipeline_data->faery_priming_timer_id = 0;
        return G_SOURCE_REMOVE;
    }

    return G_SOURCE_CONTINUE;
}

static gboolean faery_frame_received_callback(GIOChannel *source,
                                               GIOCondition condition,
                                               gpointer data) {
    PipelineData *pipeline_data = data;
    ControlData *control_data = pipeline_data->control_data;

    int width = control_data->eventCameraWidth > 0
        ? control_data->eventCameraWidth : DEFAULT_EVENT_CAMERA_WIDTH;
    int height = control_data->eventCameraHeight > 0
        ? control_data->eventCameraHeight : DEFAULT_EVENT_CAMERA_HEIGHT;
    int frame_rate = control_data->eventCameraFrameRate > 0
        ? control_data->eventCameraFrameRate : DEFAULT_EVENT_CAMERA_FRAMERATE;

    gsize frame_size = (gsize)(width * height * 3);

    // Allocate the partial-frame accumulator on first use.
    if (pipeline_data->faery_partial_buf == NULL) {
        pipeline_data->faery_partial_buf = g_malloc(frame_size);
        if (pipeline_data->faery_partial_buf == NULL) {
            timestamp_prefix_err();
            g_printerr("Failed to allocate frame buffer for faery frame.\n");
            return G_SOURCE_CONTINUE;
        }
        pipeline_data->faery_bytes_accumulated = 0;
    }

    // Read as many bytes as are available right now into the accumulator.
    gint fd = g_io_channel_unix_get_fd(source);
    guint8 *buf = pipeline_data->faery_partial_buf;
    gsize accumulated = pipeline_data->faery_bytes_accumulated;

    gssize n = read(fd, buf + accumulated, frame_size - accumulated);
    if (n < 0) {
        if (errno == EINTR || errno == EAGAIN) {
            // No data available right now — wait for the next G_IO_IN firing.
            return G_SOURCE_CONTINUE;
        }
        timestamp_prefix_err();
        g_printerr("Error reading from faery channel: %s\n", strerror(errno));
        return G_SOURCE_CONTINUE;
    }
    if (n == 0) {
        // EOF — feeder subprocess closed its stdout
        timestamp_prefix_err();
        g_printerr("Faery frame feeder closed unexpectedly.\n");
        return G_SOURCE_REMOVE;
    }
    accumulated += (gsize)n;
    pipeline_data->faery_bytes_accumulated = accumulated;

    if (accumulated < frame_size) {
        // Don't have a complete frame yet; wait for the next G_IO_IN.
        return G_SOURCE_CONTINUE;
    }

    // We have a complete frame.  Cancel the priming timer now that real data
    // has arrived (it may already have been cancelled on a previous frame).
    if (pipeline_data->faery_priming_timer_id != 0) {
        g_source_remove(pipeline_data->faery_priming_timer_id);
        pipeline_data->faery_priming_timer_id = 0;
    }

    // Reset the accumulator so the next frame starts fresh even if
    // we return early due to a buffer-allocation failure.
    pipeline_data->faery_bytes_accumulated = 0;

    GstBuffer *gst_buf = gst_buffer_new_wrapped(buf, frame_size);
    if (gst_buf == NULL) {
        timestamp_prefix_err();
        g_printerr("Failed to create GstBuffer for faery frame.\n");
        g_free(buf);
        pipeline_data->faery_partial_buf = NULL;
        return G_SOURCE_CONTINUE;
    }
    // buf ownership transferred to gst_buf; clear the pointer.
    pipeline_data->faery_partial_buf = NULL;

    GST_BUFFER_PTS(gst_buf)      = pipeline_data->faery_pts;
    GST_BUFFER_DURATION(gst_buf) = GST_SECOND / frame_rate;
    pipeline_data->faery_pts    += GST_SECOND / frame_rate;

    GstFlowReturn ret = gst_app_src_push_buffer(
        GST_APP_SRC(pipeline_data->source), gst_buf);
    if (ret != GST_FLOW_OK) {
        timestamp_prefix_err();
        g_printerr("Failed to push faery frame buffer to appsrc: %d\n", ret);
    }

    return G_SOURCE_CONTINUE;
}

int setup_event_camera_source(PipelineData *pipeline_data) {
    ControlData *control_data = pipeline_data->control_data;

    int width = control_data->eventCameraWidth > 0
        ? control_data->eventCameraWidth : DEFAULT_EVENT_CAMERA_WIDTH;
    int height = control_data->eventCameraHeight > 0
        ? control_data->eventCameraHeight : DEFAULT_EVENT_CAMERA_HEIGHT;
    int frame_rate = control_data->eventCameraFrameRate > 0
        ? control_data->eventCameraFrameRate : DEFAULT_EVENT_CAMERA_FRAMERATE;

    gchar *width_str     = g_strdup_printf("%d", width);
    gchar *height_str    = g_strdup_printf("%d", height);
    gchar *framerate_str = g_strdup_printf("%d", frame_rate);

    gchar *argv[] = {
        "python3",
        FEEDER_SCRIPT,
        "--width",      width_str,
        "--height",     height_str,
        "--frame-rate", framerate_str,
        NULL
    };

    GError *error = NULL;
    gint stdout_fd = -1;
    gboolean spawned = g_spawn_async_with_pipes(
        NULL,                        /* working directory (inherit) */
        argv,
        NULL,                        /* envp (inherit) */
        G_SPAWN_SEARCH_PATH,
        NULL, NULL,                  /* child setup */
        &pipeline_data->faery_feeder_pid,
        NULL,                        /* stdin fd */
        &stdout_fd,                  /* stdout fd */
        NULL,                        /* stderr fd */
        &error
    );

    g_free(width_str);
    g_free(height_str);
    g_free(framerate_str);

    if (!spawned) {
        timestamp_prefix_err();
        g_printerr("Failed to spawn faery frame feeder: %s\n",
                   error ? error->message : "unknown error");
        if (error)
            g_error_free(error);
        return -1;
    }

    pipeline_data->faery_channel = g_io_channel_unix_new(stdout_fd);
    g_io_channel_set_encoding(pipeline_data->faery_channel, NULL, NULL);
    g_io_channel_set_flags(pipeline_data->faery_channel, G_IO_FLAG_NONBLOCK, NULL);

    pipeline_data->faery_pts              = 0;
    pipeline_data->faery_partial_buf      = NULL;
    pipeline_data->faery_bytes_accumulated = 0;

    // Start a timer that pushes black frames at approximately the configured
    // frame rate so the pipeline stays warm and VLC doesn't time out waiting
    // for the first real frame from the Python feeder (which takes 3-5 s to
    // start).  The GLib timer interval is in whole milliseconds, so 30 fps
    // gives 33 ms (≈33.33 ms); this small rounding does not affect PTS
    // correctness because GST_BUFFER_DURATION is computed in nanoseconds.
    pipeline_data->faery_priming_timer_id = g_timeout_add(
        1000 / frame_rate,
        push_priming_frame,
        pipeline_data);

    pipeline_data->faery_watch_id = g_io_add_watch(
        pipeline_data->faery_channel, G_IO_IN,
        (GIOFunc)faery_frame_received_callback, pipeline_data);

    timestamp_prefix_log();
    g_print("Event camera source set up (PID: %d).\n",
            (int)pipeline_data->faery_feeder_pid);
    return 0;
}

void cleanup_event_camera_source(PipelineData *pipeline_data) {
    if (pipeline_data->faery_priming_timer_id != 0) {
        g_source_remove(pipeline_data->faery_priming_timer_id);
        pipeline_data->faery_priming_timer_id = 0;
    }

    if (pipeline_data->faery_watch_id != 0) {
        if (!g_source_remove(pipeline_data->faery_watch_id)) {
            timestamp_prefix_err();
            g_printerr("Failed to remove faery channel watch.\n");
        }
        pipeline_data->faery_watch_id = 0;
    }

    if (pipeline_data->faery_channel != NULL) {
        GError *err = NULL;
        g_io_channel_shutdown(pipeline_data->faery_channel, FALSE, &err);
        if (err != NULL) {
            timestamp_prefix_err();
            g_printerr("Error closing faery channel: %s\n", err->message);
            g_error_free(err);
        }
        g_io_channel_unref(pipeline_data->faery_channel);
        pipeline_data->faery_channel = NULL;
    }

    if (pipeline_data->faery_feeder_pid != 0) {
        kill(pipeline_data->faery_feeder_pid, SIGTERM);
        // Wait for the process to exit to avoid zombies
        waitpid((pid_t)pipeline_data->faery_feeder_pid, NULL, 0);
        g_spawn_close_pid(pipeline_data->faery_feeder_pid);
        pipeline_data->faery_feeder_pid = 0;
    }

    // Free any partially-accumulated frame data
    if (pipeline_data->faery_partial_buf != NULL) {
        g_free(pipeline_data->faery_partial_buf);
        pipeline_data->faery_partial_buf = NULL;
        pipeline_data->faery_bytes_accumulated = 0;
    }

    timestamp_prefix_log();
    g_print("Event camera source cleaned up.\n");
}
