// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#ifndef RTC_H
#define RTC_H

#include <stdint.h>

void rtc_get_datetime(int *year, int *month, int *day, int *hour, int *minute, int *second);
void rtc_set_datetime(int year, int month, int day, int hour, int minute, int second);

#endif
