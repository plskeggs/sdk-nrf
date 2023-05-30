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
	NRF_CLOUD_COAP_AGPS,
	NRF_CLOUD_COAP_PGPS,
	NRF_CLOUD_COAP_FOTA_JOB
};

#include <zephyr/net/coap_client.h>

int client_get_sock(void);

int client_init(void);

int client_connect(int wait_ms);

bool client_is_authorized(void);

int client_provision(bool force);

/**@brief Send CoAP GET request. */
int client_get_send(const char *resource, const char *query,
		    uint8_t *buf, size_t len,
		    enum coap_content_format fmt_out,
		    enum coap_content_format fmt_in,
		    coap_client_response_cb_t cb, void *user);

/**@brief Send CoAP POST request. */
int client_post_send(const char *resource, const char *query,
		    uint8_t *buf, size_t len,
		    enum coap_content_format fmt, bool reliable,
		    coap_client_response_cb_t cb, void *user);

/**@brief Send CoAP PUT request. */
int client_put_send(const char *resource, const char *query,
		    uint8_t *buf, size_t len,
		    enum coap_content_format fmt,
		    coap_client_response_cb_t cb, void *user);

/**@brief Send CoAP DELETE request. */
int client_delete_send(const char *resource, const char *query,
		       uint8_t *buf, size_t len,
		       enum coap_content_format fmt,
		       coap_client_response_cb_t cb, void *user);

/**@brief Send CoAP FETCH request. */
int client_fetch_send(const char *resource, const char *query,
		      uint8_t *buf, size_t len,
		      enum coap_content_format fmt_out,
		      enum coap_content_format fmt_in,
		      coap_client_response_cb_t cb, void *user);

/**@brief Send CoAP PATCH request. */
int client_patch_send(const char *resource, const char *query,
  		    uint8_t *buf, size_t len,
  		    enum coap_content_format fmt,
		    coap_client_response_cb_t cb, void *user);

int client_close(void);

#endif /* COAP_CLIENT_H_ */
