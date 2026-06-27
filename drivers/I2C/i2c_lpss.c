// Copyright (c) 2023-2026 Chris (boreddevnl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.

#include "i2c_lpss.h"
#include "i2c_designware.h"
#include "acpi_i2c.h"
#include "pci.h"
#include "platform.h"
#include "memory_manager.h"
#include "paging.h"
#include "kutils.h"
#include <string.h>

extern void serial_write(const char *str);
extern void serial_write_hex(uint32_t val);
extern void serial_write_num(uint64_t num);

#define MAX_LPSS_I2C_CONTROLLERS 8

static i2c_lpss_controller_t controllers[MAX_LPSS_I2C_CONTROLLERS];
static int controller_count = 0;

static const aml_i2c_dev_t *acpi_devices[MAX_LPSS_I2C_CONTROLLERS];

static int i2c_lpss_master_xfer(i2c_adapter_t *adapter, i2c_msg_t *msgs, int num) {
    if (!adapter || !msgs || num <= 0) return -EINVAL;

    i2c_lpss_controller_t *ctrl = (i2c_lpss_controller_t *)adapter->priv;
    if (!ctrl || !ctrl->active) return -ENODEV;

    volatile uint8_t *base = (volatile uint8_t *)ctrl->base;
    int rc = 0;

    for (int i = 0; i < num; i++) {
        i2c_msg_t *msg = &msgs[i];
        if (!msg->buf || msg->len == 0) return -EINVAL;

        bool ten_bit = (msg->flags & I2C_M_TEN) != 0;
        bool read = (msg->flags & I2C_M_RD) != 0;
        bool stop = (i == num - 1);
        bool restart = (i > 0);

        if (ctrl->is_packed) {
            uint32_t con = 0x61;
            if (ten_bit) con |= (1 << 4);
            *(volatile uint32_t *)(ctrl->base + 0x00) = (msg->addr << 16) | con;
        } else {
            dwi2c_set_target_addr(base, msg->addr, ten_bit);
        }

        if (read) {
            for (uint16_t j = 0; j < msg->len; j++) {
                uint32_t cmd = IC_DATA_CMD_CMD_READ;
                if (j == 0 && restart) cmd |= IC_DATA_CMD_RESTART;
                if (j == msg->len - 1 && stop) cmd |= IC_DATA_CMD_STOP;

                if (ctrl->is_packed) {
                    *(volatile uint32_t *)(ctrl->base + 0x04) = cmd;
                    int timeout = 1000000;
                    while (!((*(volatile uint32_t *)(ctrl->base + 0x04) >> 16) & (1 << 3)) && --timeout > 0);
                    if (timeout > 0) {
                        msg->buf[j] = (uint8_t)(*(volatile uint32_t *)(ctrl->base + 0x04) & 0xFF);
                    } else {
                        rc = -ETIMEDOUT;
                        break;
                    }
                } else {
                    dwi2c_write_reg(base, IC_DATA_CMD, cmd);
                    uint32_t abort = dwi2c_read_reg(base, IC_TX_ABRT_SOURCE);
                    if (abort) { rc = -EIO; break; }

                    int timeout = 100000;
                    while (!dwi2c_rx_fifo_not_empty(base) && timeout-- > 0);
                    if (timeout <= 0) { rc = -ETIMEDOUT; break; }
                    msg->buf[j] = (uint8_t)(dwi2c_read_reg(base, IC_DATA_CMD) & 0xFF);
                }
            }
            if (rc < 0) break;
        } else {
            bool next_is_read = false;
            if (i + 1 < num && (msgs[i + 1].flags & I2C_M_RD))
                next_is_read = true;

            for (uint16_t j = 0; j < msg->len; j++) {
                uint32_t cmd = IC_DATA_CMD_DAT(msg->buf[j]) | IC_DATA_CMD_CMD_WRITE;
                if (j == msg->len - 1 && stop && !next_is_read) cmd |= IC_DATA_CMD_STOP;
                if (j == 0 && restart) cmd |= IC_DATA_CMD_RESTART;

                if (ctrl->is_packed) {
                    int timeout = 1000000;
                    while (!((*(volatile uint32_t *)(ctrl->base + 0x04) >> 16) & (1 << 1)) && --timeout > 0); // TFNF
                    if (timeout > 0) *(volatile uint32_t *)(ctrl->base + 0x04) = cmd;
                    else { rc = -ETIMEDOUT; break; }
                } else {
                    int timeout = 1000000;
                    while (!dwi2c_tx_fifo_not_full(base) && --timeout > 0);
                    if (timeout <= 0) { rc = -ETIMEDOUT; break; }
                    dwi2c_write_reg(base, IC_DATA_CMD, cmd);
                }
            }
            if (rc < 0) break;
        }

        if (stop) {
            rc = dwi2c_wait_for_idle(base);
            if (rc != 0) return rc;
        }
    }

    return 0;
}

