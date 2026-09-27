#pragma once

#include <stdbool.h>

#define SEISMIC_RECORDER_SESSION_ID_SIZE 16U

bool seismic_recorder_is_recording_object(const char *object_id);
