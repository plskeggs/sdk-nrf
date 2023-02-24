/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/net/coap.h>
#include <zephyr/net/socket.h>
#include <modem/lte_lc.h>
#include <zephyr/random/rand32.h>
#include <nrf_socket.h>
#include <nrf_modem_at.h>
#include <date_time.h>
#include <net/nrf_cloud.h>
#include <cJSON.h>
#include "dtls.h"
#include "app_jwt.h"
#include "coap_codec.h"
#include "coap_client.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(coap_client, CONFIG_NRF_CLOUD_COAP_CLIENT_LOG_LEVEL);

/* Uncomment to enable sending cell_pos parameters with GET as payload */
//#define CELL_POS_PAYLOAD

#define APP_COAP_MAX_MSG_LEN 1280
#define APP_COAP_VERSION 1

static struct sockaddr_storage server;

static int sock;
static struct pollfd fds;

static struct connection_info
{
	uint8_t s4_addr[4];
	uint8_t d4_addr[4];
} connection_info;

static uint8_t coap_buf[APP_COAP_MAX_MSG_LEN];

static sys_dlist_t con_messages;
static int num_con_messages;

struct nrf_cloud_coap_message {
	sys_dnode_t node;
	uint16_t message_id;
	uint16_t token_len;
	uint8_t token[8];
};

static const char *coap_types[] =
{
	"CON", "NON", "ACK", "RST", NULL
};

static char jwt[600];
static uint16_t next_token;

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

	err = getaddrinfo(CONFIG_COAP_SERVER_HOSTNAME, NULL, &hints, &result);
	if (err != 0) {
		LOG_ERR("ERROR: getaddrinfo failed %d", err);
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
	server4->sin_port = htons(CONFIG_COAP_SERVER_PORT);

	connection_info.s4_addr[0] = server4->sin_addr.s4_addr[0];
	connection_info.s4_addr[1] = server4->sin_addr.s4_addr[1];
	connection_info.s4_addr[2] = server4->sin_addr.s4_addr[2];
	connection_info.s4_addr[3] = server4->sin_addr.s4_addr[3];

	inet_ntop(AF_INET, &server4->sin_addr.s_addr, ipv4_addr,
		  sizeof(ipv4_addr));
	LOG_INF("Server %s IP address: %s", CONFIG_COAP_SERVER_HOSTNAME, ipv4_addr);

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

	sys_dlist_init(&con_messages);

	err = server_resolve();
	if (err) {
		LOG_ERR("Failed to resolve server name: %d", err);
		return err;
	}

	LOG_DBG("Creating socket");
#if !defined(CONFIG_COAP_DTLS)
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

#if defined(CONFIG_COAP_DTLS)
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
		LOG_INF("Connect succeeded.");
	}

	/* Initialize FDS, for poll. */
	fds.fd = sock;
	fds.events = POLLIN;

	/* Randomize token. */
	next_token = sys_rand32_get();

#if !defined(CONFIG_COAP_DTLS_PSK)
#if !defined(CONFIG_NET_SOCKETS_ENABLE_DTLS)
	err = nrf_cloud_jwt_generate(JWT_DURATION_S, jwt, sizeof(jwt));
	if (err) {
		return err;
	}
#else
	err = jwt_generate(JWT_DURATION_S, jwt, sizeof(jwt));
	if (err) {
		return err;
	}
#endif
	err = client_post_send("/auth-jwt", (uint8_t *)jwt, sizeof(jwt), false);
	if (err) {
		return err;
	}
#endif
	return 0;
}

int client_provision(bool force)
{
	int err = 0;

	if (force || IS_ENABLED(CONFIG_NET_SOCKETS_ENABLE_DTLS)) {
#if defined(CONFIG_COAP_DTLS_PSK)
		err = provision_psk();
#else
		err = provision_ca();
#endif
	}
	return err;
}

/* Returns 0 if data is available.
 * Returns -EAGAIN if timeout occured and there is no data.
 * Returns other, negative error code in case of poll error.
 */
int client_wait(int timeout)
{
	int ret = poll(&fds, 1, timeout);

	if (ret < 0) {
		LOG_ERR("poll error: %d", -errno);
		return -errno;
	}

	if (ret == 0) {
		/* Timeout. */
		return -EAGAIN;
	}

	if ((fds.revents & POLLERR) == POLLERR) {
		LOG_ERR("wait: POLLERR");
		return -EIO;
	}

	if ((fds.revents & POLLNVAL) == POLLNVAL) {
		LOG_ERR("wait: POLLNVAL");
		return -EBADF;
	}

	if ((fds.revents & POLLIN) != POLLIN) {
		return -EAGAIN;
	}

	return 0;
}

