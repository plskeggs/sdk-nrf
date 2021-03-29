/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr.h>
#include <device.h>
#include <drivers/gps.h>
#include <net/socket.h>
#include <nrf_socket.h>

#include <cJSON.h>
#include <cJSON_os.h>
#include <modem/modem_info.h>
#include <date_time.h>
#include <net/nrf_cloud_pgps.h>

#include <logging/log.h>

LOG_MODULE_REGISTER(nrf_cloud_pgps, CONFIG_NRF_CLOUD_PGPS_LOG_LEVEL);

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

enum pgps_state {
	PGPS_NONE,
	PGPS_EXPIRED,
	PGPS_REQUESTING,
	PGPS_LOADING,
	PGPS_READY,
};
static enum pgps_state state;

static struct nrf_cloud_pgps_header *pgps_storage =
      (struct nrf_cloud_pgps_header *)PM_MCUBOOT_SECONDARY_ADDRESS;
static struct stream_flash_ctx stream;
static uint8_t write_buf[4096];

/* todo: get the correct values from somewhere */
static int gps_leap_seconds = GPS_TO_UTC_LEAP_SECONDS;
static int32_t latitude;
static int32_t longitude;

static uint16_t end_gps_day;
static uint16_t end_gps_time_of_day;

int64_t nrf_cloud_utc_to_gps_sec(const int64_t utc, int16_t *gps_time_ms)
{
	int64_t utc_sec;
	int64_t gps_sec;

	utc_sec = utc / MSEC_PER_SEC;
	if (gps_time_ms) {
		*gps_time_ms = (uint16_t)(utc - (utc_sec * MSEC_PER_SEC));
	}
	gps_sec = (utc_sec - GPS_TO_UNIX_UTC_OFFSET_SECONDS) + gps_leap_seconds;

	LOG_INF("Converted UTC sec %ld to GPS sec %ld", utc_sec, gps_sec);
	return gps_sec;
}

static int64_t gps_day_time_to_sec(uint16_t gps_day, uint32_t gps_time_of_day)
{
	int64_t gps_sec = (int64_t)gps_day * SEC_PER_DAY + gps_time_of_day;

	LOG_INF("Converted GPS day %u and time of day %u to GPS sec %ld",
		gps_day, gps_time_of_day, gps_sec);
	return gps_sec;
}

static void gps_sec_to_day_time(int64_t gps_sec, uint16_t *gps_day,
				uint32_t *gps_time_of_day)
{
	*gps_day = (uint16_t)(gps_sec / SEC_PER_DAY);
	*gps_time_of_day = (uint32_t)(gps_sec - (*gps_day * SEC_PER_DAY));
	LOG_INF("Converted GPS sec %ld to day %u, time of day %u, week %u",
		gps_sec, *gps_day, *gps_time_of_day, *gps_day / DAYS_PER_WEEK);
}

