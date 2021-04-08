/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr.h>
#include <device.h>
#include <drivers/gps.h>
#include <storage/stream_flash.h>
#include <net/socket.h>
#include <nrf_socket.h>
#include <pm_config.h>

#include <cJSON.h>
#include <cJSON_os.h>
#include <modem/modem_info.h>
#include <date_time.h>
#include <net/nrf_cloud_agps.h>
#include <net/nrf_cloud_pgps.h>
#include <settings/settings.h>

#include <logging/log.h>

LOG_MODULE_REGISTER(nrf_cloud_pgps, 3); // CONFIG_NRF_CLOUD_GPS_LOG_LEVEL);

#include "nrf_cloud_transport.h"
#include "nrf_cloud_pgps_schema_v1.h"

/* (6.1.1980 UTC - 1.1.1970 UTC) */
#define GPS_TO_UNIX_UTC_OFFSET_SECONDS (315964800UL)
#define GPS_TO_UTC_LEAP_SECONDS (18UL)
#define SEC_PER_MIN (60UL)
#define MIN_PER_HOUR (60UL)
#define SEC_PER_HOUR (MIN_PER_HOUR * SEC_PER_MIN)
#define HOURS_PER_DAY (24UL)
#define SEC_PER_DAY (HOURS_PER_DAY * SEC_PER_HOUR)
#define DAYS_PER_WEEK (7UL)
#define SECONDS_PER_WEEK (SEC_PER_DAY * DAYS_PER_WEEK)
#define PGPS_MARGIN_SEC SEC_PER_HOUR

#define PGPS_JSON_APPID_KEY		"appId"
#define PGPS_JSON_APPID_VAL_PGPS	"PGPS"
#define PGPS_JSON_MSG_TYPE_KEY		"messageType"
#define PGPS_JSON_MSG_TYPE_VAL_DATA	"DATA"
#define PGPS_JSON_DATA_KEY		"data"

#define PGPS_JSON_PRED_COUNT		"predictionCount"
#define PGPS_JSON_PRED_INT_MIN		"predictionIntervalMinutes"
#define PGPS_JSON_GPS_DAY		"startGpsDay"
#define PGPS_JSON_GPS_TIME		"startGpsTimeOfDaySeconds"

#if defined(CONFIG_PGPS_INCLUDE_MODEM_INFO)
#define PGPS_JSON_MCC_KEY		"mcc"
#define PGPS_JSON_MNC_KEY		"mnc"
#define PGPS_JSON_AREA_CODE_KEY		"tac"
#define PGPS_JSON_CELL_ID_KEY		"eci"
#define PGPS_JSON_PHYCID_KEY		"phycid"
#define PGPS_JSON_TYPES_KEY		"types"
#define PGPS_JSON_CELL_LOC_KEY_DOREPLY	"doReply"
#define PGPS_JSON_APPID_VAL_SINGLE_CELL	"SCELL"
#define PGPS_JSON_APPID_VAL_MULTI_CELL	"MCELL"
#define PGPS_JSON_CELL_LOC_KEY_LAT	"lat"
#define PGPS_JSON_CELL_LOC_KEY_LON	"lon"
#define PGPS_JSON_CELL_LOC_KEY_UNCERT	"uncertainty"
#endif

#define USE_TEST_GPS_DAY

enum pgps_state {
	PGPS_NONE,
	PGPS_EXPIRED,
	PGPS_REQUESTING,
	PGPS_LOADING,
	PGPS_READY,
};
static enum pgps_state state;

static bool pgps_partial_request;
static uint16_t test_gps_day;
static uint8_t test_pnum_offset;

static struct nrf_cloud_pgps_header *pgps_storage =
      (struct nrf_cloud_pgps_header *)PM_MCUBOOT_SECONDARY_ADDRESS;
static struct stream_flash_ctx stream;
static uint8_t write_buf[4096];

/* todo: get the correct values from somewhere */
static int gps_leap_seconds = GPS_TO_UTC_LEAP_SECONDS;

struct gps_location {
	int32_t latitude;
	int32_t longitude;
	int64_t gps_sec;
};

static struct gps_location saved_location;

//static uint16_t end_gps_day;
//static uint16_t end_gps_time_of_day;

static bool json_initialized;

static int nrf_cloud_pgps_get_time(int64_t *gps_sec, uint16_t *gps_day,
			    uint32_t *gps_time_of_day);
static int settings_set(const char *key, size_t len_rd,
			settings_read_cb read_cb, void *cb_arg);
static bool validate_stored_pgps_header(void);
static int validate_stored_predictions(uint16_t *bad_day, uint32_t *bad_time);

#define SETTINGS_NAME "nrf_cloud_pgps"
#define SETTINGS_KEY_LOCATION "location"
#define SETTINGS_FULL_LOCATION SETTINGS_NAME "/" SETTINGS_KEY_LOCATION
#define SETTINGS_KEY_LEAP_SEC "g2u_leap_sec"
#define SETTINGS_FULL_LEAP_SEC SETTINGS_NAME "/" SETTINGS_KEY_LEAP_SEC

