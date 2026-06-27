// Copyright (c) 2026 Myles Wilson (myles@bleedkernel.com)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.

#ifndef ACPI_H
#define ACPI_H

//power stuff
#define PM1A_CNT    fadt->pm1a_cnt_blk
#define PM1B_CNT    fadt->pm1b_cnt_blk

#define ACPI_S5    0x5
#define SLP_EN     (1 << 13)

#define ACPI_PM1_SLEEP_CMD(slp_typ)  (((slp_typ) << 10) | SLP_EN)

extern uint16_t SLP_TYPa;
extern uint16_t SLP_TYPb;

int acpi_init(void);

uint32_t acpi_irq_to_gsi(uint32_t irq);
uint16_t acpi_irq_flags(uint32_t irq);

struct acpi_rsdp *acpi_get_rsdp(void);
struct acpi_sdt *acpi_get_sdt(const char signature[4]);
struct acpi_sdt *acpi_get_dsdt(void);

void acpi_parse_s5(void);

// ACPI Functionality Implementations
//  - do reboot properly
__attribute__((noreturn)) void acpi_shutdown(void);

#endif