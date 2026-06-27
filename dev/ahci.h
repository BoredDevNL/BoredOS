// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#ifndef AHCI_H
#define AHCI_H

#include <stdint.h>
#include <stdbool.h>

// ============================================================================
// FIS (Frame Information Structure) Types
// ============================================================================

typedef enum {
    FIS_TYPE_REG_H2D   = 0x27,    // Register FIS — Host to Device
    FIS_TYPE_REG_D2H   = 0x34,    // Register FIS — Device to Host
    FIS_TYPE_DMA_ACT   = 0x39,    // DMA Activate FIS
    FIS_TYPE_DMA_SETUP = 0x41,    // DMA Setup FIS
    FIS_TYPE_DATA      = 0x46,    // Data FIS
    FIS_TYPE_BIST      = 0x58,    // BIST Activate FIS
    FIS_TYPE_PIO_SETUP = 0x5F,    // PIO Setup FIS
    FIS_TYPE_DEV_BITS  = 0xA1,    // Set Device Bits FIS
} FIS_TYPE;

// ============================================================================
// HBA Register Structures (MMIO-mapped from ABAR)
// ============================================================================

// Port Registers (one set per port, at ABAR + 0x100 + portno*0x80)
typedef volatile struct {
    uint32_t clb;       // 0x00: Command List Base Address (lower 32 bits)
    uint32_t clbu;      // 0x04: Command List Base Address (upper 32 bits)
    uint32_t fb;        // 0x08: FIS Base Address (lower 32 bits)
    uint32_t fbu;       // 0x0C: FIS Base Address (upper 32 bits)
    uint32_t is;        // 0x10: Interrupt Status
    uint32_t ie;        // 0x14: Interrupt Enable
    uint32_t cmd;       // 0x18: Command and Status
    uint32_t rsv0;      // 0x1C: Reserved
    uint32_t tfd;       // 0x20: Task File Data
    uint32_t sig;       // 0x24: Signature
    uint32_t ssts;      // 0x28: SATA Status (SStatus)
    uint32_t sctl;      // 0x2C: SATA Control (SControl)
    uint32_t serr;      // 0x30: SATA Error (SError)
    uint32_t sact;      // 0x34: SATA Active (SCR3)
    uint32_t ci;        // 0x38: Command Issue
    uint32_t sntf;      // 0x3C: SATA Notification (SCR4)
    uint32_t fbs;       // 0x40: FIS-based Switch Control
    uint32_t rsv1[11];  // 0x44~0x6F
    uint32_t vendor[4]; // 0x70~0x7F
} HBA_PORT;

// Global HBA Memory Registers (at ABAR)
typedef volatile struct {
    uint32_t cap;       // 0x00: Host Capability
    uint32_t ghc;       // 0x04: Global Host Control
    uint32_t is;        // 0x08: Interrupt Status
    uint32_t pi;        // 0x0C: Port Implemented
    uint32_t vs;        // 0x10: Version
    uint32_t ccc_ctl;   // 0x14: Command Completion Coalescing Control
    uint32_t ccc_pts;   // 0x18: Command Completion Coalescing Ports
    uint32_t em_loc;    // 0x1C: Enclosure Management Location
    uint32_t em_ctl;    // 0x20: Enclosure Management Control
    uint32_t cap2;      // 0x24: Host Capabilities Extended
    uint32_t bohc;      // 0x28: BIOS/OS Handoff Control and Status
    uint8_t  rsv[0xA0 - 0x2C];
    uint8_t  vendor[0x100 - 0xA0];
    HBA_PORT ports[];   // Port 0 at offset 0x100 (flexible array member)
} HBA_MEM;

// ============================================================================
// Command List / Table Structures (DMA)
// ============================================================================

