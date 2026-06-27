// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#include "ahci.h"
#include "pci.h"
#include "disk.h"
#include "memory_manager.h"
#include "paging.h"
#include "io.h"
#include <stddef.h>
#include "spinlock.h"
#include "kutils.h"


extern void serial_write(const char *str);
extern void serial_write_num(uint64_t num);
extern void serial_write_hex(uint32_t val);

// ============================================================================
// AHCI Driver State
// ============================================================================

static HBA_MEM *abar = NULL;               // MMIO-mapped AHCI Base Address
static bool ahci_initialized = false;
static int active_port_count = 0;

#define MAX_AHCI_PORTS 32

typedef struct {
    bool active;
    int port_num;
    HBA_PORT *port;
    HBA_CMD_HEADER *cmd_list;   // 1KB, 1KB aligned
    void *fis_base;             // 256B, 256B aligned
    HBA_CMD_TBL *cmd_tbl;      // Command table for slot 0
    spinlock_t lock;           // Port-level lock for thread-safety
} ahci_port_state_t;

static ahci_port_state_t ports[MAX_AHCI_PORTS];

// ============================================================================
// String Helpers
// ============================================================================

static void ahci_strcpy(char *d, const char *s) {
    while ((*d++ = *s++));
}

// Kernel virtual to physical address conversion
extern uint64_t v2p(uint64_t vaddr);
extern uint64_t p2v(uint64_t paddr);
static int ahci_disk_sync(Disk *disk);
static int ahci_find_free_slot(HBA_PORT *port);

static void ahci_stop_cmd(HBA_PORT *port) {
    // Clear ST (Start)
    port->cmd &= ~HBA_PORT_CMD_ST;

    // Clear FRE (FIS Receive Enable)
    port->cmd &= ~HBA_PORT_CMD_FRE;

    // Wait until FR and CR clear
    int timeout = 500000;
    while (timeout-- > 0) {
        if (port->cmd & HBA_PORT_CMD_FR) continue;
        if (port->cmd & HBA_PORT_CMD_CR) continue;
        break;
    }
}

static void ahci_start_cmd(HBA_PORT *port) {
    // Wait until CR clears
    while (port->cmd & HBA_PORT_CMD_CR);

    // Set FRE and ST
    port->cmd |= HBA_PORT_CMD_FRE;
    port->cmd |= HBA_PORT_CMD_ST;
}

static int ahci_check_port_type(HBA_PORT *port) {
    uint32_t ssts = port->ssts;
    uint8_t ipm = (ssts >> 8) & 0x0F;
    uint8_t det = ssts & 0x0F;

    if (det != 3) return -1;   // No device detected
    if (ipm != 1) return -1;   // Not in active state

    switch (port->sig) {
        case SATA_SIG_ATA:   return 0;   // SATA drive
        case SATA_SIG_ATAPI: return 1;   // SATAPI drive
        case SATA_SIG_SEMB:  return 2;   // SEMB
        case SATA_SIG_PM:    return 3;   // Port multiplier
        default:             return -1;
    }
}

static void ahci_port_rebase(ahci_port_state_t *ps) {
    HBA_PORT *port = ps->port;

    ahci_stop_cmd(port);

    // Allocate command list (1KB, 1024-byte aligned)
    ps->cmd_list = (HBA_CMD_HEADER*)kmalloc_aligned(1024, 1024);
    if (!ps->cmd_list) return;
    memset(ps->cmd_list, 0, 1024);

    uint64_t clb_phys = v2p((uint64_t)ps->cmd_list);
    port->clb = (uint32_t)(clb_phys & 0xFFFFFFFF);
    port->clbu = (uint32_t)(clb_phys >> 32);

    // Allocate FIS receive area (256 bytes, 256-byte aligned)
    ps->fis_base = kmalloc_aligned(256, 256);
    if (!ps->fis_base) return;
    memset(ps->fis_base, 0, 256);

    uint64_t fb_phys = v2p((uint64_t)ps->fis_base);
    port->fb = (uint32_t)(fb_phys & 0xFFFFFFFF);
    port->fbu = (uint32_t)(fb_phys >> 32);

    int cmd_tbl_size = sizeof(HBA_CMD_TBL) + 32 * sizeof(HBA_PRDT_ENTRY);
    ps->cmd_tbl = (HBA_CMD_TBL*)kmalloc_aligned(cmd_tbl_size, 256);
    if (!ps->cmd_tbl) return;
    memset(ps->cmd_tbl, 0, cmd_tbl_size);

    uint64_t ctba_phys = v2p((uint64_t)ps->cmd_tbl);
    for (int i = 0; i < 32; i++) {
        ps->cmd_list[i].ctba = (uint32_t)(ctba_phys & 0xFFFFFFFF);
        ps->cmd_list[i].ctbau = (uint32_t)(ctba_phys >> 32);
        ps->cmd_list[i].prdtl = 1;  
    }

    // Clear error and interrupt status
    port->serr = 0xFFFFFFFF;
    port->is = 0xFFFFFFFF;

    ahci_start_cmd(port);
}

