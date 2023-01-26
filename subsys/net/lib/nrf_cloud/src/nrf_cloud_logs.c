/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <net/nrf_cloud.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <date_time.h>
#include "nrf_cloud_fsm.h"
#include "nrf_cloud_mem.h"
#include "nrf_cloud_codec.h"
#include "nrf_cloud_transport.h"
#include <net/nrf_cloud_rest.h>
#include <net/nrf_cloud_logs.h>

#if defined(CONFIG_NRF_CLOUD_LOGGING)

LOG_MODULE_REGISTER(nrf_cloud_logs, CONFIG_NRF_CLOUD_LOG_LEVEL);

static void log_init(const struct log_backend *const backend);
static void log_process(const struct log_backend *const backend, union log_msg_generic *msg);
static void log_dropped(const struct log_backend *const backend, uint32_t cnt);
static void log_panic(const struct log_backend *const backend);
static int log_is_ready(const struct log_backend *const backend);
static int log_format_set(const struct log_backend *const backend, uint32_t log_type);
static void log_notify(const struct log_backend *const backend, enum log_backend_evt event,
		       union log_backend_evt_arg *arg);
static int log_out(uint8_t *buf, size_t size, void *ctx);

static const struct log_backend_api log_api = {
	.init		= log_init,
	.format_set	= log_format,
	.process	= log_process,
	.panic		= log_panic,
	.dropped	= log_dropped,
	.is_ready	= log_is_ready,
	.notify		= log_notify
};

static uint8_t log_buf[CONFIG_NRF_CLOUD_LOG_BUF_SIZE];
static uint32_t log_format_current = CONFIG_NRF_CLOUD_LOG_BACKEND_OUTPUT_DEFAULT;
static uint32_t log_output_flags = LOG_OUTPUT_FLAG_CRLF_NONE;
static int logs_enabled = IS_ENABLED(CONFIG_NRF_CLOUD_LOGGING);
static atomic_t log_sequence;

LOG_BACKEND_DEFINE(log_nrf_cloud_backend, log_api, false);
LOG_OUTPUT_DEFINE(log_nrf_cloud_output, log_out, log_buf, sizeof(log_buf));

static void log_init(const struct log_backend *const backend)
{
#if defined(CONFIG_NRF_CLOUD_REST)
	(void)nrf_cloud_codec_init(NULL);
#endif
}

static void log_process(const struct log_backend *const backend, union log_msg_generic *msg)
{
	log_format_func_t log_output_func = log_format_func_t_get(log_format_current);
	struct nrf_cloud_log_context *context = k_malloc(sizeof(struct_log_context));
	void *source = (void *)log_msg_get_source(&msg->log);
	struct log_source_const_data *fixed = source;
	int16_t source_id;

	context->domain = log_msg_get_domain(&msg->log);
	context->level = log_msg_get_level(&msg->log);
	context->ts_ms = log_msg_get_timestamp(&msg->log);
	context->source = (source != NULL) ?
				(IS_ENABLED(CONFIG_LOG_RUNTIME_FILTERING) ?
					log_dynamic_source_id(source) :
					log_const_source_id(source)) :
				0U;
	if (IS_ENABLED(CONFIG_LOG_MULTIDOMAIN) && context->domain != Z_LOG_LOCAL_DOMAIN_ID) {
		/* Remote domain is converting source pointer to ID */
		source_id = (int16_t)(uintptr_t)log_msg_get_source(&msg->log);
	} else {
		if (source != NULL) {
			source_id = IS_ENABLED(CONFIG_LOG_RUNTIME_FILTERING) ?
					log_dynamic_source_id(source) :
					log_const_source_id(source);
		} else {
			source_id = -1;
		}
	}
	context->src_name = source_id >= 0 ? log_source_name_get(domain_id, source_id) : "n/a";
	context->sequence = log_sequence++;

	log_output_ctx_set(log_nrf_cloud_output, context);
	log_output_func(backend, &msg->log, log_output_flags);
}

static void log_dropped(const struct log_backend *const backend, uint32_t cnt)
{
	log_output_dropped_process(backend, cnt);
}

static void log_panic(const struct log_backend *const backend)
{
	log_output_flush(backend);
}

static int log_is_ready(const struct log_backend *const backend)
{
#if defined(CONFIG_NRF_CLOUD_MQTT)
	if (nfsm_get_current_state() != STATE_DC_CONNECTED) {
		return -EBUSY;
	}
#endif
#if defined(CONFIG_NRF_CLOUD_REST)
	/* Assume it is ready, for now. */
#endif
	return 0; /* Logging is ready. */
}