static uint32_t lpss_read_packed(uintptr_t base, uint32_t dw_offset) {
    if (dw_offset == 0x00) return *(volatile uint32_t *)(base + 0x00) & 0xFFFF; // IC_CON
    if (dw_offset == 0x04) return (*(volatile uint32_t *)(base + 0x00) >> 16) & 0xFFFF; // IC_TAR
    if (dw_offset == 0x10) return *(volatile uint32_t *)(base + 0x04) & 0xFFFF; // IC_DATA_CMD
    if (dw_offset == 0x70) return *(volatile uint32_t *)(base + 0x04) >> 16; // IC_STATUS
    return *(volatile uint32_t *)(base + dw_offset); // Fallback
}

static void lpss_write_packed(uintptr_t base, uint32_t dw_offset, uint32_t val) {
    if (dw_offset == 0x00) {
        uint32_t tmp = *(volatile uint32_t *)(base + 0x00) & 0xFFFF0000;
        *(volatile uint32_t *)(base + 0x00) = tmp | (val & 0xFFFF);
    } else if (dw_offset == 0x04) {
        uint32_t tmp = *(volatile uint32_t *)(base + 0x00) & 0x0000FFFF;
        *(volatile uint32_t *)(base + 0x00) = tmp | ((val & 0xFFFF) << 16);
    } else if (dw_offset == 0x10) {
        uint32_t tmp = *(volatile uint32_t *)(base + 0x04) & 0xFFFF0000;
        *(volatile uint32_t *)(base + 0x04) = tmp | (val & 0xFFFF);
    } else {
        *(volatile uint32_t *)(base + dw_offset) = val;
    }
}

static uint32_t lpss_read_private(uintptr_t base, uint32_t offset) {
    return *(volatile uint32_t *)(base + offset);
}

static void lpss_write_private(uintptr_t base, uint32_t offset, uint32_t value) {
    *(volatile uint32_t *)(base + offset) = value;
}

static int lpss_i2c_private_init(volatile uint8_t *base) {
    if (!base) return -EINVAL;

    uint32_t clk = lpss_read_private((uintptr_t)base, LPSS_PRIVATE_CLOCK_GATE);
    clk |= LPSS_CLOCK_GATE_CLK_EN;
    lpss_write_private((uintptr_t)base, LPSS_PRIVATE_CLOCK_GATE, clk);

    uint32_t rst = lpss_read_private((uintptr_t)base, LPSS_PRIVATE_RESET);
    rst |= LPSS_RESET_RESET_REL;
    lpss_write_private((uintptr_t)base, LPSS_PRIVATE_RESET, rst);

    return 0;
}


