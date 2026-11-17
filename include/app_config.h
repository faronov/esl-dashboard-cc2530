/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026, cc2530-zigbee contributors. See LICENSE.
 */
#ifndef ESL_DASHBOARD_APP_CONFIG_H
#define ESL_DASHBOARD_APP_CONFIG_H
/* cc2530-zigbee external application configuration (macros only; included by
 * the stack's cc2530_mmio.h for CC2530_APP, board BOARD_EXTERNAL). */

/* The identity the stack's LG ESL29 board reported; the ZHA quirk
 * (tools/ha/zha_quirk_lg_esl29_l8.py) matches on these strings. */
#define BOARD_MANUFACTURER "cc2530-zigbee"
#define BOARD_MODEL "LG-ESL29"
#define BOARD_MODEL_LENGTH 8u

#endif
