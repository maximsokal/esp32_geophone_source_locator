#pragma once

#ifdef _WIN32

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

struct dirent {
    char d_name[MAX_PATH];
};

typedef struct {
    HANDLE handle;
    WIN32_FIND_DATAA data;
    int first_entry;
    struct dirent entry;
} DIR;

static inline DIR *opendir(const char *path)
{
    if (path == NULL) {
        return NULL;
    }
    DIR *directory = calloc(1U, sizeof(*directory));
    if (directory == NULL) {
        return NULL;
    }
    char pattern[MAX_PATH];
    const int written = snprintf(pattern, sizeof(pattern), "%s\\*", path);
    if (written < 0 || written >= (int)sizeof(pattern)) {
        free(directory);
        return NULL;
    }
    directory->handle = FindFirstFileA(pattern, &directory->data);
    if (directory->handle == INVALID_HANDLE_VALUE) {
        free(directory);
        return NULL;
    }
    directory->first_entry = 1;
    return directory;
}

static inline struct dirent *readdir(DIR *directory)
{
    if (directory == NULL) {
        return NULL;
    }
    if (directory->first_entry != 0) {
        directory->first_entry = 0;
    } else if (FindNextFileA(directory->handle, &directory->data) == 0) {
        return NULL;
    }
    (void)snprintf(
        directory->entry.d_name, sizeof(directory->entry.d_name), "%s", directory->data.cFileName
    );
    return &directory->entry;
}

static inline int closedir(DIR *directory)
{
    if (directory == NULL) {
        return -1;
    }
    (void)FindClose(directory->handle);
    free(directory);
    return 0;
}

#else

#include_next <dirent.h>

#endif
