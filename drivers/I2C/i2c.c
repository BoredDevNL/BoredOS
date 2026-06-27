// Copyright (c) 2023-2026 Chris (boreddevnl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.

#include "i2c.h"
#include <string.h>

#define MAX_I2C_ADAPTERS 8

static i2c_adapter_t *g_i2c_adapters[MAX_I2C_ADAPTERS];
static size_t g_i2c_adapter_count = 0;

int i2c_adapter_register(i2c_adapter_t *adapter) {
    if (!adapter || !adapter->master_xfer || !adapter->name) return -EINVAL;
    if (g_i2c_adapter_count >= MAX_I2C_ADAPTERS) return -ENOSPC;
    for (size_t i = 0; i < g_i2c_adapter_count; i++) {
        if (g_i2c_adapters[i] == adapter) return -EEXIST;
        if (g_i2c_adapters[i]->name && adapter->name && strcmp(g_i2c_adapters[i]->name, adapter->name) == 0)
            return -EEXIST;
    }
    g_i2c_adapters[g_i2c_adapter_count++] = adapter;
    adapter->active = true;
    return 0;
}

int i2c_adapter_unregister(i2c_adapter_t *adapter) {
    if (!adapter) return -EINVAL;
    for (size_t i = 0; i < g_i2c_adapter_count; i++) {
        if (g_i2c_adapters[i] == adapter) {
            adapter->active = false;
            for (size_t j = i; j + 1 < g_i2c_adapter_count; j++) {
                g_i2c_adapters[j] = g_i2c_adapters[j + 1];
            }
            g_i2c_adapters[--g_i2c_adapter_count] = NULL;
            return 0;
        }
    }
    return -ENOENT;
}

size_t i2c_adapter_count(void) {
    return g_i2c_adapter_count;
}

i2c_adapter_t *i2c_adapter_get(size_t index) {
    if (index >= g_i2c_adapter_count) return NULL;
    return g_i2c_adapters[index];
}

i2c_adapter_t *i2c_adapter_find_by_acpi(const char *hid) {
    if (!hid) return NULL;
    for (size_t i = 0; i < g_i2c_adapter_count; i++) {
        const aml_i2c_dev_t *dev = g_i2c_adapters[i]->acpi_dev;
        if (!dev || !dev->valid) continue;
        if (memcmp(dev->hid, hid, AML_HID_LEN - 1) == 0) {
            return g_i2c_adapters[i];
        }
    }
    return NULL;
}

i2c_adapter_t *i2c_adapter_find_by_name(const char *name) {
    if (!name) return NULL;
    for (size_t i = 0; i < g_i2c_adapter_count; i++) {
        if (g_i2c_adapters[i]->name && strcmp(g_i2c_adapters[i]->name, name) == 0) {
            return g_i2c_adapters[i];
        }
    }
    return NULL;
}

int i2c_master_xfer(i2c_adapter_t *adapter, i2c_msg_t *msgs, int num) {
    if (!adapter || !msgs || num <= 0) return -EINVAL;
    if (!adapter->active) return -ENODEV;
    if (!adapter->master_xfer) return -ENOSYS;
    return adapter->master_xfer(adapter, msgs, num);
}
