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

    guint8 *buf_data = g_malloc(frame_size);
    if (buf_data == NULL) {
        timestamp_prefix_err();
        g_printerr("Failed to allocate frame buffer for faery frame.\n");
        return G_SOURCE_CONTINUE;
    }

    // Read exactly frame_size bytes from the pipe fd
    gint fd = g_io_channel_unix_get_fd(source);
    gsize bytes_read = 0;
    while (bytes_read < frame_size) {
        gssize n = read(fd, buf_data + bytes_read, frame_size - bytes_read);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            timestamp_prefix_err();
            g_printerr("Error reading from faery channel: %s\n", strerror(errno));
            g_free(buf_data);
            return G_SOURCE_CONTINUE;
        }
        if (n == 0) {
            // EOF — feeder subprocess closed its stdout
            timestamp_prefix_err();
            g_printerr("Faery frame feeder closed unexpectedly.\n");
            g_free(buf_data);
            return G_SOURCE_REMOVE;
        }
        bytes_read += n;
    }

    // Wrap the raw pixel data in a GstBuffer (takes ownership of buf_data)
    GstBuffer *gst_buf = gst_buffer_new_wrapped(buf_data, frame_size);
    if (gst_buf == NULL) {
        timestamp_prefix_err();
        g_printerr("Failed to create GstBuffer for faery frame.\n");
        g_free(buf_data);
        return G_SOURCE_CONTINUE;
    }

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

    pipeline_data->faery_pts      = 0;
    pipeline_data->faery_watch_id = g_io_add_watch(
        pipeline_data->faery_channel, G_IO_IN,
        (GIOFunc)faery_frame_received_callback, pipeline_data);

    timestamp_prefix_log();
    g_print("Event camera source set up (PID: %d).\n",
            (int)pipeline_data->faery_feeder_pid);
    return 0;
}

void cleanup_event_camera_source(PipelineData *pipeline_data) {
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

    timestamp_prefix_log();
    g_print("Event camera source cleaned up.\n");
}
