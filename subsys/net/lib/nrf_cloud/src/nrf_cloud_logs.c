/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <net/nrf_cloud.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/logging/log_output.h>
#include <zephyr/logging/log_backend.h>
#include <date_time.h>
#include "nrf_cloud_fsm.h"
#include "nrf_cloud_mem.h"
#include "nrf_cloud_codec.h"
#include "nrf_cloud_transport.h"
#include <net/nrf_cloud_rest.h>
#include <net/nrf_cloud_logs.h>

LOG_MODULE_REGISTER(nrf_cloud_logs, CONFIG_NRF_CLOUD_LOGGER_LOG_LEVEL);

static void logger_init(const struct log_backend *const backend);
static void logger_process(const struct log_backend *const backend, union log_msg_generic *msg);
static void logger_dropped(const struct log_backend *const backend, uint32_t cnt);
static void logger_panic(const struct log_backend *const backend);
static int logger_is_ready(const struct log_backend *const backend);
static int logger_format_set(const struct log_backend *const backend, uint32_t log_type);
static void logger_notify(const struct log_backend *const backend, enum log_backend_evt event,
		       union log_backend_evt_arg *arg);
static int logger_out(uint8_t *buf, size_t size, void *ctx);

#if defined(CONFIG_NRF_CLOUD_LOGS)

static const struct log_backend_api logger_api = {
	.init		= logger_init,
	.format_set	= logger_format_set,
	.process	= logger_process,
	.panic		= logger_panic,
	.dropped	= logger_dropped,
	.is_ready	= logger_is_ready,
	.notify		= logger_notify
};

static uint8_t log_buf[CONFIG_NRF_CLOUD_LOG_BUF_SIZE];
static uint32_t log_format_current = LOG_OUTPUT_TEXT;
static uint32_t log_output_flags = LOG_OUTPUT_FLAG_CRLF_NONE;
static int nrf_cloud_log_level = CONFIG_NRF_CLOUD_LOGS_LEVEL;
static bool enabled;
static bool initialized;
static atomic_t log_sequence;

struct nrf_cloud_log_context context;

LOG_BACKEND_DEFINE(log_nrf_cloud_backend, logger_api, false);
LOG_OUTPUT_DEFINE(log_nrf_cloud_output, logger_out, log_buf, sizeof(log_buf));

#endif /* CONFIG_NRF_CLOUD_LOGS */

static void logger_init(const struct log_backend *const backend)
{
	ARG_UNUSED(backend);

	log_output_ctx_set(&log_nrf_cloud_output, &context);
	if (IS_ENABLED(CONFIG_NRF_CLOUD_REST)) {
		initialized = true;
		(void)nrf_cloud_codec_init(NULL);
		printk("logger initialized\n");
	}
}

