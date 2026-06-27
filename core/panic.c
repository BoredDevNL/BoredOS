// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#include "process.h"
#include "graphics.h"
#include "io.h"
#include "kutils.h"
#include "kconsole.h"

const char* exception_name(uint8_t vector) {
    static const char* names[32] = {
        "Divide Error", "Debug", "Non-maskable Interrupt", "Breakpoint",
        "Overflow", "Bound Range Exceeded", "Invalid Opcode", "Device Not Available",
        "Double Fault", "Coprocessor Segment Overrun", "Invalid TSS", "Segment Not Present",
        "Stack Segment Fault", "General Protection Fault", "Page Fault", "Reserved",
        "x87 FPU Error", "Alignment Check", "Machine Check", "SIMD Exception",
        "Virtualization Exception", "Control Protection", "Reserved", "Reserved",
        "Reserved", "Reserved", "Reserved", "Reserved", "Reserved", "Reserved", 
        "Security Exception", "Reserved"
    };
    return vector < 32 ? names[vector] : "Unknown Internal Exception";
}

//  colours
#define COL_BG          0x00000000
#define COL_WHITE       0xFFFFFFFF
#define COL_RED         0xFF990000
#define COL_REG         0xFFFFFFFF
#define COL_SECTION     0xFF99CCFF
#define COL_HIGHLIGHT   0xFFFFFF55
#define COL_FAULT       0xFFFF6666

// settings (change these if you want but these are what i found are most readable)
#define BSOD_MARGIN_PCT 25

// register collumn macro function
#define RCOL(ci, lbl, val) \
    ptext(margin + (ci) * col_w, dump_y, fmt_reg(buf, (lbl), (val)), COL_REG)

//  page fault error code decoder 
static void decode_pf_error(uint64_t err, char *out) {
    const char *labels[] = { "P=", " W=", " U=", " R=", " I=" };
    int pos = 0;
    out[pos++] = '(';
    for (int p = 0; p < 5; p++) {
        const char *s = labels[p];
        while (*s) out[pos++] = *s++;
        out[pos++] = '0' + ((err >> p) & 1);
    }
    out[pos++] = ')';
    out[pos]   = 0;
}

//  helpers 
static char *fmt_reg(char *buf, const char *label, uint64_t value) {
    static const char digits[] = "0123456789ABCDEF";
    int pos = 0;
    while (label[pos]) { buf[pos] = label[pos]; pos++; }
    buf[pos++] = '0'; buf[pos++] = 'x';
    for (int i = 15; i >= 0; i--) {
        buf[pos + i] = digits[value & 0xF];
        value >>= 4;
    }
    buf[pos + 16] = 0;
    return buf;
}

static void serial_reg(const char *label, uint64_t value) {
    char hex[17];
    serial_write(label);
    itoa_hex(value, hex);
    serial_write(hex);
    serial_write("\n");
}

//  layout 
static int g_sw, g_sh;

static void ptext(int x, int y, const char *s, uint32_t col) {
    draw_string(x, y, s, col);
}

static void ptext_centered(int y, const char *s, uint32_t col) {
    int x = (g_sw - strlen(s) * 8) / 2;
    if (x < 0) x = 0;
    draw_string(x, y, s, col);
}

#define COLOR_START 0x002A7B9B
#define COLOR_END   0x00002F4D