SETTINGS_STATIC_HANDLER_DEFINE(nrf_cloud_pgps, SETTINGS_NAME, NULL, settings_set,
			       NULL, NULL);

static int settings_set(const char *key, size_t len_rd,
			settings_read_cb read_cb, void *cb_arg)
{
	if (!key) {
		return -EINVAL;
	}

	LOG_DBG("Settings key: %s, size: %d", log_strdup(key), len_rd);

	if (!strncmp(key, SETTINGS_KEY_LOCATION,
		     strlen(SETTINGS_KEY_LOCATION)) &&
	    (len_rd == sizeof(saved_location))) {
		if (read_cb(cb_arg, (void *)&saved_location, len_rd) == len_rd) {
			LOG_DBG("Read location: %d, %d, gps sec:%lld",
				saved_location.latitude, saved_location.longitude,
				saved_location.gps_sec);
			return 0;
		}
	}
	if (!strncmp(key, SETTINGS_KEY_LEAP_SEC,
		     strlen(SETTINGS_KEY_LEAP_SEC)) &&
	    (len_rd == sizeof(gps_leap_seconds))) {
		if (read_cb(cb_arg, (void *)&gps_leap_seconds, len_rd) == len_rd) {
			LOG_DBG("Read gps to utc leap seconds offset: %d",
				gps_leap_seconds);
			return 0;
		}
	}
	return -ENOTSUP;
}

/* @TODO: consider rate-limiting these updates to reduce Flash wear */
static int save_location(void)
{
	int ret = 0;

	LOG_DBG("Saving location: %d, %d",
		saved_location.latitude, saved_location.longitude);
	ret = settings_save_one(SETTINGS_FULL_LOCATION,
				&saved_location, sizeof(saved_location));
	return ret;
}

static int save_leap_sec(void)
{
	int ret = 0;

	LOG_DBG("Saving gps to utc leap seconds offset: %d", gps_leap_seconds);
	ret = settings_save_one(SETTINGS_FULL_LEAP_SEC,
				&gps_leap_seconds, sizeof(gps_leap_seconds));
	return ret;
}

static int settings_init(void)
{
	int ret = 0;

	ret = settings_subsys_init();
	if (ret) {
		LOG_ERR("Settings init failed: %d", ret);
		return ret;
	}
	ret = settings_load_subtree(settings_handler_nrf_cloud_pgps.name);
	if (ret) {
		LOG_ERR("Cannot load settings: %d", ret);
	}
	return ret;
}

void nrf_cloud_set_location_normalized(int32_t latitude, int32_t longitude)
{
	int64_t sec;

	if (nrf_cloud_pgps_get_time(&sec, NULL, NULL)) {
		sec = saved_location.gps_sec; /* could not get time; use prev */
	}

	if ((latitude != saved_location.latitude) ||
	    (longitude != saved_location.longitude) ||
	    (sec != saved_location.gps_sec)) {
		saved_location.latitude = latitude;
		saved_location.longitude = longitude;
		saved_location.gps_sec = sec;
		save_location();
	}
}

void nrf_cloud_set_location(double latitude, double longitude)
{
	int32_t lat;
	int32_t lng;

	lat = (int32_t)((latitude / 90.0) * (1 << 23));
	lng = (int32_t)((longitude / 360.0) * (1 << 24));

	nrf_cloud_set_location_normalized(lat, lng);
}

void nrf_cloud_set_leap_seconds(int leap_seconds)
{
	if (gps_leap_seconds != leap_seconds) {
		gps_leap_seconds = leap_seconds;
		save_leap_sec();
	}
}

int64_t nrf_cloud_utc_to_gps_sec(const int64_t utc, int16_t *gps_time_ms)
{
	int64_t utc_sec;
	int64_t gps_sec;

	utc_sec = utc / MSEC_PER_SEC;
	if (gps_time_ms) {
		*gps_time_ms = (uint16_t)(utc - (utc_sec * MSEC_PER_SEC));
	}
	gps_sec = (utc_sec - GPS_TO_UNIX_UTC_OFFSET_SECONDS) + gps_leap_seconds;

	LOG_DBG("Converted UTC sec %lld to GPS sec %lld", utc_sec, gps_sec);
	return gps_sec;
}

static int64_t gps_day_time_to_sec(uint16_t gps_day, uint32_t gps_time_of_day)
{
	int64_t gps_sec = (int64_t)gps_day * SEC_PER_DAY + gps_time_of_day;

	LOG_DBG("Converted GPS day %u and time of day %u to GPS sec %lld",
		gps_day, gps_time_of_day, gps_sec);
	return gps_sec;
}

static void gps_sec_to_day_time(int64_t gps_sec, uint16_t *gps_day,
				uint32_t *gps_time_of_day)
{
	*gps_day = (uint16_t)(gps_sec / SEC_PER_DAY);
	*gps_time_of_day = (uint32_t)(gps_sec - (*gps_day * SEC_PER_DAY));
	LOG_DBG("Converted GPS sec %lld to day %u, time of day %u, week %u",
		gps_sec, *gps_day, *gps_time_of_day,
		(uint16_t)(*gps_day / DAYS_PER_WEEK));
}

