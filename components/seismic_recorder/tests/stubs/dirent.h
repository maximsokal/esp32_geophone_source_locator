#pragma once

#ifndef _MSC_VER
#include_next <dirent.h>
#else

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <windows.h>

struct dirent
{
    char d_name[MAX_PATH];
};

typedef struct
{
    HANDLE handle;
    WIN32_FIND_DATAA data;
    bool first;
    struct dirent entry;
} DIR;

static DIR *opendir(const char *path)
{
    if (path == NULL) {
        return NULL;
    }

    char pattern[MAX_PATH];
    const int written = snprintf(pattern, sizeof(pattern), "%s\\*", path);
    if (written < 0 || written >= (int)sizeof(pattern)) {
        return NULL;
    }

    DIR *directory = (DIR *)calloc(1U, sizeof(*directory));
    if (directory == NULL) {
        return NULL;
    }
    directory->handle = FindFirstFileA(pattern, &directory->data);
    if (directory->handle == INVALID_HANDLE_VALUE) {
        free(directory);
        return NULL;
    }
    directory->first = true;
    return directory;
}

static struct dirent *readdir(DIR *directory)
{
    if (directory == NULL) {
        return NULL;
    }

    for (;;) {
        const BOOL available = directory->first
            ? TRUE
            : FindNextFileA(directory->handle, &directory->data);
        directory->first = false;
        if (!available) {
            return NULL;
        }
        if (strcmp(directory->data.cFileName, ".") == 0 ||
            strcmp(directory->data.cFileName, "..") == 0) {
            continue;
        }
        strncpy(directory->entry.d_name, directory->data.cFileName, MAX_PATH - 1U);
        directory->entry.d_name[MAX_PATH - 1U] = '\0';
        return &directory->entry;
    }
}

static int closedir(DIR *directory)
{
    if (directory == NULL) {
        return -1;
    }
    const BOOL closed = FindClose(directory->handle);
    free(directory);
    return closed ? 0 : -1;
}

#endif
