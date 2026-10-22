#pragma once
#ifdef _MSC_VER
struct timeval { long tv_sec; long tv_usec; };
#else
#include_next <sys/time.h>
#endif
