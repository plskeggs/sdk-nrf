/*
 * Copyright (c) 2022 Nordic Semiconductor ASA
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
#if defined(CONFIG_LWM2M_CARRIER)
#include <lwm2m_carrier.h>
#endif
#include <nrf_socket.h>
#include <nrf_modem_at.h>
#include <date_time.h>
#include <net/nrf_cloud.h>
#include <dk_buttons_and_leds.h>
#include "nrf_cloud_codec.h"
#include "coap_client.h"
#include "coap_codec.h"
#include "dtls.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(nrf_cloud_coap_client, CONFIG_NRF_CLOUD_COAP_CLIENT_LOG_LEVEL);

#define CREDS_REQ_WAIT_SEC 10

//#define COAP_POC

/* Uncomment to incrementally increase time between coap packets */
/* #define DELAY_INTERPACKET_PERIOD */

/* Open and close socket every cycle */
/* #define OPEN_AND_SHUT */

#define BTN_NUM 1

static char device_id[NRF_CLOUD_CLIENT_ID_MAX_LEN];

/* Semaphore to indicate a button has been pressed */
static K_SEM_DEFINE(button_press_sem, 0, 1);

static void button_handler(uint32_t button_states, uint32_t has_changed)
{
	if (has_changed & button_states & BIT(BTN_NUM - 1)) {
		LOG_DBG("Button %d pressed", BTN_NUM);
		k_sem_give(&button_press_sem);
	}
}

static bool offer_update_creds(void)
{
	int ret;
	bool update_creds = false;

	k_sem_reset(&button_press_sem);

	LOG_INF("---> Press button %d to update credentials in modem", BTN_NUM);
	LOG_INF("     Waiting %d seconds...", CREDS_REQ_WAIT_SEC);

	ret = k_sem_take(&button_press_sem, K_SECONDS(CREDS_REQ_WAIT_SEC));
	if (ret == 0) {
		update_creds = true;
		LOG_INF("Credentials will be updated");
	} else {
		if (ret != -EAGAIN) {
			LOG_ERR("k_sem_take error: %d", ret);
		}

		LOG_INF("Credentials will not be updated");
	}

	return update_creds;
}

#if defined(CONFIG_NRF_MODEM_LIB)
/**@brief Recoverable modem library error. */
void nrf_modem_recoverable_error_handler(uint32_t err)
{
	LOG_ERR("Modem library recoverable error: %u", (unsigned int)err);
}
#endif /* defined(CONFIG_NRF_MODEM_LIB) */

#if defined(CONFIG_LWM2M_CARRIER)
K_SEM_DEFINE(carrier_registered, 0, 1);

void lwm2m_carrier_event_handler(const lwm2m_carrier_event_t *event)
{
	switch (event->type) {
	case LWM2M_CARRIER_EVENT_BSDLIB_INIT:
		LOG_INF("LWM2M_CARRIER_EVENT_BSDLIB_INIT");
		break;
	case LWM2M_CARRIER_EVENT_CONNECT:
		LOG_INF("LWM2M_CARRIER_EVENT_CONNECT");
		break;
	case LWM2M_CARRIER_EVENT_DISCONNECT:
		LOG_INF("LWM2M_CARRIER_EVENT_DISCONNECT");
		break;
	case LWM2M_CARRIER_EVENT_READY:
		LOG_INF("LWM2M_CARRIER_EVENT_READY");
		k_sem_give(&carrier_registered);
		break;
	case LWM2M_CARRIER_EVENT_FOTA_START:
		LOG_INF("LWM2M_CARRIER_EVENT_FOTA_START");
		break;
	case LWM2M_CARRIER_EVENT_REBOOT:
		LOG_INF("LWM2M_CARRIER_EVENT_REBOOT");
		break;
	}
}
#endif /* defined(CONFIG_LWM2M_CARRIER) */

K_SEM_DEFINE(lte_ready, 0, 1);

static void lte_handler(const struct lte_lc_evt *const evt)
{
	switch (evt->type) {
	case LTE_LC_EVT_NW_REG_STATUS:
		if ((evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME) ||
		    (evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_ROAMING)) {
			LOG_INF("Connected to LTE network");
			k_sem_give(&lte_ready);
		} else {
			LOG_INF("reg status %d", evt->nw_reg_status);
		}
		break;

	default:
		LOG_INF("LTE event %d (0x%x)", evt->type, evt->type);
		break;
	}
}


