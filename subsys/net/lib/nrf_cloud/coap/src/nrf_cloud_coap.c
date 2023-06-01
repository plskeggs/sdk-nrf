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
#include <net/nrf_cloud.h>
#include <net/nrf_cloud_rest.h>
#include <net/nrf_cloud_agps.h>
#include <net/nrf_cloud_pgps.h>
#include "nrf_cloud_codec_internal.h"
#include "nrf_cloud_coap.h"
#include "nrf_cloud_mem.h"
#include "coap_client.h"
#include "coap_codec.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(nrf_cloud_coap, CONFIG_NRF_CLOUD_COAP_LOG_LEVEL);

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

#if defined(CONFIG_NRF_CLOUD_AGPS)
static int agps_err;

static void get_agps(int16_t result_code,
		     size_t offset, const uint8_t *payload, size_t len,
		     bool last_block, void *user_data)
{
	struct nrf_cloud_rest_agps_result *result = user_data;

	if (!result) {
		LOG_ERR("Cannot process result");
		agps_err = -EINVAL;
		return;
	}
	LOG_DBG("result_code: %d.%02d, offset:0x%X, len:0x%X, last_block:%d",
		result_code / 32u, result_code & 0x1f, offset, len, last_block);
	if (result_code != COAP_RESPONSE_CODE_CONTENT) {
		agps_err = result_code;
		return;
	}
	if (((offset + len) <= result->buf_sz) && result->buf && payload) {
		memcpy(&result->buf[offset], payload, len);
		result->agps_sz += len;
	} else {
		agps_err = -EOVERFLOW;
		return;
	}
	if (last_block) {
		agps_err = 0;
	}
}

int nrf_cloud_coap_agps(struct nrf_cloud_rest_agps_request const *const request,
			struct nrf_cloud_rest_agps_result *result)
{
	size_t len = sizeof(buffer);
	bool query_string;
	int err;

	err = coap_codec_encode_agps(request, buffer, &len, &query_string,
				     COAP_CONTENT_FORMAT_APP_CBOR);
	if (err) {
		LOG_ERR("Unable to encode A-GPS request: %d", err);
		return err;
	}

	result->agps_sz = 0;
	if (query_string) {
		err = client_get_send("loc/agps", (const char *)buffer,
				      NULL, 0, COAP_CONTENT_FORMAT_APP_CBOR,
				      COAP_CONTENT_FORMAT_APP_CBOR, get_agps, result);
	} else {
		err = client_fetch_send("loc/agps", NULL,
				      buffer, len, COAP_CONTENT_FORMAT_APP_CBOR,
				      COAP_CONTENT_FORMAT_APP_CBOR, get_agps, result);
	}

	if (!err && !agps_err) {
		LOG_INF("Got A-GPS data");
	} else if (err == -EAGAIN) {
		LOG_ERR("Timeout waiting for A-GPS data");
	} else {
		LOG_ERR("Error getting A-GPS; agps_err:%d, err:%d", agps_err, err);
		err = agps_err;
	}

	return err;
}
#endif /* CONFIG_NRF_CLOUD_AGPS */

#if defined(CONFIG_NRF_CLOUD_PGPS)
static int pgps_err;

static void get_pgps(int16_t result_code,
		     size_t offset, const uint8_t *payload, size_t len,
		     bool last_block, void *user)
{
	LOG_DBG("result_code: %d.%02d, offset:0x%X, len:0x%X, last_block:%d",
		result_code / 32u, result_code & 0x1f, offset, len, last_block);
	if (result_code != COAP_RESPONSE_CODE_CONTENT) {
		pgps_err = result_code;
	} else {
		pgps_err = coap_codec_decode_pgps_resp(user, payload, len, COAP_CONTENT_FORMAT_APP_CBOR);
	}
}

int nrf_cloud_coap_pgps(struct nrf_cloud_rest_pgps_request const *const request,
			struct nrf_cloud_pgps_result *result)
{
	size_t len = sizeof(buffer);
	bool query_string = false;
	int err;

