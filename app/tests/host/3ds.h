#pragma once

// Host stand-in for the libctru types that the platform headers name. HostStubs.cpp gives the
// host versions of the classes that use them.
typedef int LightLock;
typedef struct HostThread* Thread;