static int nrf_cloud_pgps_get_time(int64_t *gps_sec, uint16_t *gps_day,
			    uint32_t *gps_time_of_day)
{
	int64_t now;
	int err;

	err = date_time_now(&now);
	if (!err) {
		now = nrf_cloud_utc_to_gps_sec(now, NULL);
#if defined(USE_TEST_GPS_DAY)
		uint16_t test_day;
		uint32_t test_time;

		if (test_gps_day) {
			gps_sec_to_day_time(now, &test_day, &test_time);
			test_day = test_gps_day; /* override with fake day */
			now = gps_day_time_to_sec(test_day, test_time);
		}
#endif
		if ((gps_day != NULL) && (gps_time_of_day != NULL)) {
			gps_sec_to_day_time(now, gps_day, gps_time_of_day);
		}
		if (gps_sec != NULL) {
			*gps_sec = now;
		}
	}
	return err;
}

static int validate_prediction(struct nrf_cloud_pgps_prediction *p,
			       uint16_t gps_day,
			       uint32_t gps_time_of_day,
			       uint16_t period_min,
			       bool exact)
{
	int err = 0;

	/* validate that this prediction was actually updated and matches */
	if ((p->schema_version != NRF_CLOUD_AGPS_BIN_SCHEMA_VERSION) ||
	    (p->time_type != NRF_CLOUD_AGPS_GPS_SYSTEM_CLOCK) ||
	    (p->time_count != 1) ||
	    (p->time.date_day != gps_day)) {
		err = -EINVAL;
	} else if (exact && (p->time.time_full_s != gps_time_of_day)) {
		err = -EINVAL;
	} else if (p->time.time_full_s > gps_time_of_day) {
		err = -EINVAL;
	} else if (gps_time_of_day >
		 (p->time.time_full_s +
		  ((period_min + 1) * SEC_PER_MIN))) {
		/* even with an extra minute of slop, the current time of
		 * day is past the end of this prediction
		 */
		err = -EINVAL;
	}

	if ((p->ephemeris_type != NRF_CLOUD_AGPS_EPHEMERIDES) ||
	    (p->ephemeris_count != NRF_CLOUD_PGPS_NUM_SV)) {
		err = -EINVAL;
	}

	if (exact) {
		uint32_t expected_sentinel;
		uint32_t stored_sentinel;

		expected_sentinel = gps_day_time_to_sec(gps_day, gps_time_of_day);
		stored_sentinel = p->sentinel;
		if (expected_sentinel != stored_sentinel) {
			LOG_ERR("prediction at %p has stored_sentinel:0x%08X, "
				"expected:0x%08X", p, stored_sentinel,
				expected_sentinel);
			err = -EINVAL;
		}
	}
	return err;
}

int nrf_cloud_find_prediction(struct nrf_cloud_pgps_prediction **prediction)
{
	int64_t cur_gps_sec;
	int64_t offset_sec;
	int64_t start_sec;
	int64_t end_sec;
	uint16_t cur_gps_day;
	uint32_t cur_gps_time_of_day;
	uint16_t start_day = pgps_storage->gps_day;
	uint32_t start_time = pgps_storage->gps_time_of_day;
	uint16_t period_min = pgps_storage->prediction_period_min;
	uint16_t count = pgps_storage->prediction_count;
	int err;
	int pnum;

	err = nrf_cloud_pgps_get_time(&cur_gps_sec, &cur_gps_day,
				      &cur_gps_time_of_day);
	if (err) {
		LOG_INF("Unknown current time");
		cur_gps_sec = 0;
	}

	LOG_INF("Looking for current gps_sec:%llu, day:%u, time:%u",
		cur_gps_sec, cur_gps_day, cur_gps_time_of_day);

	start_sec = gps_day_time_to_sec(start_day, start_time);
	offset_sec = cur_gps_sec - start_sec;
	end_sec = start_sec + period_min * SEC_PER_MIN * count;

	LOG_INF("First stored gps_sec:%llu, day:%u, time:%u",
		start_sec, start_day, start_time);

	if (offset_sec < 0) {
		/* current time must be unknown or very inaccurate; it
		 * falls before the first valid prediction; default to
		 * first entry and day and time for it
		 */
		err = EAPPROXIMATE;
		pnum = 0;
		if (saved_location.gps_sec > start_sec) {
			/* our most recently-stored location time is
			 * better, so use that
			 */
			gps_sec_to_day_time(saved_location.gps_sec, &cur_gps_day,
					    &cur_gps_time_of_day);
		} else {
			cur_gps_day = start_day;
			cur_gps_time_of_day = start_time;
		}
	} else if (cur_gps_sec > end_sec) {
		if ((cur_gps_sec - end_sec) > PGPS_MARGIN_SEC) {
			return -ETIMEDOUT;
		}
		/* data is expired; use most recent entry */
		pnum = count - 1;
	} else {
		pnum = offset_sec / (SEC_PER_MIN * period_min);
	}

	*prediction = &pgps_storage->predictions[pnum];
	return validate_prediction(*prediction, cur_gps_day, cur_gps_time_of_day,
				   period_min, false);
}

