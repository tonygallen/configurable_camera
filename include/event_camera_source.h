#ifndef EVENT_CAMERA_SOURCE_H
#define EVENT_CAMERA_SOURCE_H

#include "pipeline.h"

// Initialise event camera source: spawns the faery frame feeder subprocess
// and sets up the GIOChannel to push frames into appsrc.
// Returns 0 on success, -1 on failure.
int setup_event_camera_source(PipelineData *pipeline_data);

// Cleanup: kill feeder subprocess and remove GIOChannel watch.
void cleanup_event_camera_source(PipelineData *pipeline_data);

#endif
