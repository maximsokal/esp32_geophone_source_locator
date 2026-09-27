#pragma once

#include <stdlib.h>

#define MALLOC_CAP_SPIRAM 1U
#define MALLOC_CAP_8BIT 2U
#define heap_caps_malloc(size, caps) ((void)(caps), malloc(size))