static int log_format_set(const struct log_backend *const backend, uint32_t log_type)
{
	/* Usually either LOG_OUTPUT_TEXT or LOG_OUTPUT_DICT */
	log_format_current = log_type;
	return 0;
}

static void log_notify(const struct log_backend *const backend, enum log_backend_evt event,
		       union log_backend_evt_arg *arg)
{
	if (event == LOG_BACKEND_EVT_PROCESS_THREAD_DONE) {
		/* Not sure how helpful this is... */
	}
}

static int log_out(uint8_t *buf, size_t size, void *ctx)
{
	struct nrf_cloud_log_context *context = ctx;


}

#endif /* CONFIG_NRF_CLOUD_LOGGING */

#if defined(CONFIG_NRF_CLOUD_MQTT)
int nrf_cloud_alert_send(enum nrf_cloud_alert_type type,
			 float value,
			 const char *description)
{
#if defined(CONFIG_NRF_CLOUD_LOGGING)
	struct nct_dc_data output = {.message_id = NCT_MSG_ID_USE_NEXT_INCREMENT};
	int err;

	if (!alerts_enabled) {
		return 0;
	}

	if (nfsm_get_current_state() != STATE_DC_CONNECTED) {
		LOG_ERR("Cannot send alert; not connected");
		return -EACCES;
	}

	err = alert_prepare(&output.data, type, value, description);
	if (!err) {
		LOG_DBG("Encoded alert: %s", (const char *)output.data.ptr);
	} else {
		LOG_ERR("Error encoding alert: %d", err);
		return err;
	}

	err = nct_dc_send(&output);
	if (!err) {
		LOG_DBG("Sent alert via MQTT");
	} else {
		LOG_ERR("Error sending alert via MQTT: %d", err);
	}

	if (output.data.ptr != NULL) {
		cJSON_free((void *)output.data.ptr);
	}
	return err;
#else
	ARG_UNUSED(type);
	ARG_UNUSED(value);
	ARG_UNUSED(description);

	return 0;
#endif /* CONFIG_NRF_CLOUD_LOGGING */
}
#endif /* CONFIG_NRF_CLOUD_MQTT */

#if defined(CONFIG_NRF_CLOUD_REST)
int nrf_cloud_rest_alert_send(struct nrf_cloud_rest_context *const rest_ctx,
			      const char *const device_id,
			      const bool bulk,
			      enum nrf_cloud_alert_type type,
			      float value,
			      const char *description)
{
#if defined(CONFIG_NRF_CLOUD_LOGGING)
	struct nrf_cloud_data data;
	int err;

	if (!alerts_enabled) {
		return 0;
	}
	(void)nrf_cloud_codec_init(NULL);
	err = alert_prepare(&data, type, value, description);
	if (!err) {
		LOG_DBG("Encoded alert: %s", (const char *)data.ptr);
	} else {
		LOG_ERR("Error encoding alert: %d", err);
		return err;
	}

	/* send to d2c topic, unless bulk is true, in which case d2c/bulk is used */
	err = nrf_cloud_rest_send_device_message(rest_ctx, device_id, data.ptr, bulk, NULL);
	if (!err) {
		LOG_DBG("Send alert via REST");
	} else {
		LOG_ERR("Error sending alert via REST: %d", err);
	}

	if (data.ptr != NULL) {
		cJSON_free((void *)data.ptr);
	}
	return err;
#else
	ARG_UNUSED(rest_ctx);
	ARG_UNUSED(device_id);
	ARG_UNUSED(bulk);
	ARG_UNUSED(type);
	ARG_UNUSED(value);
	ARG_UNUSED(description);

	return 0;
#endif /* CONFIG_NRF_CLOUD_LOGGING */
}
#endif /* CONFIG_NRF_CLOUD_REST */

void nrf_cloud_log_control(int level)
{
#if defined(CONFIG_NRF_CLOUD_LOGGING)
	if (nrf_cloud_log_level != level) {
		LOG_DBG("Changing log level from:%d to:%d", nrf_cloud_log_level, level);
		nrf_cloud_log_level = level;
	}
#else
	ARG_UNUSED(level);
#endif /* CONFIG_NRF_CLOUD_LOGGING */
}
