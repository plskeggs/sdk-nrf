/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/net/coap.h>
#include <date_time.h>
#include <dk_buttons_and_leds.h>
#include <net/nrf_cloud_rest.h>
#include "nrf_cloud_codec.h"
#include "nrf_cloud_coap.h"
#include "coap_client.h"
#include "coap_codec.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(nrf_cloud_coap, CONFIG_NRF_CLOUD_COAP_CLIENT_LOG_LEVEL);

static uint8_t buffer[500];
static char topic[100];

int nrf_cloud_coap_init(const char *device_id)
{
	snprintf(topic, sizeof(topic) - 1, "d/%s/d2c", device_id);
	return 0;
}

static int64_t get_ts(void)
{
	int64_t ts;
	int err;

	ts = k_uptime_get();
	err = date_time_uptime_to_unix_time_ms(&ts);
	if (err) {
		LOG_ERR("Error converting time: %d", err);
		ts = 0;
	}
	return ts;
}

int nrf_cloud_coap_agps(struct nrf_cloud_rest_agps_request const *const request)
{
	size_t len = sizeof(buffer);
	bool query_string;
	int err;

	err = coap_codec_encode_agps(request, buffer, &len, &query_string,
				     COAP_CONTENT_FORMAT_APP_JSON);
	if (err) {
		LOG_ERR("Unable to encode A-GPS request: %d", err);
		return err;
	}
	if (query_string) {
		err = client_get_send("poc/loc/agps", (const char *)buffer,
				      NULL, 0, COAP_CONTENT_FORMAT_APP_JSON,
				      COAP_CONTENT_FORMAT_APP_JSON);
	} else {
		err = client_get_send("poc/loc/agps", NULL,
				      buffer, len, COAP_CONTENT_FORMAT_APP_JSON,
				      COAP_CONTENT_FORMAT_APP_JSON);
	}
	if (err) {
		LOG_ERR("Failed to send GET request: %d", err);
	}
	return err;
}

int nrf_cloud_coap_send_sensor(const char *app_id, double value)
{
	int64_t ts = get_ts();
	size_t len = sizeof(buffer);
	int err;

	err = coap_codec_encode_sensor(app_id, value,
				       topic, ts, buffer, &len,
				       COAP_CONTENT_FORMAT_APP_JSON);
	if (err) {
		LOG_ERR("Unable to encode sensor data: %d", err);
		return err;
	}
	err = client_post_send("poc/msg", NULL, buffer, len,
			     COAP_CONTENT_FORMAT_APP_JSON);
	if (err) {
		LOG_ERR("Failed to send POST request: %d", err);
	}
	return err;
}

int nrf_cloud_coap_get_location(struct lte_lc_cells_info const *const cell_info,
				struct wifi_scan_info const *const wifi_info,
				struct nrf_cloud_location_result *const result)
{
	size_t len = sizeof(buffer);
	int err;

	err = coap_codec_encode_location_req(cell_info, wifi_info, buffer, &len,
					     COAP_CONTENT_FORMAT_APP_JSON);
	if (err) {
		LOG_ERR("Unable to encode cell pos data: %d", err);
		return err;
	}
	err = client_fetch_send("poc/loc/ground-fix", NULL, buffer, len,
				COAP_CONTENT_FORMAT_APP_JSON,
				COAP_CONTENT_FORMAT_APP_JSON);
	if (err) {
		LOG_ERR("Failed to send POST request: %d", err);
	}

	/* Once we switch to "official" coap_client library, wait for
	 * result and return it; for now, return nothing
	 */
	ARG_UNUSED(result);
	return err;
}


int nrf_cloud_get_current_fota_job(struct nrf_cloud_fota_job_info *const job)
{
	int err;

	err = client_get_send("poc/fota/exec/current", NULL, NULL, 0,
			      COAP_CONTENT_FORMAT_APP_JSON,
			      COAP_CONTENT_FORMAT_APP_JSON);
	if (err) {
		LOG_ERR("Failed to send GET request: %d", err);
	}

	/* Once we switch to "official" coap_client library, wait for
	 * result and return it; for now, return nothing
	 */
	ARG_UNUSED(job);
	return err;
}
