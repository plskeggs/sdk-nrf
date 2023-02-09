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

#define LOCAL_LOG_NAME nrf_cloud_logs
LOG_MODULE_REGISTER(LOCAL_LOG_NAME, CONFIG_NRF_CLOUD_LOGGER_LOG_LEVEL);

/* Uncomment to log even more about this logger */
/* #define EXTRA_DEBUG */

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
static uint32_t log_format_current = CONFIG_LOG_BACKEND_NRF_CLOUD_OUTPUT_DEFAULT;
static uint32_t log_output_flags = LOG_OUTPUT_FLAG_CRLF_NONE;
static int nrf_cloud_log_level = CONFIG_NRF_CLOUD_LOGS_LEVEL;
static bool enabled;
static atomic_t log_sequence;
static uint32_t self_source_id;

struct nrf_cloud_log_context context;

LOG_BACKEND_DEFINE(log_nrf_cloud_backend, logger_api, false);
LOG_OUTPUT_DEFINE(log_nrf_cloud_output, logger_out, log_buf, sizeof(log_buf));

#endif /* CONFIG_NRF_CLOUD_LOGS */

static void logger_init(const struct log_backend *const backend)
{
	if (backend != &log_nrf_cloud_backend) {
		return;
	}
	static bool initialized;

	if (initialized) {
		return;
	}
	initialized = true;

	log_output_ctx_set(&log_nrf_cloud_output, &context);

	(void)nrf_cloud_codec_init(NULL);
}

static void prevent_self_logging(void)
{
	static bool once = false;

	if (once) {
		return;
	}
	once = true;

	uint32_t actual_level;

	self_source_id = log_source_id_get(STRINGIFY(LOCAL_LOG_NAME));
	actual_level = log_filter_set(&log_nrf_cloud_backend,
				      Z_LOG_LOCAL_DOMAIN_ID, self_source_id, 0);

	if (actual_level != 0) {
		LOG_WRN("Unable to filter self-generated logs");
	}
	LOG_DBG("self_source_id:%u, actual_level:%u", self_source_id, actual_level);
	LOG_DBG("domain name:%s, num domains:%u, num sources:%u",
		log_domain_name_get(Z_LOG_LOCAL_DOMAIN_ID),
		log_domains_count(),
		log_src_cnt_get(Z_LOG_LOCAL_DOMAIN_ID));
}

#if defined(CONFIG_NRF_CLOUD_LOGS)
static void logger_process(const struct log_backend *const backend, union log_msg_generic *msg)
{
	if (backend != &log_nrf_cloud_backend) {
		return;
	}
	static atomic_t in_logger_proc = 0;
	int level = log_msg_get_level(&msg->log);

	prevent_self_logging();

	if (!enabled || (level > nrf_cloud_log_level)) {
		/* Filter logs here, in case runtime filtering is off, and the cloud changed
		 * the log level in the shadow.
		 */
		return;
	}

	if (atomic_test_and_set_bit(&in_logger_proc, 0)) {
		return; /* it was already set */
	}

	context.level = level;
	context.dom_id = log_msg_get_domain(&msg->log);
	context.dom_name = log_domain_name_get(context.dom_id);
	context.ts_ms = log_msg_get_timestamp(&msg->log);

	void *source_data = (void *)log_msg_get_source(&msg->log);

	if (IS_ENABLED(CONFIG_LOG_MULTIDOMAIN) && (context.dom_id != Z_LOG_LOCAL_DOMAIN_ID)) {
		/* Remote domain is converting source pointer to ID */
		context.src_id = (uint32_t)(uintptr_t)source_data;
	} else {
		context.src_id = (source_data != NULL) ?
					(IS_ENABLED(CONFIG_LOG_RUNTIME_FILTERING) ?
						log_dynamic_source_id(source_data) :
						log_const_source_id(source_data)) :
					UNKNOWN_LOG_SOURCE;
	}

	/* Prevent locally-generated logs from being self-logged, which runs away. */
	if (context.src_id == self_source_id) {
		atomic_clear_bit(&in_logger_proc, 0);
		return;
	}

	context.src_name = context.src_id != UNKNOWN_LOG_SOURCE ?
			   log_source_name_get(context.dom_id, context.src_id) : NULL;
	context.sequence = log_sequence++;

#if defined(EXTRA_DEBUG)
	LOG_DBG("dom_id:%d, dom_name:%s, src_id:%u, src_name:%s, lvl:%d, ts_ms:%u, seq:%u",
		context.dom_id, context.dom_name ? context.dom_name : "?",
		context.src_id, context.src_name ? context.src_name : "?",
		context.level, context.ts_ms, context.sequence);
#endif

	log_format_func_t log_output_func = log_format_func_t_get(log_format_current);

	log_output_func(&log_nrf_cloud_output, &msg->log, log_output_flags);
	atomic_clear_bit(&in_logger_proc, 0);
}

static void logger_dropped(const struct log_backend *const backend, uint32_t cnt)
{
	if (backend != &log_nrf_cloud_backend) {
		return;
	}
	log_output_dropped_process(&log_nrf_cloud_output, cnt);
}

static void logger_panic(const struct log_backend *const backend)
{
	if (backend != &log_nrf_cloud_backend) {
		return;
	}
	log_output_flush(&log_nrf_cloud_output);
}

