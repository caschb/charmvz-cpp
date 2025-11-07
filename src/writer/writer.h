/**
 * @file writer.h
 * @brief Functions for writing timeline data to output
 */

#ifndef WRITER_H
#define WRITER_H

#include "src/utils/timeline.h"

/**
 * @brief Write a timeline to output
 * @param timeline The Timeline to write
 */
void write_timeline(const Timeline &timeline);

#endif
