/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/net/coap.h>
#include <zephyr/net/coap_client.h>
#include <zephyr/net/socket.h>
#include <modem/lte_lc.h>
#include <zephyr/random/rand32.h>
#include <nrf_socket.h>
#include <nrf_modem_at.h>
#include <date_time.h>
#include <net/nrf_cloud.h>
#include <cJSON.h>
#include <version.h>
#include "dtls.h"
#include "app_jwt.h"
#include "coap_codec.h"
#include "coap_client.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(coap_client, CONFIG_NRF_CLOUD_COAP_LOG_LEVEL);

/* Uncomment to enable sending cell_pos parameters with GET as payload */
//#define CELL_POS_PAYLOAD

#define APP_COAP_MAX_MSG_LEN 1280
#define APP_COAP_VERSION 1
#define APP_COAP_RECEIVE_INTERVAL_MS 5000

/** @TODO: figure out whether to make this a Kconfig value or place in a header */
#define CDDL_VERSION 1

static struct sockaddr_storage server;

static int sock;
static struct pollfd fds;
static bool initialized;
static bool authorized;

static struct connection_info
{
	uint8_t s4_addr[4];
	uint8_t d4_addr[4];
} connection_info;

static struct coap_client coap_client;

static char jwt[700];

bool client_is_authorized(void)
{
	return authorized;
}

/**@brief Resolves the configured hostname. */
static int server_resolve(void)
{
	int err;
	struct addrinfo *result;
	struct addrinfo hints = {
		.ai_family = AF_INET,
		.ai_socktype = SOCK_DGRAM
	};
	char ipv4_addr[NET_IPV4_ADDR_LEN];

	LOG_DBG("Looking up server %s", CONFIG_NRF_CLOUD_COAP_SERVER_HOSTNAME);
	err = getaddrinfo(CONFIG_NRF_CLOUD_COAP_SERVER_HOSTNAME, NULL, &hints, &result);
	if (err != 0) {
		LOG_ERR("ERROR: getaddrinfo for %s failed: %d",
			CONFIG_NRF_CLOUD_COAP_SERVER_HOSTNAME, err);
		return -EIO;
	}

	if (result == NULL) {
		LOG_ERR("ERROR: Address not found");
		return -ENOENT;
	}

	/* IPv4 Address. */
	struct sockaddr_in *server4 = ((struct sockaddr_in *)&server);

	server4->sin_addr.s_addr =
		((struct sockaddr_in *)result->ai_addr)->sin_addr.s_addr;
	server4->sin_family = AF_INET;
	server4->sin_port = htons(CONFIG_NRF_CLOUD_COAP_SERVER_PORT);

	connection_info.s4_addr[0] = server4->sin_addr.s4_addr[0];
	connection_info.s4_addr[1] = server4->sin_addr.s4_addr[1];
	connection_info.s4_addr[2] = server4->sin_addr.s4_addr[2];
	connection_info.s4_addr[3] = server4->sin_addr.s4_addr[3];

	inet_ntop(AF_INET, &server4->sin_addr.s_addr, ipv4_addr,
		  sizeof(ipv4_addr));
	LOG_INF("Server %s IP address: %s, port: %u",
		CONFIG_NRF_CLOUD_COAP_SERVER_HOSTNAME, ipv4_addr,
		CONFIG_NRF_CLOUD_COAP_SERVER_PORT);

	/* Free the address. */
	freeaddrinfo(result);

	return 0;
}

int client_get_sock(void)
{
	return sock;
}

/**@brief Initialize the CoAP client */
int client_init(void)
{
	int err;

	authorized = false;
	LOG_INF("Initializing async coap client");
	err = coap_client_init(&coap_client, NULL);
	if (err) {
		LOG_ERR("Failed to initialize coap client: %d", err);
		return err;
	}
	err = server_resolve();
	if (err) {
		LOG_ERR("Failed to resolve server name: %d", err);
		return err;
	}

	LOG_DBG("Creating socket");
#if !defined(CONFIG_NRF_CLOUD_COAP_DTLS)
	LOG_DBG("IPPROTO_UDP");
	sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#else
#if !defined(CONFIG_NET_SOCKETS_ENABLE_DTLS)
	LOG_DBG("IPPROTO_DTLS_1_2");
	sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_DTLS_1_2);
#else
	LOG_DBG("SPLIT STACK IPPROTO_DTLS_1_2");
	sock = socket(AF_INET, SOCK_DGRAM | SOCK_NATIVE_TLS, IPPROTO_DTLS_1_2);
