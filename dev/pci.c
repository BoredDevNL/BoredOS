// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#include <stdint.h>
#include "pci.h"
#include "io.h"

uint32_t pci_read_config(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
    uint32_t address = (uint32_t)((1u << 31) | (bus << 16) | (device << 11) | (function << 8) | (offset & 0xFC));
    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

void pci_write_config(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint32_t value) {
    uint32_t address = (uint32_t)((1u << 31) | (bus << 16) | (device << 11) | (function << 8) | (offset & 0xFC));
    outl(PCI_CONFIG_ADDRESS, address);
    outl(PCI_CONFIG_DATA, value);
}

int pci_device_exists(uint8_t bus, uint8_t device, uint8_t function) {
    uint16_t vendor_id = pci_get_vendor_id(bus, device, function);
    return vendor_id != 0xFFFF;
}

uint16_t pci_get_vendor_id(uint8_t bus, uint8_t device, uint8_t function) {
    uint32_t config = pci_read_config(bus, device, function, 0x00);
    return (uint16_t)(config & 0xFFFF);
}

uint16_t pci_get_device_id(uint8_t bus, uint8_t device, uint8_t function) {
    uint32_t config = pci_read_config(bus, device, function, 0x00);
    return (uint16_t)((config >> 16) & 0xFFFF);
}

uint8_t pci_get_class_code(uint8_t bus, uint8_t device, uint8_t function) {
    uint32_t config = pci_read_config(bus, device, function, 0x08);
    return (uint8_t)((config >> 24) & 0xFF);
}

uint8_t pci_get_subclass(uint8_t bus, uint8_t device, uint8_t function) {
    uint32_t config = pci_read_config(bus, device, function, 0x08);
    return (uint8_t)((config >> 16) & 0xFF);
}

uint8_t pci_get_prog_if(uint8_t bus, uint8_t device, uint8_t function) {
    uint32_t config = pci_read_config(bus, device, function, 0x08);
    return (uint8_t)((config >> 8) & 0xFF);
}

int pci_enumerate_devices(pci_device_t* devices, int max_devices) {
    int count = 0;
    for (uint8_t bus = 0; bus < 256 && count < max_devices; bus++) {
        for (uint8_t dev = 0; dev < 32 && count < max_devices; dev++) {
            if (pci_device_exists(bus, dev, 0)) {
                uint32_t config_val = pci_read_config(bus, dev, 0, 0x0C);
                uint8_t header_type = (uint8_t)((config_val >> 16) & 0xFF);
                uint8_t num_functions = (header_type & 0x80) ? 8 : 1;
                for (uint8_t fn = 0; fn < num_functions && count < max_devices; fn++) {
                    if (pci_device_exists(bus, dev, fn)) {
                        devices[count].bus = bus;
                        devices[count].device = dev;
                        devices[count].function = fn;
                        devices[count].vendor_id = pci_get_vendor_id(bus, dev, fn);
                        devices[count].device_id = pci_get_device_id(bus, dev, fn);
                        devices[count].class_code = pci_get_class_code(bus, dev, fn);
                        devices[count].subclass = pci_get_subclass(bus, dev, fn);
                        devices[count].prog_if = pci_get_prog_if(bus, dev, fn);
                        count++;
                    }
                }
            }
        }
    }
    return count;
}

int pci_find_device(uint16_t vendor_id, uint16_t device_id, pci_device_t* device) {
    pci_device_t devices[32];
    int count = pci_enumerate_devices(devices, 32);
    for (int i = 0; i < count; i++) {
        if (devices[i].vendor_id == vendor_id && devices[i].device_id == device_id) {
            *device = devices[i];
            return 1;
        }
    }
    return 0;
}

int pci_find_device_by_class(uint8_t class_code, uint8_t subclass, pci_device_t* device) {
    pci_device_t devices[32];
    int count = pci_enumerate_devices(devices, 32);
    for (int i = 0; i < count; i++) {
        if (devices[i].class_code == class_code && devices[i].subclass == subclass) {
            *device = devices[i];
            return 1;
        }
    }
    return 0;
}

uint64_t pci_get_bar(pci_device_t *dev, int bar_num) {
    if (!dev || bar_num < 0 || bar_num > 5) return 0;
    uint8_t offset = 0x10 + (bar_num * 4);
    uint32_t bar_low = pci_read_config(dev->bus, dev->device, dev->function, offset);
    
    // Check if this is a 64-bit memory BAR (Type bits 1-2 == 2)
    if ((bar_low & 0x01) == 0 && ((bar_low >> 1) & 0x03) == 0x02) {
        if (bar_num < 5) {
            uint32_t bar_high = pci_read_config(dev->bus, dev->device, dev->function, offset + 4);
            return ((uint64_t)bar_high << 32) | (bar_low & ~0xF);
        }
    }
    
    // Standard 32-bit BAR or I/O BAR
    if (bar_low & 0x01) return bar_low & ~0x3; // I/O
    return bar_low & ~0xF; // Memory
}

void pci_enable_bus_mastering(pci_device_t *dev) {
    if (!dev) return;
    uint32_t cmd = pci_read_config(dev->bus, dev->device, dev->function, 0x04);
    cmd |= (1 << 2); // Set Bus Master bit
    pci_write_config(dev->bus, dev->device, dev->function, 0x04, cmd);
}

void pci_enable_mmio(pci_device_t *dev) {
    if (!dev) return;
    uint32_t cmd = pci_read_config(dev->bus, dev->device, dev->function, 0x04);
    cmd |= (1 << 1); // Set Memory Space bit
    pci_write_config(dev->bus, dev->device, dev->function, 0x04, cmd);
}