int client_receive(enum nrf_cloud_coap_response expected_response)
{
	int err;
	int received;

	LOG_DBG("Calling recv()");
	received = recv(sock, coap_buf, sizeof(coap_buf), MSG_DONTWAIT);
	if (received < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			LOG_WRN("socket EAGAIN");
			return 0;
		} else {
			LOG_ERR("Socket error: %d", -errno);
			return -errno;
		}
	}

	if (received == 0) {
		LOG_WRN("Empty datagram");
		return 0;
	}

	LOG_DBG("Calling client_handle_get_response()");
	err = client_handle_get_response(expected_response, coap_buf, received);
	if (err < 0) {
		LOG_ERR("Invalid response: %d", err);
	}

	return err;
}

/**@brief Send CoAP ACK or RST response. */
static int client_response(struct coap_packet *req,
			   const char *resource, uint16_t mid,
			   uint16_t token_len, uint8_t *token, bool ack)
{
	int err;
	struct coap_packet response;
	enum coap_msgtype msg_type = ack ? COAP_TYPE_ACK : COAP_TYPE_RESET;

	err = coap_packet_init(&response, coap_buf, sizeof(coap_buf),
			       APP_COAP_VERSION, msg_type,
			       token_len, token, 0, mid);
	if (err < 0) {
		LOG_ERR("Failed to create CoAP response, %d", err);
		return err;
	}

	if (resource && strlen(resource)) {
		err = coap_packet_append_option(&response, COAP_OPTION_URI_PATH,
						(uint8_t *)resource,
						strlen(resource));
		if (err < 0) {
			LOG_ERR("Failed to encode CoAP option, %d", err);
			return err;
		}
	}

	err = send(sock, response.data, response.offset, 0);
	if (err < 0) {
		LOG_ERR("Failed to send CoAP response, %d", -errno);
		return -errno;
	}

	LOG_INF("CoAP %s response sent: token 0x%04x", coap_types[msg_type], next_token);

	return 0;
}

