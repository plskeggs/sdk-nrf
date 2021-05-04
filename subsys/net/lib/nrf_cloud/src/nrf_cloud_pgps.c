/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr.h>
#include <nrfx_nvmc.h>
#include <device.h>
#include <drivers/gps.h>
#include <storage/stream_flash.h>
#include <net/socket.h>
#include <nrf_socket.h>
#include <net/download_client.h>
#include <pm_config.h>

#include <cJSON.h>
#include <cJSON_os.h>
#include <modem/modem_info.h>
#include <date_time.h>
#include <net/nrf_cloud_agps.h>
#include <net/nrf_cloud_pgps.h>
#include <settings/settings.h>
#include <power/reboot.h>
#include <logging/log_ctrl.h>

#include <logging/log.h>

LOG_MODULE_REGISTER(nrf_cloud_pgps, CONFIG_NRF_CLOUD_GPS_LOG_LEVEL);

#include "nrf_cloud_transport.h"
#include "nrf_cloud_pgps_schema_v1.h"

#define TEST_INTERRUPTED 0 /* set to 1 to test interrupted downloads */
#define INTERRUPTED_PNUM 5

#define REPLACEMENT_THRESHOLD (CONFIG_NRF_CLOUD_PGPS_NUM_PREDICTIONS - 1) /* / 10) */
#define FORCE_HTTP_DL 0
#define CHECK_FOR_ERASED_FROM_END 0
#define PGPS_DEBUG 0
#define DATA_OVER_MQTT CONFIG_NRF_CLOUD_PGPS_DATA_OVER_MQTT
#define USE_APPROXIMATE_EPHEMERIS 0
#define USE_STALE_TIMESTAMP 0
#define USE_TEST_GPS_DAY 0
#define SEC_TAG CONFIG_NRF_CLOUD_SEC_TAG /* 42 */
#define FRAGMENT_SIZE 1700U /* CONFIG_DOWNLOAD_CLIENT_BUF_SIZE */

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

#define LAT_DEG_TO_DEV_UNITS(_lat_) ((int32_t)(((_lat_) / 90.0) * (1 << 23)))
#define LNG_DEG_TO_DEV_UNITS(_lng_) ((int32_t)(((_lng_) / 360.0) * (1 << 24)))
#define SAVED_LOCATION_MIN_DELTA_SEC (12 * SEC_PER_HOUR)
#define SAVED_LOCATION_LAT_DELTA LAT_DEG_TO_DEV_UNITS(0.1) /* ~11km (at equator) */
#define SAVED_LOCATION_LNG_DELTA LNG_DEG_TO_DEV_UNITS(0.1) /* ~11km */

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

#define RCV_ITEM_IDX_FILE_HOST 0
#define RCV_ITEM_IDX_FILE_PATH 1

#define LOCK_PGPS
#define UNLOCK_PGPS

#if USE_TEST_GPS_DAY
static uint16_t test_gps_day;
static uint8_t test_pnum_offset;
#endif

enum pgps_state {
	PGPS_NONE,
	PGPS_EXPIRED,
	PGPS_REQUESTING,
	PGPS_LOADING,
	PGPS_READY,
};
static enum pgps_state state;

struct pgps_index {
	struct nrf_cloud_pgps_header header;
	int64_t start_sec;
	int64_t end_sec;
	uint32_t dl_offset;
	uint16_t pred_offset;
	uint16_t expected_count;
	uint16_t loading_count;
	uint16_t period_sec;
	uint8_t dl_pnum;
	uint8_t pnum_offset;
	bool partial_request;
	bool stale_server_data;

	/* array of pointers to predictions, in sorted time order */
	struct nrf_cloud_pgps_prediction *predictions[CONFIG_NRF_CLOUD_PGPS_NUM_PREDICTIONS];
};

static struct pgps_index index;

static pgps_event_handler_t handler;
/* array of potentially out-of-time-order predictions */
static struct nrf_cloud_pgps_prediction *storage =
      (struct nrf_cloud_pgps_prediction *)PM_MCUBOOT_SECONDARY_ADDRESS;
static struct stream_flash_ctx stream;
static uint8_t *write_buf;
static uint32_t flash_page_size;

#if !DATA_OVER_MQTT
K_SEM_DEFINE(pgps_active, 1, 1);
static struct download_client dlc;
static int socket_retries_left;
static uint8_t prediction_buf[PGPS_PREDICTION_STORAGE_SIZE];
#endif

/* todo: get the correct values from somewhere */
static int gps_leap_seconds = GPS_TO_UTC_LEAP_SECONDS;

struct gps_location {
	int32_t latitude;
	int32_t longitude;
	int64_t gps_sec;
};

static struct gps_location saved_location;

static struct nrf_cloud_pgps_header saved_header;

static bool json_initialized;
static bool ignore_packets;

static int nrf_cloud_pgps_get_time(int64_t *gps_sec, uint16_t *gps_day,
			    uint32_t *gps_time_of_day);
static int settings_set(const char *key, size_t len_rd,
			settings_read_cb read_cb, void *cb_arg);
static int validate_stored_predictions(uint16_t *bad_day, uint32_t *bad_time);
static void log_pgps_header(const char *msg, struct nrf_cloud_pgps_header *header);
#if !DATA_OVER_MQTT
static int download_client_callback(const struct download_client_evt *event);
#endif
static int consume_pgps_header(const char *buf, size_t buf_len);
static void cache_pgps_header(struct nrf_cloud_pgps_header *header);
static int consume_pgps_data(uint8_t pnum, const char *buf, size_t buf_len);

#define SETTINGS_NAME "nrf_cloud_pgps"
#define SETTINGS_KEY_PGPS_HEADER "pgps_header"
#define SETTINGS_FULL_PGPS_HEADER SETTINGS_NAME "/" SETTINGS_KEY_PGPS_HEADER
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

	LOG_DBG("Settings key:%s, size:%d", log_strdup(key), len_rd);

	if (!strncmp(key, SETTINGS_KEY_PGPS_HEADER,
		     strlen(SETTINGS_KEY_PGPS_HEADER)) &&
		(len_rd == sizeof(saved_header))) {
		if (read_cb(cb_arg, (void *)&saved_header, len_rd) == len_rd) {
			LOG_DBG("Read pgps_header: count:%u, period:%u, day:%d, time:%d",
				saved_header.prediction_count, saved_header.prediction_period_min,
				saved_header.gps_day, saved_header.gps_time_of_day);
			return 0;
		}
	}
	if (!strncmp(key, SETTINGS_KEY_LOCATION,
		     strlen(SETTINGS_KEY_LOCATION)) &&
	    (len_rd == sizeof(saved_location))) {
		if (read_cb(cb_arg, (void *)&saved_location, len_rd) == len_rd) {
			LOG_DBG("Read location:%d, %d, gps sec:%lld",
				saved_location.latitude, saved_location.longitude,
				saved_location.gps_sec);
			return 0;
		}
	}
	if (!strncmp(key, SETTINGS_KEY_LEAP_SEC,
		     strlen(SETTINGS_KEY_LEAP_SEC)) &&
	    (len_rd == sizeof(gps_leap_seconds))) {
		if (read_cb(cb_arg, (void *)&gps_leap_seconds, len_rd) == len_rd) {
			LOG_DBG("Read gps to utc leap seconds offset:%d",
				gps_leap_seconds);
			return 0;
		}
	}
	return -ENOTSUP;
}

