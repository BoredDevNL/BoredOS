// Copyright (c) 2026 Myles Wilson (myles@bleedkernel.com)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.

#ifndef ACPI_I2C_H
#define ACPI_I2C_H

#include <stdint.h>
#include <stddef.h>
#include "acpi_aml.h"

#define ACPI_I2C_MAX_DEVICES  32

int acpi_i2c_enumerate(void);

size_t acpi_i2c_count(void);

const aml_i2c_dev_t *acpi_i2c_get(size_t index);

#endif