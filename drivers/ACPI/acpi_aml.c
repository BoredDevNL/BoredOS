// Copyright (c) 2026 Myles Wilson (myles@bleedkernel.com)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.

#include "acpi_aml.h"
#include "kutils.h"
#include "kconsole.h"

// Internal helpers for decoding AML 

static uint64_t decode_pkglen(const uint8_t **pp, const uint8_t *end) {
    if (*pp >= end) return 0;
    uint8_t b0 = *(*pp)++;
    uint8_t count = (b0 >> 6);
    if (count == 0) return (b0 & 0x3F);

    uint64_t len = (b0 & 0x0F);
    for (uint8_t i = 0; i < count; i++) {
        if (*pp >= end) break;
        len |= ((uint64_t)*(*pp)++) << (4 + 8 * i);
    }
    return len;
}

static uint64_t decode_integer(const uint8_t **pp, const uint8_t *end) {
    if (*pp >= end) return 0;
    uint8_t op = *(*pp)++;
    switch (op) {
        case AML_ZERO_OP: return 0;
        case AML_ONE_OP:  return 1;
        case AML_BYTE_PREFIX: {
            if (*pp >= end) return 0;
            return *(*pp)++;
        }
        case AML_WORD_PREFIX: {
            if (*pp + 1 >= end) return 0;
            uint16_t v = (uint16_t)(*pp)[0] | ((uint16_t)(*pp)[1] << 8);
            *pp += 2;
            return v;
        }
        case AML_DWORD_PREFIX: {
            if (*pp + 3 >= end) return 0;
            uint32_t v = (uint32_t)(*pp)[0] | ((uint32_t)(*pp)[1] << 8) | 
                         ((uint32_t)(*pp)[2] << 16) | ((uint32_t)(*pp)[3] << 24);
            *pp += 4;
            return v;
        }
        case AML_QWORD_PREFIX: {
            if (*pp + 7 >= end) return 0;
            uint64_t v = 0;
            for (int i = 0; i < 8; i++) v |= ((uint64_t)(*pp)[i] << (8 * i));
            *pp += 8;
            return v;
        }
    }
    return 0;
}

static void decode_nameseg(const uint8_t **pp, const uint8_t *end, char out[AML_NAME_LEN]) {
    if (*pp + 3 >= end) {
        memcpy(out, "    ", 4);
        out[4] = '\0';
        return;
    }
    memcpy(out, *pp, 4);
    out[4] = '\0';
    *pp += 4;
}

static void eisaid_to_str(uint32_t v, char out[AML_HID_LEN]) {
    // Decode EISA ID: compressed 32-bit ID as per ACPI specification.
    
    // Some firmware swaps the bytes, so handle both
    uint32_t id = v;
    if (((id >> 26) & 0x1F) == 0) {
        // Swap bytes
        id = ((v >> 24) & 0xFF) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | ((v << 24) & 0xFF000000);
    }

    out[0] = (char)('@' + ((id >> 26) & 0x1F));
    out[1] = (char)('@' + ((id >> 21) & 0x1F));
    out[2] = (char)('@' + ((id >> 16) & 0x1F));
    
    static const char hex[] = "0123456789ABCDEF";
    out[3] = hex[(id >> 12) & 0xF];
    out[4] = hex[(id >>  8) & 0xF];
    out[5] = hex[(id >>  4) & 0xF];
    out[6] = hex[(id >>  0) & 0xF];
    out[7] = '\0';
}