/**@brief Configures modem to provide LTE link. Blocks until link is
 * successfully established.
 */
static void modem_configure(void)
{
	lte_lc_register_handler(lte_handler);
#if defined(CONFIG_LTE_LINK_CONTROL)
	if (IS_ENABLED(CONFIG_LTE_AUTO_INIT_AND_CONNECT)) {
		/* Do nothing, modem is already turned on
		 * and connected.
		 */
	} else {
#if defined(CONFIG_LWM2M_CARRIER)
		/* Wait for the LWM2M_CARRIER to configure the modem and
		 * start the connection.
		 */
		LOG_INF("Waiting for carrier registration...");
		k_sem_take(&carrier_registered, K_FOREVER);
		LOG_INF("Registered!");
#else /* defined(CONFIG_LWM2M_CARRIER) */
		int err;

		LOG_INF("LTE Link Connecting ...");
		err = lte_lc_init_and_connect();
		__ASSERT(err == 0, "LTE link could not be established.");
		k_sem_take(&lte_ready, K_FOREVER);
		LOG_INF("LTE Link Connected!");
		err = lte_lc_psm_req(true);
		if (err) {
			LOG_ERR("Unable to enter PSM mode: %d", err);
		}

		err = nrf_modem_at_printf("AT+CEREG=5");
		if (err) {
			LOG_ERR("Can't subscribe to +CEREG events.");
		}

#endif /* defined(CONFIG_LWM2M_CARRIER) */
	}
#endif /* defined(CONFIG_LTE_LINK_CONTROL) */
}

static void check_connection(void)
{
	static int64_t next_keep_serial_alive_time;
	char scratch_buf[256];
	int err;

	if (k_uptime_get() >= next_keep_serial_alive_time) {
		next_keep_serial_alive_time = k_uptime_get() + APP_COAP_CONNECTION_CHECK_MS;
		scratch_buf[0] = '\0';
		err = nrf_modem_at_cmd(scratch_buf, sizeof(scratch_buf), "%s", "AT%XMONITOR");
		LOG_INF("%s", scratch_buf);
		if (err) {
			LOG_ERR("Error on at cmd: %d", err);
		}
	}
}

int init(void)
{
	int err;

	/* Init the button */
	err = dk_buttons_init(button_handler);
	if (err) {
		LOG_ERR("Failed to initialize button: error: %d", err);
		return err;
	}

	err = client_provision(offer_update_creds());
	if (err) {
		LOG_ERR("Failed to provision credentials: %d", err);
		return err;
	}

	modem_configure();

	err = get_device_id(device_id, sizeof(device_id));
	if (err) {
		LOG_ERR("Error getting device id: %d", err);
		return err;
	}

	err = client_init();
	if (err) {
		LOG_ERR("Failed to initialize CoAP client: %d", err);
		return err;
	}

	return err;
}