/**@brief Handles responses from the remote CoAP server. */
int client_handle_get_response(enum nrf_cloud_coap_response expected_response,
			       uint8_t *buf, int received)
{
	int err;
	struct coap_packet reply;
	struct coap_option options[16] = {};
	int count;
	const uint8_t *payload;
	uint16_t payload_len;
	uint8_t token[8] = {0};
	uint16_t token_len;
	uint8_t temp_buf[100];
	char uri_path[64];
	enum coap_content_format format = 0;
	uint16_t message_id;
	uint8_t code;
	uint8_t type;
	struct nrf_cloud_coap_message *msg = NULL;

	err = coap_packet_parse(&reply, buf, received, NULL, 0);
	if (err < 0) {
		LOG_ERR("Malformed response received: %d", err);
		goto done;
	}

	token_len = coap_header_get_token(&reply, token);
	message_id = coap_header_get_id(&reply);
	code = coap_header_get_code(&reply);
	type = coap_header_get_type(&reply);
	
	if (type > COAP_TYPE_RESET) {
		LOG_ERR("Illegal CoAP type: %u", type);
		err = -EINVAL;
		goto done;
	}

	count = ARRAY_SIZE(options) - 1;
	count = coap_find_options(&reply, COAP_OPTION_URI_PATH,
				   options, count);
	if (count > 1) {
		LOG_ERR("Unexpected number of URI path options: %d", count);
		err = -EINVAL;
		goto done;
	} else if (count == 1) {
		memcpy(uri_path, options[0].value, options[0].len);
		uri_path[options[0].len] = '\0';
	} else {
		uri_path[0] = '\0';
	}

	LOG_INF("Got response uri:%s, code:0x%02x (%d.%02d), type:%u %s,"
		"MID:0x%04x, token:0x%02x%02x (len %u)",
		uri_path, code, code / 32u, code & 0x1f,
		type, coap_types[type], message_id,
		token[1], token[0], token_len);

	err = -ENOMSG;
	SYS_DLIST_FOR_EACH_CONTAINER(&con_messages, msg, node) {
		LOG_DBG("  mid:0x%04x, token:0x%02x%02x ?",
			msg->message_id, msg->token[1], msg->token[0]);
		if ((type == COAP_TYPE_CON) || (type == COAP_TYPE_NON_CON)) {
			/* match token only */
			if ((token_len == msg->token_len) &&
			    (memcmp(msg->token, token, token_len) == 0)) {
				LOG_INF("Matched token");
				err = 0;
				break;
			} else {
				LOG_DBG("  token not found yet");
			}
		} else { /* ACK or RESET */
			/* EMPTY responses: match MID only */
			if (code == 0) { 
				if (msg->message_id == message_id) {
					LOG_INF("Matched empty %s.",
						(type == COAP_TYPE_ACK) ? "ACK" : "RESET");
					err = 0;
					break;
				} else {
					LOG_DBG("  MID not found yet");
				}
			} else {
				if ((msg->message_id == message_id) &&
				    (token_len == msg->token_len) &&
				    (memcmp(msg->token, token, token_len) == 0)) {
					LOG_INF("Found MID and token");
					err = 0;
					break;
				} else {
					LOG_DBG("  MID and token not found yet");
				}
			}
		}
	}
	if (err) {
		LOG_ERR("No match for message and token");
		LOG_INF("Sending RESET to server");
		err = client_response(&reply, NULL, message_id, 0, NULL, false);
		if (err) {
			goto done;
		}
	} else if (type == COAP_TYPE_CON) {
		LOG_INF("ACKing a CON from server");
		err = client_response(&reply, uri_path, message_id, token_len, token, true);
		if (err) {
			goto done;
		}
	}

	count = ARRAY_SIZE(options) - 1;
	count = coap_find_options(&reply, COAP_OPTION_CONTENT_FORMAT,
				   options, count);
	if (count > 1) {
		LOG_ERR("Unexpected number of content format options: %d", count);
		err = -EINVAL;
		goto done;
	} else if (count == 1) {
		if (options[0].len == 0) {
			format = COAP_CONTENT_FORMAT_TEXT_PLAIN;
		} else if (options[0].len > 1) {
			LOG_ERR("Unexpected content format length: %d", options[0].len);
			err = -EINVAL;
			goto done;
		} else {
			format = options[0].value[0];
		}
		LOG_DBG("Content format: %d", format);
	}

	payload = coap_packet_get_payload(&reply, &payload_len);
	if (payload_len > 0) {
		if (format == COAP_CONTENT_FORMAT_APP_CBOR) {
			err = cbor_decode_response(expected_response, payload, payload_len,
						   temp_buf, sizeof(temp_buf));
			if (err) {
				goto done;
			}
		} else {
			snprintf(temp_buf, MIN(payload_len + 1, sizeof(temp_buf)), "%s", payload);
			LOG_INF("CoAP payload: %s", temp_buf);
		}
	} else {
		LOG_INF("CoAP payload: EMPTY");
	}

done:
	if (msg != NULL) {
		sys_dlist_remove(&msg->node);
		k_free(msg);
		num_con_messages--;
		LOG_INF("messages left: %d", num_con_messages);
	}
	return err;
}

