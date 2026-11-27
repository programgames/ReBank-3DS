#pragma once

// Host build of PKSM-Core for compat_tests: data comes from ../romfs (tests run from app/tests),
// and MinGW has no POSIX getline.
#include <cstdio>
#include <cstdlib>

inline long hostGetline(char** line, std::size_t* size, std::FILE* file) {
    std::size_t length = 0;
    for (int c = std::fgetc(file); c != EOF; c = std::fgetc(file)) {
        if (length + 2 > *size) {
            *size = *size * 2 + 128;
            *line = static_cast<char*>(std::realloc(*line, *size));
        }
        (*line)[length++] = static_cast<char>(c);
        if (c == '\n') {
            break;
        }
    }
    if (length == 0) {
        return -1;
    }
    (*line)[length] = '\0';
    return static_cast<long>(length);
}

#define _PKSMCORE_LANG_FOLDER "../romfs/assets/strings/"
#define _PKSMCORE_PERSONAL_FOLDER "../romfs/assets/personal/"
#define _PKSMCORE_GETLINE_FUNC hostGetline
#define _PKSMCORE_DISABLE_THREAD_SAFETY