static int save_pgps_header(struct nrf_cloud_pgps_header *header)
{
	int ret = 0;

	log_pgps_header("Save pgps_header: ", header);
	ret = settings_save_one(SETTINGS_FULL_PGPS_HEADER, header, sizeof(*header));
	return ret;
}

/* @TODO: consider rate-limiting these updates to reduce Flash wear */
static int save_location(void)
{
	int ret = 0;

	LOG_DBG("Saving location:%d, %d; gps sec:%lld",
		saved_location.latitude, saved_location.longitude,
		saved_location.gps_sec);
	ret = settings_save_one(SETTINGS_FULL_LOCATION,
				&saved_location, sizeof(saved_location));
	return ret;
}

static int save_leap_sec(void)
{
	int ret = 0;

	LOG_DBG("Saving gps to utc leap seconds offset:%d", gps_leap_seconds);
	ret = settings_save_one(SETTINGS_FULL_LEAP_SEC,
				&gps_leap_seconds, sizeof(gps_leap_seconds));
	return ret;
}

static int settings_init(void)
{
	int ret = 0;

	ret = settings_subsys_init();
	if (ret) {
		LOG_ERR("Settings init failed:%d", ret);
		return ret;
	}
	ret = settings_load_subtree(settings_handler_nrf_cloud_pgps.name);
	if (ret) {
		LOG_ERR("Cannot load settings:%d", ret);
	}
	return ret;
}

void nrf_cloud_set_location_normalized(int32_t latitude, int32_t longitude)
{
	int64_t sec;

	if (nrf_cloud_pgps_get_time(&sec, NULL, NULL)) {
		sec = saved_location.gps_sec; /* could not get time; use prev */
	}

	if (((latitude - saved_location.latitude) > SAVED_LOCATION_LAT_DELTA) ||
	    ((longitude - saved_location.longitude) > SAVED_LOCATION_LNG_DELTA) ||
	    ((sec - saved_location.gps_sec) > SAVED_LOCATION_MIN_DELTA_SEC)) {
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

	lat = LAT_DEG_TO_DEV_UNITS(latitude);
	lng = LNG_DEG_TO_DEV_UNITS(longitude);

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

	LOG_DBG("Converted UTC sec:%lld to GPS sec:%lld", utc_sec, gps_sec);
	return gps_sec;
}

static int64_t gps_day_time_to_sec(uint16_t gps_day, uint32_t gps_time_of_day)
{
	int64_t gps_sec = (int64_t)gps_day * SEC_PER_DAY + gps_time_of_day;

#if PGPS_DEBUG
	LOG_DBG("Converted GPS day:%u, time of day:%u to GPS sec:%lld",
		gps_day, gps_time_of_day, gps_sec);
#endif
	return gps_sec;
}

static void gps_sec_to_day_time(int64_t gps_sec, uint16_t *gps_day,
				uint32_t *gps_time_of_day)
{
	uint16_t day;
	uint32_t time;

	day = (uint16_t)(gps_sec / SEC_PER_DAY);
	time = (uint32_t)(gps_sec - (day * SEC_PER_DAY));
#if PGPS_DEBUG
	LOG_DBG("Converted GPS sec:%lld to day:%u, time of day:%u, week:%u",
		gps_sec, day, time,
		(uint16_t)(day / DAYS_PER_WEEK));
#endif
	if (gps_day) {
		*gps_day = day;
	}
	if (gps_time_of_day) {
		*gps_time_of_day = time;
	}
}

static int nrf_cloud_pgps_get_time(int64_t *gps_sec, uint16_t *gps_day,
			    uint32_t *gps_time_of_day)
{
	int64_t now;
	int err;

	err = date_time_now(&now);
	if (!err) {
		now = nrf_cloud_utc_to_gps_sec(now, NULL);
#if USE_TEST_GPS_DAY
		uint16_t test_day;
		uint32_t test_time;

		if (test_gps_day) {
			gps_sec_to_day_time(now, &test_day, &test_time);
			test_day = test_gps_day; /* override with fake day */
			now = gps_day_time_to_sec(test_day, test_time);
		}
#endif
		gps_sec_to_day_time(now, gps_day, gps_time_of_day);
		if (gps_sec != NULL) {
			*gps_sec = now;
		}
	}
	return err;
}

static int nrf_cloud_pgps_get_usable_time(int64_t *gps_sec, uint16_t *gps_day,
					  uint32_t *gps_time_of_day)
{
	int err = nrf_cloud_pgps_get_time(gps_sec, gps_day, gps_time_of_day);

	if (!err) {
		return err;
	}

#if USE_STALE_TIMESTAMP
	if (saved_location.gps_sec) {
		if (gps_sec) {
			*gps_sec = saved_location.gps_sec;
		}
		gps_sec_to_day_time(saved_location.gps_sec,
				    gps_day, gps_time_of_day);
		return EAPPROXIMATE;
	}
#endif

	return -ENODATA;
}

static int determine_prediction_num(struct nrf_cloud_pgps_header *header,
				    struct nrf_cloud_pgps_prediction *p)
{
	int64_t start_sec = gps_day_time_to_sec(header->gps_day, header->gps_time_of_day);
	uint32_t period_sec = header->prediction_period_min * SEC_PER_MIN;
	int64_t end_sec = start_sec + header->prediction_count * period_sec;
	int64_t pred_sec = gps_day_time_to_sec(p->time.date_day, p->time.time_full_s);

	if ((pred_sec >= start_sec) && (pred_sec < end_sec)) {
		return (int)((pred_sec - start_sec) / period_sec);
	} else {
		return -EINVAL;
	}
}

static void log_pgps_header(const char *msg, struct nrf_cloud_pgps_header *header)
{
	LOG_INF("%sSchema version:%u, type:%u, num:%u, "
		"count:%u", msg ? msg : "",
		header->schema_version & 0xFFU, header->array_type & 0xFFU,
		header->num_items, header->prediction_count);
	LOG_INF("  size:%u, period (minutes):%u, GPS day:%u, GPS time:%u",
		header->prediction_size,
		header->prediction_period_min,
		header->gps_day & 0xFFFFU, header->gps_time_of_day);
}

static bool validate_pgps_header(struct nrf_cloud_pgps_header *header)
{
	log_pgps_header("Checking PGPS header: ", header);
	if ((header->schema_version != NRF_CLOUD_PGPS_BIN_SCHEMA_VERSION) ||
	    (header->array_type != NRF_CLOUD_PGPS_PREDICTION_HEADER) ||
	    (header->num_items != 1) ||
	    (header->prediction_period_min !=
	     CONFIG_NRF_CLOUD_PGPS_PREDICTION_PERIOD) ||
	    (header->prediction_count <= 0) ||
	    (header->prediction_count >
	     CONFIG_NRF_CLOUD_PGPS_NUM_PREDICTIONS)) {
		if ((((uint8_t)header->schema_version) == 0xff) &&
		    (((uint8_t)header->array_type) == 0xff)) {
			LOG_WRN("Flash is erased.");
		} else {
			LOG_WRN("One or more fields are wrong");
		}
		return false;
	}
	return true;
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
	    (p->time_count != 1)) {
		LOG_ERR("invalid prediction header");
		err = -EINVAL;
	} else if (exact && (p->time.date_day != gps_day)) {
		LOG_ERR("prediction day:%u, expected:%u",
			p->time.date_day, gps_day);
		err = -EINVAL;
	} else if (exact && (p->time.time_full_s != gps_time_of_day)) {
		LOG_ERR("prediction time:%u, expected:%u",
			p->time.time_full_s, gps_time_of_day);
		err = -EINVAL;
	}

	int64_t gps_sec = gps_day_time_to_sec(gps_day, gps_time_of_day);
	int64_t pred_sec = gps_day_time_to_sec(p->time.date_day, p->time.time_full_s);
	int64_t end_sec = pred_sec + period_min * SEC_PER_MIN;

	if ((gps_sec < pred_sec) || (gps_sec > end_sec)) {
		LOG_ERR("prediction does not contain desired time; "
			"start:%lld, cur:%lld, end:%lld",
			pred_sec, gps_sec, end_sec);
		err = -EINVAL;
	}

	if ((p->ephemeris_type != NRF_CLOUD_AGPS_EPHEMERIDES) ||
	    (p->ephemeris_count != NRF_CLOUD_PGPS_NUM_SV)) {
		LOG_ERR("ephemeris header bad:%u, %u",
			p->ephemeris_type, p->ephemeris_count);
		err = -EINVAL;
	}

	if (exact && !err) {
		uint32_t expected_sentinel;
		uint32_t stored_sentinel;

		expected_sentinel = gps_day_time_to_sec(gps_day, gps_time_of_day);
		stored_sentinel = p->sentinel;
		if (expected_sentinel != stored_sentinel) {
			LOG_ERR("prediction at:%p has stored_sentinel:0x%08X, "
				"expected:0x%08X", p, stored_sentinel,
				expected_sentinel);
			err = -EINVAL;
		}
	}
	return err;
}