static bool is_lpss_i2c_device(pci_device_t *dev) {
    if (dev->vendor_id != 0x8086) return false;  // Intel only

    // Standard I2C class
    if (dev->class_code == PCI_CLASS_SERIAL_BUS_CONTROLLER && dev->subclass == PCI_SUBCLASS_I2C)
        return true;

    // Signal Processing / Other (0x1180) - used by some LPSS implementations
    if (dev->class_code == 0x11 && dev->subclass == 0x80) {
        uint16_t id = dev->device_id;
        // Sky Lake / Kaby Lake
        if (id >= 0x9D60 && id <= 0x9D6F) return true;
        if (id >= 0xA160 && id <= 0xA16F) return true;
        // Whiskey / Coffee / Comet Lake
        if (id >= 0x9DE8 && id <= 0x9DEB) return true;
        if (id >= 0xA368 && id <= 0xA36B) return true;
        if (id >= 0x02E8 && id <= 0x02EB) return true;
        if (id >= 0x06E8 && id <= 0x06EB) return true;
        // Ice Lake / Tiger Lake / Alder Lake / Raptor Lake
        if (id >= 0x34E8 && id <= 0x34EB) return true;
        if (id >= 0x9A00 && id <= 0x9AFF) return true;
        if (id >= 0xA000 && id <= 0xA0FF) return true;
        if (id >= 0x43E8 && id <= 0x43EB) return true;
        if (id >= 0x51E8 && id <= 0x51EB) return true;
        if (id >= 0x54E8 && id <= 0x54EB) return true;
        if (id >= 0x7A50 && id <= 0x7A7D) return true;
        if (id >= 0x7E50 && id <= 0x7E51) return true;
    }

    return false;
}

static int map_bar0_to_kernel(uint64_t bar0_phys, uintptr_t *kernel_addr) {
    if (!kernel_addr) return -EINVAL;
    if (bar0_phys == 0) return -EINVAL;

    // Map 4KB pages starting from bar0_phys
    // Assume bar0 is at least 4KB
    uint64_t virt_base = p2v(bar0_phys);
    
    // Map first 0x2000 bytes (8KB) to cover search range + registers
    paging_map_page(paging_get_pml4_phys(), virt_base, bar0_phys, 
                   PT_PRESENT | PT_RW | PT_CACHE_DISABLE);
    paging_map_page(paging_get_pml4_phys(), virt_base + 0x1000, bar0_phys + 0x1000, 
                   PT_PRESENT | PT_RW | PT_CACHE_DISABLE);

    *kernel_addr = virt_base;
    return 0;
}

