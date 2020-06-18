/*
 * Copyright (c) 2018 Nordic Semiconductor ASA.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr.h>

/* The following asserts ensure compile-time consistency between the macros
 * used in board.c and the ones defined in Kconfig.
 */

BUILD_ASSERT_MSG(!IS_ENABLED(CONFIG_BOARD_APRGW_NRF52840_RESET) ||
		 IS_ENABLED(CONFIG_BOARD_APRGW_NRF52840_RESET_P0_06) ||
		 IS_ENABLED(CONFIG_BOARD_APRGW_NRF52840_RESET_P0_05) ||
		 IS_ENABLED(CONFIG_BOARD_APRGW_NRF52840_RESET_P0_26) ||
		 IS_ENABLED(CONFIG_BOARD_APRGW_NRF52840_RESET_P0_27) ||
		 IS_ENABLED(CONFIG_BOARD_APRGW_NRF52840_RESET_P0_28) ||
		 IS_ENABLED(CONFIG_BOARD_APRGW_NRF52840_RESET_P0_30) ||
		 IS_ENABLED(CONFIG_BOARD_APRGW_NRF52840_RESET_P0_03) ||
		 IS_ENABLED(CONFIG_BOARD_APRGW_NRF52840_RESET_P1_11),
		 "No reset line selected, please check Kconfig macros");