	err = coap_codec_encode_pgps(request, buffer, &len, &query_string,
				     COAP_CONTENT_FORMAT_APP_CBOR);
	if (err) {
		LOG_ERR("Unable to encode P-GPS request: %d", err);
		return err;
	}
	if (query_string) {
		err = client_get_send("loc/pgps", (const char *)buffer,
				      NULL, 0, COAP_CONTENT_FORMAT_APP_CBOR,
				      COAP_CONTENT_FORMAT_APP_CBOR, get_pgps, result);
	} else {
		err = client_get_send("loc/pgps", NULL,
				      buffer, len, COAP_CONTENT_FORMAT_APP_CBOR,
				      COAP_CONTENT_FORMAT_APP_CBOR, get_pgps, result);
	}

	if (!err && !pgps_err) {
		LOG_INF("Got P-GPS data");
	} else if (err == -EAGAIN) {
		LOG_ERR("Timeout waiting for P-GPS data");
	} else {
		LOG_ERR("Error getting P-GPS; pgps_err:%d, err:%d", pgps_err, err);
		err = pgps_err;
	}

	return err;
}
#endif /* CONFIG_NRF_CLOUD_PGPS */

int nrf_cloud_coap_send_sensor(const char *app_id, double value)
{
	int64_t ts = get_ts();
	size_t len = sizeof(buffer);
	int err;

	err = coap_codec_encode_sensor(app_id, value, ts, buffer, &len,
				       COAP_CONTENT_FORMAT_APP_CBOR);
	if (err) {
		LOG_ERR("Unable to encode sensor data: %d", err);
		return err;
	}
	err = client_post_send("msg/d2c", NULL, buffer, len,
			       COAP_CONTENT_FORMAT_APP_CBOR, false, NULL, NULL);
	if (err) {
		LOG_ERR("Failed to send POST request: %d", err);
	}
	return err;
}

int nrf_cloud_coap_send_gnss_pvt(const struct nrf_cloud_gnss_pvt *pvt)
{
	int64_t ts = get_ts();
	size_t len = sizeof(buffer);
	int err;

	err = coap_codec_encode_pvt("GNSS", pvt, ts, buffer, &len,
				    COAP_CONTENT_FORMAT_APP_CBOR);
	if (err) {
		LOG_ERR("Unable to encode GNSS PVT data: %d", err);
		return err;
	}
	err = client_post_send("msg/d2c", NULL, buffer, len,
			       COAP_CONTENT_FORMAT_APP_CBOR, false, NULL, NULL);
	if (err) {
		LOG_ERR("Failed to send POST request: %d", err);
	}
	return err;
}

static int loc_err;

static void get_location(int16_t result_code,
			 size_t offset, const uint8_t *payload, size_t len,
			 bool last_block, void *user)
{
	LOG_DBG("result_code: %d.%02d, offset:0x%X, len:0x%X, last_block:%d",
		result_code / 32u, result_code & 0x1f, offset, len, last_block);
	if (result_code != COAP_RESPONSE_CODE_CONTENT) {
		loc_err = result_code;
	} else {
		loc_err = coap_codec_decode_ground_fix_resp(user, payload, len,
							    COAP_CONTENT_FORMAT_APP_CBOR);
	}
}

int nrf_cloud_coap_get_location(struct lte_lc_cells_info const *const cell_info,
				struct wifi_scan_info const *const wifi_info,
				struct nrf_cloud_location_result *const result)
{
	size_t len = sizeof(buffer);
	int err;

	err = coap_codec_encode_ground_fix_req(cell_info, wifi_info, buffer, &len,
					       COAP_CONTENT_FORMAT_APP_CBOR);
	if (err) {
		LOG_ERR("Unable to encode cell pos data: %d", err);
		return err;
	}
	err = client_fetch_send("loc/ground-fix", NULL, buffer, len,
				COAP_CONTENT_FORMAT_APP_CBOR,
				COAP_CONTENT_FORMAT_APP_CBOR, get_location, result);

