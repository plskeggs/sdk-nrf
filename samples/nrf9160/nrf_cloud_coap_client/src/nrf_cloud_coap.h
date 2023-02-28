/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef NRF_CLOUD_COAP_H_
#define NRF_CLOUD_COAP_H_

int nrf_cloud_coap_init(const char *device_id);

int nrf_cloud_coap_send_sensor(const char *app_id, double value);

int nrf_cloud_coap_get_location(struct lte_lc_cells_info *cell_info,
				struct nrf_cloud_location_result *const result);

int nrf_cloud_get_current_fota_job(struct nrf_cloud_fota_job_info *const job);

#endif /* NRF_CLOUD_COAP_H_ */
