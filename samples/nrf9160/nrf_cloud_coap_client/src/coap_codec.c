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

#define AGPS_FILTERED			"filtered=true"
#define AGPS_ELEVATION_MASK		"&mask=%u"
#define AGPS_NET_INFO			"&mcc=%u&mnc=%u&tac=%u&eci=%u"
#define AGPS_CUSTOM_TYPE		"&customTypes=%s"

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

int coap_codec_encode_cell_pos(struct lte_lc_cells_info const *const cell_info,
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

int cbor_encode_agps(struct nrf_cloud_rest_agps_request const *const request,
		     uint8_t *buf, size_t *len)
{
	return 0;
}

/* Macro to print the comma separated list of custom types */
#define AGPS_TYPE_PRINT(buf, type)		\
	if (pos != 0) {				\
		buf[pos++] = ',';		\
	}					\
	buf[pos++] = (char)('0' + type)

static int format_agps_custom_types_str(struct nrf_modem_gnss_agps_data_frame const *const req,
	char *const types_buf)
{
	__ASSERT_NO_MSG(req != NULL);
	__ASSERT_NO_MSG(types_buf != NULL);

	int pos = 0;

	if (req->data_flags & NRF_MODEM_GNSS_AGPS_GPS_UTC_REQUEST) {
		AGPS_TYPE_PRINT(types_buf, NRF_CLOUD_AGPS_UTC_PARAMETERS);
	}
	if (req->sv_mask_ephe) {
		AGPS_TYPE_PRINT(types_buf, NRF_CLOUD_AGPS_EPHEMERIDES);
	}
	if (req->sv_mask_alm) {
		AGPS_TYPE_PRINT(types_buf, NRF_CLOUD_AGPS_ALMANAC);
	}
	if (req->data_flags & NRF_MODEM_GNSS_AGPS_KLOBUCHAR_REQUEST) {
		AGPS_TYPE_PRINT(types_buf, NRF_CLOUD_AGPS_KLOBUCHAR_CORRECTION);
	}
	if (req->data_flags & NRF_MODEM_GNSS_AGPS_NEQUICK_REQUEST) {
		AGPS_TYPE_PRINT(types_buf, NRF_CLOUD_AGPS_NEQUICK_CORRECTION);
	}
	if (req->data_flags & NRF_MODEM_GNSS_AGPS_SYS_TIME_AND_SV_TOW_REQUEST) {
		AGPS_TYPE_PRINT(types_buf, NRF_CLOUD_AGPS_GPS_TOWS);
		AGPS_TYPE_PRINT(types_buf, NRF_CLOUD_AGPS_GPS_SYSTEM_CLOCK);
	}
	if (req->data_flags & NRF_MODEM_GNSS_AGPS_POSITION_REQUEST) {
		AGPS_TYPE_PRINT(types_buf, NRF_CLOUD_AGPS_LOCATION);
	}
	if (req->data_flags & NRF_MODEM_GNSS_AGPS_INTEGRITY_REQUEST) {
		AGPS_TYPE_PRINT(types_buf, NRF_CLOUD_AGPS_INTEGRITY);
	}

	types_buf[pos] = '\0';

	return pos ? 0 : -EBADF;
}

int coap_codec_encode_agps(struct nrf_cloud_rest_agps_request const *const request,
			   uint8_t *buf, size_t *len, bool *query_string,
			   enum coap_content_format fmt)
{
	int ret;
	if (fmt == COAP_CONTENT_FORMAT_APP_CBOR) {
		*query_string = false; /* Data should be sent in payload */
		return cbor_encode_agps(request, buf, len);
	} else {
		char *url = (char *)buf;
		char custom_types[NRF_CLOUD_AGPS__LAST * 2];
		size_t pos = 0;
		size_t remain = *len;

		*query_string = true;
		*url = '\0';
		if (request->filtered) {
			ret = snprintk(&url[pos], remain, AGPS_FILTERED);
			if ((ret < 0) || (ret >= remain)) {
				LOG_ERR("Could not format URL: filtered");
				ret = -ETXTBSY;
				goto clean_up;
			}
			pos += ret;
			remain -= ret;
			ret = snprintk(&url[pos], remain, AGPS_ELEVATION_MASK, request->mask_angle);
			if ((ret < 0) || (ret >= remain)) {
				LOG_ERR("Could not format URL: mask angle");
				ret = -ETXTBSY;
				goto clean_up;
			}
			pos += ret;
			remain -= ret;
		}

#if 0 /* Not needed for CoAP? */
		if (req_type) {
			ret = snprintk(&url[pos], remain, AGPS_REQ_TYPE, req_type);
			if ((ret < 0) || (ret >= remain)) {
				LOG_ERR("Could not format URL: request type");
				ret = -ETXTBSY;
				goto clean_up;
			}
			pos += ret;
			remain -= ret;
		}
#endif

		if (request->type == NRF_CLOUD_REST_AGPS_REQ_CUSTOM) {
			ret = format_agps_custom_types_str(request->agps_req, custom_types);
			ret = snprintk(&url[pos], remain, AGPS_CUSTOM_TYPE, custom_types);
			if ((ret < 0) || (ret >= remain)) {
				LOG_ERR("Could not format URL: custom types");
				ret = -ETXTBSY;
				goto clean_up;
			}
			pos += ret;
			remain -= ret;
		}

		if (request->net_info) {
			ret = snprintk(&url[pos], remain, AGPS_NET_INFO,
				       request->net_info->current_cell.mcc,
				       request->net_info->current_cell.mnc,
				       request->net_info->current_cell.tac,
				       request->net_info->current_cell.id);
			if ((ret < 0) || (ret >= remain)) {
				LOG_ERR("Could not format URL: network info");
				ret = -ETXTBSY;
				goto clean_up;
			}
			pos += ret;
			remain -= ret;
		}

		if (buf[0] == '&') {
			char *s = &buf[1];
			char *d = buf;

			while (*s) {
				*(d++) = *(s++);
			}
			*d = '\0';
		}
		*len = pos;
		LOG_INF("A-GPS query: %s", (const char *)buf);
		return 0;

clean_up:
		buf[0] = 0;
		*len = 0;
		return ret;
	}
}

