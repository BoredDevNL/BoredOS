// Copyright (c) 2023-2026 Chris (boreddevnl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.

#include "i2c_designware.h"
#include "kutils.h"

extern void serial_write(const char *str);
extern void serial_write_hex(uint32_t val);
extern void serial_write_num(uint64_t num);

int dwi2c_init(volatile uint8_t *base, i2c_speed_t speed_hz, uint32_t input_clock_hz) {
    if (!base) return -EINVAL;
    if (!input_clock_hz) return -EINVAL;

    dwi2c_enable(base, false);
        int timeout = 1000000;
    while (timeout-- > 0) {
        if (!dwi2c_is_enabled(base)) break;
    }
    if (timeout <= 0) return -ETIMEDOUT;
    uint32_t con = IC_CON_MASTER | IC_CON_RESTART_EN | IC_CON_SLAVE_DISABLE;
        if (speed_hz >= 1000000) {
        con |= (3 << 1);  // Fast-plus (1 MHz)
    } else if (speed_hz >= 400000) {
        con |= (2 << 1);  // Fast (400 kHz)
    } else {
        con |= (1 << 1);  // Standard (100 kHz)
    }
    
    dwi2c_write_reg(base, IC_CON, con);
    uint32_t hcnt, lcnt;
    
    if (speed_hz >= 1000000) {
        // Fast-plus (1 MHz) — roughly 50% duty cycle
        hcnt = input_clock_hz / (2 * 1000000);
        lcnt = input_clock_hz / (2 * 1000000);
    } else if (speed_hz >= 400000) {
        // Fast (400 kHz) — roughly 50% duty cycle
        hcnt = input_clock_hz / (2 * 400000);
        lcnt = input_clock_hz / (2 * 400000);
    } else {
        // Standard (100 kHz) — roughly 50% duty cycle
        hcnt = input_clock_hz / (2 * 100000);
        lcnt = input_clock_hz / (2 * 100000);
    }

    // Clamp to reasonable ranges (8-bit on some implementations)
    if (hcnt > 0xFFFF) hcnt = 0xFFFF;
    if (lcnt > 0xFFFF) lcnt = 0xFFFF;
    if (hcnt < 6) hcnt = 6;    // Minimum per spec
    if (lcnt < 8) lcnt = 8;    // Minimum per spec

    dwi2c_write_reg(base, IC_SS_SCL_HCNT, hcnt);
    dwi2c_write_reg(base, IC_SS_SCL_LCNT, lcnt);
    dwi2c_write_reg(base, IC_FS_SCL_HCNT, hcnt);
    dwi2c_write_reg(base, IC_FS_SCL_LCNT, lcnt);

    uint32_t sda_hold = (input_clock_hz / 1000000) / 3;  // Rough: ~300ns
    if (sda_hold < 1) sda_hold = 1;
    if (sda_hold > 0xFFFF) sda_hold = 0xFFFF;
    dwi2c_write_reg(base, IC_SDA_HOLD, sda_hold);

    dwi2c_write_reg(base, IC_INTR_MASK, 0);
    dwi2c_write_reg(base, IC_CLR_INTR, ~0);  // Clear all pending

    uint8_t tx_depth = dwi2c_get_tx_fifo_depth(base);
    uint8_t rx_depth = dwi2c_get_rx_fifo_depth(base);
    dwi2c_write_reg(base, IC_TX_TL, tx_depth / 2);
    dwi2c_write_reg(base, IC_RX_TL, rx_depth / 2);
    dwi2c_enable(base, true);

    return 0;
}

int dwi2c_write_byte(volatile uint8_t *base, uint8_t byte, bool send_stop) {
    if (!dwi2c_is_enabled(base)) return -EINVAL;

    // Wait for TX FIFO not full
    int timeout = 1000000;
    while (!dwi2c_tx_fifo_not_full(base) && --timeout > 0);
    if (timeout <= 0) return -ETIMEDOUT;

    // Write byte to data register
    uint32_t cmd = IC_DATA_CMD_DAT(byte) | IC_DATA_CMD_CMD_WRITE;
    if (send_stop) cmd |= IC_DATA_CMD_STOP;
    
    dwi2c_write_reg(base, IC_DATA_CMD, cmd);

    return 0;
}

