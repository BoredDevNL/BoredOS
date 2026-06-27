#ifndef I2C_DESIGNWARE_H
#define I2C_DESIGNWARE_H

#include <stdint.h>
#include <stdbool.h>
#include "i2c.h"

// Register Offsets
#define IC_CON            0x00
#define IC_TAR            0x04
#define IC_SAR            0x08
#define IC_DATA_CMD       0x10
#define IC_SS_SCL_HCNT    0x14
#define IC_SS_SCL_LCNT    0x18
#define IC_FS_SCL_HCNT    0x1c
#define IC_FS_SCL_LCNT    0x20
#define IC_INTR_STAT      0x2c
#define IC_INTR_MASK      0x30
#define IC_RAW_INTR_STAT  0x34
#define IC_RX_TL          0x38
#define IC_TX_TL          0x3c
#define IC_CLR_INTR       0x40
#define IC_CLR_RX_UNDER   0x44
#define IC_CLR_RX_OVER    0x48
#define IC_CLR_TX_OVER    0x4c
#define IC_CLR_RD_REQ     0x50
#define IC_CLR_TX_ABRT    0x54
#define IC_CLR_RX_DONE    0x58
#define IC_CLR_ACTIVITY   0x5c
#define IC_CLR_STOP_DET   0x60
#define IC_CLR_START_DET  0x64
#define IC_CLR_GEN_CALL   0x68
#define IC_ENABLE         0x6c
#define IC_STATUS         0x70
#define IC_TXFLR          0x74
#define IC_RXFLR          0x78
#define IC_SDA_HOLD       0x7c
#define IC_TX_ABRT_SOURCE 0x80
#define IC_ENABLE_STATUS  0x9c
#define IC_COMP_PARAM_1   0xf4
#define IC_COMP_VERSION   0xf8
#define IC_COMP_TYPE      0xfc

// IC_CON bits
#define IC_CON_MASTER        (1 << 0)
#define IC_CON_SPEED_STD     (1 << 1)
#define IC_CON_SPEED_FAST    (2 << 1)
#define IC_CON_SPEED_HIGH    (3 << 1)
#define IC_CON_10BITADDR_SLAVE (1 << 3)
#define IC_CON_10BITADDR_MASTER (1 << 4)
#define IC_CON_RESTART_EN    (1 << 5)
#define IC_CON_SLAVE_DISABLE (1 << 6)

// IC_DATA_CMD bits
#define IC_DATA_CMD_DAT(x)   ((x) & 0xFF)
#define IC_DATA_CMD_CMD_WRITE (0 << 8)
#define IC_DATA_CMD_CMD_READ  (1 << 8)
#define IC_DATA_CMD_STOP      (1 << 9)
#define IC_DATA_CMD_RESTART   (1 << 10)

// IC_STATUS bits
#define IC_STATUS_ACTIVITY    (1 << 0)
#define IC_STATUS_TFNF        (1 << 1)
#define IC_STATUS_TFEE        (1 << 2)
#define IC_STATUS_RFNE        (1 << 3)
#define IC_STATUS_RFF         (1 << 4)
#define IC_STATUS_MST_ACTIVITY (1 << 5)

// IC_RAW_INTR_STAT bits
#define IC_INTR_RX_UNDER      (1 << 0)
#define IC_INTR_RX_OVER       (1 << 1)
#define IC_INTR_RX_FULL       (1 << 2)
#define IC_INTR_TX_OVER       (1 << 3)
#define IC_INTR_TX_EMPTY      (1 << 4)
#define IC_INTR_RD_REQ        (1 << 5)
#define IC_INTR_TX_ABRT       (1 << 6)
#define IC_INTR_RX_DONE       (1 << 7)
#define IC_INTR_ACTIVITY      (1 << 8)
#define IC_INTR_STOP_DET      (1 << 9)
#define IC_INTR_START_DET     (1 << 10)
#define IC_INTR_GEN_CALL      (1 << 11)

// Error codes
#ifndef EINVAL
#define EINVAL    22
#endif
#ifndef ETIMEDOUT
#define ETIMEDOUT 110
#endif
#ifndef EIO
#define EIO       5
#endif

// Inline register access
static inline void dwi2c_write_reg(volatile uint8_t *base, uint32_t reg, uint32_t val) {
    *(volatile uint32_t*)(base + reg) = val;
}

static inline uint32_t dwi2c_read_reg(volatile uint8_t *base, uint32_t reg) {
    return *(volatile uint32_t*)(base + reg);
}

static inline void dwi2c_enable(volatile uint8_t *base, bool enable) {
    dwi2c_write_reg(base, IC_ENABLE, enable ? 1 : 0);
}

static inline bool dwi2c_is_enabled(volatile uint8_t *base) {
    return (dwi2c_read_reg(base, IC_ENABLE_STATUS) & 1) != 0;
}

static inline bool dwi2c_tx_fifo_not_full(volatile uint8_t *base) {
    return (dwi2c_read_reg(base, IC_STATUS) & IC_STATUS_TFNF) != 0;
}

static inline bool dwi2c_rx_fifo_not_empty(volatile uint8_t *base) {
    return (dwi2c_read_reg(base, IC_STATUS) & IC_STATUS_RFNE) != 0;
}

static inline uint32_t dwi2c_tx_fifo_level(volatile uint8_t *base) {
    return dwi2c_read_reg(base, IC_TXFLR);
}

static inline uint32_t dwi2c_rx_fifo_level(volatile uint8_t *base) {
    return dwi2c_read_reg(base, IC_RXFLR);
}

static inline uint8_t dwi2c_get_tx_fifo_depth(volatile uint8_t *base) {
    return (uint8_t)((dwi2c_read_reg(base, IC_COMP_PARAM_1) >> 16) & 0xFF) + 1;
}

static inline uint8_t dwi2c_get_rx_fifo_depth(volatile uint8_t *base) {
    return (uint8_t)((dwi2c_read_reg(base, IC_COMP_PARAM_1) >> 8) & 0xFF) + 1;
}

static inline void dwi2c_set_target_addr(volatile uint8_t *base, uint16_t addr, bool ten_bit) {
    uint32_t con = dwi2c_read_reg(base, IC_CON);
    if (ten_bit) con |= IC_CON_10BITADDR_MASTER;
    else con &= ~IC_CON_10BITADDR_MASTER;
    dwi2c_write_reg(base, IC_CON, con);
    dwi2c_write_reg(base, IC_TAR, addr);
}

// Function prototypes
int dwi2c_init(volatile uint8_t *base, i2c_speed_t speed_hz, uint32_t input_clock_hz);
int dwi2c_write_byte(volatile uint8_t *base, uint8_t byte, bool send_stop);
int dwi2c_read_byte(volatile uint8_t *base, uint8_t *byte, bool send_stop);
int dwi2c_write_bytes(volatile uint8_t *base, const uint8_t *buf, size_t len);
int dwi2c_read_bytes(volatile uint8_t *base, uint8_t *buf, size_t len);
int dwi2c_wait_for_idle(volatile uint8_t *base);
void dwi2c_dump_status(volatile uint8_t *base);

#endif