/**@brief Send CoAP GET request. */
int client_get_send(const char *resource, uint8_t *buf, size_t len)
{
	int err;
	int i;
	int num;
	struct coap_packet request = {0};
	uint8_t format = COAP_CONTENT_FORMAT_APP_CBOR;
	uint16_t message_id = coap_next_id();
	enum coap_msgtype msg_type = COAP_TYPE_CON;
	struct nrf_cloud_coap_message *msg;

	next_token++;

	err = coap_packet_init(&request, coap_buf, sizeof(coap_buf),
			       APP_COAP_VERSION, msg_type,
			       sizeof(next_token), (uint8_t *)&next_token,
			       COAP_METHOD_GET, message_id);
	if (err < 0) {
		LOG_ERR("Failed to create CoAP request, %d", err);
		return err;
	}

	err = coap_packet_append_option(&request, COAP_OPTION_URI_PATH,
					(uint8_t *)resource,
					strlen(resource));
	if (err < 0) {
		LOG_ERR("Failed to encode CoAP option, %d", err);
		return err;
	}

	if (buf) {
		err = coap_packet_append_option(&request, COAP_OPTION_CONTENT_FORMAT,
						&format, sizeof(format));
		if (err < 0) {
			LOG_ERR("Failed to encode CoAP content format option, %d", err);
			return err;
		}

		err = coap_packet_append_payload_marker(&request);
		if (err < 0) {
			LOG_ERR("Failed to add CoAP payload marker, %d", err);
			return err;
		}

		err = coap_packet_append_payload(&request, buf, len);
		if (err < 0) {
			LOG_ERR("Failed to add CoAP payload, %d", err);
			return err;
		}
	}

	err = send(sock, request.data, request.offset, 0);
	if (err < 0) {
		LOG_ERR("Failed to send CoAP request, errno %d, err %d, sock %d, "
			"request.data %p, request.offset %u",
			-errno, err, sock, request.data, request.offset);
		return -errno;
	}

	num = (msg_type == COAP_TYPE_CON) ? 2 : 1;
	for (i = 0; i < num; i++) {
		msg = k_malloc(sizeof(struct nrf_cloud_coap_message));
		if (msg) {
			sys_dnode_init(&msg->node);
			msg->message_id = message_id;
			memcpy(msg->token, &next_token, sizeof(next_token));
			msg->token_len = sizeof(next_token);
			sys_dlist_append(&con_messages, &msg->node);
			num_con_messages++;
			LOG_INF("Added MID:0x%04x, token:0x%04x to list; len:%d",
				message_id, next_token, num_con_messages);
		}
	}
	LOG_INF("CoAP request sent: RESOURCE:%s, MID:0x%04x, token:0x%04x",
		resource, message_id, next_token);

	return 0;
}

/**@brief Send CoAP POST request. */
int client_post_send(const char *resource, uint8_t *buf, size_t buf_len, bool cbor_fmt)
{
	int err;
	struct coap_packet request = {0};
	uint8_t format = cbor_fmt ? COAP_CONTENT_FORMAT_APP_CBOR :
				    COAP_CONTENT_FORMAT_APP_JSON;
	uint16_t message_id = coap_next_id();
	enum coap_msgtype msg_type = COAP_TYPE_CON;
	struct nrf_cloud_coap_message *msg;

	next_token++;

	err = coap_packet_init(&request, coap_buf, sizeof(coap_buf),
			       APP_COAP_VERSION, msg_type,
			       sizeof(next_token), (uint8_t *)&next_token,
			       COAP_METHOD_POST, message_id);
	if (err < 0) {
		LOG_ERR("Failed to create CoAP request, %d", err);
		return err;
	}

	err = coap_packet_append_option(&request, COAP_OPTION_URI_PATH,
					(uint8_t *)resource,
					strlen(resource));
	if (err < 0) {
		LOG_ERR("Failed to encode CoAP URI option, %d", err);
		return err;
	}

	err = coap_packet_append_option(&request, COAP_OPTION_CONTENT_FORMAT,
					&format, sizeof(format));
	if (err < 0) {
		LOG_ERR("Failed to encode CoAP content format option, %d", err);
		return err;
	}

	err = coap_packet_append_payload_marker(&request);
	if (err < 0) {
		LOG_ERR("Failed to add CoAP payload marker, %d", err);
		return err;
	}

	err = coap_packet_append_payload(&request, buf, buf_len);
	if (err < 0) {
		LOG_ERR("Failed to add CoAP payload, %d", err);
		return err;
	}

	err = send(sock, request.data, request.offset, 0);
	if (err < 0) {
		LOG_ERR("Failed to send CoAP request, errno %d, err %d, sock %d, "
			"request.data %p, request.offset %u",
			-errno, err, sock, request.data, request.offset);
		return -errno;
	}

	if (msg_type == COAP_TYPE_CON) {
		/* we expect an ACK back from the server */
		msg = k_malloc(sizeof(struct nrf_cloud_coap_message));
		if (msg) {
			sys_dnode_init(&msg->node);
			msg->message_id = message_id;
			memcpy(msg->token, &next_token, sizeof(next_token));
			msg->token_len = sizeof(next_token);
			sys_dlist_append(&con_messages, &msg->node);
			num_con_messages++;
			LOG_INF("Added MID:0x%04x, token:0x%04x to list; len:%d",
				message_id, next_token, num_con_messages);
		}
	}

	LOG_INF("CoAP request sent: RESOURCE:%s, MID:0x%04x, token:0x%04x",
		resource, message_id, next_token);

	return 0;
}

int client_close(void)
{
	return close(sock);
}

