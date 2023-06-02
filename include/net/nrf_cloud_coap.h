/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef NRF_CLOUD_COAP_H_
#define NRF_CLOUD_COAP_H_

#include <net/nrf_cloud_rest.h>
#include <net/nrf_cloud_agps.h>
#include <net/nrf_cloud_pgps.h>

int nrf_cloud_coap_init(const char *device_id);

int nrf_cloud_coap_agps(struct nrf_cloud_rest_agps_request const *const request,
			struct nrf_cloud_rest_agps_result *result);

int nrf_cloud_coap_pgps(struct nrf_cloud_rest_pgps_request const *const request,
			struct nrf_cloud_pgps_result *result);

int nrf_cloud_coap_send_sensor(const char *app_id, double value);

int nrf_cloud_coap_send_gnss_pvt(const struct nrf_cloud_gnss_pvt *pvt);

int nrf_cloud_coap_get_location(struct lte_lc_cells_info const *const cell_info,
				struct wifi_scan_info const *const wifi_info,
				struct nrf_cloud_location_result *const result);

int nrf_cloud_coap_get_current_fota_job(struct nrf_cloud_fota_job_info *const job);
int nrf_cloud_coap_fota_job_update(const char *const job_id,
	const enum nrf_cloud_fota_status status, const char * const details);

int nrf_cloud_coap_shadow_delta_get(char *buf, size_t buf_len);
int nrf_cloud_coap_shadow_state_update(const char * const shadow_json);
int nrf_cloud_coap_shadow_device_status_update(const struct nrf_cloud_device_status
					       *const dev_status);
int nrf_cloud_coap_shadow_service_info_update(const struct nrf_cloud_svc_info * const svc_inf);

#endif /* NRF_CLOUD_COAP_H_ */
