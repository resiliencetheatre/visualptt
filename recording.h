#ifndef VISUALPTT_RECORDING_H
#define VISUALPTT_RECORDING_H
#include <stddef.h>
/* Caller owns the exclusively reserved descriptor; path is a non-final .part. */
int recording_reserve(char *name, size_t namesize, char *path, size_t pathsize);
/* Copy/fsync into destination then rename without replacement. Keep source on error. */
int recording_publish(const char *source, const char *name, const char *directory);
/* Strict legacy/new/origin-prefixed basename parser. Returns timestamp offset or -1. */
int recording_timestamp_offset(const char *name);
#endif
