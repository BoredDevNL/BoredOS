// Copyright (c) 2023-2026 Chris (boreddevnl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.

#ifndef I2C_LPSS_H
#define I2C_LPSS_H

#include <stdint.h>
#include <stdbool.h>
#include "i2c.h"
#include "pci.h"

typedef struct {
    uintptr_t       base;           // MMIO base address (kernel virtual)
    uint64_t        base_phys;      // MMIO physical address
    uint8_t         pci_bus;
    uint8_t         pci_dev;
    uint8_t         pci_fn;
    uint16_t        vendor_id;
    uint16_t        device_id;
    uint32_t        input_clock_hz; // Input clock frequency (typically 133 MHz on Tiger Lake)
    bool            active;
    bool            is_packed;      // True for Tiger Lake 16-bit MMIO
    char            name[32];
    i2c_adapter_t   adapter;
} i2c_lpss_controller_t;


#define LPSS_PRIVATE_CLOCK_GATE  0x200    // Clock gate control
#define LPSS_PRIVATE_RESET       0x204    // Software reset

#define LPSS_CLOCK_GATE_CLK_EN   (1 << 0)   // Enable clock gating
#define LPSS_RESET_RESET_REL     (1 << 0)   // Release reset


int i2c_lpss_init(void);
int i2c_lpss_get_count(void);
i2c_lpss_controller_t* i2c_lpss_get(int index);
i2c_lpss_controller_t* i2c_lpss_get_by_base(uint64_t base_phys);
const aml_i2c_dev_t* i2c_lpss_get_acpi_device(i2c_lpss_controller_t *ctrl);

#endif