static cJSON *json_create_req_obj(const char *const app_id,
				   const char *const msg_type)
{
	__ASSERT_NO_MSG(app_id != NULL);
	__ASSERT_NO_MSG(msg_type != NULL);

	if (!json_initialized) {
		cJSON_Init();
		json_initialized = true;
	}

	cJSON *resp_obj = cJSON_CreateObject();

	if (!cJSON_AddStringToObject(resp_obj,
				     PGPS_JSON_APPID_KEY,
				     app_id) ||
	    !cJSON_AddStringToObject(resp_obj,
				     PGPS_JSON_MSG_TYPE_KEY,
				     msg_type)) {
		cJSON_Delete(resp_obj);
		resp_obj = NULL;
	}

	return resp_obj;
}

static int json_send_to_cloud(cJSON *const pgps_request)
{
	__ASSERT_NO_MSG(pgps_request != NULL);

	char *msg_string;
	int err;

	msg_string = cJSON_PrintUnformatted(pgps_request);
	if (!msg_string) {
		LOG_ERR("Could not allocate memory for P-GPS request message");
		return -ENOMEM;
	}

	LOG_DBG("Created P-GPS request: %s", log_strdup(msg_string));

	struct nct_dc_data msg = {
		.data.ptr = msg_string,
		.data.len = strlen(msg_string)
	};

	err = nct_dc_send(&msg);
	if (err) {
		/* @TODO: if device is offline, we need to defer this to later */
		LOG_ERR("Failed to send P-GPS request, error: %d", err);
	} else {
		LOG_DBG("P-GPS request sent");
	}

	k_free(msg_string);

	return err;
}

int nrf_cloud_pgps_request(const struct gps_pgps_request *request)
{
	int err = 0;
	cJSON *data_obj;
	cJSON *pgps_req_obj;
	cJSON *ret;

	/* Create request JSON containing a data object */
	pgps_req_obj = json_create_req_obj(PGPS_JSON_APPID_VAL_PGPS,
					   PGPS_JSON_MSG_TYPE_VAL_DATA);
	data_obj = cJSON_AddObjectToObject(pgps_req_obj, PGPS_JSON_DATA_KEY);

	if (!pgps_req_obj || !data_obj) {
		err = -ENOMEM;
		goto cleanup;
	}

#if defined(CONFIG_PGPS_INCLUDE_MODEM_INFO)
	/* Add modem info and P-GPS types to the data object */
	err = json_add_modem_info(data_obj);
	if (err) {
		LOG_ERR("Failed to add modem info to P-GPS request: %d", err);
		goto cleanup;
	}
#endif
	if (request->prediction_count < CONFIG_NRF_CLOUD_PGPS_NUM_PREDICTIONS) {
		pgps_partial_request = true;
	} else {
		pgps_partial_request = false;
	}

	ret = cJSON_AddNumberToObject(data_obj, PGPS_JSON_PRED_COUNT,
				      request->prediction_count);
	if (ret == NULL) {
		LOG_ERR("Failed to add pred count to P-GPS request %d", err);
		goto cleanup;
	}
	ret = cJSON_AddNumberToObject(data_obj, PGPS_JSON_PRED_INT_MIN,
				      request->prediction_period_min);
	if (ret == NULL) {
		LOG_ERR("Failed to add pred int min to P-GPS request %d", err);
		goto cleanup;
	}
	ret = cJSON_AddNumberToObject(data_obj, PGPS_JSON_GPS_DAY,
				      request->gps_day);
	if (ret == NULL) {
		LOG_ERR("Failed to add gps day to P-GPS request %d", err);
		goto cleanup;
	}
	ret = cJSON_AddNumberToObject(data_obj, PGPS_JSON_GPS_TIME,
				      request->gps_time_of_day);
	if (ret == NULL) {
		LOG_ERR("Failed to add gps time to P-GPS request %d", err);
		goto cleanup;
	}

	err = json_send_to_cloud(pgps_req_obj);

cleanup:
	cJSON_Delete(pgps_req_obj);

	return err;
}

int nrf_cloud_pgps_request_all(void)
{
	uint16_t gps_day;
	uint32_t gps_time_of_day;
	int err;

	err = nrf_cloud_pgps_get_time(NULL, &gps_day, &gps_time_of_day);
	if (err) {
		/* unknown time; ask server for newest data */
		gps_day = 0;
		gps_time_of_day = 0;
	}

	struct gps_pgps_request request = {
		.prediction_count = CONFIG_NRF_CLOUD_PGPS_NUM_PREDICTIONS,
		.prediction_period_min = CONFIG_NRF_CLOUD_PGPS_PREDICTION_PERIOD,
		.gps_day = gps_day,
		.gps_time_of_day = gps_time_of_day
	};

	return nrf_cloud_pgps_request(&request);
}