	if (!err && !loc_err) {
		LOG_INF("Location: %d, %.12g, %.12g, %d", result->type,
			result->lat, result->lon, result->unc);
	} else if (err == -EAGAIN) {
		LOG_ERR("Timeout waiting for location");
	} else {
		LOG_ERR("Error getting location; loc_err:%d, err:%d", loc_err, err);
		err = loc_err;
	}

	return err;
}

static int fota_err;

static void get_fota(int16_t result_code,
		     size_t offset, const uint8_t *payload, size_t len,
		     bool last_block, void *user)
{
	LOG_DBG("result_code: %d.%02d, offset:0x%X, len:0x%X, last_block:%d",
		result_code / 32u, result_code & 0x1f, offset, len, last_block);
	if (result_code != COAP_RESPONSE_CODE_CONTENT) {
		fota_err = result_code;
	} else {
		LOG_INF("Got FOTA response: %.*s", len, (const char *)payload);
		fota_err = coap_codec_decode_fota_resp(user, payload, len,
						       COAP_CONTENT_FORMAT_APP_JSON);
	}
}

int nrf_cloud_coap_get_current_fota_job(struct nrf_cloud_fota_job_info *const job)
{
	int err;

	err = client_get_send("fota/exec/current", NULL, NULL, 0,
			      COAP_CONTENT_FORMAT_APP_CBOR,
			      COAP_CONTENT_FORMAT_APP_JSON, get_fota, job);

	if (!err && !fota_err) {
		LOG_INF("FOTA job received; type:%d, id:%s, host:%s, path:%s, size:%d",
			job->type, job->id, job->host, job->path, job->file_size);
	} else if (err == -EAGAIN) {
		LOG_ERR("Timeout waiting for FOTA job");
	} else {
		LOG_ERR("Error getting current FOTA job; FOTA err:%d, err:%d",
			fota_err, err);
		err = fota_err;
	}
	return err;
}

int nrf_cloud_coap_fota_job_update(const char *const job_id,
	const enum nrf_cloud_fota_status status, const char * const details)
{
	__ASSERT_NO_MSG(device_id != NULL);
	__ASSERT_NO_MSG(job_id != NULL);
	__ASSERT_NO_MSG(status < JOB_STATUS_STRING_COUNT);
#define API_FOTA_JOB_EXEC		"fota/exec"
#define API_UPDATE_FOTA_URL_TEMPLATE	(API_FOTA_JOB_EXEC "/%s")
#define API_UPDATE_FOTA_BODY_TEMPLATE	"{\"status\":\"%s\"}"
#define API_UPDATE_FOTA_DETAILS_TMPLT	"{\"status\":\"%s\", \"details\":\"%s\"}"
	/* Mapping of enum to strings for Job Execution Status. */
	static const char *const job_status_strings[] = {
		[NRF_CLOUD_FOTA_QUEUED]      = "QUEUED",
		[NRF_CLOUD_FOTA_IN_PROGRESS] = "IN_PROGRESS",
		[NRF_CLOUD_FOTA_FAILED]      = "FAILED",
		[NRF_CLOUD_FOTA_SUCCEEDED]   = "SUCCEEDED",
		[NRF_CLOUD_FOTA_TIMED_OUT]   = "TIMED_OUT",
		[NRF_CLOUD_FOTA_REJECTED]    = "REJECTED",
		[NRF_CLOUD_FOTA_CANCELED]    = "CANCELLED",
		[NRF_CLOUD_FOTA_DOWNLOADING] = "DOWNLOADING",
	};
#define JOB_STATUS_STRING_COUNT (sizeof(job_status_strings) / \
				 sizeof(*job_status_strings))

	int ret;
	size_t buff_sz;
	char *url = NULL;
	char *payload = NULL;

	/* Format API URL with device and job ID */
	buff_sz = sizeof(API_UPDATE_FOTA_URL_TEMPLATE) + strlen(job_id);
	url = nrf_cloud_malloc(buff_sz);
	if (!url) {
		ret = -ENOMEM;
		goto clean_up;
	}

	ret = snprintk(url, buff_sz, API_UPDATE_FOTA_URL_TEMPLATE, job_id);
	if ((ret < 0) || (ret >= buff_sz)) {
		LOG_ERR("Could not format URL");
		ret = -ETXTBSY;
		goto clean_up;
	}

	/* Format payload */
	if (details) {
		buff_sz = sizeof(API_UPDATE_FOTA_DETAILS_TMPLT) +
			  strlen(job_status_strings[status]) +
			  strlen(details);
	} else {
		buff_sz = sizeof(API_UPDATE_FOTA_BODY_TEMPLATE) +
			  strlen(job_status_strings[status]);
	}

	payload = nrf_cloud_malloc(buff_sz);
	if (!payload) {
		ret = -ENOMEM;
		goto clean_up;
	}

	if (details) {
		ret = snprintk(payload, buff_sz, API_UPDATE_FOTA_DETAILS_TMPLT,
			       job_status_strings[status], details);
	} else {
		ret = snprintk(payload, buff_sz, API_UPDATE_FOTA_BODY_TEMPLATE,
			       job_status_strings[status]);
	}
	if ((ret < 0) || (ret >= buff_sz)) {
		LOG_ERR("Could not format payload");
		ret = -ETXTBSY;
		goto clean_up;
	}

	ret = client_patch_send(url, NULL, payload, ret,
				COAP_CONTENT_FORMAT_APP_JSON, NULL, NULL);

