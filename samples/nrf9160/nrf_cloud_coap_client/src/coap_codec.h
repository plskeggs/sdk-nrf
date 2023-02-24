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

int cbor_encode_sensor(double value, int64_t ts, uint8_t *buf, size_t *len);

int cbor_encode_cell_pos(bool do_reply, unsigned int mcc, unsigned int mnc, unsigned int eci,
			 unsigned int tac, unsigned int adv, unsigned int earfcn,
			 float rsrp, float rsrq, uint8_t *buf, size_t *len);

#endif /* COAP_CODEC_H_ */