static size_t get_next_pgps_element(struct nrf_cloud_apgs_element *element,
				    const char *buf)
{
	static uint16_t elements_left_to_process;
	static enum nrf_cloud_agps_type element_type;
	size_t len = 0;

	/* Check if there are more elements left in the array to process.
	 * The element type is only given once before the array, and not for
	 * each element.
	 */
	if (elements_left_to_process == 0) {
		element->type =
			(enum nrf_cloud_agps_type)buf[NRF_CLOUD_AGPS_BIN_TYPE_OFFSET];
		element_type = element->type;
		elements_left_to_process =
			*(uint16_t *)&buf[NRF_CLOUD_AGPS_BIN_COUNT_OFFSET] - 1;
		len += NRF_CLOUD_AGPS_BIN_TYPE_SIZE +
			NRF_CLOUD_AGPS_BIN_COUNT_SIZE;
		LOG_INF("  New element type:%u, count:%u", element_type, elements_left_to_process + 1);
	} else {
		element->type = element_type;
		elements_left_to_process -= 1;
	}

	switch (element->type) {
	case NRF_CLOUD_AGPS_EPHEMERIDES:
		element->ephemeris = (struct nrf_cloud_agps_ephemeris *)(buf + len);
		len += sizeof(struct nrf_cloud_agps_ephemeris);
		break;
	case NRF_CLOUD_AGPS_GPS_SYSTEM_CLOCK:
		element->time_and_tow =
			(struct nrf_cloud_agps_system_time *)(buf + len);
		len += sizeof(struct nrf_cloud_agps_system_time) -
			sizeof(element->time_and_tow->sv_tow) + 4;
		break;
	case NRF_CLOUD_AGPS_UTC_PARAMETERS:
		element->utc = (struct nrf_cloud_agps_utc *)(buf + len);
		len += sizeof(struct nrf_cloud_agps_utc);
		break;
	case NRF_CLOUD_AGPS_LOCATION:
		element->location = (struct nrf_cloud_agps_location *)(buf + len);
		len += sizeof(struct nrf_cloud_agps_location);
		break;
	default:
		LOG_DBG("Unhandled P-GPS data type: %d", element->type);
		return 0;
	}

	LOG_DBG("  size:%zd, count:%u", len, elements_left_to_process);
	return len;
}

static int open_storage(uint32_t offset)
{
	int err;
	const struct device *flash_dev;
	uint32_t block_size;
	uint32_t block_offset = 0;

	flash_dev = device_get_binding(PM_MCUBOOT_SECONDARY_DEV_NAME);
	if (flash_dev == NULL) {
		LOG_ERR("Failed to get device '%s'",
			PM_MCUBOOT_SECONDARY_DEV_NAME);
		return -EFAULT;
	}

	/* @TODO: get the size of the flash pages */
	block_size = MAX(4096, flash_get_write_block_size(flash_dev));
	block_offset = offset % block_size;
	LOG_INF("Flash write block size: %u, block offset %u",
		block_size, block_offset);
	if (block_offset != 0) {
		offset -= block_offset;
	}

	err = stream_flash_init(&stream, flash_dev,
				write_buf, sizeof(write_buf),
				PM_MCUBOOT_SECONDARY_ADDRESS + offset,
				PM_MCUBOOT_SECONDARY_SIZE - offset, NULL);
	if (err) {
		LOG_ERR("Failed to init flash stream for offset %u: %d",
			offset, err);
		state = PGPS_NONE;
	} else {
		uint8_t *p = (uint8_t *)(PM_MCUBOOT_SECONDARY_ADDRESS + offset);

		err = stream_flash_buffered_write(&stream, p, block_offset, false);
		if (err) {
			LOG_ERR("Error writing back %u original bytes", block_offset);
		}
	}
	return err;
}

static int store_prediction_header(struct nrf_cloud_pgps_header *header)
{
	int err;

	if (header == NULL) {
		return -EINVAL;
	}

	err = stream_flash_buffered_write(&stream, (uint8_t *)header,
					  sizeof(*header), false);
	if (err) {
		LOG_ERR("Error writing pgps header: %d", err);
	}
	return err;
}

static int store_prediction(uint8_t *p, size_t len, int64_t sentinel, bool last)
{
	int err;
	uint8_t schema = NRF_CLOUD_AGPS_BIN_SCHEMA_VERSION;
	size_t schema_offset = ((size_t) &((struct nrf_cloud_pgps_prediction *)0)->schema_version);

	err = stream_flash_buffered_write(&stream, p, schema_offset, false);
	if (err) {
		LOG_ERR("Error writing pgps prediction: %d", err);
		return err;
	}
	p += schema_offset;
	len -= schema_offset;
	err = stream_flash_buffered_write(&stream, &schema, sizeof(schema), false);
	if (err) {
		LOG_ERR("Error writing schema: %d", err);
		return err;
	}
	err = stream_flash_buffered_write(&stream, p, len, false);
	if (err) {
		LOG_ERR("Error writing pgps prediction: %d", err);
		return err;
	}
	err = stream_flash_buffered_write(&stream, (uint8_t *)&sentinel,
					  sizeof(sentinel), last);
	if (err) {
		LOG_ERR("Error writing sentinel: %d", err);
	}
	return err;
}