#if defined(CONFIG_NRF_CLOUD_LOGS)
static void logger_process(const struct log_backend *const backend, union log_msg_generic *msg)
{
	int level = log_msg_get_level(&msg->log);
	static atomic_t in_logger_proc = 0;

	if (!initialized) {
		initialized = true;
		(void)nrf_cloud_codec_init(NULL);
		printk("logger initialized\n");
	}

	if (!enabled || (level > nrf_cloud_log_level)) {
		/* Filter logs here, in case runtime filtering is off, and the cloud changed
		 * the log level in the shadow.
		 */
		return;
	}

	if (atomic_test_and_set_bit(&in_logger_proc, 0)) {
		return; /* it was already set */
	}

	void *source = (void *)log_msg_get_source(&msg->log);
	log_format_func_t log_output_func = log_format_func_t_get(log_format_current);
	int16_t source_id;

	context.level = level;
	context.domain = log_msg_get_domain(&msg->log);
	context.ts_ms = log_msg_get_timestamp(&msg->log);
	context.source = (source != NULL) ?
				(IS_ENABLED(CONFIG_LOG_RUNTIME_FILTERING) ?
					log_dynamic_source_id(source) :
					log_const_source_id(source)) :
				0U;
	if (IS_ENABLED(CONFIG_LOG_MULTIDOMAIN) && (context.domain != Z_LOG_LOCAL_DOMAIN_ID)) {
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
	context.src_name = source_id >= 0 ? log_source_name_get(context.domain, source_id) : NULL;
	context.sequence = log_sequence++;

#if 0
	printk("context: dom:%d, lvl:%d, ts_ms::%lu, src_id:%u, src:%s, seq:%u\n",
	       context.domain, context.level, context.ts_ms, context.source,
	       context.src_name, context.sequence);
#endif

	log_output_func(&log_nrf_cloud_output, &msg->log, log_output_flags);
	//printk("Done logger_process()\n");
	atomic_clear_bit(&in_logger_proc, 0);
}

static void logger_dropped(const struct log_backend *const backend, uint32_t cnt)
{
	log_output_dropped_process(&log_nrf_cloud_output, cnt);
}

static void logger_panic(const struct log_backend *const backend)
{
	log_output_flush(&log_nrf_cloud_output);
}

static int logger_is_ready(const struct log_backend *const backend)
{
	if (IS_ENABLED(CONFIG_NRF_CLOUD_MQTT)) {
		if (nfsm_get_current_state() != STATE_DC_CONNECTED) {
			return -EBUSY;
		}
	}
	/* For REST, assume is ready. */
	return 0; /* Logging is ready. */
}

static int logger_format_set(const struct log_backend *const backend, uint32_t log_type)
{
	/* Usually either LOG_OUTPUT_TEXT or LOG_OUTPUT_DICT */
	log_format_current = log_type;
	return 0;
}

static void logger_notify(const struct log_backend *const backend, enum log_backend_evt event,
		       union log_backend_evt_arg *arg)
{
	if (event == LOG_BACKEND_EVT_PROCESS_THREAD_DONE) {
		/* Not sure how helpful this is... */
	}
}

static int logger_out(uint8_t *buf, size_t size, void *ctx)
{
	ARG_UNUSED(ctx);
	int err = 0;
	struct nct_dc_data output = {.message_id = NCT_MSG_ID_USE_NEXT_INCREMENT};
	static atomic_t in_logger_out = 0;

	if (atomic_test_and_set_bit(&in_logger_out, 0)) {
		return 0; /* it was already set */
	}

#if 0
	printk("context: dom:%d, lvl:%d, ts_ms::%u, src_id:%u, src:%s, seq:%u, msg:%*s\n",
	       context.domain, context.level, context.ts_ms, context.source,
	       context.src_name, context.sequence, size, buf);
#endif
	err = nrf_cloud_encode_log(&context, buf, size, &output.data);
	if (err) {
		printk("Error encoding log: %d\n", err);
		goto end;
	}

#if 1
	if (IS_ENABLED(CONFIG_NRF_CLOUD_MQTT)) {
		err = nct_dc_send(&output);
		if (!err) {
			printk("log sent: %s\n", (const char *)output.data.ptr);
		} else {
			printk("Error sending log via MQTT: %d\n", err);
		}
	}
	if (IS_ENABLED(CONFIG_NRF_CLOUD_REST)) {
		/* send to d2c topic, unless bulk is true, in which case d2c/bulk is used */
		err = nrf_cloud_rest_send_device_message(context.rest_ctx, context.device_id,
							 output.data.ptr, false, NULL);
		if (err) {
			printk("Error sending log via REST: %d\n", err);
		}
	}
#endif
end:
	if (output.data.ptr != NULL) {
		cJSON_free((void *)output.data.ptr);
	}
	atomic_clear_bit(&in_logger_out, 0);
	return size;
}
#endif /* CONFIG_NRF_CLOUD_LOGS */

#if defined(NRF_CLOUD_REST)
void nrf_cloud_rest_log_context_set(struct nrf_cloud_rest_context *ctx, const char *dev_id)
{
#if defined(CONFIG_NRF_CLOUD_LOGS)
	context.rest_ctx = ctx;
	strncpy(context.device_id, dev_id, NRF_CLOUD_CLIENT_ID_MAX_LEN);
#else
	ARG_UNUSED(ctx);
	ARG_UNUSED(dev_id);
#endif
}
#endif /* CONFIG_NRF_CLOUD_REST */

void nrf_cloud_log_control_set(int level)
{
#if defined(CONFIG_NRF_CLOUD_LOGS)
	if (nrf_cloud_log_level != level) {
		LOG_DBG("Changing log level from:%d to:%d", nrf_cloud_log_level, level);
		nrf_cloud_log_level = level;
		log_backend_enable(&log_nrf_cloud_backend, NULL, level);
	}
#else
	ARG_UNUSED(level);
#endif /* CONFIG_NRF_CLOUD_LOGS */
}

int nrf_cloud_log_control_get(void)
{
	if (IS_ENABLED(CONFIG_NRF_CLOUD_LOGS)) {
		return nrf_cloud_log_level;
	} else {
		return 0;
	}
}

void nrf_cloud_log_enable(bool enable)
{
	if (enable) {
		printk("ENABLING LOGGER\n");
		log_backend_enable(&log_nrf_cloud_backend, NULL, nrf_cloud_log_level);
	} else {
		log_backend_disable(&log_nrf_cloud_backend);
	}
	enabled = enable;
}