static int validate_stored_predictions(uint16_t *first_bad_day,
				       uint32_t *first_bad_time)
{
	int err;
	uint16_t i;
	uint16_t count = index.header.prediction_count;
	uint16_t period_min = index.header.prediction_period_min;
	uint16_t gps_day = index.header.gps_day;
	uint32_t gps_time_of_day = index.header.gps_time_of_day;
	uint8_t *p = (uint8_t *)storage;
	struct nrf_cloud_pgps_prediction *pred;
	int64_t start_gps_sec = index.start_sec;
	int64_t gps_sec;
	int pnum;

	/* reset catalog of predictions */
	for (pnum = 0; pnum < count; pnum++) {
		index.predictions[pnum] = NULL;
	}

	/* build catalog of predictions */
	for (i = 0; i < count; i++) {
		pred = (struct nrf_cloud_pgps_prediction *)p;

		pnum = determine_prediction_num(&index.header, pred);
		if (pnum < 0) {
			LOG_ERR("prediction idx:%u, ofs:0x%08u, out of expected time range;"
				" day:%u, time:%u", i, (uint32_t)p, pred->time.date_day,
				pred->time.time_full_s);
		} else if (index.predictions[pnum] == NULL) {
			index.predictions[pnum] = pred;
			LOG_INF("Prediction num:%d stored at idx:%d", pnum, i);
		} else {
			LOG_WRN("Prediction num:%d stored more than once!", pnum);
		}
		p += PGPS_PREDICTION_STORAGE_SIZE;
	}

	/* validate predictions in time order, independent of storage order */
	for (pnum = 0; pnum < count; pnum++) {
		pred = index.predictions[pnum];
		if (pred == NULL) {
			LOG_WRN("pnum:%d missing", pnum);
		}

		/* calculate expected time signature */
		gps_sec = start_gps_sec + pnum * period_min * SEC_PER_MIN;
		gps_sec_to_day_time(gps_sec, &gps_day, &gps_time_of_day);

		err = validate_prediction(pred, gps_day, gps_time_of_day,
					  period_min, true);
		if (err) {
			LOG_ERR("Prediction num:%u, gps_day:%u, "
				"gps_time_of_day:%u is bad:%d; loc:%p",
				pnum, gps_day, gps_time_of_day, err, pred);
			/* request partial data; download interrupted? */
			*first_bad_day = gps_day;
			*first_bad_time = gps_time_of_day;
			break;
		}
		LOG_INF("Prediction num:%u, gps_day:%u, gps_time_of_day:%u, loc:%p",
			pnum, gps_day, gps_time_of_day, pred);
	}

	return pnum;
}

static void get_prediction_day_time(int pnum, int64_t *gps_sec, uint16_t *gps_day,
				    uint32_t *gps_time_of_day)
{
	int64_t psec = index.start_sec + (uint32_t)pnum * index.period_sec;

	if (gps_sec) {
		*gps_sec = psec;
	}
	if (gps_day || gps_time_of_day) {
		gps_sec_to_day_time(psec, gps_day, gps_time_of_day);
	}
}

static void discard_oldest_predictions(int num)
{
	LOCK_PGPS;

	int i;
	int last = MIN(num, index.header.prediction_count);

	/* move predictions we are keeping to the start,
	 * then clear their old locations
	 */
	for (i = last; i < index.header.prediction_count; i++) {
		index.predictions[i - last] = index.predictions[i];
		index.predictions[i] = NULL;
	}

	/* update index and header to start at  */
	get_prediction_day_time(num, &index.start_sec,
				&index.header.gps_day,
				&index.header.gps_time_of_day);

	UNLOCK_PGPS;
}

