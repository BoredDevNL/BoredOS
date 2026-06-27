// Copyright (c) 2026 Myles Wilson (myles@bleedkernel.com)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.

#ifndef ACPI_AML_H
#define ACPI_AML_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define AML_ZERO_OP         0x00
#define AML_ONE_OP          0x01
#define AML_NAME_OP         0x08
#define AML_BYTE_PREFIX     0x0A
#define AML_WORD_PREFIX     0x0B
#define AML_DWORD_PREFIX    0x0C
#define AML_STRING_PREFIX   0x0D
#define AML_QWORD_PREFIX    0x0E
#define AML_SCOPE_OP        0x10
#define AML_BUFFER_OP       0x11
#define AML_PACKAGE_OP      0x12
#define AML_METHOD_OP       0x14
#define AML_EXTOP_PREFIX    0x5B
#define AML_DEVICE_OP       0x82   // always preceded by 0x5B
#define AML_RETURN_OP       0xA4

//  ACPI resource descriptor tags
#define ACPI_RESOURCE_END_TAG       0x79
#define ACPI_LARGE_ITEM             0x80
#define ACPI_LARGE_I2C_SERIAL_BUS   0x8E
#define ACPI_I2C_SERIAL_BUS_TYPE    0x01

//  I2cSerialBusV2 resource descriptor
typedef struct __attribute__((packed)) {
    uint8_t  tag;
    uint16_t length;
    uint8_t  revision_id;
    uint8_t  resource_source_index;
    uint8_t  serial_bus_type;
    uint8_t  general_flags;
    uint16_t type_specific_flags;
    uint8_t  type_specific_revision_id;
    uint16_t type_data_length;
    uint32_t connection_speed;
    uint16_t slave_address;
} aml_i2c_resource_t;

/*
    GUID: 3cdff6f7-4267-4555-ad05-b30a3d8938de, mixed-endian AML layout
    sorry i know its a magic number but tis the way of things
*/
#define ACPI_I2C_HID_DSM_GUID \
    "\xf7\xf6\xdf\x3c\x67\x42\x55\x45\xad\x05\xb3\x0a\x3d\x89\x38\xde"

#define AML_PWR_HAS_PS0   (1 << 0)
#define AML_PWR_HAS_PS3   (1 << 1)
#define AML_PWR_HAS_PR0   (1 << 2)
#define AML_PWR_HAS_PR3   (1 << 3)

#define AML_HID_LEN   9
#define AML_NAME_LEN  5

typedef struct {
    char     name[AML_NAME_LEN];
    char     hid[AML_HID_LEN];
    uint16_t slave_address;
    uint32_t speed_hz;
    uint8_t  ten_bit_addr;
    uint16_t hid_desc_addr;
    uint8_t  has_dsm;
    uint8_t  power_flags;
    uint8_t  valid;
} aml_i2c_dev_t;

typedef struct {
    aml_i2c_dev_t *devices;
    size_t         capacity;
    size_t         count;
} aml_walk_ctx_t;

/// @brief Walk one DSDT or SSDT AML region, emitting I2C device records
/// @param aml first AML byte (SDT base + 36)
/// @param len byte length of the AML region
/// @param ctx output context; devices array must be pre-allocated
void aml_find_i2c_controllers(const uint8_t *aml, size_t len);
void aml_walk_table(const uint8_t *aml, size_t len, aml_walk_ctx_t *ctx);

bool is_touchpad_hid(const char *hid);

/// @brief Scan DSDT AML for _S5_ and extract SLP_TYPa/b for S5 power-off
/// @param aml first AML byte (DSDT base + 36)
/// @param len byte length of the AML region
/// @param slp_typa out - SLP_TYPa pre-shifted to PM1_CNT bit 10
/// @param slp_typb out - SLP_TYPb pre-shifted to PM1_CNT bit 10
/// @return 1 if _S5_ found, 0 if not found (caller should use fallback)
int aml_parse_s5(const uint8_t *aml, size_t len,
                 uint16_t *slp_typa, uint16_t *slp_typb);

#endif
