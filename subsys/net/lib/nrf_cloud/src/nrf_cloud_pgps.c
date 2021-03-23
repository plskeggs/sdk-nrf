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
#define GPS_TO_UTC_LEAP_SECONDS (37UL)
#define SEC_PER_MIN (60UL)
#define MIN_PER_HOUR (60UL)
#define SEC_PER_HOUR (MIN_PER_HOUR * SEC_PER_MIN)
#define HOURS_PER_DAY (24UL)
#define SEC_PER_DAY (HOURS_PER_DAY * SEC_PER_HOUR)
#define DAYS_PER_WEEK (7UL)
#define SECONDS_PER_WEEK (SEC_PER_DAY * DAYS_PER_WEEK)
#define PGPS_MARGIN_SEC SEC_PER_HOUR

#define PGPS_JSON_MSG_TYPE_KEY		"messageType"
#define PGPS_JSON_MSG_TYPE_VAL_DATA	"DATA"

#define PGPS_JSON_DATA_KEY		"data"
#define PGPS_JSON_MCC_KEY		"mcc"
#define PGPS_JSON_MNC_KEY		"mnc"
#define PGPS_JSON_AREA_CODE_KEY		"tac"
#define PGPS_JSON_CELL_ID_KEY		"eci"
#define PGPS_JSON_PHYCID_KEY		"phycid"
#define PGPS_JSON_TYPES_KEY		"types"
#define PGPS_JSON_CELL_LOC_KEY_DOREPLY	"doReply"

#define PGPS_JSON_APPID_KEY		"appId"
#define PGPS_JSON_APPID_VAL_PGPS	"PGPS"
#define PGPS_JSON_APPID_VAL_SINGLE_CELL	"SCELL"
#define PGPS_JSON_APPID_VAL_MULTI_CELL	"MCELL"
#define PGPS_JSON_CELL_LOC_KEY_LAT	"lat"
#define PGPS_JSON_CELL_LOC_KEY_LON	"lon"
#define PGPS_JSON_CELL_LOC_KEY_UNCERT	"uncertainty"

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