int nrf_cloud_find_prediction(struct nrf_cloud_pgps_prediction **prediction)
{
	int64_t cur_gps_sec;
	int64_t offset_sec;
	int64_t start_sec = index.start_sec;
	int64_t end_sec = index.end_sec;
	uint16_t cur_gps_day;
	uint32_t cur_gps_time_of_day;
	uint16_t start_day = index.header.gps_day;
	uint32_t start_time = index.header.gps_time_of_day;
	uint16_t period_min = index.header.prediction_period_min;
	uint16_t count = index.header.prediction_count;
	int err;
	int pnum;

	if (index.stale_server_data) {
		LOG_ERR("server error: expired data");
		return -ENODATA;
	}

	err = nrf_cloud_pgps_get_usable_time(&cur_gps_sec, &cur_gps_day,
				      &cur_gps_time_of_day);
	if (err < 0) {
		LOG_INF("Unknown current time");
		cur_gps_sec = 0;
	} else if (err) {
		LOG_WRN("Using approximate time");
	}

	LOG_INF("Looking for prediction for current gps_sec:%llu, day:%u, time:%u",
		cur_gps_sec, cur_gps_day, cur_gps_time_of_day);

	offset_sec = cur_gps_sec - start_sec;

	LOG_INF("First stored gps_sec:%llu, day:%u, time:%u; offset_sec:%lld",
		start_sec, start_day, start_time, offset_sec);

	if (offset_sec < 0) {
		/* current time must be unknown or very inaccurate; it
		 * falls before the first valid prediction; default to
		 * first entry and day and time for it
		 */
		err = EAPPROXIMATE;
#if USE_APPROXIMATE_EPHEMERIS
		pnum = 0;
		cur_gps_day = start_day;
		cur_gps_time_of_day = start_time;
		LOG_INF("Picking first prediction due to inaccurate time");
#else
		LOG_WRN("cannot find prediction; real time not known");
		return EAPPROXIMATE;
#endif
	} else if (cur_gps_sec > end_sec) {
		if ((cur_gps_sec - end_sec) > PGPS_MARGIN_SEC) {
			LOG_WRN("data expired!");
			return -ETIMEDOUT;
		}
		/* data is expired; use most recent entry */
		pnum = count - 1;
	} else {
		pnum = offset_sec / (SEC_PER_MIN * period_min);
		if (pnum >= index.header.prediction_count) {
			LOG_WRN("pnum:%u -- too large", pnum);
			pnum = index.header.prediction_count - 1;
		}
	}

	LOG_INF("Selected pnum:%u", pnum);
	*prediction = index.predictions[pnum];
	if (*prediction) {
		err = validate_prediction(*prediction,
					  cur_gps_day, cur_gps_time_of_day,
					  period_min, false);
		if (!err) {
			return pnum;
		} else {
			return err;
		}
	} else {
		if ((state == PGPS_REQUESTING) || (state == PGPS_LOADING)) {
			LOG_WRN("Prediction:%u not loaded yet", pnum);
			return ELOADING;
		} else {
			LOG_ERR("Prediction:%u not available; state:%d", pnum, state);
			return -EINVAL;
		}
	}
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
		LOG_ERR("Failed to send P-GPS request, error:%d", err);
	} else {
		LOG_DBG("P-GPS request sent");
	}

	k_free(msg_string);

	return err;
}

bool nrf_cloud_pgps_loading(void)
{
	return ((state == PGPS_REQUESTING) || (state == PGPS_LOADING));
}

int nrf_cloud_pgps_request(const struct gps_pgps_request *request)
{
	int err = 0;
	cJSON *data_obj;
	cJSON *pgps_req_obj;
	cJSON *ret;

	if (nrf_cloud_pgps_loading()) {
		return 0;
	}
	ignore_packets = false;

	LOG_INF("Requesting %u predictions...", request->prediction_count);

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
		LOG_ERR("Failed to add modem info to P-GPS request:%d", err);
		goto cleanup;
	}
#endif
	if (request->prediction_count < index.header.prediction_count) {
		index.partial_request = true;
		index.pnum_offset = index.header.prediction_count -
				    request->prediction_count;
	} else {
		index.partial_request = false;
		index.pnum_offset = 0;
	}
#if USE_TEST_GPS_DAY
	index.pnum_offset = test_pnum_offset;
#endif
	index.expected_count = request->prediction_count;

	ret = cJSON_AddNumberToObject(data_obj, PGPS_JSON_PRED_COUNT,
				      request->prediction_count);
	if (ret == NULL) {
		LOG_ERR("Failed to add pred count to P-GPS request:%d", err);
		err = -ENOMEM;
		goto cleanup;
	}
	ret = cJSON_AddNumberToObject(data_obj, PGPS_JSON_PRED_INT_MIN,
				      request->prediction_period_min);
	if (ret == NULL) {
		LOG_ERR("Failed to add pred int min to P-GPS request:%d", err);
		err = -ENOMEM;
		goto cleanup;
	}
	ret = cJSON_AddNumberToObject(data_obj, PGPS_JSON_GPS_DAY,
				      request->gps_day);
	if (ret == NULL) {
		LOG_ERR("Failed to add gps day to P-GPS request:%d", err);
		err = -ENOMEM;
		goto cleanup;
	}
	ret = cJSON_AddNumberToObject(data_obj, PGPS_JSON_GPS_TIME,
				      request->gps_time_of_day);
	if (ret == NULL) {
		LOG_ERR("Failed to add gps time to P-GPS request:%d", err);
		err = -ENOMEM;
		goto cleanup;
	}

	err = json_send_to_cloud(pgps_req_obj);
	if (!err) {
		state = PGPS_REQUESTING;
	}

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

int nrf_cloud_pgps_preemptive_updates(int current)
{
	/* keep unexpired, read newer subsequent to last */
	struct gps_pgps_request request;
	int count = index.header.prediction_count;
	uint16_t period_min = index.header.prediction_period_min;
	uint16_t gps_day = 0;
	uint32_t gps_time_of_day = 0;

	if ((count - current) >= REPLACEMENT_THRESHOLD) {
		LOG_DBG("Updates not needed yet");
		return 0;
	}

	if (handler) {
		handler(PGPS_EVT_LOADING);
	}

	LOG_INF("Replacing %d oldest predictions", current);
	discard_oldest_predictions(current);
	gps_sec_to_day_time(index.end_sec, &gps_day, &gps_time_of_day);

	request.gps_day = gps_day;
	request.gps_time_of_day = gps_time_of_day;
	request.prediction_count = current;
	request.prediction_period_min = period_min;
	return nrf_cloud_pgps_request(&request);
}