static int ahci_find_free_slot(HBA_PORT *port) {
    uint32_t slots = (port->sact | port->ci);
    for (int i = 0; i < 32; i++) {
        if (!(slots & (1 << i))) return i;
    }
    return -1;
}

static int ahci_identify(int port_num, uint32_t *sectors, char *model) {
    ahci_port_state_t *ps = &ports[port_num];
    HBA_PORT *port = ps->port;

    uint64_t rflags = spinlock_acquire_irqsave(&ps->lock);
    port->is = 0xFFFFFFFF;
    int slot = ahci_find_free_slot(port);
    if (slot < 0) { spinlock_release_irqrestore(&ps->lock, rflags); return -1; }

    HBA_CMD_HEADER *cmd_hdr = &ps->cmd_list[slot];
    cmd_hdr->cfl = sizeof(FIS_REG_H2D) / sizeof(uint32_t);
    cmd_hdr->w = 0;
    cmd_hdr->prdtl = 1;

    HBA_CMD_TBL *cmd_tbl = ps->cmd_tbl;
    memset(cmd_tbl, 0, sizeof(HBA_CMD_TBL) + 32 * sizeof(HBA_PRDT_ENTRY));

    uint16_t *buf = (uint16_t*)kmalloc_aligned(512, 512);
    uint64_t phys = v2p((uint64_t)buf);

    cmd_tbl->prdt[0].dba = (uint32_t)(phys & 0xFFFFFFFF);
    cmd_tbl->prdt[0].dbau = (uint32_t)(phys >> 32);
    cmd_tbl->prdt[0].dbc = 511; // 512 bytes
    cmd_tbl->prdt[0].i = 1;

    FIS_REG_H2D *fis = (FIS_REG_H2D*)(&cmd_tbl->cfis);
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->c = 1;  // Command
    fis->command = 0xEC; // IDENTIFY DEVICE

    // Wait for port to be idle
    int timeout = 1000000;
    while ((port->tfd & (ATA_SR_BSY | ATA_SR_DRQ)) && --timeout > 0);
    if (timeout <= 0) { kfree(buf); spinlock_release_irqrestore(&ps->lock, rflags); return -1; }

    port->ci = (1 << slot);

    while (1) {
        if ((port->ci & (1 << slot)) == 0) break;
        if (port->is & HBA_PORT_IS_TFES) { kfree(buf); spinlock_release_irqrestore(&ps->lock, rflags); return -1; }
    }

    // Extract sectors (28-bit LBA for now, or 48-bit if supported)
    uint32_t s28 = *((uint32_t*)&buf[60]);
    uint64_t s48 = *((uint64_t*)&buf[100]);

    if (s48 > 0) *sectors = (uint32_t)s48;
    else *sectors = s28;

    // Extract model name (Words 27-46, 40 bytes, big-endian shorts)
    for (int i = 0; i < 20; i++) {
        model[i*2] = (char)(buf[27+i] >> 8);
        model[i*2+1] = (char)(buf[27+i] & 0xFF);
    }
    model[40] = 0;

    // Swap bytes in model string (ATA strings are byte-swapped)
    for (int i = 0; i < 40; i += 2) {
        char tmp = model[i];
        model[i] = model[i+1];
        model[i+1] = tmp;
    }

    kfree(buf);
    spinlock_release_irqrestore(&ps->lock, rflags);
    return 0;
}