int do_next_test(void)
{
	static double temp = 21.5;
	static int post_count = 1;
	static int get_count = 1;
	static int cur_test = 1;
	int err;
	uint8_t buffer[500];
	char topic[100];
	uint8_t *get_payload;
	size_t len;
	int64_t ts;
	struct lte_lc_cells_info cell_info = {0};
	struct lte_lc_cell *c = &cell_info.current_cell;

	LOG_INF("\n");
	ts = k_uptime_get();
	err = date_time_uptime_to_unix_time_ms(&ts);
	if (err) {
		LOG_ERR("Error converting time: %d", err);
	}

	switch (cur_test) {
	case 1:
		LOG_INF("%d. Sending temperature", post_count++);
		len = sizeof(buffer);
		snprintf(topic, sizeof(topic) - 1, "d/%s/d2c", device_id);
		err = coap_codec_encode_sensor(NRF_CLOUD_JSON_APPID_VAL_TEMP, temp,
					       topic, ts, buffer, &len,
					       COAP_CONTENT_FORMAT_APP_JSON);
		if (err) {
			LOG_ERR("Unable to encode temp sensor data: %d", err);
			break;
		}
		temp += 0.1;
		if (client_post_send("poc/msg", buffer, len,
				     COAP_CONTENT_FORMAT_APP_JSON, false) != 0) {
			LOG_ERR("Failed to send POST request");
		}
		break;
	case 2:
		LOG_INF("%d. Getting cell position", post_count++);
		len = sizeof(buffer);
		c->mcc = 260;
		c->mnc = 310;
		c->id = 21858829;
		c->tac = 333;
		c->timing_advance = 0;
		c->earfcn = 0;
		c->rsrp = -157.0F;
		c->rsrq = -34.5F;
		err = coap_codec_encoder_cell_pos(&cell_info, buffer, &len,
						  COAP_CONTENT_FORMAT_APP_JSON);
		if (err) {
			LOG_ERR("Unable to encode cell pos data: %d", err);
			break;
		}
		if (client_post_send("poc/loc/ground-fix", buffer, len,
				     COAP_CONTENT_FORMAT_APP_JSON, true) != 0) {
			LOG_ERR("Failed to send POST request");
		}
		break;
	case 3:
		LOG_INF("%d. Getting pending FOTA job execution", get_count++);
		get_payload = buffer;
		len = 0;
		if (client_get_send("poc/fota/exec/current", get_payload, len,
				    COAP_CONTENT_FORMAT_APP_JSON) != 0) {
			LOG_ERR("Failed to send GET request");
		}
		break;
	}

	if (++cur_test > 3) {
		cur_test = 1;
	}
	return err;
}


void main(void)
{
	int64_t next_msg_time;
	int delta_ms = APP_COAP_SEND_INTERVAL_MS;
	int err;
	int i = 1;
	bool reconnect = false;
	bool authorized = false;
	enum nrf_cloud_coap_response expected_response = NRF_CLOUD_COAP_LOCATION;

	LOG_INF("\n");
	LOG_INF("The nRF Cloud CoAP client sample started\n");

	err = init();
	if (err) {
		LOG_ERR("Halting.");
		for (;;) {
		}
	}
	next_msg_time = k_uptime_get() + delta_ms;

	while (1) {
		if (authorized && (k_uptime_get() >= next_msg_time)) {
			if (reconnect) {
				reconnect = false;
				authorized = false;
				LOG_INF("Going online");
				err = lte_lc_normal();
				if (err) {
					LOG_ERR("Error going online: %d", err);
				} else {
					k_sem_take(&lte_ready, K_FOREVER);
					if (client_init() != 0) {
						LOG_ERR("Failed to initialize CoAP client");
						return;
					}
				}
			}

			err = do_next_test();

			delta_ms = APP_COAP_SEND_INTERVAL_MS * i;
			LOG_INF("Next transfer in %d minutes, %d seconds",
				delta_ms / 60000, (delta_ms / 1000) % 60);
			next_msg_time += delta_ms;
#if defined(DELAY_INTERPACKET_PERIOD)
			if (++i > APP_COAP_INTERVAL_LIMIT) {
				i = APP_COAP_INTERVAL_LIMIT;
			}
#endif
#if defined(CONFIG_COAP_DTLS)
			dtls_print_connection_id(client_get_sock(), false);
#endif
		}

		if (reconnect) {
			k_sleep(K_MSEC(APP_COAP_RECEIVE_INTERVAL_MS));
			check_connection();
			continue;
		}
		err = client_wait(APP_COAP_RECEIVE_INTERVAL_MS);
		if (err < 0) {
			if (err == -EAGAIN) {
				check_connection();
				continue;
			}

			LOG_ERR("Poll error, exit...");
			break;
		}

		err = client_receive(expected_response);

		if (!err && !authorized) {
			err = client_check_ack();
			if (!err) {
				LOG_INF("Authorization received");
				authorized = true;
			}
		}
#if defined(OPEN_AND_SHUT)
		if (delta_ms > APP_COAP_CLOSE_THRESHOLD_MS) {
			reconnect = true;
			err = client_close();
			if (err) {
				LOG_ERR("Error closing socket: %d", err);
			} else {
				LOG_INF("Socket closed.");
			}
			LOG_INF("Going offline");
			err = lte_lc_offline();
			if (err) {
				LOG_ERR("Error going offline: %d", err);
			} else {
			}
		}
#endif
	}
	client_close();
}
