/*
 * build_stamp.h — per-build freshness stamp (Commandment VI).
 */
#pragma once

/* Compile timestamp of build_stamp.c. The build system touches the .c
 * before every compile, so this string changes on every build even when
 * nothing else does — unlike __DATE__ in a file that may not recompile. */
const char *kast_build_stamp(void);