int ahci_read_sectors(int port_num, uint64_t lba, uint32_t count, uint8_t *buffer) {
    if (!ahci_initialized || port_num < 0 || port_num >= MAX_AHCI_PORTS) return -1;
    ahci_port_state_t *ps = &ports[port_num];
    if (!ps->active) return -1;

    uint64_t rflags = spinlock_acquire_irqsave(&ps->lock);
    HBA_PORT *port = ps->port;

    // Clear any pending interrupts/errors
    port->is = 0xFFFFFFFF;

    int slot = ahci_find_free_slot(port);
    if (slot < 0) return -1;

    HBA_CMD_HEADER *cmd_hdr = &ps->cmd_list[slot];
    cmd_hdr->cfl = sizeof(FIS_REG_H2D) / sizeof(uint32_t);
    cmd_hdr->w = 0;   // Read
    cmd_hdr->prdtl = 1;

    HBA_CMD_TBL *cmd_tbl = ps->cmd_tbl;
    memset(cmd_tbl, 0, sizeof(HBA_CMD_TBL) + 32 * sizeof(HBA_PRDT_ENTRY));

    extern uint64_t paging_get_pml4_phys(void);
    extern uint64_t paging_virt2phys(uint64_t pml4_phys, uint64_t virtual_addr);
    uint64_t pml4 = paging_get_pml4_phys();
    uint64_t buf_addr = (uint64_t)buffer;
    uint32_t remaining = count * 512;
    int prd_idx = 0;

    while (remaining > 0 && prd_idx < 32) {
        uint64_t phys = paging_virt2phys(pml4, buf_addr);
        if (!phys) {
            spinlock_release_irqrestore(&ps->lock, rflags);
            return -1;
        }

        uint32_t offset = buf_addr & 0xFFF;
        uint32_t can_do = 4096 - offset;
        if (can_do > remaining) can_do = remaining;

        cmd_tbl->prdt[prd_idx].dba = (uint32_t)(phys & 0xFFFFFFFF);
        cmd_tbl->prdt[prd_idx].dbau = (uint32_t)(phys >> 32);
        cmd_tbl->prdt[prd_idx].dbc = can_do - 1; // 0-based
        cmd_tbl->prdt[prd_idx].i = 0;

        buf_addr += can_do;
        remaining -= can_do;
        prd_idx++;
    }
    
    if (prd_idx > 0) cmd_tbl->prdt[prd_idx - 1].i = 1; // Interrupt on last
    cmd_hdr->prdtl = prd_idx;

    // Setup Command FIS
    FIS_REG_H2D *fis = (FIS_REG_H2D*)&cmd_tbl->cfis;
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->c = 1;  // Command
    fis->command = ATA_CMD_READ_DMA_EX;

    fis->lba0 = (uint8_t)(lba);
    fis->lba1 = (uint8_t)(lba >> 8);
    fis->lba2 = (uint8_t)(lba >> 16);
    fis->device = 1 << 6;  // LBA mode
    fis->lba3 = (uint8_t)(lba >> 24);
    fis->lba4 = (uint8_t)(lba >> 32);
    fis->lba5 = (uint8_t)(lba >> 40);

    fis->countl = (uint8_t)(count);
    fis->counth = (uint8_t)(count >> 8);

    // Issue command
    port->ci = (1 << slot);

    // Wait for completion
    int timeout = 1000000;
    while (timeout-- > 0) {
        if (!(port->ci & (1 << slot))) break;
        if (port->is & (1 << 30)) {  // Task File Error
            serial_write("\n");
            spinlock_release_irqrestore(&ps->lock, rflags);
            return -1;
        }
    }

    if (timeout <= 0) {
        serial_write("[AHCI] Read timeout on port ");
        serial_write_num(port_num);
        serial_write("\n");
        spinlock_release_irqrestore(&ps->lock, rflags);
        return -1;
    }

    spinlock_release_irqrestore(&ps->lock, rflags);
    return 0;
}

