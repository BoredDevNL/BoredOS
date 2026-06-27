// Copyright (c) 2023-2026 Chris (boreddevnl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.

#ifndef I2C_H
#define I2C_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "errno.h"
#include "acpi_aml.h"

#define I2C_M_RD      (1 << 0)
#define I2C_M_TEN     (1 << 1)
#define I2C_M_NOSTART (1 << 2)

typedef enum {
    I2C_SPEED_STANDARD = 100000,
    I2C_SPEED_FAST = 400000,
    I2C_SPEED_FAST_PLUS = 1000000,
    I2C_SPEED_HIGH = 3400000
} i2c_speed_t;

typedef struct {
    uint16_t addr;     // 7-bit or 10-bit slave address
    uint16_t flags;    // I2C_M_* flags
    uint16_t len;      // number of bytes in buffer
    uint8_t *buf;      // data buffer
} i2c_msg_t;

struct i2c_adapter;
typedef int (*i2c_master_xfer_fn)(struct i2c_adapter *adapter, i2c_msg_t *msgs, int num);

typedef struct i2c_adapter {
    const char *name;
    const aml_i2c_dev_t *acpi_dev;
    void *priv;
    i2c_master_xfer_fn master_xfer;
    bool active;
} i2c_adapter_t;

int i2c_adapter_register(i2c_adapter_t *adapter);
int i2c_adapter_unregister(i2c_adapter_t *adapter);
size_t i2c_adapter_count(void);
i2c_adapter_t *i2c_adapter_get(size_t index);
i2c_adapter_t *i2c_adapter_find_by_acpi(const char *hid);
i2c_adapter_t *i2c_adapter_find_by_name(const char *name);
int i2c_master_xfer(i2c_adapter_t *adapter, i2c_msg_t *msgs, int num);

#endif // I2C_H