int nrf_cloud_pgps_inject(struct nrf_cloud_pgps_prediction *p,
			  struct gps_agps_request *request,
			  const int *socket)
{
	int err;
	int ret = 0;
	struct gps_agps_request processed;

	nrf_cloud_agps_processed(&processed);
	/* we will get more accurate data from AGPS for these */
	if (processed.position) {
		if (request->position) {
			LOG_DBG("AGPS already received position; skipping");
			request->position = 0;
		}
	}
	if (processed.system_time_tow) {
		if (request->system_time_tow) {
			LOG_DBG("AGPS already received time; skipping");
			request->system_time_tow = 0;
		}
	}

	if (request->system_time_tow) {
		struct pgps_sys_time sys_time;
		uint16_t day;
		uint32_t sec;

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

		LOG_INF("GPS unit needs time assistance. Injecting day:%u, time:%u",
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
	} else {
		LOG_INF("GPS unit does not need time assistance.");
	}

	if (request->position && saved_location.gps_sec) {
		struct pgps_location location;

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

		LOG_INF("GPS unit needs position. Injecting lat:%d, lng:%d",
			saved_location.latitude,
			saved_location.longitude);
		/* send location */

		err = nrf_cloud_agps_process((const char *)&location, sizeof(location),
					     socket);
		if (err) {
			LOG_ERR("Error injecting PGPS location (%d, %d): %d",
				location.location.latitude, location.location.longitude,
				err);
			ret = err;
		}
	} else if (request->position) {
		LOG_WRN("GPS unit needs location, but it is unknown!");
	} else {
		LOG_INF("GPS unit does not need location assistance.");
	}

	if (request->sv_mask_ephe) {
		LOG_INF("GPS unit needs ephemerides. Injecting %u.", p->ephemeris_count);

		/* send ephemerii */
		err = nrf_cloud_agps_process((const char *)&p->schema_version,
					     sizeof(p->schema_version) +
					     sizeof(p->ephemeris_type) +
					     sizeof(p->ephemeris_count) +
					     sizeof(p->ephemerii), socket);
		if (err) {
			LOG_ERR("Error injecting ephermerii:%d", err);
			ret = err;
		}
	} else {
		LOG_INF("GPS unit does not need ephemerides.");
	}
	return ret; /* just return last non-zero error, if any */
}

#if !DATA_OVER_MQTT
static int download_init(void)
{
	return download_client_init(&dlc, download_client_callback);
}

static int download_start(const char *host, const char *file, int sec_tag,
		   const char *apn, size_t fragment_size)
{
	if (host == NULL || file == NULL) {
		return -EINVAL;
	}

	int err;

	err = k_sem_take(&pgps_active, K_NO_WAIT);
	if (err) {
		LOG_ERR("PGPS download already active.");
		return err;
	}
	LOG_DBG("pgps_active LOCKED");

	socket_retries_left = CONFIG_FOTA_SOCKET_RETRIES;

	struct download_client_cfg config = {
		.sec_tag = sec_tag,
		.apn = apn,
		.frag_size_override = fragment_size,
		.set_tls_hostname = (sec_tag != -1),
	};

	err = download_client_connect(&dlc, host, &config);
	if (err != 0) {
		goto cleanup;
	}

	err = download_client_start(&dlc, file, 0);
	if (err != 0) {
		download_client_disconnect(&dlc);
		goto cleanup;
	}

	return 0;

cleanup:
	k_sem_give(&pgps_active);
	LOG_DBG("pgps_active UNLOCKED");
	return err;
}

static int download_client_callback(const struct download_client_evt *event)
{
	int err = 0;
	uint8_t *buf = (uint8_t *)event->fragment.buf;
	size_t len = event->fragment.len;
	size_t need;
	int64_t gps_sec;

	if (event == NULL) {
		return -EINVAL;
	}

	switch (event->id) {
	case DOWNLOAD_CLIENT_EVT_FRAGMENT:
		if (index.dl_offset == 0) {
			struct nrf_cloud_pgps_header *header;

			if (len < sizeof(*header)) {
				err = -EINVAL; /* need full header, for now */
				break;
			}
			LOG_DBG("Consuming PGPS header len:%zd", len);
			err = consume_pgps_header(buf, len);
			if (err) {
				break;
			}
			LOG_INF("Storing PGPS header");
			header = (struct nrf_cloud_pgps_header *)buf;
			cache_pgps_header(header);
#if !USE_TEST_GPS_DAY
			err = nrf_cloud_pgps_get_usable_time(&gps_sec, NULL, NULL);
			if (!err) {
				if ((index.start_sec <= gps_sec) &&
				    (gps_sec <= index.end_sec)) {
					LOG_INF("Received data covers good timeframe");
				} else {
					LOG_ERR("Received data is already expired!");
					index.stale_server_data = true;
					err = -EINVAL;
					break;
				}
			}
#endif
			save_pgps_header(header);

			len -= sizeof(*header);
			buf += sizeof(*header);
			index.dl_offset += sizeof(*header);
			index.dl_pnum = index.pnum_offset;
			index.pred_offset = 0;
		}

		need = MIN((PGPS_PREDICTION_DL_SIZE - index.pred_offset), len);
		memcpy(&prediction_buf[index.pred_offset], buf, need);
		LOG_DBG("need:%zd bytes; pred_offset:%u, fragment len:%zd, dl_ofs:%zd",
			need, index.pred_offset, len, index.dl_offset);
		len -= need;
		buf += need;
		index.pred_offset += need;
		index.dl_offset += need;

		if (index.pred_offset == PGPS_PREDICTION_DL_SIZE) {
			LOG_DBG("consuming data pnum:%u, remainder:%zd",
				index.dl_pnum, len);
			err = consume_pgps_data(index.dl_pnum, prediction_buf,
						PGPS_PREDICTION_DL_SIZE);
			if (err) {
				break;
			}
			if (len) { /* keep extra data for next time */
				memcpy(prediction_buf, buf, len);
			}
			index.pred_offset = len;
			index.dl_pnum++;
		}
		index.dl_offset += len;
		return 0;
	case DOWNLOAD_CLIENT_EVT_DONE:
		LOG_INF("Download client done");
		break;
	case DOWNLOAD_CLIENT_EVT_ERROR: {
		/* In case of socket errors we can return 0 to retry/continue,
		 * or non-zero to stop
		 */
		if ((socket_retries_left) && ((event->error == -ENOTCONN) ||
					      (event->error == -ECONNRESET))) {
			LOG_WRN("Download socket error. %d retries left...",
				socket_retries_left);
			socket_retries_left--;
			return 0;
		} else {
			err = -EIO;
		}
		break;
	}
	default:
		return 0;
	}

	err = download_client_disconnect(&dlc);
	if (err) {
		LOG_ERR("Error disconnecting from "
			"download client:%d", err);
	}
	k_sem_give(&pgps_active);
	LOG_DBG("pgps_active UNLOCKED");
	return err;
}
#endif

#if VERIFY_FLASH
static int flash_callback(uint8_t *buf, size_t len, size_t offset)
{
       size_t erased_start = 0;
       bool erased_found = false;
       int err;

#if CHECK_FOR_ERASED_FROM_END
       uint32_t *p = (uint32_t *)buf;
       size_t offs = offset;
       size_t count = len / sizeof(uint32_t);

       while (count--) {
	       if (*(p++) == 0xFFFFFFFFU) {
		       erased_found = true;
		       erased_start = offs;
	       } else if (erased_found) {
		       erased_found = false;
	       }
	       offs += sizeof(uint32_t);
       }
#else
       struct nrf_cloud_pgps_prediction *pred = (struct nrf_cloud_pgps_prediction *)buf;

       if ((pred->time_type == 0xFFU) || (pred->sentinel == 0xFFFFFFFFU)) {
	       erased_found = true;
       }
#endif
       if (erased_found) {
	       LOG_ERR("Erased block at offset:0x%zX len:%zu contains FFs "
		       "starting at offset:0x%zX", offset, len, erased_start);
	       LOG_HEXDUMP_ERR(buf, len, "block dump");
	       err = -ENODATA;
       } else {
	       LOG_INF("Block at offset:0x%zX len %zu: written", offset, len);
	       err = 0;
       }
       return err;
}
#else
void *flash_callback;
#endif

static int open_storage(uint32_t offset, bool preserve)
{
	int err;
	const struct device *flash_dev;
	uint32_t block_offset = 0;

	flash_dev = device_get_binding(PM_MCUBOOT_SECONDARY_DEV_NAME);
	if (flash_dev == NULL) {
		LOG_ERR("Failed to get device:'%s'",
			PM_MCUBOOT_SECONDARY_DEV_NAME);
		return -EFAULT;
	}

	block_offset = offset % flash_page_size;
#if PGPS_DEBUG
	LOG_DBG("flash_page_size:%u, block_offset:%u, offset:%u, preserve:%d",
		flash_page_size, block_offset, offset, preserve);
#endif
	offset -= block_offset;

	err = stream_flash_init(&stream, flash_dev,
				write_buf, flash_page_size,
				PM_MCUBOOT_SECONDARY_ADDRESS + offset,
				PM_MCUBOOT_SECONDARY_SIZE - offset, flash_callback);
	if (err) {
		LOG_ERR("Failed to init flash stream for offset %u: %d",
			offset, err);
		state = PGPS_NONE;
		return err;
	}

	if (preserve && (block_offset != 0) && (block_offset < flash_page_size)) {
		uint8_t *p = (uint8_t *)(PM_MCUBOOT_SECONDARY_ADDRESS + offset);

#if PGPS_DEBUG
		LOG_DBG("preserving %u bytes", block_offset);
		LOG_HEXDUMP_DBG(p, MIN(32, block_offset), "start of preserved data");
#endif
		err = stream_flash_buffered_write(&stream, p, block_offset, false);
		if (err) {
			LOG_ERR("Error writing back %u original bytes", block_offset);
		}
	}
	return err;
}

static int store_prediction(uint8_t *p, size_t len, uint32_t sentinel, bool last)
{
	static bool first = true;
	static uint8_t pad[PGPS_PREDICTION_PAD];
	int err;
	uint8_t schema = NRF_CLOUD_AGPS_BIN_SCHEMA_VERSION;
	size_t schema_offset = ((size_t) &((struct nrf_cloud_pgps_prediction *)0)->schema_version);

	if (first) {
		memset(pad, 0xff, PGPS_PREDICTION_PAD);
		first = false;
	}

	err = stream_flash_buffered_write(&stream, p, schema_offset, false);
	if (err) {
		LOG_ERR("Error writing pgps prediction:%d", err);
		return err;
	}
	p += schema_offset;
	len -= schema_offset;
	err = stream_flash_buffered_write(&stream, &schema, sizeof(schema), false);
	if (err) {
		LOG_ERR("Error writing schema:%d", err);
		return err;
	}
	err = stream_flash_buffered_write(&stream, p, len, false);
	if (err) {
		LOG_ERR("Error writing pgps prediction:%d", err);
		return err;
	}
	err = stream_flash_buffered_write(&stream, (uint8_t *)&sentinel,
					  sizeof(sentinel), false);
	if (err) {
		LOG_ERR("Error writing sentinel:%d", err);
	}
	err = stream_flash_buffered_write(&stream, pad, PGPS_PREDICTION_PAD, last);
	if (err) {
		LOG_ERR("Error writing sentinel:%d", err);
	}
	return err;
}

#if 0
static int store_flush(void)
{
	return stream_flash_buffered_write(&stream, NULL, 0, true);
}
#endif

static int consume_pgps_header(const char *buf, size_t buf_len)
{
	struct nrf_cloud_pgps_header *header = (struct nrf_cloud_pgps_header *)buf;

	if (!validate_pgps_header(header)) {
		state = PGPS_NONE;
		return -EINVAL;
	}

	if (index.partial_request) {
		LOG_INF("Partial request; starting at pnum:%u", index.pnum_offset);

		/* fix up header with full set info */
		/* @TODO: allow for discarding oldest -- check for match of start */
		header->prediction_count = index.header.prediction_count;
		header->gps_day = index.header.gps_day;
		header->gps_time_of_day = index.header.gps_time_of_day;
	}

	return 0;
}

static void cache_pgps_header(struct nrf_cloud_pgps_header *header)
{
	memcpy(&index.header, header, sizeof(*header));

	index.start_sec = gps_day_time_to_sec(index.header.gps_day,
					      index.header.gps_time_of_day);
	index.period_sec = index.header.prediction_period_min * SEC_PER_MIN;
	index.end_sec = index.start_sec +
			index.period_sec * index.header.prediction_count;
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
		element->type = (enum nrf_cloud_agps_type)
				buf[NRF_CLOUD_AGPS_BIN_TYPE_OFFSET];
		element_type = element->type;
		elements_left_to_process =
			*(uint16_t *)&buf[NRF_CLOUD_AGPS_BIN_COUNT_OFFSET] - 1;
		len += NRF_CLOUD_AGPS_BIN_TYPE_SIZE +
			NRF_CLOUD_AGPS_BIN_COUNT_SIZE;
	} else {
		element->type = element_type;
		elements_left_to_process -= 1;
	}

	switch (element->type) {
	case NRF_CLOUD_AGPS_EPHEMERIDES:
		element->ephemeris = (struct nrf_cloud_agps_ephemeris *)(buf + len);
#if FIX_IODC
		if ((element->ephemeris->iodc > 1023) || ((element->ephemeris->iodc & 0x1F) == 0)) {
			LOG_WRN("changing IODC from:0x%X to:0x%X",
				element->ephemeris->iodc, element->ephemeris->iodc >> 5);
			element->ephemeris->iodc >>= 5;
			if (element->ephemeris->iodc > 1023) {
				LOG_ERR("IODC is still too large!");
			}
		}
#endif
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
		LOG_DBG("Unhandled P-GPS data type:%d", element->type);
		return 0;
	}

#if PGPS_DEBUG
	LOG_DBG("  size:%zd, count:%u", len, elements_left_to_process);
#endif
	return len;
}

