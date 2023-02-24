/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/net/coap.h>
#include <tinycbor/cbor.h>
#include <tinycbor/cbor_buf_reader.h>
#include <tinycbor/cbor_buf_writer.h>
#include <modem/lte_lc.h>
#include "nrf_cloud_codec.h"
#include <cJSON.h>
#include "coap_client.h"
#include "coap_codec.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(coap_codec, CONFIG_NRF_CLOUD_COAP_CLIENT_LOG_LEVEL);

int cbor_decode_response(enum nrf_cloud_coap_response response,
			 const uint8_t *payload, uint16_t payload_len,
			 char *temp_buf, size_t temp_size)
{
	struct CborParser parser;
	struct cbor_buf_reader reader;
	CborError err_cbor;
	CborType type_cbor;
	CborValue value;
	CborValue map_value;
	int map_len = 0;
	char fulfilled_with[11] = {0};
	size_t len;
	double lat;
	double lon;
	double uncertainty;

	cbor_buf_reader_init(&reader, payload, payload_len);
	err_cbor = cbor_parser_init(&reader.r, 0, &parser, &value);
	if (err_cbor != CborNoError) {
		LOG_ERR("Error parsing response: %d", err_cbor);
		return -EIO;
	}

	type_cbor = cbor_value_get_type(&value);
	if (type_cbor != CborArrayType) {
		LOG_ERR("Expected CBOR array; got %d", (int)type_cbor);
		return -EBADMSG;
	}

	/* TODO: use response to look up entry in array of
	 * structs that indicate expected type for each value,
	 * then uses that to decode
	 */
	err_cbor = cbor_value_get_array_length(&value, &map_len);
	if ((err_cbor != CborNoError) || (map_len != 4)) {
		LOG_ERR("Error getting array length: %d", err_cbor);
		return -EBADMSG;
	}

	/* Enter the array */
	err_cbor = cbor_value_enter_container(&value, &map_value);
	if (err_cbor != CborNoError) {
		LOG_ERR("Error entering array: %d", err_cbor);
		return -EACCES;
	}

	len = sizeof(fulfilled_with) - 1;
	err_cbor = cbor_value_copy_text_string(&map_value, fulfilled_with,
					       &len, &map_value);
	if (err_cbor != CborNoError) {
		LOG_ERR("Error getting fulfilled_with: %d", err_cbor);
		return -EBADMSG;
	}
	fulfilled_with[len] = '\0';

	err_cbor = cbor_value_get_double(&map_value, &lat);
	if (err_cbor != CborNoError) {
		LOG_ERR("Error getting lat: %d", err_cbor);
		return -EBADMSG;
	}

	err_cbor = cbor_value_get_double(&map_value, &lon);
	if (err_cbor != CborNoError) {
		LOG_ERR("Error getting lon: %d", err_cbor);
		return -EBADMSG;
	}

	err_cbor = cbor_value_get_double(&map_value, &uncertainty);
	if (err_cbor != CborNoError) {
		LOG_ERR("Error getting uncertainty: %d", err_cbor);
		return -EBADMSG;
	}

	/* Exit array */
	err_cbor = cbor_value_leave_container(&value, &map_value);
	if (err_cbor != CborNoError) {
		return -EACCES;
	}

	snprintf(temp_buf, temp_size - 1, "fulfilledWith:%s, lat:%g, lon:%g, unc:%g\n",
	       fulfilled_with, lat, lon, uncertainty);

	return 0;
}

int cbor_encode_sensor(const char *app_id, double value, int64_t ts, uint8_t *buf, size_t *len)
{
	ARG_UNUSED(app_id); /* make a way to encode this */
	struct cbor_buf_writer wr_encoder;
	struct CborEncoder encoder;
	CborEncoder array_encoder;

	cbor_buf_writer_init(&wr_encoder, buf, *len);
	cbor_encoder_init(&encoder, &wr_encoder.enc, 0);

	cbor_encoder_create_array(&encoder, &array_encoder, 2);
	cbor_encode_double(&array_encoder, value);
	cbor_encode_uint(&array_encoder, ts);
	cbor_encoder_close_container(&encoder, &array_encoder);

	*len = cbor_buf_writer_buffer_size(&wr_encoder, buf);
	LOG_HEXDUMP_DBG(buf, *len, "CBOR TEMP");

	return 0;
}

int coap_codec_encode_sensor(const char *app_id, double value, const char *topic,
			     int64_t ts, uint8_t *buf, size_t *len, enum coap_content_format fmt)
{
	int err;

	if (fmt == COAP_CONTENT_FORMAT_APP_CBOR) {
		err = cbor_encode_sensor(app_id, value, ts, buf, len);
	} else {
		struct nrf_cloud_data out;

		err = nrf_cloud_encode_message(app_id, value, NULL, topic, ts, &out);
		if (err) {
			*len = 0;
		} else if (*len < out.len) {
			*len = 0;
			cJSON_free((void *)out.ptr);
			err = -E2BIG;
		} else {
			*len = MIN(out.len, *len);
			memcpy(buf, out.ptr, *len);
			cJSON_free((void *)out.ptr);
		}
	}
	return err;
}

int cbor_encode_cell_pos(bool do_reply, unsigned int mcc, unsigned int mnc, unsigned int eci,
			 unsigned int tac, unsigned int adv, unsigned int earfcn,
			 float rsrp, float rsrq, uint8_t *buf, size_t *len)
{
	struct cbor_buf_writer wr_encoder;
	struct CborEncoder encoder;
	CborEncoder array_encoder;
	CborEncoder nbr_array_encoder;

	cbor_buf_writer_init(&wr_encoder, buf, *len);
	cbor_encoder_init(&encoder, &wr_encoder.enc, 0);

	cbor_encoder_create_array(&encoder, &array_encoder, 8);
	cbor_encode_uint(&array_encoder, eci);
	cbor_encode_uint(&array_encoder, mcc);
	cbor_encode_uint(&array_encoder, mnc);
	cbor_encode_uint(&array_encoder, tac);
	cbor_encode_uint(&array_encoder, adv);
	cbor_encode_uint(&array_encoder, earfcn);
	cbor_encoder_create_array(&array_encoder, &nbr_array_encoder, CborIndefiniteLength);
	cbor_encoder_close_container(&array_encoder, &nbr_array_encoder);
	cbor_encode_float(&array_encoder, rsrp);
	cbor_encode_float(&array_encoder, rsrq);
	cbor_encoder_close_container(&encoder, &array_encoder);

	*len = cbor_buf_writer_buffer_size(&wr_encoder, buf);
	LOG_HEXDUMP_DBG(buf, *len, "CBOR CELL_POS");

	return 0;
}

int coap_codec_encoder_cell_pos(struct lte_lc_cells_info const *const cell_info,
				uint8_t *buf, size_t *len, enum coap_content_format fmt)
{
	int err;

	if (fmt == COAP_CONTENT_FORMAT_APP_CBOR) {
		const struct lte_lc_cell *c = &cell_info->current_cell;

		err = cbor_encode_cell_pos(true, c->mcc, c->mnc, c->id, c->tac, c->timing_advance,
					   c->earfcn, c->rsrp, c->rsrq, buf, len);
	} else {
		char *out;

		err = nrf_cloud_format_location_req(cell_info, NULL, &out);
		if (!err) {
			if (strlen(out) >= *len) {
				err = -E2BIG;
			} else {
				*len = MIN(strlen(out), *len);
				memcpy(buf, out, *len);
				buf[*len] = '\0';
			}
			cJSON_free((void *)out);
		}
	}
	return err;
}


