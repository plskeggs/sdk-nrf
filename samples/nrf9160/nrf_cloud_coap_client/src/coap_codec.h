/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef COAP_CODEC_H_
#define COAP_CODEC_H_

#include "coap_client.h"

int cbor_decode_response(enum nrf_cloud_coap_response response,
			 const uint8_t *payload, uint16_t payload_len,
			 char *temp_buf, size_t temp_size);

int coap_codec_encode_sensor(const char *app_id, double value, const char *topic,
			     int64_t ts, uint8_t *buf, size_t *len,
			     enum coap_content_format fmt);

int coap_codec_encoder_cell_pos(struct lte_lc_cells_info const *const cell_info,
				uint8_t *buf, size_t *len,
				enum coap_content_format fmt);

#endif /* COAP_CODEC_H_ */