static void parse_hid(const uint8_t **pp, const uint8_t *end, aml_i2c_dev_t *dev) {
    if (*pp >= end) return;
    uint8_t op = **pp;

    if (op == AML_STRING_PREFIX) {
        (*pp)++;
        const char *s = (const char *)*pp;
        size_t len = 0;
        while (*pp < end && **pp) { len++; (*pp)++; }
        if (*pp < end) (*pp)++;
        
        if (len >= AML_HID_LEN) len = AML_HID_LEN - 1;
        memcpy(dev->hid, s, len);
        dev->hid[len] = '\0';
    } else if (op == AML_DWORD_PREFIX) {
        (*pp)++;
        if (*pp + 3 < end) {
            uint32_t v = (uint32_t)(*pp)[0] | ((uint32_t)(*pp)[1] << 8) | 
                         ((uint32_t)(*pp)[2] << 16) | ((uint32_t)(*pp)[3] << 24);
            eisaid_to_str(v, dev->hid);
            *pp += 4;
        }
    } else {
        (*pp)++;
    }
}

static void parse_crs(const uint8_t **pp, const uint8_t *end, aml_i2c_dev_t *dev) {
    if (*pp >= end || **pp != AML_BUFFER_OP) return;
    (*pp)++;

    const uint8_t *pkg_start = *pp;
    size_t pkglen = decode_pkglen(pp, end);
    if (!pkglen) return;

    const uint8_t *buf_end = pkg_start + pkglen;
    if (buf_end > end) buf_end = end;

    decode_integer(pp, buf_end);   // skip BufferSize 

    const uint8_t *res = *pp;
    while (res < buf_end) {
        uint8_t tag = *res;

        if (tag == ACPI_RESOURCE_END_TAG) break;

        if (tag == ACPI_LARGE_I2C_SERIAL_BUS) {
            uint16_t item_len = (uint16_t)(res[1]) | ((uint16_t)(res[2]) << 8);
            if (res + 3 + item_len > buf_end) break;
            
            // Offset 5 is Serial Bus Type (1 = I2C)
            if (res[5] == ACPI_I2C_SERIAL_BUS_TYPE && item_len >= 15) {
                uint32_t speed = (uint32_t)res[12] | ((uint32_t)res[13] << 8) |
                                 ((uint32_t)res[14] << 16) | ((uint32_t)res[15] << 24);
                uint16_t addr = (uint16_t)res[16] | ((uint16_t)res[17] << 8);
                
                if (addr != 0) {
                    dev->speed_hz = speed;
                    dev->slave_address = addr;
                }
            }
            res += 3 + item_len;
            continue;
        }

        if (tag & 0x80) {
            if (res + 3 > buf_end) break;
            uint16_t len = (uint16_t)(res[1]) | ((uint16_t)(res[2]) << 8);
            res += 3 + len;
        } else {
            res += 1 + (tag & 0x07);
        }
    }
    *pp = buf_end;
}

static void record_power_state(const char name[AML_NAME_LEN], aml_i2c_dev_t *dev) {
    if      (memcmp(name, "_PS0", 4) == 0) dev->power_flags |= AML_PWR_HAS_PS0;
    else if (memcmp(name, "_PS3", 4) == 0) dev->power_flags |= AML_PWR_HAS_PS3;
    else if (memcmp(name, "_PR0", 4) == 0) dev->power_flags |= AML_PWR_HAS_PR0;
    else if (memcmp(name, "_PR3", 4) == 0) dev->power_flags |= AML_PWR_HAS_PR3;
}

bool is_touchpad_hid(const char *hid) {
    if (!hid || !hid[0]) return false;
    if (memcmp(hid, "PNP0C50", 7) == 0) return true; // Generic HID-over-I2C
    if (memcmp(hid, "SYNA", 4) == 0)    return true; // Synaptics
    if (memcmp(hid, "ELAN", 4) == 0)    return true; // Elantech
    if (memcmp(hid, "ALPS", 4) == 0)    return true; // Alps
    return false;
}