static void draw_table_flip(int cx, int cy, int block) {
    
    static const char *art[] = {
        "              ####                                     ",
        "            #######                                    ",
        "             ######                                    ",
        "             #######                                   ",
        "              #######      ######                      ",
        "               ######   ############                   ",
        "                ###### ##############                  ",
        "                ###########   ########                 ",
        "                 ########       #######                ",
        "                  #######        ######                ",
        "                   #######     ########                ",
        "                    ##################                 ",
        "                     ###############                   ",
        "        ######################################         ",
        "        ######################################         ",
        "        ######################################         "
    };

    int rows = sizeof(art) / sizeof(art[0]);
    int cols = 54;
    
    int block_w = block / 2; 
    if (block_w < 1) block_w = 1;
    int block_h = block;

    int grid_w = cols * block_w;
    int ox = cx - grid_w / 2;
    int oy = cy;
    
    // extract rgb values for the linear gradients
    int r_start = (COLOR_START >> 16) & 0xFF;
    int g_start = (COLOR_START >> 8)  & 0xFF;
    int b_start =  COLOR_START        & 0xFF;

    int r_end = (COLOR_END >> 16) & 0xFF;
    int g_end = (COLOR_END >> 8)  & 0xFF;
    int b_end =  COLOR_END        & 0xFF;

    for (int r = 0; r < rows; r++) {
        int divisor = (rows > 1) ? (rows - 1) : 1;
        
        int curr_r = r_start + ((r_end - r_start) * r) / divisor;
        int curr_g = g_start + ((g_end - g_start) * r) / divisor;
        int curr_b = b_start + ((b_end - b_start) * r) / divisor;
        
        unsigned int row_color = (0xFF000000) | (curr_r << 16) | (curr_g << 8) | curr_b;

        for (int c = 0; art[r][c] != '\0'; c++) {
            if (art[r][c] != ' ') {
                draw_rect(ox + c * block_w, oy + r * block_h, block_w, block_h, row_color);
            }
        }
    }
}

volatile bool g_in_panic = false;

