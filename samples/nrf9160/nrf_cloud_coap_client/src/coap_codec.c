/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/net/coap.h>
#include <modem/lte_lc.h>
#include <net/wifi_location_common.h>
#include <cJSON.h>
#include "nrf_cloud_codec.h"
#include "cbor_encode_types.h"
#include "cbor_encode.h"
#include "coap_client.h"
#include "coap_codec.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(coap_codec, CONFIG_NRF_CLOUD_COAP_CLIENT_LOG_LEVEL);

#define AGPS_FILTERED			"filtered=true"
#define AGPS_ELEVATION_MASK		"&mask=%u"
#define AGPS_NET_INFO			"&mcc=%u&mnc=%u&tac=%u&eci=%u"
#define AGPS_CUSTOM_TYPE		"&customTypes=%s"

#if 0
int cbor_decode_loc_response(enum nrf_cloud_coap_response response,
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
#endif

int coap_codec_encode_sensor(const char *app_id, double value, const char *topic,
			     int64_t ts, uint8_t *buf, size_t *len, enum coap_content_format fmt)
{
	int err;

	if (fmt == COAP_CONTENT_FORMAT_APP_CBOR) {
		err = -ENOTSUP; // cbor_encode_sensor(app_id, value, ts, buf, len);
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

#if 0
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
#endif

static void copy_cell(struct cell *dst, struct lte_lc_cell const *const src)
{
	dst->_cell_mcc = src->mcc;
	dst->_cell_mnc = src->mnc;
	dst->_cell_eci = src->id;
	dst->_cell_tac = src->tac;
	if (src->earfcn != NRF_CLOUD_LOCATION_CELL_OMIT_EARFCN) {
		dst->_cell_earfcn._cell_earfcn = src->earfcn;
		dst->_cell_earfcn_present = true;
	} else {
		dst->_cell_earfcn_present = false;
	}
	if (src->timing_advance != NRF_CLOUD_LOCATION_CELL_OMIT_TIME_ADV) {
		uint16_t t_adv = src->timing_advance;

		if (t_adv > NRF_CLOUD_LOCATION_CELL_TIME_ADV_MAX) {
			t_adv = NRF_CLOUD_LOCATION_CELL_TIME_ADV_MAX;
		}
		dst->_cell_adv._cell_adv = t_adv;
		dst->_cell_adv_present = true;
	} else {
		dst->_cell_adv_present = false;
	}
	if (src->rsrp != NRF_CLOUD_LOCATION_CELL_OMIT_RSRP) {
		dst->_cell_rsrp._cell_rsrp = RSRP_IDX_TO_DBM(src->rsrp);
		dst->_cell_rsrp_present = true;
	} else {
		dst->_cell_rsrp_present = false;
	}
	if (src->rsrq != NRF_CLOUD_LOCATION_CELL_OMIT_RSRQ) {
		dst->_cell_rsrq._cell_rsrq = RSRQ_IDX_TO_DB(src->rsrq);
		dst->_cell_rsrq_present = true;
	} else {
		dst->_cell_rsrq_present = false;
	}
}

static void copy_ncells(struct ncell *dst, int num, struct lte_lc_ncell *src)
{
	for (int i = 0; i < num; i++) {
		dst->_ncell_earfcn = src->earfcn;
		dst->_ncell_pci = src->phys_cell_id;
		if (src->rsrp != NRF_CLOUD_LOCATION_CELL_OMIT_RSRP) {
			dst->_ncell_rsrp._ncell_rsrp = RSRP_IDX_TO_DBM(src->rsrp);
			dst->_ncell_rsrp_present = true;
		} else {
			dst->_ncell_rsrp_present = false;
		}
		if (src->rsrq != NRF_CLOUD_LOCATION_CELL_OMIT_RSRQ) {
			dst->_ncell_rsrq._ncell_rsrq = RSRQ_IDX_TO_DB(src->rsrq);
			dst->_ncell_rsrq_present = true;
		} else {
			dst->_ncell_rsrq_present = false;
		}
		if (src->time_diff != LTE_LC_CELL_TIME_DIFF_INVALID) {
			dst->_ncell_timeDiff._ncell_timeDiff = src->time_diff;
			dst->_ncell_timeDiff_present = true;
		} else {
			dst->_ncell_timeDiff_present = false;
		}
		src++;
		dst++;
	}
}

static void copy_cell_info(struct lte_ar *lte_encode,
			   struct lte_lc_cells_info const *const cell_info)
{
	if (cell_info != NULL) {
		struct cell *cell = lte_encode->_lte_ar__cell;
		size_t num_cells = ARRAY_SIZE(lte_encode->_lte_ar__cell);

		if (cell_info->current_cell.id != LTE_LC_CELL_EUTRAN_ID_INVALID) {
			lte_encode->_lte_ar__cell_count++;
			copy_cell(cell, &cell_info->current_cell);
			cell->_cell_nmr_ncells_count = cell_info->ncells_count;
			if (cell_info->ncells_count) {
				copy_ncells(cell->_cell_nmr_ncells,
					    MIN(ARRAY_SIZE(cell->_cell_nmr_ncells),
						cell_info->ncells_count),
					    cell_info->neighbor_cells);
			}
			cell++;
			num_cells--;
		}
		if (cell_info->gci_cells != NULL) {
			for (int i = 0; i < cell_info->gci_cells_count; i++) {
				lte_encode->_lte_ar__cell_count++;
				copy_cell(cell, &cell_info->gci_cells[i]);
				cell++;
				num_cells--;
				if (!num_cells) {
					break;
				}
			}
		}
	}
}

static void copy_wifi_info(struct wifi_ob *wifi_encode,
			   struct wifi_scan_info const *const wifi_info)
{
	struct ap *dst = wifi_encode->_wifi_ob_accessPoints__ap;
	struct wifi_scan_result *src = wifi_info->ap_info;
	size_t num_aps = ARRAY_SIZE(wifi_encode->_wifi_ob_accessPoints__ap);

	wifi_encode->_wifi_ob_accessPoints__ap_count = 0;

	for (int i = 0; i < wifi_info->cnt; i++) {
		wifi_encode->_wifi_ob_accessPoints__ap_count++;

		dst->_ap_macAddress.value = src->mac;
		dst->_ap_macAddress.len = src->mac_length;
		if (src->ssid_length && src->ssid[0]) {
			dst->_ap_ssid._ap_ssid.value = src->ssid;
			dst->_ap_ssid._ap_ssid.len = src->ssid_length;
			dst->_ap_ssid_present = true;
		} else {
			dst->_ap_ssid_present = false;
		}
		dst->_ap_age_present = false;
		if (src->channel != NRF_CLOUD_LOCATION_WIFI_OMIT_CHAN) {
			dst->_ap_channel._ap_channel = src->channel;
			dst->_ap_channel_present = true;
		} else {
			dst->_ap_channel_present = false;
		}
		if (src->rssi != NRF_CLOUD_LOCATION_WIFI_OMIT_RSSI) {
			dst->_ap_signalStrength._ap_signalStrength = src->rssi;
			dst->_ap_signalStrength_present = true;
		} else {
			dst->_ap_signalStrength_present = false;
		}
		num_aps--;
		if (!num_aps) {
			break;
		}
	}
}

int coap_codec_encode_location_req(struct lte_lc_cells_info const *const cell_info,
				   struct wifi_scan_info const *const wifi_info,
				   uint8_t *buf, size_t *len, enum coap_content_format fmt)
{
	int err;

	if (fmt == COAP_CONTENT_FORMAT_APP_CBOR) {
		struct location_req input;
		size_t out_len;

		memset(&input, 0, sizeof(struct location_req));
		input._location_req_lte_present = (cell_info != NULL);
		if (cell_info) {
			copy_cell_info(&input._location_req_lte._location_req_lte, cell_info);
		}
		input._location_req_wifi_present = (wifi_info != NULL);
		if (wifi_info) {
			copy_wifi_info(&input._location_req_wifi._location_req_wifi, wifi_info);
		}
		err = cbor_encode_location_req(buf, *len, &input, &out_len);
		if (err) {
			LOG_ERR("Error %d encoding groundfix", err);
			err = -EINVAL;
			*len = 0;
		} else {
			*len = out_len;
		}
	} else {
		char *out;

		err = nrf_cloud_format_location_req(cell_info, wifi_info, &out);
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
		return -ENOTSUP; // cbor_encode_agps(request, buf, len);
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

