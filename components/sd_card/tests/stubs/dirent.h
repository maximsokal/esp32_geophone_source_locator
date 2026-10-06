#ifndef SD_CARD_PROBE_TEST_DIRENT_H
#define SD_CARD_PROBE_TEST_DIRENT_H

#ifndef _WIN32
#include_next <dirent.h>
#else

struct dirent {
    char d_name[260];
    unsigned char d_type;
};

typedef struct DIR DIR;

DIR *opendir(const char *path);
struct dirent *readdir(DIR *directory);
int closedir(DIR *directory);

#endif

#endif