void kernel_panic(registers_t *regs, const char *error_name) {
    asm volatile("cli");
    g_in_panic = true;

    uint64_t cr0, cr2, cr3, cr4;
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    asm volatile("mov %%cr2, %0" : "=r"(cr2));
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    asm volatile("mov %%cr4, %0" : "=r"(cr4));

    uint64_t rflags = regs ? regs->rflags : 0;

    g_sw = get_screen_width();
    g_sh = get_screen_height();
    graphics_clear_back_buffer(COL_BG);

    int margin = (g_sw * BSOD_MARGIN_PCT) / 100;
    int cw     = g_sw - margin * 2;
    char buf[128];

    //  ascii art
    int face_top = g_sh * 20 / 100;
    int block    = g_sw / 120;
    if (block < 3) block = 3;
    draw_table_flip(g_sw / 2, face_top, block);
    int face_h = 16 * block;

    //  user facing text
    int y = face_top + face_h + (block * 4);
    ptext_centered(y, "*** [KERNEL PANIC] ***", COL_RED);
    y += 18;
    ptext_centered(y, "Your computer has encountered an issue. We have stopped the machine to prevent damage or data loss.", COL_RED);
    y += 18;

    y += 65;
    ptext_centered(y, error_name, COL_HIGHLIGHT);
    y += 14;

    //  for PF only
    if (regs) {
        if (regs->int_no == 14) {
            ptext_centered(y, fmt_reg(buf, "Fault address: 0x", cr2), COL_FAULT);
            y += 18;
        }
    }

    int dump_y = g_sh * 62 / 100;
    if (regs) {
        ptext(margin,          dump_y, fmt_reg(buf, "RIP:    0x", regs->rip),  COL_HIGHLIGHT);
        ptext(margin + cw / 2, dump_y, fmt_reg(buf, "RFLAGS: 0x", rflags),     COL_HIGHLIGHT);
        dump_y += 14;
        int col_w = cw / 4;

        RCOL(0,"RAX: 0x",regs->rax); RCOL(1,"RSI: 0x",regs->rsi);
        RCOL(2,"R8:  0x",regs->r8);  RCOL(3,"R12: 0x",regs->r12); dump_y += 14;

        RCOL(0,"RBX: 0x",regs->rbx); RCOL(1,"RDI: 0x",regs->rdi);
        RCOL(2,"R9:  0x",regs->r9);  RCOL(3,"R13: 0x",regs->r13); dump_y += 14;

        RCOL(0,"RCX: 0x",regs->rcx); RCOL(1,"RBP: 0x",regs->rbp);
        RCOL(2,"R10: 0x",regs->r10); RCOL(3,"R14: 0x",regs->r14); dump_y += 14;

        RCOL(0,"RDX: 0x",regs->rdx); RCOL(1,"RSP: 0x",regs->rsp);
        RCOL(2,"R11: 0x",regs->r11); RCOL(3,"R15: 0x",regs->r15); dump_y += 18;

        #undef RCOL

        // Error code
        fmt_reg(buf, "Error Code: 0x", regs->err_code);
        if (regs->int_no == 14) {
            char pf[24]; decode_pf_error(regs->err_code, pf);
            int bp = strlen(buf);
            buf[bp++] = ' ';
            int pi = 0; while (pf[pi]) buf[bp++] = pf[pi++];
            buf[bp] = 0;
        }
        ptext(margin, dump_y, buf, COL_HIGHLIGHT); dump_y += 14;
    }

    {
        int col_w = cw / 4;
        ptext(margin + 0 * col_w, dump_y, fmt_reg(buf, "CR0 0x", cr0), COL_REG);
        ptext(margin + 1 * col_w, dump_y, fmt_reg(buf, "CR2 0x", cr2), COL_REG);
        ptext(margin + 2 * col_w, dump_y, fmt_reg(buf, "CR3 0x", cr3), COL_REG);
        ptext(margin + 3 * col_w, dump_y, fmt_reg(buf, "CR4 0x", cr4), COL_REG);
    }

    ptext_centered(g_sh - 40, "There is nothing more to be done, the CPU has stopped", COL_WHITE);
    ptext_centered(g_sh - 28, "Hold the Power Button for 10 seconds to force shutdown", COL_WHITE);

    graphics_mark_screen_dirty();
    graphics_flip_buffer();

    //  Serial output 
    serial_write("\n==================================\n");
    serial_write("          KERNEL PANIC\n");
    serial_write("==================================\n");
    serial_write("Stop code: "); serial_write(error_name); serial_write("\n");

    if (regs) {
        const char *vec_name = exception_name(regs->int_no);
        char hex[17];

        serial_write("\n-- Exception --\n");
        itoa_hex(regs->int_no, hex);
        serial_write("Vector  : 0x"); serial_write(hex);
        serial_write("  ("); serial_write(vec_name); serial_write(")\n");
        serial_reg("ErrCode : 0x", regs->err_code);
        if (regs->int_no == 14) {
            char pf[24]; decode_pf_error(regs->err_code, pf);
            serial_write("PF flags: "); serial_write(pf); serial_write("\n");
            serial_reg("CR2     : 0x", cr2);
        }
        serial_reg("RIP     : 0x", regs->rip);
        serial_reg("RFLAGS  : 0x", rflags);
        serial_reg("CS      : 0x", regs->cs);
        serial_reg("SS      : 0x", regs->ss);

        serial_write("\n-- General Purpose Registers --\n");
        serial_reg("RAX: 0x", regs->rax); serial_reg("RSI: 0x", regs->rsi);
        serial_reg("RBX: 0x", regs->rbx); serial_reg("RDI: 0x", regs->rdi);
        serial_reg("RCX: 0x", regs->rcx); serial_reg("RBP: 0x", regs->rbp);
        serial_reg("RDX: 0x", regs->rdx); serial_reg("RSP: 0x", regs->rsp);
        serial_reg("R8 : 0x", regs->r8);  serial_reg("R12: 0x", regs->r12);
        serial_reg("R9 : 0x", regs->r9);  serial_reg("R13: 0x", regs->r13);
        serial_reg("R10: 0x", regs->r10); serial_reg("R14: 0x", regs->r14);
        serial_reg("R11: 0x", regs->r11); serial_reg("R15: 0x", regs->r15);
    }

    serial_write("\n-- Control Registers --\n");
    serial_reg("CR0: 0x", cr0);
    serial_reg("CR2: 0x", cr2);
    serial_reg("CR3: 0x", cr3);
    serial_reg("CR4: 0x", cr4);
    serial_write("\n");

    while (1) {
        asm volatile("cli; hlt");
    }
}