#endif
#endif
	LOG_DBG("sock = %d", sock);
	if (sock < 0) {
		LOG_ERR("Failed to create CoAP socket: %d.", -errno);
		return -errno;
	}

#if defined(CONFIG_NRF_CLOUD_COAP_DTLS)
	err = dtls_init(sock);
	if (err < 0) {
		LOG_ERR("Failed to initialize the DTLS client: %d", err);
		return err;
	}
#endif
	err = connect(sock, (struct sockaddr *)&server,
		      sizeof(struct sockaddr_in));
	if (err < 0) {
		LOG_ERR("Connect failed : %d", -errno);
		return -errno;
	} else {
		LOG_DBG("Connect succeeded.");
	}

#if 0 // defined(CONFIG_NRF_CLOUD_COAP_DTLS_CID)
	err = dtls_load_session(sock);
	if (err) {
		LOG_ERR("Error loading session: %d", errno);
	} else {
		LOG_INF("Session loaded.");
	}
#endif

	/* Initialize FDS, for poll. */
	fds.fd = sock;
	fds.events = POLLIN;

	initialized = true;
	return err;
}

static K_SEM_DEFINE(con_ack_sem, 0, 1);

void auth_cb(int16_t result_code, size_t offset, const uint8_t *payload, size_t len,
	     bool last_block, void *user_data)
{
	LOG_INF("Authorization result_code: %u.%02u", result_code / 32, result_code & 0x1f);
	if (result_code < COAP_RESPONSE_CODE_BAD_REQUEST) {
		authorized = true;
	}
	k_sem_give(&con_ack_sem);
}

int client_connect(int wait_ms)
{
	int err;
	
	authorized = false;

#if !defined(CONFIG_NRF_CLOUD_COAP_DTLS_PSK)
	LOG_DBG("Generate JWT");
#if !defined(CONFIG_NET_SOCKETS_ENABLE_DTLS)
	err = nrf_cloud_jwt_generate(NRF_CLOUD_JWT_VALID_TIME_S_MAX, jwt, sizeof(jwt));
	if (err) {
		LOG_ERR("Error generating JWT with modem: %d", err);
	}
#else
	err = app_jwt_generate(NRF_CLOUD_JWT_VALID_TIME_S_MAX, jwt, sizeof(jwt));
	if (err) {
		LOG_ERR("Error generating JWT: %d", err);
		return err;
	}
#endif
	char ver_string[120] = {0};
	char mfw_string[60];

	err = modem_info_get_fw_version(mfw_string, sizeof(mfw_string));
	if (!err) {
		snprintf(ver_string, sizeof(ver_string) - 1, "mver=%s&cver=%s&dver=%d",
			 mfw_string, STRINGIFY(BUILD_VERSION), CDDL_VERSION);
	} else {
		LOG_ERR("Unable to obtain the modem firmware version: %d", err);
	}
	LOG_INF("Request authorization with JWT");
	err = client_post_send("auth/jwt", err ? NULL : ver_string,
			       (uint8_t *)jwt, strlen(jwt),
			       COAP_CONTENT_FORMAT_TEXT_PLAIN, true, auth_cb, NULL);
	if (err) {
		return err;
	}

	err = k_sem_take(&con_ack_sem, K_MSEC(wait_ms));
#endif
	if (authorized) {
		LOG_INF("Authorized");
	}
	return err;
}

int client_provision(bool force)
{
	int err = 0;

	if (force || IS_ENABLED(CONFIG_NET_SOCKETS_ENABLE_DTLS)) {
#if defined(CONFIG_NRF_CLOUD_COAP_DTLS_PSK)
		err = provision_psk();
#else
		err = provision_ca();
#endif
	}
	return err;
}

static K_SEM_DEFINE(cb_sem, 0, 1);

struct user_cb {
	coap_client_response_cb_t cb;
	void *user_data;
};

void client_callback(int16_t result_code, size_t offset, const uint8_t *payload, size_t len,
					  bool last_block, void *user_data)
{
	struct user_cb *user_cb = (struct user_cb *)user_data;

	LOG_DBG("Got callback: rc:%u.%02u, ofs:%u, lb:%u", result_code / 32, result_code & 0x1f, offset, last_block);
	if (payload && len) {
		LOG_HEXDUMP_DBG(payload, MIN(len, 96), "payload received");
	}
	if ((user_cb != NULL) && (user_cb->cb != NULL)) {
		LOG_DBG("Calling user's callback %p", user_cb->cb);
		user_cb->cb(result_code, offset, payload, len, last_block, user_cb->user_data);
	}
	if (last_block || (result_code >= COAP_RESPONSE_CODE_BAD_REQUEST)) {
		LOG_DBG("Giving sem");
		k_sem_give(&cb_sem);
	}
}

