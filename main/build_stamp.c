/*
 * build_stamp.c — see build_stamp.h. Rebuilt on every compile by a CMake
 * touch step in main/CMakeLists.txt; keep this translation unit tiny.
 */
#include "build_stamp.h"

const char *kast_build_stamp(void)
{
    return __DATE__ " " __TIME__;
}