/* todo: get the correct value from somewhere */
static int gps_leap_seconds = GPS_TO_UTC_LEAP_SECONDS;
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
	return (int64_t)gps_day * SEC_PER_DAY + gps_time_of_day;
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
			       uint16_t period_min)
{
	int err = 0;

	/* validate that this prediction was actually updated and matches */
	if (p->time_and_tow_type != NRF_CLOUD_AGPS_GPS_SYSTEM_CLOCK) {
		err = -EINVAL;
	}
	else if (p->time_and_tow.date_day != gps_day) {
		err = -EINVAL;
	}
	else if (p->time_and_tow.time_full_sec > gps_time_of_day) {
		err = -EINVAL;
	} else if (gps_time_of_day >
		   (p->time_and_tow.time_full_sec + 
		    ((period_min + 1) * SEC_PER_MIN)) {
		/* even with an extra minute of slop, the current time of
		 * day is past the end of this prediction
		 */
		err = -EINVAL;
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
			err = EEXPIRED;
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

static int json_format_data_obj(cJSON *const data_obj,
	const struct modem_param_info *const modem_info)
{
	__ASSERT_NO_MSG(data_obj != NULL);
	__ASSERT_NO_MSG(modem_info != NULL);

	if (!cJSON_AddNumberToObject(data_obj, PGPS_JSON_MCC_KEY,
		modem_info->network.mcc.value) ||
	    !cJSON_AddNumberToObject(data_obj, PGPS_JSON_MNC_KEY,
		modem_info->network.mnc.value) ||
	    !cJSON_AddNumberToObject(data_obj, PGPS_JSON_AREA_CODE_KEY,
		modem_info->network.area_code.value) ||
	    !cJSON_AddNumberToObject(data_obj, PGPS_JSON_CELL_ID_KEY,
		(uint32_t)modem_info->network.cellid_dec) ||
	    !cJSON_AddNumberToObject(data_obj, PGPS_JSON_PHYCID_KEY, 0)) {
		return -ENOMEM;
	}

	return 0;
}

static int json_add_types_array(cJSON *const obj, enum gps_pgps_type *types,
				const size_t type_count)
{
	__ASSERT_NO_MSG(obj != NULL);
	__ASSERT_NO_MSG(types != NULL);

	cJSON *array;

	if (!type_count) {
		return -EINVAL;
	}

	array = cJSON_AddArrayToObject(obj, PGPS_JSON_TYPES_KEY);
	if (!array) {
		return -ENOMEM;
	}

	for (size_t i = 0; i < type_count; i++) {
		cJSON_AddItemToArray(array, cJSON_CreateNumber(types[i]));
	}

	if (cJSON_GetArraySize(array) != type_count) {
		cJSON_DeleteItemFromObject(obj, PGPS_JSON_TYPES_KEY);
		return -ENOMEM;
	}

	return 0;
}

static int json_add_modem_info(cJSON *const data_obj)
{
	__ASSERT_NO_MSG(data_obj != NULL);

	struct modem_param_info modem_info = {0};
	int err;

	err = get_modem_info(&modem_info);
	if (err) {
		return err;
	}

	return json_format_data_obj(data_obj, &modem_info);
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

	/* Add modem info and P-GPS types to the data object */
	err = json_add_modem_info(data_obj);
	if (err) {
		LOG_ERR("Failed to add modem info to P-GPS request: %d", err);
		goto cleanup;
	}
	err = json_add_types_array(data_obj, types, type_count);
	if (err) {
		LOG_ERR("Failed to add types array to P-GPS request %d", err);
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

static int copy_ephemeris(nrf_gnss_pgps_data_ephemeris_t *dst,
			  struct nrf_cloud_apgs_element *src)
{
	if ((src == NULL) || (dst == NULL)) {
		return -EINVAL;
	}

	dst->sv_id	= src->ephemeris->sv_id;
	dst->health	= src->ephemeris->health;
	dst->iodc	= src->ephemeris->iodc;
	dst->toc	= src->ephemeris->toc;
	dst->af2	= src->ephemeris->af2;
	dst->af1	= src->ephemeris->af1;
	dst->af0	= src->ephemeris->af0;
	dst->tgd	= src->ephemeris->tgd;
	dst->ura	= src->ephemeris->ura;
	dst->fit_int	= src->ephemeris->fit_int;
	dst->toe	= src->ephemeris->toe;
	dst->w		= src->ephemeris->w;
	dst->delta_n	= src->ephemeris->delta_n;
	dst->m0		= src->ephemeris->m0;
	dst->omega_dot	= src->ephemeris->omega_dot;
	dst->e		= src->ephemeris->e;
	dst->idot	= src->ephemeris->idot;
	dst->sqrt_a	= src->ephemeris->sqrt_a;
	dst->i0		= src->ephemeris->i0;
	dst->omega0	= src->ephemeris->omega0;
	dst->crs	= src->ephemeris->crs;
	dst->cis	= src->ephemeris->cis;
	dst->cus	= src->ephemeris->cus;
	dst->crc	= src->ephemeris->crc;
	dst->cic	= src->ephemeris->cic;
	dst->cuc	= src->ephemeris->cuc;

	return 0;
}

static int copy_location(nrf_gnss_pgps_data_location_t *dst,
			 struct nrf_cloud_apgs_element *src)
{
	if ((src == NULL) || (dst == NULL)) {
		return -EINVAL;
	}

	dst->latitude		= src->location->latitude;
	dst->longitude		= src->location->longitude;
	dst->altitude		= src->location->altitude;
	dst->unc_semimajor	= src->location->unc_semimajor;
	dst->unc_semiminor	= src->location->unc_semiminor;
	dst->orientation_major	= src->location->orientation_major;
	dst->unc_altitude	= src->location->unc_altitude;
	dst->confidence		= src->location->confidence;

	return 0;
}

static int copy_time_and_tow(nrf_gnss_pgps_data_system_time_and_sv_tow_t *dst,
			     struct nrf_cloud_apgs_element *src)
{
	if ((src == NULL) || (dst == NULL)) {
		return -EINVAL;
	}

	dst->date_day		= src->time_and_tow->date_day;
	dst->time_full_s	= src->time_and_tow->time_full_s;
	dst->time_frac_ms	= src->time_and_tow->time_frac_ms;
	dst->sv_mask		= src->time_and_tow->sv_mask;

	if (src->time_and_tow->sv_mask == 0U) {
		LOG_DBG("SW TOW mask is zero, not copying TOW array");
		memset(dst->sv_tow, 0, sizeof(dst->sv_tow));

		return 0;
	}

	for (size_t i = 0; i < NRF_CLOUD_AGPS_MAX_SV_TOW; i++) {
		dst->sv_tow[i].flags = src->time_and_tow->sv_tow[i].flags;
		dst->sv_tow[i].tlm = src->time_and_tow->sv_tow[i].tlm;
	}

	return 0;
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
		return -1;
	}

	return len;
}

int nrf_cloud_pgps_process(const char *buf, size_t buf_len)
{
	int err;
	struct nrf_cloud_apgs_element element = {};
	struct nrf_cloud_pgps_header *header;
	size_t parsed_len = 0;
	uint8_t *p;
	static bool first;
	static uint16_t pcount;

	if (state < PGPS_LOADING) {
		state = PGPS_LOADING;
		first = true;
		pcount = 0;
	}

	/* todo:
	   - use data from first element (store in RAM so have it before
	   written to flash?
	   - use pcount to index into flash
	   - call stream_flash_init(), stream_flash_erase_page(),
	   stream_flash_buffered_write()
	   */

	header = (struct nrf_cloud_pgps_header *)buf;

	if (header->schema_version != NRF_CLOUD_PGPS_BIN_SCHEMA_VERSION) {
		LOG_ERR("Cannot parse schema version: %d", header->schema_version);
		state = PGPS_NONE;
		return -EINVAL;
	}
	if (header->num_items != 1) {
		LOG_ERR("num elements wrong: %u", header->num_elements);
		state = PGPS_NONE;
		return -EINVAL;
	}

	LOG_INF("Received PGPS data. Schema version:%u, type:%u, num:%u, count:%u",
		header->schema_version, header->array_type, header->num_items,
		header->prediction_count);
	LOG_INF("  size:%u, period (minutes):%u, GPS day:%u, GPS time:%u",
		header->prediction_size, header->prediction_period_min,
		header->gps_day, header->gps_time_of_day);

	p = (uint8_t *)&header->predictions[0];
	while (parsed_len < buf_len) {
		struct agps_header *elem = (struct agps_header *)p;
		size_t element_size =
			get_next_pgps_element(&element, p);

		LOG_INF("Element type:%u, count:%u, size:%\n",
			elem->type, elem->count, element_size);

		if (element_size == 0) {
			if (parsed_len != header->prediction_size) {
				LOG_ERR("Unexpected size of prediction; got:%u, expected:%u",
					parsed_len, header->prediction_size);
				break;
			}
			
			LOG_INF("Parsing finished\n");
			if (first) {
				store_prediction_header(header);
				first = false;
			}
			store_prediction(pcount, &header->predictions[0]);
			pcount++;
			break;
		} else if (element_size < 0) {
			LOG_ERR("Invalid A-GPS element detected in PGPS packet");
			break;
		}

		parsed_len += element_size;
		p += parsed_len;
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
		/* request predictions? */
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
		uint16_t gps_day_now;
		uint32_t gps_time_of_day_now;
		int64_t gps_sec_now;
		int err;

		err = nrf_cloud_pgps_get_time(&gps_sec_now, &gps_day_now,
					      &gps_time_of_day_now);
		if (err) {
			gps_day_now = 0;
			gps_time_of_day_now = 0;
		}

		LOG_INF("Checking stored PGPS data; count:%u, period_min:%u",
			count, period_min);

		for (i = 0; i < count; i++, p++) {
			if ((p->time_and_tow_type != NRF_CLOUD_AGPS_GPS_SYSTEM_CLOCK) ||
			    (p->time_and_tow_count != 1) ||
			    (p->time_and_tow.date_day != gps_day) ||
			    (p->time_and_tow.time_full_s != gps_time_of_day)) {
				LOG_ERR("prediction %u is bad", i);
				/* @TODO: request partial data; download interrupted? */
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
		}
	}
	return err;
}