clean_up:
	if (url) {
		nrf_cloud_free(url);
	}
	if (payload) {
		nrf_cloud_free(payload);
	}

	return ret;
}

struct get_shadow_data  {
	char *buf;
	size_t buf_len;
} get_shadow_data;
static int shadow_err;

static void get_shadow(int16_t result_code,
		       size_t offset, const uint8_t *payload, size_t len,
		       bool last_block, void *user)
{
	struct get_shadow_data *data = (struct get_shadow_data *)user;

	LOG_DBG("result_code: %d.%02d, offset:0x%X, len:0x%X, last_block:%d",
		result_code / 32u, result_code & 0x1f, offset, len, last_block);
	if (result_code != COAP_RESPONSE_CODE_CONTENT) {
		shadow_err = result_code;
	} else {
		shadow_err = 0;
		memcpy(data->buf, payload, MIN(data->buf_len, len));
	}
}

int nrf_cloud_coap_shadow_delta_get(char *buf, size_t buf_len)
{
	__ASSERT_NO_MSG(buf != NULL);

	get_shadow_data.buf = buf;
	get_shadow_data.buf_len = buf_len;

	return client_patch_send("state", NULL, NULL, 0,
				COAP_CONTENT_FORMAT_APP_JSON, get_shadow, &get_shadow_data);
}

int nrf_cloud_coap_shadow_state_update(const char * const shadow_json)
{
	__ASSERT_NO_MSG(shadow_json != NULL);

	return client_patch_send("state", NULL, (uint8_t *)shadow_json, strlen(shadow_json),
				 COAP_CONTENT_FORMAT_APP_JSON, NULL, NULL);
}

int nrf_cloud_coap_shadow_device_status_update(const struct nrf_cloud_device_status
					       *const dev_status)
{
	__ASSERT_NO_MSG(dev_status != NULL);

	int ret;
	struct nrf_cloud_data data_out;

	(void)nrf_cloud_codec_init(NULL);

	ret = nrf_cloud_shadow_dev_status_encode(dev_status, &data_out, false);
	if (ret) {
		LOG_ERR("Failed to encode device status, error: %d", ret);
		return ret;
	}

	ret = nrf_cloud_coap_shadow_state_update(data_out.ptr);
	if (ret) {
		LOG_ERR("Failed to update device shadow, error: %d", ret);
	}

	nrf_cloud_device_status_free(&data_out);

	return ret;
}

int nrf_cloud_coap_shadow_service_info_update(const struct nrf_cloud_svc_info * const svc_inf)
{
	if (svc_inf == NULL) {
		return -EINVAL;
	}

	const struct nrf_cloud_device_status dev_status = {
		.modem = NULL,
		.svc = (struct nrf_cloud_svc_info *)svc_inf
	};

	return nrf_cloud_coap_shadow_device_status_update(&dev_status);
}