int ahci_write_sectors(int port_num, uint64_t lba, uint32_t count, const uint8_t *buffer) {
    if (!ahci_initialized || port_num < 0 || port_num >= MAX_AHCI_PORTS) return -1;
    ahci_port_state_t *ps = &ports[port_num];
    if (!ps->active) return -1;

    uint64_t rflags = spinlock_acquire_irqsave(&ps->lock);
    HBA_PORT *port = ps->port;

    port->is = 0xFFFFFFFF;

    int slot = ahci_find_free_slot(port);
    if (slot < 0) return -1;

    HBA_CMD_HEADER *cmd_hdr = &ps->cmd_list[slot];
    cmd_hdr->cfl = sizeof(FIS_REG_H2D) / sizeof(uint32_t);
    cmd_hdr->w = 1;   // Write
    cmd_hdr->prdtl = 1;

    HBA_CMD_TBL *cmd_tbl = ps->cmd_tbl;
    memset(cmd_tbl, 0, sizeof(HBA_CMD_TBL) + 32 * sizeof(HBA_PRDT_ENTRY));

    // Setup PRDT - handle buffers spanning multiple physical pages
    extern uint64_t paging_get_pml4_phys(void);
    extern uint64_t paging_virt2phys(uint64_t pml4_phys, uint64_t virtual_addr);
    uint64_t pml4 = paging_get_pml4_phys();
    uint64_t buf_addr = (uint64_t)buffer;
    uint32_t remaining = count * 512;
    int prd_idx = 0;

    while (remaining > 0 && prd_idx < 32) {
        uint64_t phys = paging_virt2phys(pml4, buf_addr);
        if (!phys) {
            spinlock_release_irqrestore(&ps->lock, rflags);
            return -1;
        }

        uint32_t offset = buf_addr & 0xFFF;
        uint32_t can_do = 4096 - offset;
        if (can_do > remaining) can_do = remaining;

        cmd_tbl->prdt[prd_idx].dba = (uint32_t)(phys & 0xFFFFFFFF);
        cmd_tbl->prdt[prd_idx].dbau = (uint32_t)(phys >> 32);
        cmd_tbl->prdt[prd_idx].dbc = can_do - 1; // 0-based
        cmd_tbl->prdt[prd_idx].i = 0;

        buf_addr += can_do;
        remaining -= can_do;
        prd_idx++;
    }

    if (prd_idx > 0) cmd_tbl->prdt[prd_idx - 1].i = 1; // Interrupt on last
    cmd_hdr->prdtl = prd_idx;

    FIS_REG_H2D *fis = (FIS_REG_H2D*)&cmd_tbl->cfis;
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->c = 1;
    fis->command = ATA_CMD_WRITE_DMA_EX;

    fis->lba0 = (uint8_t)(lba);
    fis->lba1 = (uint8_t)(lba >> 8);
    fis->lba2 = (uint8_t)(lba >> 16);
    fis->device = 1 << 6;
    fis->lba3 = (uint8_t)(lba >> 24);
    fis->lba4 = (uint8_t)(lba >> 32);
    fis->lba5 = (uint8_t)(lba >> 40);

    fis->countl = (uint8_t)(count);
    fis->counth = (uint8_t)(count >> 8);

    port->ci = (1 << slot);

    int timeout = 1000000;
    while (timeout-- > 0) {
        if (!(port->ci & (1 << slot))) break;
        if (port->is & (1 << 30)) {
            serial_write("[AHCI] Write error on port ");
            serial_write_num(port_num);
            serial_write("\n");
            spinlock_release_irqrestore(&ps->lock, rflags);
            return -1;
        }
    }

    if (timeout <= 0) {
        serial_write("[AHCI] Write timeout on port ");
        serial_write_num(port_num);
        serial_write("\n");
        spinlock_release_irqrestore(&ps->lock, rflags);
        return -1;
    }

    spinlock_release_irqrestore(&ps->lock, rflags);
    return 0;
}

// ============================================================================
// AHCI Disk Integration — wrap AHCI into Disk read/write_sector
// ============================================================================

typedef struct {
    int ahci_port;
} AHCIDriverData;

static int ahci_disk_read_sector(Disk *disk, uint32_t sector, uint8_t *buffer) {
    AHCIDriverData *data = (AHCIDriverData*)disk->driver_data;

    // For partitions, add offset and use parent's port
    if (disk->is_partition && disk->parent) {
        AHCIDriverData *pdata = (AHCIDriverData*)disk->parent->driver_data;
        return ahci_read_sectors(pdata->ahci_port,
                                 (uint64_t)sector + disk->partition_lba_offset, 1, buffer);
    }

    return ahci_read_sectors(data->ahci_port, (uint64_t)sector, 1, buffer);
}

