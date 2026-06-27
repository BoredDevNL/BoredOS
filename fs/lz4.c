// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.

#include "lz4.h"

static inline void lz4_memcpy(void *dest, const void *src, int n) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    for (int i = 0; i < n; i++) {
        d[i] = s[i];
    }
}

/**
 * Decompress a raw LZ4 compressed block.
 */
static int lz4_decompress_block(const uint8_t *src, int src_len, uint8_t *dst, int dst_len) {
    const uint8_t *ip = src;
    const uint8_t *ip_limit = src + src_len;
    uint8_t *op = dst;
    uint8_t *op_limit = dst + dst_len;

    while (ip < ip_limit) {
        uint8_t token = *ip++;
        int literal_len = token >> 4;

        if (literal_len == 15) {
            uint8_t s;
            do {
                if (ip >= ip_limit) return -1; // Out of bounds
                s = *ip++;
                literal_len += s;
            } while (s == 255);
        }

        // Copy literals
        if (literal_len > 0) {
            if (op + literal_len > op_limit || ip + literal_len > ip_limit) {
                return -2; // Destination or source overflow
            }
            lz4_memcpy(op, ip, literal_len);
            op += literal_len;
            ip += literal_len;
        }

        if (ip >= ip_limit) {
            break; // End of block
        }

        // Get match offset (16-bit little-endian)
        if (ip + 2 > ip_limit) return -3;
        uint16_t offset = ip[0] | (ip[1] << 8);
        ip += 2;

        if (offset == 0) {
            return -4; // Invalid offset
        }

        int match_len = token & 0x0F;
        if (match_len == 15) {
            uint8_t s;
            do {
                if (ip >= ip_limit) return -5;
                s = *ip++;
                match_len += s;
            } while (s == 255);
        }
        match_len += 4; // Minimum match length in LZ4 is 4

        // Copy match
        uint8_t *ref = op - offset;
        if (ref < dst || op + match_len > op_limit) {
            return -6; // Invalid back-reference copy
        }
        
        for (int i = 0; i < match_len; i++) {
            *op++ = *ref++;
        }
    }
    return (int)(op - dst);
}

/**
 * Decompress a standard LZ4 frame with magic number 0x184D2204.
 */
int lz4_decompress_frame(const uint8_t *src, int src_len, uint8_t *dst, int dst_len) {
    if (src_len < 7) return -10; // Frame too small
    
    // 1. Magic number check (0x184D2204 in little-endian)
    uint32_t magic = src[0] | (src[1] << 8) | (src[2] << 16) | (src[3] << 24);
    if (magic != 0x184D2204) {
        return -11; // Invalid magic number
    }
    
    int pos = 4;
    
    // 2. FLG and BD bytes
    uint8_t flg = src[pos++];
    uint8_t bd = src[pos++];
    (void)bd;
    
    int version = flg >> 6;
    if (version != 1) return -12; // Only version 1 is supported
    
    int block_checksum = (flg >> 4) & 1;
    int content_size_flag = (flg >> 3) & 1;
    int content_checksum = (flg >> 2) & 1;
    int dict_id_flag = flg & 1;
    
    // Parse content size (8 bytes, little-endian)
    if (content_size_flag) {
        if (pos + 8 > src_len) return -13;
        pos += 8;
    }
    
    // Parse dictionary ID (4 bytes)
    if (dict_id_flag) {
        if (pos + 4 > src_len) return -14;
        pos += 4;
    }
    
    // Skip Header Checksum (1 byte)
    if (pos + 1 > src_len) return -15;
    pos++;
    
    int dst_pos = 0;
    
    // 3. Process blocks sequentially until EndMark (size 0)
    while (pos + 4 <= src_len) {
        uint32_t block_size = src[pos] | (src[pos+1] << 8) | (src[pos+2] << 16) | (src[pos+3] << 24);
        pos += 4;
        
        // End of consecutive blocks
        if (block_size == 0) {
            break;
        }
        
        bool uncompressed = (block_size & 0x80000000) != 0;
        block_size &= 0x7FFFFFFF;
        
        if (pos + (int)block_size > src_len) {
            return -16; // Block size exceeds compressed source buffer
        }
        
        if (uncompressed) {
            if (dst_pos + (int)block_size > dst_len) return -17; // Destination buffer overflow
            lz4_memcpy(dst + dst_pos, src + pos, block_size);
            dst_pos += block_size;
            pos += block_size;
        } else {
            int decomp_bytes = lz4_decompress_block(src + pos, block_size, dst + dst_pos, dst_len - dst_pos);
            if (decomp_bytes < 0) {
                return decomp_bytes; // Decompression failure inside block
            }
            dst_pos += decomp_bytes;
            pos += block_size;
        }
        
        // Skip Block Checksum if present
        if (block_checksum) {
            if (pos + 4 > src_len) return -18;
            pos += 4;
        }
    }
    
    // Optional Content Checksum (4 bytes) at the end of the frame
    if (content_checksum) {
        // Just skip it since we don't calculate validation checksums at boot stage
        pos += 4;
    }
    
    return dst_pos;
}