// Command Header (32 bytes each, 32 entries per port = 1KB)
typedef struct {
    uint8_t  cfl:5;     // Command FIS Length (in DWORDs)
    uint8_t  a:1;       // ATAPI
    uint8_t  w:1;       // Write (1=H2D, 0=D2H)
    uint8_t  p:1;       // Prefetchable

    uint8_t  r:1;       // Reset
    uint8_t  b:1;       // BIST
    uint8_t  c:1;       // Clear Busy upon R_OK
    uint8_t  rsv0:1;
    uint8_t  pmp:4;     // Port Multiplier Port

    uint16_t prdtl;     // Physical Region Descriptor Table Length (entries)

    volatile uint32_t prdbc;  // PRD Byte Count transferred

    uint32_t ctba;      // Command Table Descriptor Base Address (lower 32)
    uint32_t ctbau;     // Command Table Descriptor Base Address (upper 32)

    uint32_t rsv1[4];   // Reserved
} __attribute__((packed)) HBA_CMD_HEADER;

// Physical Region Descriptor Table Entry
typedef struct {
    uint32_t dba;       // Data Base Address (lower 32)
    uint32_t dbau;      // Data Base Address (upper 32)
    uint32_t rsv0;      // Reserved
    uint32_t dbc:22;    // Byte Count (0-based, max 4MB)
    uint32_t rsv1:9;    // Reserved
    uint32_t i:1;       // Interrupt on Completion
} __attribute__((packed)) HBA_PRDT_ENTRY;

// Host-to-Device Register FIS
typedef struct {
    uint8_t  fis_type;  // FIS_TYPE_REG_H2D
    uint8_t  pmport:4;  // Port Multiplier
    uint8_t  rsv0:3;    // Reserved
    uint8_t  c:1;       // 1=Command, 0=Control
    uint8_t  command;   // Command register
    uint8_t  featurel;  // Feature register (7:0)
    uint8_t  lba0;      // LBA (7:0)
    uint8_t  lba1;      // LBA (15:8)
    uint8_t  lba2;      // LBA (23:16)
    uint8_t  device;    // Device register
    uint8_t  lba3;      // LBA (31:24)
    uint8_t  lba4;      // LBA (39:32)
    uint8_t  lba5;      // LBA (47:40)
    uint8_t  featureh;  // Feature register (15:8)
    uint8_t  countl;    // Count (7:0)
    uint8_t  counth;    // Count (15:8)
    uint8_t  icc;       // Isochronous Command Completion
    uint8_t  control;   // Control register
    uint8_t  rsv1[4];   // Reserved
} __attribute__((packed)) FIS_REG_H2D;

// Command Table (256-byte aligned)
typedef struct {
    uint8_t  cfis[64];      // Command FIS
    uint8_t  acmd[16];      // ATAPI Command
    uint8_t  rsv[48];       // Reserved
    HBA_PRDT_ENTRY prdt[];  // PRDT entries (variable, at least 1)
} __attribute__((packed)) HBA_CMD_TBL;

// ============================================================================
// Port Signature Values
// ============================================================================

#define SATA_SIG_ATA    0x00000101  // SATA drive
#define SATA_SIG_ATAPI  0xEB140101  // SATAPI drive
#define SATA_SIG_SEMB   0xC33C0101  // Enclosure management bridge
#define SATA_SIG_PM     0x96690101  // Port multiplier

// ============================================================================
// Port Command Bits
// ============================================================================

#define HBA_PORT_CMD_ST     0x0001  // Start
#define HBA_PORT_CMD_FRE    0x0010  // FIS Receive Enable
#define HBA_PORT_CMD_FR     0x4000  // FIS Receive Running
#define HBA_PORT_CMD_CR     0x8000  // Command List Running
#define HBA_PORT_IS_TFES    (1u << 30) 

#define ATA_SR_BSY     0x80
#define ATA_SR_DRDY    0x40
#define ATA_SR_DRQ     0x08
#define ATA_SR_ERR     0x01

// ============================================================================
// ATA Commands
// ============================================================================

#define ATA_CMD_READ_DMA_EX  0x25
#define ATA_CMD_WRITE_DMA_EX 0x35
#define ATA_CMD_IDENTIFY     0xEC

// ============================================================================
// Public API
// ============================================================================

void ahci_init(void);
int ahci_read_sectors(int port_num, uint64_t lba, uint32_t count, uint8_t *buffer);
int ahci_write_sectors(int port_num, uint64_t lba, uint32_t count, const uint8_t *buffer);
int ahci_get_port_count(void);
bool ahci_port_is_active(int port_num);

#endif