static int consume_pgps_data(uint8_t pnum, const char *buf, size_t buf_len)
{
	struct nrf_cloud_apgs_element element = {};
	uint8_t *prediction_ptr = (uint8_t *)buf;
	uint8_t *element_ptr = prediction_ptr;
	struct agps_header *elem = (struct agps_header *)element_ptr;
	size_t parsed_len = 0;
	int64_t gps_sec;
	bool finished = false;

	gps_sec = 0;

	LOG_DBG("Parsing prediction num:%u, idx:%u, type:%u, count:%u, buf len:%u",
		pnum, index.loading_count, elem->type, elem->count, buf_len);

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
				LOG_INF("Marking ephemeris:%u as empty",
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
		LOG_DBG("Parsing finished");

		if (index.predictions[pnum]) {
			LOG_WRN("Received duplicate MQTT packet; ignoring");
		} else if (gps_sec == 0) {
			LOG_ERR("Prediction did not include GPS day and time of day; ignoring");
			LOG_HEXDUMP_DBG(prediction_ptr, buf_len, "bad data");
		} else {
			uint32_t offset = index.loading_count * PGPS_PREDICTION_STORAGE_SIZE;

			LOG_INF("Storing prediction num:%u idx:%u for gps sec:%lld offset:%u\n",
				pnum, index.loading_count, gps_sec, offset);

			index.loading_count++;
			finished = (index.loading_count == index.expected_count);
#if TEST_INTERRUPTED
			if (!index.partial_request && (pnum == INTERRUPTED_PNUM)) {
				finished = true;
			}
#endif
			store_prediction(prediction_ptr, buf_len, (uint32_t)gps_sec, finished);
			index.predictions[pnum] = (struct nrf_cloud_pgps_prediction *)
						  (((uint8_t *)storage) + offset);
			if (handler) {
				handler((index.loading_count > 1) ?
					PGPS_EVT_STORING : PGPS_EVT_LOADING);
			}

#if TEST_INTERRUPTED
			if (!index.partial_request && (pnum == INTERRUPTED_PNUM)) {
				LOG_INF("TEST IS INTERRUPTING DOWNLOAD!");
				k_sleep(K_SECONDS(2));
				LOG_PANIC();
				sys_reboot(0);
			}
#endif
		}
	} else {
		LOG_ERR("Parsing incomplete; aborting.");
		state = PGPS_NONE;
		return -EINVAL;
	}

	if (finished) {
		LOG_INF("All PGPS data received. Done.");
		state = PGPS_READY;
		if (handler) {
			handler(PGPS_EVT_READY);
		}
	}

	return 0;
}