int nrf_cloud_pgps_inject(struct nrf_cloud_pgps_prediction *p, const int *socket)
{
	struct pgps_sys_time sys_time;
	struct pgps_location location;
	uint16_t day;
	uint32_t sec;
	int err;
	int ret = 0;

	/* start with time this prediction starts */
	sys_time.schema_version = NRF_CLOUD_AGPS_BIN_SCHEMA_VERSION;
	sys_time.type = NRF_CLOUD_AGPS_GPS_SYSTEM_CLOCK;
	sys_time.count = 1;
	sys_time.time.date_day = p->time.date_day;
	sys_time.time.time_full_s = p->time.time_full_s;
	sys_time.time.time_frac_ms = 0;
	sys_time.time.sv_mask = 0;

	/* use current time if available */
	err = nrf_cloud_pgps_get_time(NULL, &day, &sec);
	if (!err) {
		sys_time.time.date_day = day;
		sys_time.time.time_full_s = sec;
		/* it is ok to ignore this err in the function return below */
	}

	LOG_INF("Injecting GPS_SYSTEM_CLOCK day:%u, time:%u",
		sys_time.time.date_day, sys_time.time.time_full_s);

	/* send time */
	err = nrf_cloud_agps_process((const char *)&sys_time,
				     sizeof(sys_time) - sizeof(sys_time.time.sv_tow),
				     socket);
	if (err) {
		LOG_ERR("Error injecting PGPS sys_time (%u, %u): %d",
			sys_time.time.date_day, sys_time.time.time_full_s,
			err);
		ret = err;
	}

	if (saved_location.gps_sec) {
		location.schema_version = NRF_CLOUD_AGPS_BIN_SCHEMA_VERSION;
		location.type = NRF_CLOUD_AGPS_LOCATION;
		location.count = 1;
		location.location.latitude = saved_location.latitude;
		location.location.longitude = saved_location.longitude;
		location.location.altitude = 0;
		location.location.unc_semimajor = 0;
		location.location.unc_semiminor = 0;
		location.location.orientation_major = 0;
		location.location.unc_altitude = 0xFF; /* tell modem it is invalid */
		location.location.confidence = 0;

		LOG_INF("Injecting LOCATION %u, %u",
			saved_location.latitude,
			saved_location.longitude);
		/* send location */

		err = nrf_cloud_agps_process((const char *)&location, sizeof(location),
					     socket);
		if (err) {
			LOG_ERR("Error injecting PGPS location (%u, %u): %d",
				location.location.latitude, location.location.longitude,
				err);
			ret = err;
		}
	} else {
		LOG_WRN("Location unknown!");
	}

	LOG_INF("Injecting %u EPHEMERIDES", p->ephemeris_count);

	/* send ephemerii */
	err = nrf_cloud_agps_process((const char *)&p->schema_version,
				     sizeof(p->schema_version) +
				     sizeof(p->ephemeris_type) +
				     sizeof(p->ephemeris_count) +
				     sizeof(p->ephemerii), socket);
	if (err) {
		LOG_ERR("Error injecting ephermerii: %d", err);
		ret = err;
	}
	return ret; /* just return last non-zero error, if any */
}

