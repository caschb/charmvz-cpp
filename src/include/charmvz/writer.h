/**
 * @file writer.h
 * @brief Functions for writing timeline data to output
 */

#ifndef CHARMVZ_WRITER_H
#define CHARMVZ_WRITER_H

#include "charmvz/timeline.h"

/**
 * @brief Write a timeline to output
 * @param timeline The Timeline to write
 */
void write_timeline(const Timeline &timeline);

#endif
