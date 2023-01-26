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

#if defined(CONFIG_NRF_CLOUD_LOGGING)

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
static int nrf_cloud_log_level = CONFIG_NRF_CLOUD_LOGGING_LEVEL;
static int logs_enabled = IS_ENABLED(CONFIG_NRF_CLOUD_LOGGING);
static atomic_t log_sequence;

#if defined(NRF_CLOUD_REST)
static struct nrf_cloud_rest_context *rest_ctx;
static const char device_id[NRF_CLOUD_CLIENT_ID_MAX_LEN + 1];
#endif /* CONFIG_NRF_CLOUD_REST */

LOG_BACKEND_DEFINE(log_nrf_cloud_backend, log_api, false);
LOG_OUTPUT_DEFINE(log_nrf_cloud_output, log_out, log_buf, sizeof(log_buf));

#endif /* CONFIG_NRF_CLOUD_LOGGING */

static void log_init(const struct log_backend *const backend)
{
	ARG_UNUSED(backend);
	if (IS_ENABLED(CONFIG_NRF_CLOUD_REST)) {
		(void)nrf_cloud_codec_init(NULL);
	}
}

#if defined(CONFIG_NRF_CLOUD_LOGGING)
static void log_process(const struct log_backend *const backend, union log_msg_generic *msg)
{
	int level = log_msg_get_level(&msg->log);
	void *source = (void *)log_msg_get_source(&msg->log);
	log_format_func_t log_output_func = log_format_func_t_get(log_format_current);
	struct nrf_cloud_log_context *context = k_malloc(sizeof(struct_log_context));
	struct log_source_const_data *fixed = source;
	int16_t source_id;

	context->level = level;
	context->domain = log_msg_get_domain(&msg->log);
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
	context->src_name = source_id >= 0 ? log_source_name_get(domain_id, source_id) : NULL;
	context->sequence = log_sequence++;
	if (IS_ENABLED(CONFIG_NRF_CLOUD_REST)) {
		context->rest_ctx = rest_ctx;
		strncpy(context->device_id, device_id, NRF_CLOUD_CLIENT_ID_MAX_LEN);
	}

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
	if (IS_ENABLED(CONFIG_NRF_CLOUD_MQTT)) {
		if (nfsm_get_current_state() != STATE_DC_CONNECTED) {
			return -EBUSY;
		}
	}
	/* For REST, assume is ready. */
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
	int err;
	struct nrf_cloud_log_context *context = ctx;
	struct nct_dc_data output = {.message_id = NCT_MSG_ID_USE_NEXT_INCREMENT};

	err = nrf_cloud_encode_log(context, buf, size, &output.data);
	if (!err) {
		LOG_DBG("Encoded log buf: %s", (const char *)output.data.ptr);
	} else {
		LOG_ERR("Error encoding log line: %d", err);
		goto end;
	}

	if (IS_ENABLED(CONFIG_NRF_CLOUD_MQTT)) {
		err = nct_dc_send(&output);
		if (!err) {
			LOG_DBG("Sent log via MQTT");
		} else {
			LOG_ERR("Error sending log via MQTT: %d", err);
		}
	}
	if (IS_ENABLED(CONFIG_NRF_CLOUD_REST)) {
		/* send to d2c topic, unless bulk is true, in which case d2c/bulk is used */
		err = nrf_cloud_rest_send_device_message(context->rest_ctx, context->device_id,
							 output.data.ptr, FALSE, NULL);
		if (!err) {
			LOG_DBG("Send alert via REST");
		} else {
			LOG_ERR("Error sending alert via REST: %d", err);
		}
	}
end:
	if (output.data.ptr != NULL) {
		cJSON_free((void *)output.data.ptr);
	}
	if (ctx != NULL) {
		k_free(ctx);
	}
	return err;
}
#endif /* CONFIG_NRF_CLOUD_LOGGING */

#if defined(NRF_CLOUD_REST)
void nrf_cloud_rest_log_context_set(struct nrf_cloud_rest_context *ctx, const char *dev_id)
{
#if defined(CONFIG_NRF_CLOUD_LOGGING)
	rest_ctx = ctx;
	strncpy(device_id, dev_id, NRF_CLOUD_CLIENT_ID_MAX_LEN);
#else
	ARG_UNUSED(ctx);
	ARG_UNUSED(dev_id);
#endif
}
#endif /* CONFIG_NRF_CLOUD_REST */

void nrf_cloud_log_control(int level)
{
#if defined(CONFIG_NRF_CLOUD_LOGGING)
	if (nrf_cloud_log_level != level) {
		LOG_DBG("Changing log level from:%d to:%d", nrf_cloud_log_level, level);
		nrf_cloud_log_level = level;
		log_backend_enable(&log_nrf_cloud_backend, ctx, level);
	}
#else
	ARG_UNUSED(level);
#endif /* CONFIG_NRF_CLOUD_LOGGING */
}