/* handle incoming MQTT packets */
int nrf_cloud_pgps_process(const char *buf, size_t buf_len)
{
	static uint16_t expected_count;
	static uint16_t pcount;
	static uint16_t period_sec;
	static uint8_t pnum;
	static uint8_t prev_pnum;
	static uint8_t pnum_offset;
	static bool prediction_present[CONFIG_NRF_CLOUD_PGPS_NUM_PREDICTIONS];
	int err;
	struct nrf_cloud_apgs_element element = {};
	struct nrf_cloud_pgps_header *header = NULL;
	size_t parsed_len = 0;
	uint8_t *prediction_ptr;
	int64_t gps_sec;
	uint32_t offset;

	if (buf_len) {
		pnum = ((uint8_t *)buf++)[0];
		if (pnum != (prev_pnum + 1)) {
			LOG_WRN("OUT OF ORDER PACKETS: pnum %u not %u",
				pnum, prev_pnum + 1);
		}
		prev_pnum = pnum;
		buf_len--;
	} else {
		LOG_ERR("Zero length packet received");
		state = PGPS_NONE;
		return -EINVAL;
	}

	if (state < PGPS_LOADING) {
		state = PGPS_LOADING;
		pcount = 0;
		expected_count = CONFIG_NRF_CLOUD_PGPS_NUM_PREDICTIONS;
		memset(prediction_present, 0, sizeof(prediction_present));
	}

	if (pnum == 0) {
		header = (struct nrf_cloud_pgps_header *)buf;
		prediction_ptr = (uint8_t *)&header->predictions[0];

		LOG_INF("Received PGPS header. Schema version:%u, type:%u, num:%u, "
			"count:%u",
			header->schema_version, header->array_type,
			header->num_items, header->prediction_count);
		LOG_INF("  size:%u, period (minutes):%u, GPS day:%u, GPS time:%u",
			header->prediction_size, header->prediction_period_min,
			header->gps_day, header->gps_time_of_day);
		LOG_INF("agps_system_time size:%u, "
			"agps_ephemeris size:%u, "
			"prediction struct size:%u",
			sizeof(struct nrf_cloud_pgps_system_time),
			sizeof(struct nrf_cloud_agps_ephemeris),
			sizeof(struct nrf_cloud_pgps_prediction));

		if (header->schema_version != NRF_CLOUD_PGPS_BIN_SCHEMA_VERSION) {
			LOG_ERR("Cannot parse schema version: %d",
				header->schema_version);
			state = PGPS_NONE;
			return -EINVAL;
		}
		if (header->num_items != 1) {
			LOG_ERR("num elements wrong: %u", header->num_items);
			LOG_HEXDUMP_ERR(buf, buf_len, "packet");
			state = PGPS_NONE;
			return -EINVAL;
		}

		expected_count = header->prediction_count;
		period_sec = header->prediction_period_min * SEC_PER_MIN;

		if (pgps_partial_request) {
			int64_t orig_sec = gps_day_time_to_sec(pgps_storage->gps_day,
							       pgps_storage->gps_time_of_day);
			int64_t part_sec = gps_day_time_to_sec(header->gps_day,
							       header->gps_time_of_day);

			pnum_offset = (part_sec - orig_sec) / period_sec;
#if defined(TEST_PNUM_OFFSET)
			if (pnum_offset == 0) {
				pnum_offset = test_pnum_offset;
			}
#endif
			LOG_INF("Partial request; starting at pnum %u", pnum_offset);

			/* fix up header with full set info */
			/* @TODO: allow for discarding oldest -- check for match of start */
			header->prediction_count = pgps_storage->prediction_count;
			header->gps_day = pgps_storage->gps_day;
			header->gps_time_of_day = pgps_storage->gps_time_of_day;
		} else {
			pnum_offset = 0;
		}
		buf_len -= sizeof(*header);
	} else {
		prediction_ptr = (uint8_t *)buf;
	}

	pnum += pnum_offset;
	if (pnum == 0) {
		offset = 0;
	} else {
		offset = (uint32_t)(sizeof(struct nrf_cloud_pgps_header) +
				   pnum * sizeof(struct nrf_cloud_pgps_prediction));
	}

	gps_sec = 0;
	uint8_t *element_ptr = prediction_ptr;
	struct agps_header *elem = (struct agps_header *)element_ptr;

	LOG_INF("Packet %u, type:%u, count:%u, buf len:%u",
		pnum, elem->type, elem->count, buf_len);

	while (parsed_len < buf_len) {
		bool empty;
		size_t element_size = get_next_pgps_element(&element, element_ptr);

		if (element_size == 0) {
			LOG_INF("  End of element");
			break;
		}
		switch (element.type) {
		case NRF_CLOUD_AGPS_GPS_SYSTEM_CLOCK:
			gps_sec = gps_day_time_to_sec(element.time_and_tow->date_day,
						      element.time_and_tow->time_full_s);
			break;
		case NRF_CLOUD_AGPS_UTC_PARAMETERS:
			nrf_cloud_set_leap_seconds(element.utc->delta_tls);
			break;
		case NRF_CLOUD_AGPS_LOCATION:
			nrf_cloud_set_location_normalized(element.location->latitude,
							  element.location->longitude);
			break;
		case NRF_CLOUD_AGPS_EPHEMERIDES:
			/* check for all zeros except first byte (sv_id) */
			empty = true;
			for (int i = 1; i < sizeof(struct nrf_cloud_agps_ephemeris); i++) {
				if (element_ptr[i] != 0) {
					empty = false;
					break;
				}
			}
			if (empty) {
				LOG_INF("Marking ephemeris %u as empty",
					element.ephemeris->sv_id);
				element.ephemeris->health = NRF_CLOUD_PGPS_EMPTY_EPHEM_HEALTH;
			}
			break;
		default: /* just store */
			break;
		}

		parsed_len += element_size;
		element_ptr += element_size;
	}

	if (parsed_len == buf_len) {
		LOG_INF("Parsing finished");

		if (prediction_present[pnum]) {
			LOG_WRN("Received duplicate MQTT packet; ignoring");
		} else if (gps_sec == 0) {
			LOG_ERR("Prediction did not include GPS day and time of day; ignoring");
		} else {
			err = open_storage(offset);
			if (err) {
				state = PGPS_NONE;
				return err;
			}

			if (pnum == 0) {
				LOG_INF("Storing PGPS header");
				store_prediction_header(header);
			}

			LOG_INF("Storing prediction %u for gps sec %lld at offset %u\n",
				pnum, gps_sec, offset);

			store_prediction(prediction_ptr, buf_len, gps_sec, true);
			prediction_present[pnum] = true;
			pcount++;
		}
	} else {
		LOG_ERR("Parsing incomplete; aborting.");
		state = PGPS_NONE;
		return -EINVAL;
	}

	if (pcount == expected_count) {
		LOG_INF("All PGPS data received. Done.");
		state = PGPS_READY;
	}

	return 0;
}