static int ahci_disk_write_sector(Disk *disk, uint32_t sector, const uint8_t *buffer) {
    AHCIDriverData *data = (AHCIDriverData*)disk->driver_data;

    if (disk->is_partition && disk->parent) {
        AHCIDriverData *pdata = (AHCIDriverData*)disk->parent->driver_data;
        return ahci_write_sectors(pdata->ahci_port,
                                  (uint64_t)sector + disk->partition_lba_offset, 1, buffer);
    }

    return ahci_write_sectors(data->ahci_port, (uint64_t)sector, 1, buffer);
}

static int ahci_disk_read_sectors(Disk *disk, uint32_t sector, uint32_t count, uint8_t *buffer) {
    AHCIDriverData *data = (AHCIDriverData*)disk->driver_data;
    if (disk->is_partition && disk->parent) {
        AHCIDriverData *pdata = (AHCIDriverData*)disk->parent->driver_data;
        return ahci_read_sectors(pdata->ahci_port, (uint64_t)sector + disk->partition_lba_offset, count, buffer);
    }
    return ahci_read_sectors(data->ahci_port, (uint64_t)sector, count, buffer);
}

static int ahci_disk_write_sectors(Disk *disk, uint32_t sector, uint32_t count, const uint8_t *buffer) {
    AHCIDriverData *data = (AHCIDriverData*)disk->driver_data;
    if (disk->is_partition && disk->parent) {
        AHCIDriverData *pdata = (AHCIDriverData*)disk->parent->driver_data;
        return ahci_write_sectors(pdata->ahci_port, (uint64_t)sector + disk->partition_lba_offset, count, buffer);
    }
    return ahci_write_sectors(data->ahci_port, (uint64_t)sector, count, buffer);
}

// ============================================================================
// Initialization
// ============================================================================

int ahci_get_port_count(void) {
    return active_port_count;
}

bool ahci_port_is_active(int port_num) {
    if (port_num < 0 || port_num >= MAX_AHCI_PORTS) return false;
    return ports[port_num].active;
}

void ahci_init(void) {
    serial_write("[AHCI] Scanning PCI for AHCI controller...\n");

    // Find AHCI controller (Class 0x01, Subclass 0x06)
    pci_device_t pci_dev;
    if (!pci_find_device_by_class(PCI_CLASS_MASS_STORAGE, PCI_SUBCLASS_SATA, &pci_dev)) {
        serial_write("[AHCI] No AHCI controller found\n");
        return;
    }

    serial_write("[AHCI] Found AHCI controller (");
    serial_write("vendor=0x");
    serial_write_hex(pci_dev.vendor_id);
    serial_write(", device=0x");
    serial_write_hex(pci_dev.device_id);
    serial_write(")\n");

    // Enable Bus Mastering and MMIO
    pci_enable_bus_mastering(&pci_dev);
    pci_enable_mmio(&pci_dev);

    // Read ABAR (BAR5)
    uint32_t abar_raw = pci_get_bar(&pci_dev, 5);
    uint64_t abar_phys = abar_raw & 0xFFFFF000;  // Mask out lower bits

    if (abar_phys == 0) {
        serial_write("[AHCI] Invalid ABAR address\n");
        return;
    }

    serial_write("[AHCI] ABAR physical address: 0x");
    serial_write_hex((uint32_t)abar_phys);
    serial_write("\n");

    uint64_t abar_virt = p2v(abar_phys);
    for (uint64_t offset = 0; offset < 0x2000; offset += 4096) {
        paging_map_page(paging_get_pml4_phys(), abar_virt + offset,
                        abar_phys + offset,
                        PT_PRESENT | PT_RW | PT_CACHE_DISABLE);
    }

    abar = (HBA_MEM*)abar_virt;

    // Enable AHCI mode
    abar->ghc |= (1 << 31);  // AE (AHCI Enable)

    serial_write("[AHCI] Version: ");
    serial_write_num(abar->vs >> 16);
    serial_write(".");
    serial_write_num(abar->vs & 0xFFFF);
    serial_write("\n");

    // Probe ports
    uint32_t pi = abar->pi;
    active_port_count = 0;

    for (int i = 0; i < 32; i++) {
        ports[i].active = false;

        HBA_PORT *port = &abar->ports[i];
        ports[i].lock = SPINLOCK_INIT;
        int type = ahci_check_port_type(port);

        if (type == 0) { // SATA drive
            serial_write("[AHCI] Port ");
            serial_write_num(i);
            serial_write(": SATA drive detected\n");

            ports[i].port_num = i;
            ports[i].port = port;
            ahci_port_rebase(&ports[i]);
            ports[i].active = true;
            active_port_count++;
            ahci_initialized = true;

            // Register as a block device
            Disk *disk = (Disk*)kmalloc(sizeof(Disk));
            if (disk) {
                AHCIDriverData *drv = (AHCIDriverData*)kmalloc(sizeof(AHCIDriverData));
                drv->ahci_port = i;

                disk->devname[0] = 0; // Auto-assign
                disk->type = DISK_TYPE_SATA;
                
                uint32_t sectors = 0;
                char model[64];
    if (ahci_identify(i, &sectors, model) == 0) {
        ahci_strcpy(disk->label, model);
        disk->total_sectors = sectors;
    } else {
        ahci_strcpy(disk->label, "SATA Drive");
        disk->total_sectors = 0;
    }

    disk->read_sector = ahci_disk_read_sector;
    disk->write_sector = ahci_disk_write_sector;
    disk->read_sectors = ahci_disk_read_sectors;
    disk->write_sectors = ahci_disk_write_sectors;
    disk->sync = ahci_disk_sync;
    disk->driver_data = drv;
                disk->partition_lba_offset = 0;
                disk->parent = NULL;
                disk->is_partition = false;
                disk->is_fat32 = false;

                disk_register(disk);

                extern void serial_write(const char *str);
                extern int disk_rescan(Disk *disk);
                serial_write("[AHCI] Probing partitions on /dev/");
                serial_write(disk->devname);
                serial_write("...\n");
                disk_rescan(disk);
            }
        } else if (type == 1) {
            serial_write("[AHCI] Port ");
            serial_write_num(i);
            serial_write(": SATAPI drive (ignored)\n");
        }
    }

    if (active_port_count > 0) {
        ahci_initialized = true;
        serial_write("[AHCI] Initialization complete: ");
        serial_write_num(active_port_count);
        serial_write(" SATA port(s) active\n");
    } else {
        serial_write("[AHCI] No active SATA ports found\n");
    }
}