static int scan_pci_for_lpss_i2c(void) {
    pci_device_t pci_devs[256];
    int count = pci_enumerate_devices(pci_devs, 256);

    controller_count = 0;
    for (int i = 0; i < count && controller_count < MAX_LPSS_I2C_CONTROLLERS; i++) {
        if (!is_lpss_i2c_device(&pci_devs[i])) continue;

        pci_device_t *dev = &pci_devs[i];

        // Enable MMIO and bus mastering
        pci_enable_mmio(dev);
        pci_enable_bus_mastering(dev);

        // Wake up device (D3 -> D0)
        uint32_t status = pci_read_config(dev->bus, dev->device, dev->function, 0x06);
        if (status & (1 << 4)) { // Capabilities List
            uint8_t cap_ptr = (uint8_t)(pci_read_config(dev->bus, dev->device, dev->function, 0x34) & 0xFF);
            while (cap_ptr) {
                uint32_t cap = pci_read_config(dev->bus, dev->device, dev->function, cap_ptr);
                if ((cap & 0xFF) == 0x01) { // Power Management
                    uint32_t pmcsr = pci_read_config(dev->bus, dev->device, dev->function, cap_ptr + 4);
                    if ((pmcsr & 0x03) != 0) {
                        serial_write("[I2C-LPSS] Performing Intel D0 Power Transition...\n");
                        pmcsr &= ~0x03; // Set D0
                        pmcsr |= (1 << 15); // Clear PME_Status (Intel requirement (fuck intel))
                        pci_write_config(dev->bus, dev->device, dev->function, cap_ptr + 4, pmcsr);
                        
                        // Intel requirement: Wait at least 10ms after D3->D0
                        for(volatile int j=0; j<10000000; j++); 
                    }
                    break;
                }
                cap_ptr = (uint8_t)((cap >> 8) & 0xFF);
            }
        }

        // Enable Bus Master + MMIO
        uint32_t cmd = pci_read_config(dev->bus, dev->device, dev->function, 0x04);
        pci_write_config(dev->bus, dev->device, dev->function, 0x04, cmd | 0x06);

        // Read BAR0 (64-bit)
        uint64_t bar0_phys = pci_get_bar(dev, 0);
        uint64_t real_phys = bar0_phys;

        if (real_phys == 0) {
            serial_write("[I2C-LPSS] Invalid BAR\n");
            continue;
        }

        // Map to kernel address space
        uintptr_t kernel_addr;
        if (map_bar0_to_kernel(real_phys, &kernel_addr) != 0) {
            serial_write("[I2C-LPSS] Failed to map BAR\n");
            continue;
        }

        // Store controller info
        i2c_lpss_controller_t *ctrl = &controllers[controller_count];
        ctrl->base = kernel_addr;
        ctrl->base_phys = bar0_phys;
        ctrl->pci_bus = dev->bus;
        ctrl->pci_dev = dev->device;
        ctrl->pci_fn = dev->function;
        ctrl->vendor_id = dev->vendor_id;
        ctrl->device_id = dev->device_id;
        // Generation-specific configuration
        uint16_t id = dev->device_id;
        if ((id >= 0x9D60 && id <= 0x9D6F) || (id >= 0xA160 && id <= 0xA16F) ||
            (id >= 0x9DE8 && id <= 0x9DEB) || (id >= 0xA368 && id <= 0xA36B) ||
            (id >= 0x02E8 && id <= 0x02EB) || (id >= 0x06E8 && id <= 0x06EB)) {
            // Sky Lake / Kaby Lake / Coffee Lake / Comet Lake
            ctrl->input_clock_hz = 120000000;
            ctrl->is_packed = false;
        } else {
            // Ice Lake / Tiger Lake / Alder Lake / Raptor Lake / Meteor Lake
            ctrl->input_clock_hz = 133333333;
            ctrl->is_packed = true;
        }
        ctrl->adapter.active = false;
        ctrl->adapter.priv = ctrl;
        ctrl->adapter.master_xfer = NULL;
        ctrl->adapter.acpi_dev = NULL;
        ctrl->adapter.name = ctrl->name;
        memset(ctrl->name, 0, sizeof(ctrl->name));

        char tmp[3];
        char *p = ctrl->name;
        const char prefix[] = "lpss-i2c-";
        memcpy(p, prefix, sizeof(prefix) - 1);
        p += sizeof(prefix) - 1;

        itoa_hex(dev->bus, tmp);
        if (tmp[1] == '\0') { *p++ = '0'; *p++ = tmp[0]; } else { *p++ = tmp[0]; *p++ = tmp[1]; }
        *p++ = ':';

        itoa_hex(dev->device, tmp);
        if (tmp[1] == '\0') { *p++ = '0'; *p++ = tmp[0]; } else { *p++ = tmp[0]; *p++ = tmp[1]; }
        *p++ = '.';

        itoa_hex(dev->function, tmp);
        if (tmp[1] == '\0') { *p++ = '0'; *p++ = tmp[0]; } else { *p++ = tmp[0]; *p++ = tmp[1]; }
        *p = '\0';

        // Initialize LPSS private registers
        if (lpss_i2c_private_init((volatile uint8_t *)kernel_addr) != 0) {
            continue;
        }

        // Initialize DesignWare core
        if (dwi2c_init((volatile uint8_t *)kernel_addr, I2C_SPEED_STANDARD, ctrl->input_clock_hz) != 0) {
            continue;
        }

        ctrl->active = true;
        acpi_devices[controller_count] = NULL;
        ctrl->adapter.master_xfer = i2c_lpss_master_xfer;

        if (i2c_adapter_register(&ctrl->adapter) != 0) {
            continue;
        }

        controller_count++;
    }

    return controller_count;
}

static inline bool acpi_i2c_dev_is_hid(const aml_i2c_dev_t *dev) {
    return dev && dev->valid && dev->has_dsm;
}