static void skip_object(const uint8_t **pp, const uint8_t *end) {
    if (*pp >= end) return;
    uint8_t op = **pp;
    switch (op) {
        case AML_ZERO_OP:
        case AML_ONE_OP:
            (*pp)++; return;
        case AML_BYTE_PREFIX:   (*pp) += 2; return;
        case AML_WORD_PREFIX:   (*pp) += 3; return;
        case AML_DWORD_PREFIX:  (*pp) += 5; return;
        case AML_QWORD_PREFIX:  (*pp) += 9; return;
        case AML_STRING_PREFIX:
            (*pp)++;
            while (*pp < end && **pp) (*pp)++;
            if (*pp < end) (*pp)++;
            return;
        case AML_BUFFER_OP:
        case AML_PACKAGE_OP: {
            (*pp)++;
            const uint8_t *start = *pp;
            size_t len = decode_pkglen(pp, end);
            if (len) *pp = start + len;
            return;
        }
        default:
            (*pp)++;
    }
}

static void scan_device_scope(const uint8_t *p, const uint8_t *end, aml_i2c_dev_t *dev) {
    while (p < end) {
        uint8_t op = *p;

        if (op == AML_NAME_OP) {
            p++;
            char seg[AML_NAME_LEN];
            decode_nameseg(&p, end, seg);

            if (memcmp(seg, "_HID", 4) == 0) {
                parse_hid(&p, end, dev);
            } else if (memcmp(seg, "_CID", 4) == 0) {
                aml_i2c_dev_t tmp = {0};
                parse_hid(&p, end, &tmp);
                if (!dev->hid[0] || memcmp(dev->hid, "MCHP", 4) == 0) {
                    memcpy(dev->hid, tmp.hid, AML_HID_LEN);
                }
            } else if (memcmp(seg, "_CRS", 4) == 0) {
                parse_crs(&p, end, dev);
            } else { 
                record_power_state(seg, dev); 
                skip_object(&p, end); 
            }
            continue;
        }

        if (op == AML_METHOD_OP) {
            p++;
            const uint8_t *method_start = p;
            size_t pkglen = decode_pkglen(&p, end);
            if (!pkglen) break;
            const uint8_t *method_end = method_start + pkglen;
            if (method_end > end) method_end = end;

            // Search the method body for a literal Buffer containing an I2C descriptor
            const uint8_t *scan = p;
            while (scan + 8 < method_end && dev->speed_hz == 0) {
                if (*scan == AML_BUFFER_OP) {
                    const uint8_t *buf_ptr = scan;
                    parse_crs(&buf_ptr, method_end, dev);
                }
                scan++;
            }
            p = method_end;
            continue;
        }

        // fallback: scan for I2C tag in buffers
        if (op == AML_BUFFER_OP && dev->speed_hz == 0) {
            const uint8_t *scan = p;
            parse_crs(&scan, end, dev);
        }

        p++;
    }
}

void aml_find_i2c_controllers(const uint8_t *aml, size_t len) {
    if (!aml || !len) return;
    const uint8_t *p = aml;
    const uint8_t *end = aml + len;

    while (p + 10 < end) {
        // Look for NameOp + "_HID" or similar pattern
        if (*p == AML_NAME_OP) {
            char name[AML_NAME_LEN];
            const uint8_t *scan = p + 1;
            decode_nameseg(&scan, end, name);
            if (memcmp(name, "_HID", 4) == 0) {
                // Check if it matches INTC1040 or INTC1043
                if (scan + 8 < end && scan[0] == AML_STRING_PREFIX) {
                    const char *hid = (const char*)(scan + 1);
                    if (memcmp(hid, "INTC1040", 8) == 0 || memcmp(hid, "INTC1043", 8) == 0) {
                        
                        // Scan for Memory/DWord/QWord resource descriptors
                        const uint8_t *crs = scan;
                        while (crs + 14 < end && crs < scan + 2048) {
                            if (crs[0] == 0x86) {
                                // Memory32Fixed
                            } else if (crs[0] == 0x88) {
                                // DWord Memory
                            } else if (crs[0] == 0x89) {
                                // QWord Memory
                            } else if (crs[0] == 0x8A) {
                                // Extended Resource
                            }
                            crs++;
                        }
                    }
                    // Also look for the Touchpad itself
                    else if (is_touchpad_hid(hid)) {
                    }
                }
            }
        }
        p++;
    }
}