int ahci_flush_cache(int port_num) {
    HBA_PORT *port = &abar->ports[port_num];
    ahci_port_state_t *ps = &ports[port_num];

    if (port_num < 0 || port_num >= 32 || !ps->active) return -1;

    uint64_t rflags = spinlock_acquire_irqsave(&ps->lock);

    port->is = 0xFFFFFFFF; // Clear interrupts
    int slot = ahci_find_free_slot(port);
    if (slot == -1) { spinlock_release_irqrestore(&ps->lock, rflags); return -1; }

    HBA_CMD_HEADER *cmd_header = (HBA_CMD_HEADER*)p2v(port->clb);
    cmd_header += slot;
    cmd_header->cfl = sizeof(FIS_REG_H2D) / 4;
    cmd_header->w = 0;
    cmd_header->prdtl = 0;

    HBA_CMD_TBL *cmd_tbl = (HBA_CMD_TBL*)p2v(cmd_header->ctba);
    for (int i = 0; i < 256; i++) ((uint8_t*)cmd_tbl)[i] = 0;

    FIS_REG_H2D *fis = (FIS_REG_H2D*)(&cmd_tbl->cfis);
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->c = 1;
    fis->command = 0xEA; // FLUSH CACHE EXT

    // Wait for port to be ready
    int timeout = 1000000;
    while ((port->tfd & (ATA_SR_BSY | ATA_SR_DRQ)) && --timeout > 0);
    if (timeout == 0) { spinlock_release_irqrestore(&ps->lock, rflags); return -1; }

    port->ci = (1 << slot);

    while (1) {
        if ((port->ci & (1 << slot)) == 0) break;
        if (port->is & HBA_PORT_IS_TFES) { spinlock_release_irqrestore(&ps->lock, rflags); return -1; }
    }

    spinlock_release_irqrestore(&ps->lock, rflags);
    return 0;
}

static int ahci_disk_sync(Disk *disk) {
    AHCIDriverData *drv = (AHCIDriverData*)disk->driver_data;
    if (!drv) return -1;
    return ahci_flush_cache(drv->ahci_port);
}