static int logger_is_ready(const struct log_backend *const backend)
{
	ARG_UNUSED(backend);
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
	if (backend != &log_nrf_cloud_backend) {
		return 0;
	}
	/* Usually one of:
	 * CONFIG_LOG_BACKEND_NRF_CLOUD_OUTPUT_TEXT
	 * CONFIG_LOG_BACKEND_NRF_CLOUD_OUTPUT_DICT
	 */
	log_format_current = log_type;
	return 0;
}

static void logger_notify(const struct log_backend *const backend, enum log_backend_evt event,
		       union log_backend_evt_arg *arg)
{
	if (backend != &log_nrf_cloud_backend) {
		return;
	}
	if (event == LOG_BACKEND_EVT_PROCESS_THREAD_DONE) {
		/* Not sure how helpful this is... */
		LOG_INF("Total lines:%u, bytes:%u", context.lines_logged, context.bytes_logged);
	}
}

static int logger_out(uint8_t *buf, size_t size, void *ctx)
{
	ARG_UNUSED(ctx);
	int err = 0;
	struct nrf_cloud_tx_data output = {
		.qos = MQTT_QOS_0_AT_MOST_ONCE,
		.topic_type = (log_format_current == LOG_OUTPUT_TEXT) ?
			       NRF_CLOUD_TOPIC_MESSAGE : NRF_CLOUD_TOPIC_BIN
	};
	static atomic_t in_logger_out = 0;
	size_t orig_size = size;

	if (!size) {
		return 0;
	}
	if (atomic_test_and_set_bit(&in_logger_out, 0)) {
		return 0; /* it was already set */
	}

#if defined(EXTRA_DEBUG)
	LOG_DBG("dom_id:%d, dom_name:%s, src_id:%u, src_name:%s, lvl:%d, ts_ms:%u, seq:%u",
		context.dom_id, context.dom_name ? context.dom_name : "?",
		context.src_id, context.src_name ? context.src_name : "?",
		context.level, context.ts_ms, context.sequence);
	if (log_format_current == LOG_OUTPUT_TEXT) {
		LOG_DBG("msg:%*s", size, buf);
	} else {
		LOG_HEXDUMP_DBG(buf, size, "DICT msg:");
	}
#endif

	if (context.src_name && (log_format_current == LOG_OUTPUT_TEXT)) {
		int len = strlen(context.src_name);

		/* We want to pass the log source as a separate field for cloud filtering in the
		 * future. Because the Zephyr logging subsystem always prefixes the log text with
		 * 'src_name: ', remove it before encoding the log.
		 */
		if ((strncmp(context.src_name, (const char*)buf, MIN(size, len)) == 0) &&
		    ((len + 2) < size) &&
		    (buf[len] == ':') &&
		    (buf[len + 1] == ' ')) {
			buf += len + 2;
			size -= len + 2;
		}
	}

	err = nrf_cloud_encode_log(&context, buf, size, &output.data, log_format_current);
	if (err) {
		LOG_ERR("Error encoding log: %d", err);
		goto end;
	}

/*
TODO:
- Create a ring buffer per size defined with a CONFIG.
- Change nrf_cloud_encode_log() to take pointer to ring buffer object; have it call
ring_buf_put_claim() based on estimate of worse case msg size; encode the message there;
then call ring_buf_put_finish() with actual length.  If claimed size smaller than request,
then return an error.  Down below here, detect that error and force a flush, then
return unprocessed size so log core will call us again.
- Somehow decide when ring buffer is either full enough or enough time has passed,
and then send all of it.
*/

	if (IS_ENABLED(CONFIG_NRF_CLOUD_MQTT)) {
		err = nrf_cloud_send(&output);
	}
	else if (IS_ENABLED(CONFIG_NRF_CLOUD_REST)) {
		/* send to d2c topic, unless bulk is true, in which case d2c/bulk is used */
		err = nrf_cloud_rest_send_device_message(context.rest_ctx, context.device_id,
							 output.data.ptr, false, NULL);
	} else {
		err = -ENODEV;
	}

	if (!err) {
		context.lines_logged++;
		context.bytes_logged += output.data.len;
		if (log_format_current == LOG_OUTPUT_TEXT) {
			LOG_DBG("Log sent: %s", (const char *)output.data.ptr);
		} else {
			LOG_HEXDUMP_DBG(output.data.ptr, output.data.len, "DICT log");
		}

	} else {
		LOG_ERR("Error sending log: %d", err);
	}
end:
	if (output.data.ptr != NULL) {
		if (log_format_current == LOG_OUTPUT_TEXT) {
			cJSON_free((void *)output.data.ptr);
		} else {
			k_free((void *)output.data.ptr);
		}
	}
	atomic_clear_bit(&in_logger_out, 0);

	/* Return original size of log buffer. Otherwise, logger_out will be called
	 * again with the remainder until the full size is sent.
	 */
	return orig_size;
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
		nrf_cloud_log_enable(level != 0);
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
	if (enable != enabled) {
		if (enable) {
			LOG_DBG("enabling logger");
			log_backend_enable(&log_nrf_cloud_backend, NULL, nrf_cloud_log_level);
		} else {
			LOG_DBG("disabling logger");
			log_backend_disable(&log_nrf_cloud_backend);
		}
		enabled = enable;
	}
}

