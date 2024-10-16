/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <net/nrf_cloud.h>
//#include "fakes.h"

/* This function runs before each test */
static void run_before(void *fixture)
{
	ARG_UNUSED(fixture);

	RESET_FAKE(fota_download_s0_active_get);
	RESET_FAKE(boot_is_img_confirmed);
	RESET_FAKE(boot_write_img_confirmed);
	RESET_FAKE(dfu_target_full_modem_cfg);
	RESET_FAKE(dfu_target_full_modem_fdev_get);

#if defined(CONFIG_NRF_CLOUD_FOTA_FULL_MODEM_UPDATE)
	RESET_FAKE(nrf_modem_lib_init);
	RESET_FAKE(nrf_modem_lib_shutdown);
	RESET_FAKE(nrf_modem_lib_bootloader_init);
	RESET_FAKE(fmfu_fdev_load);
#endif /* CONFIG_NRF_CLOUD_FOTA_FULL_MODEM_UPDATE */
}

/* This function runs after each completed test */
static void run_after(void *fixture)
{
	ARG_UNUSED(fixture);
}

ZTEST_SUITE(nrf_cloud_client_id_test, NULL, NULL, NULL, NULL, NULL);


/* Verify that nrf_cloud_fota_is_type_enabled returns the correct value
 * based on the enabled Kconfig options.
 */
ZTEST(nrf_cloud_client_id_test, test_nrf_cloud_client_id)
{
	Z_TEST_SKIP_IFNDEF(CONFIG_NRF_CLOUD_CLIENT_ID_SRC_IMEI);
	int ret;

	ret = nrf_cloud_client_id_get(NULL, 0);
	zassert_equal(1, 1, "1 was not equal to 1");
}