#if !DATA_OVER_MQTT
static int get_string_from_array(const cJSON *const array, const int index,
				  char **string_out)
{
	__ASSERT_NO_MSG(string_out != NULL);

	cJSON *item = cJSON_GetArrayItem(array, index);

	if (!cJSON_IsString(item)) {
		return -EINVAL;
	}

	*string_out = item->valuestring;

	return 0;
}

#if 0
static int get_number_from_array(const cJSON *const array, const int index,
				  int *number_out)
{
	__ASSERT_NO_MSG(number_out != NULL);

	cJSON *item = cJSON_GetArrayItem(array, index);

	if (!cJSON_IsNumber(item)) {
		return -EINVAL;
	}

	*number_out = item->valueint;

	return 0;
}
#endif

static int parse_dl_info(char *host, size_t host_len,
			 char *path, size_t path_len,
			 const char *payload_in, size_t len)
{
	if (!host || !path || !payload_in) {
		return -EINVAL;
	}

	char *temp;
	char *host_ptr = NULL;
	char *path_ptr = NULL;
	int err = -ENOMSG;
	cJSON *array = cJSON_Parse(payload_in);

	if (!array || !cJSON_IsArray(array)) {
		LOG_ERR("Invalid JSON array");
		err = -EINVAL;
		goto cleanup;
	}

	temp = cJSON_PrintUnformatted(array);
	if (temp) {
		LOG_DBG("JSON array: %s", log_strdup(temp));
		cJSON_FreeString(temp);
	}

	if (get_string_from_array(array, RCV_ITEM_IDX_FILE_HOST, &host_ptr) ||
	    get_string_from_array(array, RCV_ITEM_IDX_FILE_PATH, &path_ptr)) {
		/* || get_number_from_array(array, RCV_ITEM_IDX_FILE_SIZE - offset, &job_info->file_size)) { */
		LOG_ERR("Error parsing info");
		goto cleanup;
	}

	if (host_ptr && host_len) {
		strncpy(host, host_ptr, host_len - 1);
		host[host_len - 1] = '\0';
		LOG_DBG("host: %s", log_strdup(host));
	}
	if (path_ptr && path_len) {
		strncpy(path, path_ptr, path_len - 1);
		path[path_len - 1] = '\0';
		LOG_DBG("path: %s", log_strdup(path));
	}
	err = 0;

cleanup:
	if (array) {
		cJSON_Delete(array);
	}
	return err;
}
#endif

