/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026, cc2530-zigbee contributors. See LICENSE.
 */
/* LG ESL29 (CC2530F256) with its FPC rewired to a Waveshare 4.26" SSD1677
 * panel (display.h pins). board_early_off keeps the panel unpowered and its
 * control lines low from reset. */
#include "board.h"

#define DISPLAY_POWER 0x80u
#define DISPLAY_PORT0_CONTROLS 0x3cu
#define DISPLAY_RESET 0x02u

const MCU_CODE board_description_t board_description = {BOARD_EXTERNAL, BOARD_POLICY_DISPLAY_OFF};

void board_early_off(void)
{
    /* Establish the inactive latch before either GPIO selection or direction. */
    MMIO_CLEAR(SOC_P0, DISPLAY_POWER);
    MMIO_CLEAR(SOC_APCFG, DISPLAY_POWER);
    MMIO_CLEAR(SOC_P0SEL, DISPLAY_POWER);
    MMIO_SET(SOC_P0DIR, DISPLAY_POWER);

    MMIO_CLEAR(SOC_P0, DISPLAY_PORT0_CONTROLS);
    MMIO_CLEAR(SOC_P1, DISPLAY_RESET);
    MMIO_CLEAR(SOC_APCFG, DISPLAY_PORT0_CONTROLS);
    MMIO_CLEAR(SOC_P0SEL, DISPLAY_PORT0_CONTROLS);
    MMIO_SET(SOC_P0INP, DISPLAY_POWER | DISPLAY_PORT0_CONTROLS);
    MMIO_CLEAR(SOC_P1SEL, DISPLAY_RESET);
    MMIO_SET(SOC_P0DIR, DISPLAY_PORT0_CONTROLS);
    MMIO_SET(SOC_P1DIR, DISPLAY_RESET);
}

/* The fixed LLVM link admits 45 translation units. */
#include "../src/display.c"
