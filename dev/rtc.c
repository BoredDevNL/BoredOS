// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#include "rtc.h"
#include "io.h"

#define CMOS_ADDRESS 0x70
#define CMOS_DATA    0x71

static int updating_rtc() {
    outb(CMOS_ADDRESS, 0x0A);
    return (inb(CMOS_DATA) & 0x80);
}

static uint8_t get_rtc_register(int reg) {
    outb(CMOS_ADDRESS, reg);
    return inb(CMOS_DATA);
}

void rtc_get_datetime(int *year, int *month, int *day, int *hour, int *minute, int *second) {
    uint8_t century;
    uint8_t last_second;
    uint8_t last_minute;
    uint8_t last_hour;
    uint8_t last_day;
    uint8_t last_month;
    uint8_t last_year;
    uint8_t last_century;
    uint8_t registerB;

    while (updating_rtc());
    *second = get_rtc_register(0x00);
    *minute = get_rtc_register(0x02);
    *hour = get_rtc_register(0x04);
    *day = get_rtc_register(0x07);
    *month = get_rtc_register(0x08);
    *year = get_rtc_register(0x09);
    
    do {
        last_second = *second;
        last_minute = *minute;
        last_hour = *hour;
        last_day = *day;
        last_month = *month;
        last_year = *year;

        while (updating_rtc());
        *second = get_rtc_register(0x00);
        *minute = get_rtc_register(0x02);
        *hour = get_rtc_register(0x04);
        *day = get_rtc_register(0x07);
        *month = get_rtc_register(0x08);
        *year = get_rtc_register(0x09);
    } while( (last_second != *second) || (last_minute != *minute) || (last_hour != *hour) ||
             (last_day != *day) || (last_month != *month) || (last_year != *year) );

    registerB = get_rtc_register(0x0B);

    // Convert BCD to binary values if necessary
    if (!(registerB & 0x04)) {
        *second = (*second & 0x0F) + ((*second / 16) * 10);
        *minute = (*minute & 0x0F) + ((*minute / 16) * 10);
        *hour = ( (*hour & 0x0F) + (((*hour & 0x70) / 16) * 10) ) | (*hour & 0x80);
        *day = (*day & 0x0F) + ((*day / 16) * 10);
        *month = (*month & 0x0F) + ((*month / 16) * 10);
        *year = (*year & 0x0F) + ((*year / 16) * 10);
    }

    // Convert 12 hour clock to 24 hour clock if necessary
    if (!(registerB & 0x02) && (*hour & 0x80)) {
        *hour = ((*hour & 0x7F) + 12) % 24;
    }

    // Calculate full year
    *year += 2000;
}

static void set_rtc_register(int reg, uint8_t value) {
    outb(CMOS_ADDRESS, reg);
    outb(CMOS_DATA, value);
}

void rtc_set_datetime(int year, int month, int day, int hour, int minute, int second) {
    uint8_t registerB = get_rtc_register(0x0B);

    // Disable NMI and start update
    outb(CMOS_ADDRESS, 0x8B); 
    uint8_t prev_b = inb(CMOS_DATA);
    outb(CMOS_DATA, prev_b | 0x80); // Set SET bit to prevent updates while writing

    if (year >= 2000) year -= 2000;

    if (!(registerB & 0x04)) {
        // Convert binary to BCD
        second = ((second / 10) << 4) | (second % 10);
        minute = ((minute / 10) << 4) | (minute % 10);
        hour   = ((hour / 10) << 4) | (hour % 10);
        day    = ((day / 10) << 4) | (day % 10);
        month  = ((month / 10) << 4) | (month % 10);
        year   = ((year / 10) << 4) | (year % 10);
    }

    set_rtc_register(0x00, (uint8_t)second);
    set_rtc_register(0x02, (uint8_t)minute);
    set_rtc_register(0x04, (uint8_t)hour);
    set_rtc_register(0x07, (uint8_t)day);
    set_rtc_register(0x08, (uint8_t)month);
    set_rtc_register(0x09, (uint8_t)year);

    // Re-enable updates
    outb(CMOS_ADDRESS, 0x8B);
    outb(CMOS_DATA, prev_b & ~0x80);
}
