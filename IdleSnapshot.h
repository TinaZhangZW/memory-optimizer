/* SPDX-License-Identifier: GPL-2.0 */
#ifndef IDLE_SNAPSHOT_H
#define IDLE_SNAPSHOT_H

#include <string>

struct IdleSample;

/*
 * Serialize all supplied rows, including invalid observations, without
 * validating or filtering them. Publish a hva/refs/observed TSV with mode
 * 0644, independent of umask or the previous output file's permissions.
 * Atomically replace the output after a successful close. Exceptions leave
 * the old output intact and trigger temporary-file cleanup. Replacement
 * provides atomic visibility, not crash durability; no fsync is performed.
 */
void write_idle_snapshot(const IdleSample& sample,
                         const std::string& output_path);

#endif
