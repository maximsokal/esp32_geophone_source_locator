#pragma once

#ifdef _WIN32
#include <direct.h>
#define rmdir _rmdir
#else
#include_next <unistd.h>
#endif