/* handle incoming MQTT packets */
int nrf_cloud_pgps_process(const char *buf, size_t buf_len)
{
	static uint8_t prev_pnum;
	uint8_t pnum;
	int err;

	LOG_HEXDUMP_DBG(buf, buf_len, "MQTT packet");
	if (!buf_len) {
		LOG_ERR("Zero length packet received");
		state = PGPS_NONE;
		return -EINVAL;
	}

	if (ignore_packets) {
		return -EINVAL;
	}

	if (state < PGPS_LOADING) {
		state = PGPS_LOADING;
		prev_pnum = 0xff;
		/* until we get the header (which may or may not come first,
		 * preinitialize the header with expected values
		 */
		if (!index.partial_request) {
			index.header.prediction_count =
				CONFIG_NRF_CLOUD_PGPS_NUM_PREDICTIONS;
			index.header.prediction_period_min =
				CONFIG_NRF_CLOUD_PGPS_PREDICTION_PERIOD;
			index.period_sec =
				index.header.prediction_period_min * SEC_PER_MIN;
			memset(index.predictions, 0, sizeof(index.predictions));
		} else {
			for (pnum = index.pnum_offset;
			     pnum < index.expected_count + index.pnum_offset; pnum++) {
				index.predictions[pnum] = NULL;
			}
		}
		index.loading_count = 0;

		/* @TODO: make this retain new predictions and just replace
		 * oldest ones
		 */
		err = open_storage(index.pnum_offset * PGPS_PREDICTION_STORAGE_SIZE,
				   index.partial_request);
		if (err) {
			state = PGPS_NONE;
			return err;
		}
	}

#if DATA_OVER_MQTT
	pnum = ((uint8_t *)buf++)[0];
	buf_len--;
	if ((prev_pnum != 0xff) && (pnum != (prev_pnum + 1))) {
		LOG_ERR("OUT OF ORDER PACKETS: pnum:%u not:%u",
			pnum, prev_pnum + 1);
		if (pnum > (prev_pnum + 1)) { /* skips ahead */

		} else { /* skips back */

		}
		ignore_packets = true;
		return -EINVAL;
	}
	prev_pnum = pnum;
	if (pnum == 0) {
		struct nrf_cloud_pgps_header *header;

		err = consume_pgps_header(buf, buf_len);
		if (err) {
			return err;
		}
		LOG_INF("Storing PGPS header");
		header = (struct nrf_cloud_pgps_header *)buf;
		cache_pgps_header(header);
		save_pgps_header(header);

		buf_len -= sizeof(struct nrf_cloud_pgps_header);
		buf += sizeof(struct nrf_cloud_pgps_header);
		if (buf_len < index.header.prediction_size) {
			LOG_ERR("PGPS header not followed by full prediction! "
				"size:%zd, expected:%u", buf_len,
				index.header.prediction_size);
			return -EINVAL;
		}
	}
	pnum += index.pnum_offset;
	return consume_pgps_data(pnum, buf, buf_len);

#else
	static char host[128];
	static char path[64];

	err = parse_dl_info(host, sizeof(host), path, sizeof(path), buf, buf_len);
	if (err) {
		return err;
	}
	index.dl_offset = 0;
	ignore_packets = true;
	int sec_tag = SEC_TAG;

	if (FORCE_HTTP_DL && (strncmp(host, "https", 5) == 0)) {
		int i;
		int len = strlen(host);

		for (i = 4; i < len; i++) {
			host[i] = host[i + 1];
		}
		sec_tag = -1;
	}

	return download_start(host, path, sec_tag, NULL, FRAGMENT_SIZE);
			      /* PGPS_PREDICTION_STORAGE_SIZE); */
#endif
}

int nrf_cloud_pgps_init(pgps_event_handler_t cb)
{
	int err = 0;

	handler = cb;

	flash_page_size = nrfx_nvmc_flash_page_size_get();
	if (!flash_page_size) {
		flash_page_size = 4096;
	}

	if ((state == PGPS_REQUESTING) || (state == PGPS_LOADING)) {
		return 0;
	}
	if (!write_buf) {
		write_buf = k_malloc(flash_page_size);
		if (!write_buf) {
			return -ENOMEM;
		}
#if PGPS_DEBUG
	agps_print_enable(true);

	LOG_DBG("agps_system_time size:%u, "
		"agps_ephemeris size:%u, "
		"prediction struct size:%u",
		sizeof(struct nrf_cloud_pgps_system_time),
		sizeof(struct nrf_cloud_agps_ephemeris),
		sizeof(struct nrf_cloud_pgps_prediction));
#endif
	}
	memset(&index, 0, sizeof(index));
	settings_init();
	state = PGPS_NONE;

#if !DATA_OVER_MQTT
	err = download_init();
	if (err) {
		LOG_ERR("Error initializing download client:%d", err);
		return err;
	}
#endif

	uint16_t num_valid = 0;
	uint16_t count = 0;
	uint16_t period_min  = 0;
	uint16_t gps_day = 0;
	uint32_t gps_time_of_day = 0;

	if (validate_pgps_header(&saved_header)) {
		cache_pgps_header(&saved_header);

		count = index.header.prediction_count;
		period_min = index.header.prediction_period_min;
		gps_day = index.header.gps_day;
		gps_time_of_day = index.header.gps_time_of_day;

		/* check for all predictions up to date;
		 * if missing some, get from server
		 */
		LOG_INF("Checking stored PGPS data; count:%u, period_min:%u",
			count, period_min);
		num_valid = validate_stored_predictions(&gps_day, &gps_time_of_day);
	}

	struct nrf_cloud_pgps_prediction *test_prediction;
	int pnum = -1;

#if USE_TEST_GPS_DAY
	test_gps_day = gps_day + 1;
#endif
	LOG_DBG("num_valid:%u, count:%u", num_valid, count);
	if (num_valid) {
		LOG_INF("Checking if PGPS data is expired...");
		err = nrf_cloud_find_prediction(&test_prediction);
		if (err == -ETIMEDOUT) {
			LOG_WRN("Predictions expired. Requesting predictions...");
			num_valid = 0;
		} else if (err >= 0) {
			LOG_INF("Found valid prediction, day:%u, time:%u",
				test_prediction->time.date_day,
				test_prediction->time.time_full_s);
			pnum = err;
		}
	}

	if (!num_valid) {
		/* read a full set of predictions */
		if (handler) {
			handler(PGPS_EVT_UNAVAILABLE);
		}
		err = nrf_cloud_pgps_request_all();
	} else if (num_valid < count) {
		/* read missing predictions at end */
		struct gps_pgps_request request;

#if USE_TEST_GPS_DAY
		test_pnum_offset = num_valid;
#endif
		if (handler) {
			handler(PGPS_EVT_LOADING);
		}
		LOG_INF("Incomplete PGPS data; "
			"requesting %u predictions...", count - num_valid);
		get_prediction_day_time(num_valid, NULL, &gps_day, &gps_time_of_day);
		request.gps_day = gps_day;
		request.gps_time_of_day = gps_time_of_day;
		request.prediction_count = count - num_valid;
		request.prediction_period_min = period_min;
		err = nrf_cloud_pgps_request(&request);
	} else if ((count - pnum) < REPLACEMENT_THRESHOLD) {
		/* keep unexpired, read newer subsequent to last */
		err = nrf_cloud_pgps_preemptive_updates(pnum);
	} else {
		state = PGPS_READY;
		LOG_INF("PGPS data is up to date.");
		if (handler) {
			handler(PGPS_EVT_READY);
		}
		err = 0;
	}
	return err;
}