static int client_send(enum coap_method method, const char *resource, const char *query,
		       uint8_t *buf, size_t buf_len,
		       enum coap_content_format fmt_out,
		       enum coap_content_format fmt_in,
		       bool response_expected,
		       bool reliable,
		       coap_client_response_cb_t cb, void *user)
{
	int err;
	char path[256];
	struct user_cb user_cb = {
		.cb = cb,
		.user_data = user
	};
	struct coap_client_option options[1] = {{
		.code = COAP_OPTION_ACCEPT,
		.len = 1,
		.value[0] = fmt_in
	}};
	struct coap_client_request request = {
		.method = method,
		.confirmable = reliable,
		.path = path,
		.fmt = fmt_out,
		.payload = buf,
		.len = buf_len,
		.cb = client_callback,
		.user_data = &user_cb
	};

	if (response_expected) {
		request.options = options;
		request.num_options = ARRAY_SIZE(options);
	} else {
		request.options = NULL;
		request.num_options = 0;
	}

	strncpy(path, resource, sizeof(path));
	if (query) {
		strncat(path, "?", sizeof(path));
		strncat(path, query, sizeof(path));
	}

	while ((err = coap_client_req(&coap_client, sock, NULL, &request, -1)) == -EAGAIN) {
		LOG_INF("CoAP client busy");
		k_sleep(K_MSEC(500));
	}

	if (err < 0) {
		LOG_ERR("Error sending CoAP request: %d", err);
	} else {
		LOG_DBG("Sent %d bytes", err);
		LOG_HEXDUMP_DBG(coap_client.send_buf, err, "Sent");
		err = k_sem_take(&cb_sem, K_FOREVER);
		LOG_DBG("Received sem");
	}
	return err;
}

/**@brief Send CoAP GET request. */
int client_get_send(const char *resource, const char *query,
		    uint8_t *buf, size_t len,
		    enum coap_content_format fmt_out,
		    enum coap_content_format fmt_in,
		    coap_client_response_cb_t cb, void *user)
{
	return client_send(COAP_METHOD_GET, resource, query,
			   buf, len, fmt_out, fmt_in, true, false, cb, user);
}

/**@brief Send CoAP POST request. */
int client_post_send(const char *resource, const char *query,
		    uint8_t *buf, size_t len,
		    enum coap_content_format fmt, bool reliable,
		    coap_client_response_cb_t cb, void *user)
{
	return client_send(COAP_METHOD_POST, resource, query,
			   buf, len, fmt, fmt, false, reliable, cb, user);
}

/**@brief Send CoAP PUT request. */
int client_put_send(const char *resource, const char *query,
		    uint8_t *buf, size_t len,
		    enum coap_content_format fmt,
		    coap_client_response_cb_t cb, void *user)
{
	return client_send(COAP_METHOD_PUT, resource, query,
			   buf, len, fmt, fmt, false, false, cb, user);
}

/**@brief Send CoAP DELETE request. */
int client_delete_send(const char *resource, const char *query,
		       uint8_t *buf, size_t len,
		       enum coap_content_format fmt,
		       coap_client_response_cb_t cb, void *user)
{
	return client_send(COAP_METHOD_DELETE, resource, query,
			   buf, len, fmt, fmt, false, true, cb, user);
}

/**@brief Send CoAP FETCH request. */
int client_fetch_send(const char *resource, const char *query,
		      uint8_t *buf, size_t len,
		      enum coap_content_format fmt_out,
		      enum coap_content_format fmt_in,
		      coap_client_response_cb_t cb, void *user)
{
	return client_send(COAP_METHOD_FETCH, resource, query,
			   buf, len, fmt_out, fmt_in, true, true, cb, user);
}

/**@brief Send CoAP PATCH request. */
int client_patch_send(const char *resource, const char *query,
		      uint8_t *buf, size_t len,
		      enum coap_content_format fmt,
		      coap_client_response_cb_t cb, void *user)
{
	return client_send(COAP_METHOD_PATCH, resource, query,
			   buf, len, fmt, fmt, false, true, cb, user);
}

int client_close(void)
{
	return close(sock);
}

