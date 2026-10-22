#pragma once
#include <stddef.h>
#define SOL_SOCKET 1
#define SO_SNDTIMEO 2
int setsockopt(int socket_fd, int level, int option, const void *value, size_t size);
