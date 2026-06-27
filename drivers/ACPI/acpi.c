// Copyright (c) 2026 Myles Wilson (myles@bleedkernel.com)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.

#include <stdint.h>
#include <stddef.h>

#include "acpi_structures.h"
#include "acpi_i2c.h"
#include "i2c_lpss.h"
#include "acpi.h"
#include "idt.h"
#include "limine.h"
#include "panic.h"
#include "platform.h"
#include "kconsole.h"

#define MAX_ISO 16

static struct acpi_rsdp *acpi_rsdp = NULL;
static struct acpi_madt *acpi_madt = NULL;

static fadt_t *acpi_fadt = NULL;    // my header file sucks ill make this nicer but its fine

static struct {
    uint8_t     source;
    uint32_t    gsi;
    uint16_t    flags;
} iso_table[MAX_ISO];
static uint8_t iso_count = 0;

/*
    Each ACPI table contains an 8-bit checksum field.
    When all bytes in the table are added together (including the checksum byte), 
    the lower 8 bits of the total sum must be zero for it to pass.
*/
static int acpi_checksum(void *ptr, size_t len) {
    uint8_t sum = 0;
    uint8_t *p = ptr;
    for (size_t i = 0; i < len; i++)
        sum += p[i];
    return sum == 0;
}

struct acpi_rsdp *acpi_get_rsdp(void){
    extern volatile struct limine_rsdp_request acpi_rsdp_request;

    if (!acpi_rsdp_request.response) 
        kernel_panic(NULL, "ACPI RSDP not provided by the Bootloader");
    if (!acpi_rsdp_request.response->address)
        kernel_panic(NULL, "ACPI Invalid RSDP address provided by Bootloader");

    return acpi_rsdp_request.response->address;
}

struct acpi_sdt *acpi_get_sdt(const char signature[4]) {
    if (acpi_rsdp->revision >= 2 && acpi_rsdp->xsdt_address) {
        struct acpi_xsdt *acpi_xsdt = (struct acpi_xsdt *)p2v(acpi_rsdp->xsdt_address);
        if (acpi_checksum(acpi_xsdt, acpi_xsdt->header.length)) {
            size_t entries = (acpi_xsdt->header.length - sizeof(struct acpi_sdt)) / 8;
            for (size_t i = 0; i < entries; i++) {
                struct acpi_sdt *tbl = (struct acpi_sdt *)p2v(acpi_xsdt->tables[i]);
                if (!tbl) continue;
                if (!memcmp(tbl->signature, signature, 4))
                    return tbl;
            }
        }
    }

    // RSDT fallback
    if (!acpi_rsdp->rsdt_address)
        return NULL;

    struct acpi_sdt *acpi_rsdt = (struct acpi_sdt *)p2v(acpi_rsdp->rsdt_address);
    if (!acpi_checksum(acpi_rsdt, acpi_rsdt->length))
        return NULL;

    uint32_t *tables = (uint32_t *)((uint8_t *)acpi_rsdt + sizeof(struct acpi_sdt));
    size_t entries = (acpi_rsdt->length - sizeof(struct acpi_sdt)) / 4;
    for (size_t i = 0; i < entries; i++) {
        struct acpi_sdt *tbl = (struct acpi_sdt *)p2v(tables[i]);
        if (!tbl) continue;
        if (!memcmp(tbl->signature, signature, 4))
            return tbl;
    }

    return NULL;
}

uint16_t SLP_TYPa = 0;
uint16_t SLP_TYPb = 0;

void acpi_parse_s5(void) {
    if (!acpi_fadt || !acpi_fadt->dsdt) return;

    char *dsdt = (char *)p2v((uintptr_t)acpi_fadt->dsdt);

    if (memcmp(dsdt, "DSDT", 4) != 0) return;

    uint32_t dsdt_len = *(uint32_t*)(dsdt + 4);
    char *ptr = dsdt + 36;
    char *end = dsdt + dsdt_len;

    while (ptr < end) {
        if (memcmp(ptr, "_S5_", 4) == 0) {
            ptr += 4;
            if (*ptr == 0x12) {
                ptr += 3;
                
                if (*ptr == 0x0A) ptr++; 
                SLP_TYPa = (*(uint8_t*)ptr) << 10;
                ptr++;

                if (*ptr == 0x0A) ptr++;
                SLP_TYPb = (*(uint8_t*)ptr) << 10;
                
                return;
            }
        }
        ptr++;
    }
}

