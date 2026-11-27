#pragma once

// Forced into every save test source: MinGW has no POSIX mkdir(path, mode).
#if defined(_WIN32) && defined(__cplusplus)
#include <direct.h>
inline int mkdir(const char* path, int) {
    return _mkdir(path);
}
#endif