static bool validate_stored_pgps_header(void)
{
	if ((pgps_storage->schema_version != NRF_CLOUD_PGPS_BIN_SCHEMA_VERSION) ||
	    (pgps_storage->array_type != NRF_CLOUD_PGPS_PREDICTION_HEADER) ||
	    (pgps_storage->num_items != 1) ||
	    (pgps_storage->prediction_period_min !=
	     CONFIG_NRF_CLOUD_PGPS_PREDICTION_PERIOD) ||
	    (pgps_storage->prediction_count !=
	     CONFIG_NRF_CLOUD_PGPS_NUM_PREDICTIONS)) {
		return false;
	}
	return true;
}

static int validate_stored_predictions(uint16_t *first_bad_day,
				       uint32_t *first_bad_time)
{
	/* @TODO: check for all predictions up to date;
	 * if missing some, get from server
	 */
	int err;
	uint16_t i;
	uint16_t count = pgps_storage->prediction_count;
	uint16_t period_min = pgps_storage->prediction_period_min;
	uint16_t gps_day = pgps_storage->gps_day;
	uint32_t gps_time_of_day = pgps_storage->gps_time_of_day;
	struct nrf_cloud_pgps_prediction *p = &pgps_storage->predictions[0];
	int64_t gps_sec = gps_day_time_to_sec(gps_day, gps_time_of_day);

	for (i = 0; i < count; i++, p++) {
		err = validate_prediction(p, gps_day, gps_time_of_day,
					  period_min, true);
		if (err) {
			LOG_ERR("prediction %u, gps_day:%u, "
				"gps_time_of_day:%u is bad: %d",
				i + 1, gps_day, gps_time_of_day, err);
			/* request partial data; download interrupted? */
			*first_bad_day = gps_day;
			*first_bad_time = gps_time_of_day;
			break;
		}
		LOG_INF("prediction %u, gps_day:%u, gps_time_of_day:%u",
			i + 1, gps_day, gps_time_of_day);

		/* calculate next expected time signature */
		gps_sec += period_min * SEC_PER_MIN;
		gps_sec_to_day_time(gps_sec, &gps_day, &gps_time_of_day);
	}

	return i;
}

int nrf_cloud_pgps_init(void)
{
	if ((state == PGPS_REQUESTING) || (state == PGPS_LOADING)) {
		return 0;
	}
	settings_init();
	state = PGPS_NONE;
	int err = 0;

	LOG_INF("Validating stored PGPS header...");
	if (!validate_stored_pgps_header()) {
		/* request predictions -- none seem to be stored */
		LOG_WRN("No valid PGPS data stored");
		LOG_INF("Requesting predictions...");
		err = nrf_cloud_pgps_request_all();
		if (!err) {
			state = PGPS_REQUESTING;
		}
	} else {
		/* @TODO: check for all predictions up to date;
		 * if missing some, get from server
		 */
		uint16_t i;
		uint16_t count = pgps_storage->prediction_count;
		uint16_t period_min = pgps_storage->prediction_period_min;
		uint16_t gps_day = pgps_storage->gps_day;
		uint32_t gps_time_of_day = pgps_storage->gps_time_of_day;
		struct nrf_cloud_pgps_prediction *test_prediction;
		int err;

#if defined(USE_TEST_GPS_DAY)
		test_gps_day = gps_day + 1;
#endif
		LOG_INF("Checking if PGPS data is expired...");
		err = nrf_cloud_find_prediction(&test_prediction);
		if (err == -ETIMEDOUT) {
			LOG_WRN("Predictions expired. Requesting predictions...");
			err = nrf_cloud_pgps_request_all();
			if (!err) {
				state = PGPS_REQUESTING;
				return 0;
			}
		} else if (err == 0) {
			LOG_INF("Found valid prediction, day:%u, time:%u !",
				test_prediction->time.date_day,
				test_prediction->time.time_full_s);
		}

		LOG_INF("Checking stored PGPS data; count:%u, period_min:%u",
			count, period_min);

		i = validate_stored_predictions(&gps_day, &gps_time_of_day);

		if (i == 0) {
			err = nrf_cloud_pgps_request_all();
			if (!err) {
				state = PGPS_REQUESTING;
				return 0;
			}
		} else if (i < count) {
			struct gps_pgps_request request;

#if defined(USE_TEST_GPS_DAY)
			test_pnum_offset = i;
#endif
			LOG_INF("Incomplete PGPS data; "
				"requesting %u predictions...", count - i);
			request.gps_day = gps_day;
			request.gps_time_of_day = gps_time_of_day;
			request.prediction_count = count - i;
			request.prediction_period_min = period_min;
			err = nrf_cloud_pgps_request(&request);
			if (!err) {
				state = PGPS_REQUESTING;
			}
		} else {
			state = PGPS_READY;
			LOG_INF("PGPS data is up to date.");
		}
	}
	return err;
}

