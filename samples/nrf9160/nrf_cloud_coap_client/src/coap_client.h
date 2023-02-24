/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef COAP_CLIENT_H_
#define COAP_CLIENT_H_

enum nrf_cloud_coap_response
{
	NRF_CLOUD_COAP_NONE,
	NRF_CLOUD_COAP_LOCATION,
	NRF_CLOUD_COAP_PGPS,
	NRF_CLOUD_COAP_FOTA_JOB
};

int client_get_sock(void);

int client_init(void);

int client_provision(bool force);

int client_wait(int timeout);

int client_receive(enum nrf_cloud_coap_response expected_response);

/**@brief Handles responses from the remote CoAP server. */
int client_handle_get_response(enum nrf_cloud_coap_response expected_response,
			       uint8_t *buf, int received);

/**@brief Send CoAP GET request. */
int client_get_send(const char *resource, uint8_t *buf, size_t len);

/**@brief Send CoAP POST request. */
int client_post_send(const char *resource, uint8_t *buf, size_t buf_len, bool cbor_fmt);

int client_close(void);

#endif /* COAP_CLIENT_H_ */