int dwi2c_read_byte(volatile uint8_t *base, uint8_t *byte, bool send_stop) {
    if (!dwi2c_is_enabled(base) || !byte) return -EINVAL;

    // Issue read command
    uint32_t cmd = IC_DATA_CMD_CMD_READ;
    if (send_stop) cmd |= IC_DATA_CMD_STOP;
    
    dwi2c_write_reg(base, IC_DATA_CMD, cmd);

    // Check for aborts
    uint32_t abort = dwi2c_read_reg(base, IC_TX_ABRT_SOURCE);
    if (abort) {
        return -EIO;
    }

    // Wait for RX FIFO not empty
    int timeout = 100000;
    while (!dwi2c_rx_fifo_not_empty(base) && timeout-- > 0);
    if (timeout <= 0) return -ETIMEDOUT;

    // Read byte
    *byte = (uint8_t)(dwi2c_read_reg(base, IC_DATA_CMD) & 0xFF);

    return 0;
}

int dwi2c_write_bytes(volatile uint8_t *base, const uint8_t *buf, size_t len) {
    if (!dwi2c_is_enabled(base) || !buf || len == 0) return -EINVAL;

    uint8_t tx_depth = dwi2c_get_tx_fifo_depth(base);
    size_t written = 0;

    while (written < len) {
        // Check TX FIFO level
        uint32_t tx_level = dwi2c_tx_fifo_level(base);
        if (tx_level >= tx_depth) {
            // Wait for space
            int timeout = 1000000;
            while (dwi2c_tx_fifo_level(base) >= tx_depth && --timeout > 0);
            if (timeout <= 0) return -ETIMEDOUT;
        }

        // Write byte
        uint32_t cmd = IC_DATA_CMD_DAT(buf[written]) | IC_DATA_CMD_CMD_WRITE;
        dwi2c_write_reg(base, IC_DATA_CMD, cmd);
        written++;
    }

    return (int)written;
}

int dwi2c_read_bytes(volatile uint8_t *base, uint8_t *buf, size_t len) {
    if (!dwi2c_is_enabled(base) || !buf || len == 0) return -EINVAL;

    size_t read_count = 0;

    while (read_count < len) {
        // Issue read command
        uint32_t cmd = IC_DATA_CMD_CMD_READ;
        dwi2c_write_reg(base, IC_DATA_CMD, cmd);

        // Wait for data
        int timeout = 1000000;
        while (!dwi2c_rx_fifo_not_empty(base) && --timeout > 0);
        if (timeout <= 0) return -ETIMEDOUT;

        // Read byte
        buf[read_count] = (uint8_t)(dwi2c_read_reg(base, IC_DATA_CMD) & 0xFF);
        read_count++;
    }

    return (int)read_count;
}

int dwi2c_wait_for_idle(volatile uint8_t *base) {
    int timeout = 5000000;  // ~5 second timeout
    while (timeout-- > 0) {
        uint32_t status = dwi2c_read_reg(base, IC_STATUS);
        if (!(status & IC_STATUS_ACTIVITY)) return 0;
        
        // Check for errors
        uint32_t raw_intr = dwi2c_read_reg(base, IC_RAW_INTR_STAT);
        if (raw_intr & IC_INTR_TX_ABRT) {
            dwi2c_write_reg(base, IC_CLR_TX_ABRT, 1);
            return -EIO;
        }
    }
    return -ETIMEDOUT;
}

void dwi2c_dump_status(volatile uint8_t *base) {
    serial_write("[DWI2C] Status: ");
    serial_write_hex(dwi2c_read_reg(base, IC_STATUS));
    serial_write(" Enabled: ");
    serial_write_hex(dwi2c_is_enabled(base) ? 1 : 0);
    serial_write(" TX_LEVEL: ");
    serial_write_num(dwi2c_tx_fifo_level(base));
    serial_write(" RX_LEVEL: ");
    serial_write_num(dwi2c_rx_fifo_level(base));
    serial_write("\n");
}