void aml_walk_table(const uint8_t *aml, size_t len, aml_walk_ctx_t *ctx) {
    if (!aml || !len || !ctx || !ctx->devices) return;
    const uint8_t *p   = aml;
    const uint8_t *end = aml + len;

    while (p + 2 < end) {
        uint8_t op = *p;
        if (op == AML_EXTOP_PREFIX && p[1] == AML_DEVICE_OP) {
            p += 2;
            const uint8_t *scope_start = p;
            size_t pkglen = decode_pkglen(&p, end);
            if (!pkglen) { p++; continue; }
            const uint8_t *scope_end = scope_start + pkglen;
            if (scope_end > end) scope_end = end;

            char dev_name[AML_NAME_LEN];
            decode_nameseg(&p, scope_end, dev_name);

            if (ctx->count < ctx->capacity) {
                aml_i2c_dev_t *dev = &ctx->devices[ctx->count];
                memset(dev, 0, sizeof(aml_i2c_dev_t));
                memcpy(dev->name, dev_name, AML_NAME_LEN);
                scan_device_scope(p, scope_end, dev);

                if (dev->hid[0]) {
                    if (dev->speed_hz || is_touchpad_hid(dev->hid) || memcmp(dev->name, "TPD", 3) == 0) {
                        dev->valid = 1;
                        ctx->count++;
                    }
                }

                if (dev->slave_address == 0) {
                    const uint8_t *scan = p;
                    while (scan + 20 < scope_end && dev->slave_address == 0) {
                        if (*scan == ACPI_LARGE_I2C_SERIAL_BUS) {
                            uint16_t len = (uint16_t)(scan[1]) | ((uint16_t)(scan[2]) << 8);
                            if (scan + 3 + len <= scope_end && scan[5] == ACPI_I2C_SERIAL_BUS_TYPE && len >= 15) {
                                dev->speed_hz = (uint32_t)scan[12] | ((uint32_t)scan[13] << 8) |
                                                ((uint32_t)scan[14] << 16) | ((uint32_t)scan[15] << 24);
                                dev->slave_address = (uint16_t)scan[16] | ((uint16_t)scan[17] << 8);
                            }
                        }
                        scan++;
                    }
                }
            }
            p++;
        } else if (op == AML_SCOPE_OP) {
             p++;
             size_t pkglen = decode_pkglen(&p, end);
             // Just move into the scope and keep scanning
             if (!pkglen) p++;
        } else {
            p++;
        }
    }
}

int aml_parse_s5(const uint8_t *aml, size_t len, uint16_t *slp_typa, uint16_t *slp_typb) {
    if (!aml || !len || !slp_typa || !slp_typb) return 0;
    const uint8_t *end = aml + len;
    const uint8_t *p   = aml;

    while (p + 4 < end) {
        if (p[0] == '_' && p[1] == 'S' && p[2] == '5' && p[3] == '_') {
            p += 4;
            int found_pkg = 0;
            for (int skip = 0; skip < 4 && p + skip < end; skip++) {
                if (p[skip] == AML_PACKAGE_OP) { p += skip + 1; found_pkg = 1; break; }
            }
            if (!found_pkg) continue;
            const uint8_t *pkg_start = p;
            size_t pkglen = decode_pkglen(&p, end);
            if (!pkglen) continue;
            const uint8_t *pkg_end = pkg_start + pkglen;
            if (pkg_end > end) pkg_end = end;
            if (p >= pkg_end) continue;
            uint8_t num = *p++;
            if (num < 2) continue;
            uint64_t typa = decode_integer(&p, pkg_end);
            uint64_t typb = decode_integer(&p, pkg_end);
            *slp_typa = (uint16_t)((typa & 0x7) << 10);
            *slp_typb = (uint16_t)((typb & 0x7) << 10);
            return 1;
        }
        p++;
    }
    return 0;
}