static int match_acpi_devices(void) {
    size_t acpi_count = acpi_i2c_count();

    serial_write("[I2C-LPSS] Attempting to match ");
    serial_write_num(acpi_count);
    serial_write(" ACPI device(s) to ");
    serial_write_num(controller_count);
    serial_write(" controller(s)\n");

    for (size_t acpi_idx = 0; acpi_idx < acpi_count; acpi_idx++) {
        const aml_i2c_dev_t *acpi_dev = acpi_i2c_get(acpi_idx);
        if (!acpi_dev || !acpi_dev->valid) {
            serial_write("[I2C-LPSS] Skipping invalid ACPI device at index ");
            serial_write_num(acpi_idx);
            serial_write("\n");
            continue;
        }

        serial_write("[I2C-LPSS] Considering ACPI device: ");
        serial_write(acpi_dev->hid);
        serial_write(" at slave 0x");
        serial_write_hex(acpi_dev->slave_address);
        serial_write(" speed ");
        serial_write_num(acpi_dev->speed_hz);
        serial_write(" Hz\n");
    }

    for (size_t acpi_idx = 0; acpi_idx < acpi_count; acpi_idx++) {
        const aml_i2c_dev_t *acpi_dev = acpi_i2c_get(acpi_idx);
        if (!acpi_dev || !acpi_dev->valid) continue;

        bool is_touchpad = is_touchpad_hid(acpi_dev->hid) || 
                           (memcmp(acpi_dev->name, "TPD", 3) == 0);
        if (!is_touchpad) continue;

        for (int ctrl_idx = 0; ctrl_idx < controller_count; ctrl_idx++) {
            i2c_lpss_controller_t *ctrl = &controllers[ctrl_idx];
            if (acpi_devices[ctrl_idx] != NULL) continue;

            acpi_devices[ctrl_idx] = acpi_dev;
            ctrl->adapter.acpi_dev = acpi_dev;
            serial_write("[I2C-LPSS] Priority match for touchpad: ");
            serial_write(acpi_dev->hid);
            serial_write(" to controller ");
            serial_write(ctrl->name);
            serial_write("\n");
            break;
        }
    }

    for (size_t acpi_idx = 0; acpi_idx < acpi_count; acpi_idx++) {
        const aml_i2c_dev_t *acpi_dev = acpi_i2c_get(acpi_idx);
        if (!acpi_dev || !acpi_dev->valid) continue;

        bool already_matched = false;
        for (int j = 0; j < controller_count; j++) {
            if (acpi_devices[j] == acpi_dev) { already_matched = true; break; }
        }
        if (already_matched) continue;

        for (int ctrl_idx = 0; ctrl_idx < controller_count; ctrl_idx++) {
            i2c_lpss_controller_t *ctrl = &controllers[ctrl_idx];
            if (acpi_devices[ctrl_idx] != NULL) continue;

            acpi_devices[ctrl_idx] = acpi_dev;
            ctrl->adapter.acpi_dev = acpi_dev;
            serial_write("[I2C-LPSS] Matched device ");
            serial_write(acpi_dev->hid);
            serial_write(" to controller ");
            serial_write(ctrl->name);
            serial_write("\n");
            break;
        }
    }

    return 0;
}

int i2c_lpss_init(void) {
    serial_write("[I2C-LPSS] Initializing...\n");

    int count = scan_pci_for_lpss_i2c();
    if (count <= 0) {
        serial_write("[I2C-LPSS] No controllers found\n");
        return 0;
    }

    serial_write("[I2C-LPSS] Found ");
    serial_write_num(count);
    serial_write(" controller(s)\n");
    match_acpi_devices();

    for (int i = 0; i < controller_count; i++) {
        if (controllers[i].active) {
            volatile uint32_t *lpss_priv = (volatile uint32_t*)((uintptr_t)controllers[i].base + 0x200);
            lpss_priv[1] = 3;   // Release reset
            lpss_priv[0] = 1;   // Enable clock
            lpss_priv[16] = 0x110; // LPSS_PRIVATE_REMAP_ADDR_EN
        }
    }

    return controller_count;
}

int i2c_lpss_get_count(void) {
    return controller_count;
}

i2c_lpss_controller_t* i2c_lpss_get(int index) {
    if (index < 0 || index >= controller_count) return NULL;
    return &controllers[index];
}

i2c_lpss_controller_t* i2c_lpss_get_by_base(uint64_t base_phys) {
    for (int i = 0; i < controller_count; i++) {
        if (controllers[i].base_phys == base_phys) return &controllers[i];
    }
    return NULL;
}

const aml_i2c_dev_t* i2c_lpss_get_acpi_device(i2c_lpss_controller_t *ctrl) {
    if (!ctrl) return NULL;
    
    for (int i = 0; i < controller_count; i++) {
        if (&controllers[i] == ctrl) {
            return acpi_devices[i];
        }
    }
    return NULL;
}