__attribute__((noreturn))
void acpi_shutdown(void) {
    if (SLP_TYPa == 0) acpi_parse_s5();
    if (SLP_TYPa == 0) SLP_TYPa = (5 << 10); 

    outw((uint16_t)acpi_fadt->pm1a_cnt_blk, SLP_TYPa | SLP_EN);
    
    if (acpi_fadt->pm1b_cnt_blk != 0) {
        outw((uint16_t)acpi_fadt->pm1b_cnt_blk, SLP_TYPb | SLP_EN);
    }

    //virtulizers last just incase these have some sort of unintended effect on real hw
    outw(0xB004, 0x2000);   // bochs
    outw(0x4004, 0x3400);   // vbox
    outw(0x604, 0x2000);    // QEMU
    outw(0x600, 0x34);      // Cloud Hypervisor

    asm volatile("cli");
    for(;;) asm volatile("hlt");
}

int acpi_init(void){
    acpi_rsdp = acpi_get_rsdp();
    if (!acpi_rsdp)
        kernel_panic(NULL, "ACPI does not provide a required RSDP");
    
    size_t rsdp_len = acpi_rsdp->revision >= 2 ? acpi_rsdp->length : 20;
       if (!acpi_checksum(acpi_rsdp, rsdp_len))
        kernel_panic(NULL, "bad RSDP checksum");

    acpi_fadt = (struct acpi_fadt *)acpi_get_sdt("FACP");
    if (!acpi_fadt)
        kernel_panic(NULL, "FADT not found");

    if (!acpi_checksum(acpi_fadt, acpi_fadt->header.length))
        kernel_panic(NULL, "bad FADT checksum");

    if (acpi_fadt->smi_cmd && acpi_fadt->acpi_enable) {
        outb(acpi_fadt->smi_cmd, acpi_fadt->acpi_enable);

        int timeout = 1000000;
        while (!(inw(acpi_fadt->pm1a_cnt_blk) & 1) && timeout-- > 0)
            asm("pause");
        if (timeout <= 0) {
            kernel_panic(NULL, "Enable timeout");
        }
    }

    acpi_madt = (struct acpi_madt *)acpi_get_sdt("APIC");
    if (!acpi_madt)
        kernel_panic(NULL, "MADT not found");

    if (!acpi_checksum(acpi_madt, acpi_madt->header.length))
        kernel_panic(NULL, "bad MADT checksum");

    uint8_t *ptr = acpi_madt->entries;
    uint8_t *end = (uint8_t *)acpi_madt + acpi_madt->header.length;

    while (ptr < end) {
        struct madt_entry_header *h = (void *)ptr;
        if (h->length < sizeof(struct madt_entry_header))
            break;
        if (ptr + h->length > end)
            break;

        switch (h->type) {
            case 2: {
                if (iso_count < MAX_ISO) {
                    struct madt_iso *iso = (void *)ptr;
                    iso_table[iso_count].source = iso->source;
                    iso_table[iso_count].gsi    = iso->gsi;
                    iso_table[iso_count].flags  = iso->flags;
                    iso_count++;
                }
                break;
            }

            default:
                break;
        }

        ptr += h->length;
    }

    acpi_i2c_enumerate();
    i2c_lpss_init();

    return 0;
}


uint32_t acpi_irq_to_gsi(uint32_t irq) {
    for (size_t i = 0; i < iso_count; i++) {
        if (iso_table[i].source == irq)
            return iso_table[i].gsi;
    }
    return irq;
}

uint16_t acpi_irq_flags(uint32_t irq) {
    for (size_t i = 0; i < iso_count; i++) {
        if (iso_table[i].source == irq)
            return iso_table[i].flags;
    }
    return 0;
}


struct acpi_sdt *acpi_get_dsdt(void) {
    if (!acpi_fadt) return NULL;
    
    // Check 64-bit X_DSDT first (FADT revision >= 2 usually)
    if (acpi_fadt->header.length >= 148 && acpi_fadt->x_dsdt) {
        return (struct acpi_sdt *)p2v(acpi_fadt->x_dsdt);
    }
    
    // Fallback to 32-bit DSDT
    if (acpi_fadt->dsdt) {
        return (struct acpi_sdt *)p2v((uintptr_t)acpi_fadt->dsdt);
    }
    
    return NULL;
}