int nrf_cloud_pgps_get_time(int64_t *gps_sec, uint16_t *gps_day,
			    uint32_t *gps_time_of_day)
{
	int64_t now;
	int err;

	err = date_time_now(&now);
	if (!err) {
		*gps_sec = nrf_cloud_utc_to_gps_sec(&now, NULL);
		gps_sec_to_day_time(gps_sec, gps_day, gps_time_of_day);
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
	if ((p->time_and_tow_type != NRF_CLOUD_AGPS_GPS_SYSTEM_CLOCK) ||
	    (p->time_and_tow_count != 1) ||
	    (p->time_and_tow.date_day != gps_day)) {
		err = -EINVAL;
	}
	else if (exact && (p->time_and_tow.time_full_s != gps_time_of_day)) {
		err = -EINVAL;
	} else if (p->time_and_tow.time_full_s > gps_time_of_day) {
		err = -EINVAL;
	}
	else if (gps_time_of_day >
		 (p->time_and_tow.time_full_s +
		  ((period_min + 1) * SEC_PER_MIN)) {
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
		stored_sentinel = *((uint32_t *)p++);
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
	struct nrf_cloud_pgps_header *p = pgps_storage;
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

	if (state < PGPS_LOADING) {
		return -ENODATA;
	}

	err = nrf_cloud_pgps_get_time(&cur_gps_sec, &cur_gps_day,
				      &cur_gps_time_of_day);
	if (err) {
		cur_gps_sec = 0;
	}

	start_sec = gps_day_time_to_sec(start_day, start_time);
	offset_sec = cur_gps_sec - start_sec;
	end_sec = start_sec + period_min * SEC_PER_MIN * count;

	if ((offset_sec < 0) {
		/* current time must be unknown or very inaccurate; it
		 * falls before the first valid prediction; default to
		 * first entry and day and time for it
		 */
		err = EAPPROXIMATE;
		pnum = 0;
		cur_gps_day = start_day;
		cur_gps_time_of_day = start_time;
	} else if (cur_gps_sec > end_sec) {
		if ((cur_gps_sec - end_sec) > PGPS_MARGIN_SEC) {
			err = -ETIMEDOUT;
		}
		/* data is expired; use most recent entry */
		pnum = count - 1;
	} else {
		pnum = (offset_sec / SEC_PER_MIN) / period_min;
	}

	*prediction = &pgps_storage->predictions[pnum];
	return validate_prediction(*prediction, cur_gps_day, cur_gps_time_of_day,
				   period_min);
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
	int err;
	enum gps_pgps_type types[9];
	size_t type_count = 0;
	cJSON *data_obj;
	cJSON *pgps_req_obj;

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

	err = cJSON_AddNumberToObject(data_obj, PGPS_JSON_PRED_COUNT,
				      request->prediction_count);
	if (err) {
		LOG_ERR("Failed to add pred count to P-GPS request %d", err);
		goto cleanup;
	}
	err = cJSON_AddNumberToObject(data_obj, PGPS_JSON_PRED_INT_MIN,
				      request->prediction_count);
	if (err) {
		LOG_ERR("Failed to add pred int min to P-GPS request %d", err);
		goto cleanup;
	}
	err = cJSON_AddNumberToObject(data_obj, PGPS_JSON_GPS_DAY,
				      request->prediction_count);
	if (err) {
		LOG_ERR("Failed to add gps day to P-GPS request %d", err);
		goto cleanup;
	}
	err = cJSON_AddNumberToObject(data_obj, PGPS_JSON_GPS_TIME,
				      request->prediction_count);
	if (err) {
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

	err = nrf_cloud_pgps_get_time(&gps_day, &gps_time_of_day);
	if (err) {
		/* unknown time; ask server for newest data */
		gps_day = 0;
		gps_time_of_day = 0;
	}

	struct gps_pgps_request request = {
		.prediction_count = CONFIG_NRF_CLOUD_PGPS_NUM_PREDICTIONS,
		.prediction_period_min = CONFIG_NRF_CLOUD_PGPS_PREDICTION_PERIOD,
		.gps_day = gps_day,
		.gps_time_of_day = gps_time_of_day;
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
	default:
		LOG_DBG("Unhandled P-GPS data type: %d", element->type);
		return 0;
	}

	return len;
}

static int open_storage(void)
{
	const struct device *flash_dev;

	flash_dev = device_get_binding(PM_MCUBOOT_SECONDARY_DEV_NAME);
	if (flash_dev == NULL) {
		LOG_ERR("Failed to get device '%s'",
			PM_MCUBOOT_SECONDARY_DEV_NAME);
		return -EFAULT;
	}

	err = stream_flash_init(&stream, fdev,
				write_buf, sizeof(write_buf),
				PM_MCUBOOT_SECONDARY_ADDRESS,
				PM_MCUBOOT_SECONDARY_SIZE, NULL);
	if (err) {
		LOG_ERR("Failed to init flash stream: %d", err);
		state = PGPS_NONE;
		return err;
	}
	return err;
}

static int store_prediction_header(struct nrf_cloud_pgps_header *header)
{
	int err;

	err = stream_flash_buffered_write(&stream, (uint8_t *)header,
					  sizeof(*header), false);
	if (err) {
		LOG_ERR("Error writing pgps header: %d", err);
	}
	return err;
}

static int store_prediction(struct nrf_cloud_pgps_prediction *p, bool last,
			    uint32_t sentinel)
{
	int err;

	/* create unique value for this set of predictions, to be stored
	 * right after the final prediction
	 */

	err = stream_flash_buffered_write(&stream, (uint8_t *)p,
					  sizeof(*p), false);
	if (err) {
		LOG_ERR("Error writing pgps prediction: %d", err);
	}
	if (last) {
		err = stream_flash_buffered_write(&stream, sentinel,
						  sizeof(sentinel), true);
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
	sys_time.type = NRF_CLOUD_AGPS_GPS_SYSTEM_CLOCK;
	sys_time.count = 1;
	sys_time.time.date_day = p->time_and_tow.date_day;
	sys_time.time.time_full_s = p->time_and_tow.time_full_s;
	sys_time.time.time_frac_ms = 0;
	sys_time.time.sv_mask = 0;

	/* use current time if available */
	err = nrf_cloud_pgps_get_time(NULL, &day, &sec);
	if (!err) {
		sys_time.time.date_day = day;
		sys_time.time.time_full_s = sec;
		/* it is ok to ignore this err in the function return below */
	}

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

	location.type = NRF_CLOUD_AGPS_LOCATION;
	location.count = 1;
	location.location.latitude = latitude;
	location.location.longitude = longitude;
	location.location.altitude = 0;
	location.location.unc_semimajor = 0;
	location.location.unc_semiminor = 0;
	location.location.orientation_major = 0;
	location.location.unc_altitude = 0xFF; /* tell modem it is invalid */
	location.location.confidence = 0;

	/* send location */
	err = nrf_cloud_agps_process((const char *)&location, sizeof(location),
				     socket);
	if (err) {
		LOG_ERR("Error injecting PGPS location (%u, %u): %d",
			location.location.latitude, location.location.longitude,
			err);
		ret = err;
	}

	/* send ephemerii */
	err = nrf_cloud_agps_process((const char *)&p->ephemeris_type,
				     sizeof(p->ephemeris_type) +
				     sizeof(p->ephemeris_count) +
				     sizeof(p->ephemerii), socket);
	if (err) {
		LOG_ERR("Error injecting ephermerii: %d", err);
		ret = err;
	}
	return ret; /* just return last non-zero error, if any */
}

int nrf_cloud_pgps_process(const char *buf, size_t buf_len)
{
	static bool first;
	static uint16_t expected_count;
	static uint16_t pcount;
	static int64_t expected_sec;
	static int64_t prev_expected_sec;
	static uint16_t period_sec;
	int err;
	struct nrf_cloud_apgs_element element = {};
	struct nrf_cloud_pgps_header *header;
	size_t parsed_len = 0;
	uint8_t *p;
	int64_t gps_sec;
	bool last = ((pcount + 1) == expected_count);

	if (state < PGPS_LOADING) {
		state = PGPS_LOADING;
		first = true;
		pcount = 0;
		prev_expected_sec = expected_sec = 0;

		header = (struct nrf_cloud_pgps_header *)buf;

		if (header->schema_version != NRF_CLOUD_PGPS_BIN_SCHEMA_VERSION) {
			LOG_ERR("Cannot parse schema version: %d",
				header->schema_version);
			state = PGPS_NONE;
			return -EINVAL;
		}
		if (header->num_items != 1) {
			LOG_ERR("num elements wrong: %u", header->num_elements);
			state = PGPS_NONE;
			return -EINVAL;
		}

		err = open_storage();
		if (err) {
			state = PGPS_NONE;
			return err;
		}

		LOG_INF("Received PGPS data. Schema version:%u, type:%u, num:%u, "
			"count:%u",
			header->schema_version, header->array_type,
			header->num_items, header->prediction_count);
		LOG_INF("  size:%u, period (minutes):%u, GPS day:%u, GPS time:%u",
			header->prediction_size, header->prediction_period_min,
			header->gps_day, header->gps_time_of_day);

		expected_count = header->prediction_count;
		LOG_INF("Storing PGPS header");
		store_prediction_header(header);
		first = false;
		start_sec = gps_day_time_to_sec(header->gps_day,
						header->gps_time_of_day);
		period_sec = header->prediction_period_min * SEC_PER_MIN;
		expected_sec = start_sec;

		p = (uint8_t *)&header->predictions[0];
		buf_len -= sizeof(*header);
	} else {
		p = buf;
	}

	while (parsed_len < buf_len) {
		struct agps_header *elem = (struct agps_header *)p;
		size_t element_size = get_next_pgps_element(&element, p);

		LOG_INF("Element type:%u, count:%u, size:%\n",
			elem->type, elem->count, element_size);

		if (element_size == 0) {
			break;
		}
		if (element.type == NRF_CLOUD_AGPS_GPS_SYSTEM_CLOCK) {
			gps_sec = gps_day_time_to_sec(element.time_and_tow.date_day,
						      element.time_and_tow.time_full_s);
		}

		parsed_len += element_size;
		p += element_size;
	}

	/* @TODO: make this tolerate out of order packets?
	 * currently we will at least ignore out of order, but require them
	 * in order eventually in order to be stored
	 */
	if (parsed_len == buf_len) {
		LOG_INF("Parsing finished\n");
		if (gps_sec != expected_sec) {
			if (gps_sec == prev_expected_sec) {
				LOG_WRN("Received duplicate MQTT packet; ignoring");
			} else {
				LOG_ERR("Expected %ld sec, got %ld", expected_sec,
					gps_sec);
			}
		} else {
			LOG_INF("Storing prediction %u for gps sec %ld",
				pcount, gps_sec);
			store_prediction(p, last, gps_sec);
			prev_expected_sec = expected_sec;
			expected_sec += period_sec;
			pcount++;
		}
	} else {
		LOG_ERR("Parsing incomplete; aborting.");
		state = PGPS_NONE;
		return -EINVAL;
	}

	if (last) {
		state = PGPS_READY;
	}

	return 0;
}

int nrf_cloud_pgps_init(void)
{
	state = PGPS_NONE;
	int err = 0;

	if ((pgps_storage->schema_version != NRF_CLOUD_PGPS_BIN_SCHEMA_VERSION) ||
	    (pgps_storage->array_type != NRF_CLOUD_PGPS_PREDICTION_HEADER) ||
	    (pgps_storage->num_items != 1) ||
	    (pgps_storage->prediction_period_min !=
	     CONFIG_NRF_CLOUD_PGPS_PREDICTION_PERIOD) ||
	    (pgps_storage->prediction_count !=
	     CONFIG_NRF_CLOUD_PGPS_NUM_PREDICTIONS)) {
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
		struct nrf_cloud_pgps_prediction *p = &pgps_storage->predictions[0];
		int64_t gps_sec = gps_day_time_to_sec(gps_day, gps_time_of_day);
		struct nrf_cloud_pgps_prediction *test_prediction;
		int err;

		LOG_INF("Checking stored PGPS data; count:%u, period_min:%u",
			count, period_min);

		err = nrf_cloud_find_prediction(&test_prediction);
		if (err == -ETIMEDOUT) {
			LOG_INF("Predictions expired. Requesting predictions...");
			err = nrf_cloud_pgps_request_all();
			if (!err) {
				state = PGPS_REQUESTING;
				return 0;
			}
		}

		for (i = 0; i < count; i++, p++) {
			err = validate_prediction(p, gps_day, gps_time_of_day,
						  period_min, true);
			if (err) {
				LOG_ERR("prediction %u, gps_day:%u, "
					"gps_time_of_day:%u is bad: %d",
					i + 1, gps_day, gps_time_of_day, err);
				/* request partial data; download interrupted? */
				break;
			}
			LOG_INF("prediction %u, gps_day:%u, gps_time_of_day:%u",
				i + 1, gps_day, gps_time_of_day);

			/* @TODO: check last Flash page in this prediction to see
			 * if it is erased or written
			 */

			/* calculate next expected time signature */
			gps_sec += period_min * SEC_PER_MIN;
			gps_sec_to_day_time(gps_sec, &gps_day, &gps_time_of_day);
		}
		if (i < count) {
			struct gps_pgps_request request;

			LOG_INF("Requesting %u predictions...", count - i);
			request.gps_day = gps_day;
			request.gps_time_of_day;
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

