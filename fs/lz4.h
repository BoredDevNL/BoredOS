// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.

#pragma once

#include <stdint.h>
#include <stdbool.h>

/**
 * Decompress a standard LZ4 frame into a destination buffer.
 *
 * @param src      Pointer to the compressed LZ4 frame buffer.
 * @param src_len  The total size of the compressed frame in bytes.
 * @param dst      Pointer to the destination buffer where uncompressed data will be written.
 * @param dst_len  The maximum capacity of the destination buffer.
 * @return The number of bytes written to dst on success, or a negative error code on failure.
 */
int lz4_decompress_frame(const uint8_t *src, int src_len, uint8_t *dst, int dst_len);
