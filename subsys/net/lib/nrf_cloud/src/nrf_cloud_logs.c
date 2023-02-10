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
#include <zephyr/sys/ring_buffer.h>
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

static struct nrf_cloud_log_stats 
{
	/** Total number of lines logged */
	uint32_t lines_rendered;
	/** Total number of bytes (before TLS) logged */
	uint32_t bytes_rendered;
	/** Total number of lines sent */
	uint32_t lines_sent;
	/** Total number of bytes (before TLS) sent */
	uint32_t bytes_sent;
} stats;

static uint8_t log_buf[CONFIG_NRF_CLOUD_LOG_BUF_SIZE];
static uint32_t log_format_current = CONFIG_LOG_BACKEND_NRF_CLOUD_OUTPUT_DEFAULT;
static uint32_t log_output_flags = LOG_OUTPUT_FLAG_CRLF_NONE;
static int nrf_cloud_log_level = CONFIG_NRF_CLOUD_LOGS_LEVEL;
static bool enabled;
static atomic_t log_sequence;
static uint32_t self_source_id;
static int num_msgs;
static int64_t starting_timestamp;

struct nrf_cloud_log_context context;

LOG_BACKEND_DEFINE(log_nrf_cloud_backend, logger_api, false);
LOG_OUTPUT_DEFINE(log_nrf_cloud_output, logger_out, log_buf, sizeof(log_buf));
RING_BUF_DECLARE(log_nrf_cloud_rb, CONFIG_NRF_CLOUD_LOG_RING_BUF_SIZE);

static int send_ring_buffer(void);

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
static uint32_t get_source_id(void *source_data)
{
	if (IS_ENABLED(CONFIG_LOG_MULTIDOMAIN) && (context.dom_id != Z_LOG_LOCAL_DOMAIN_ID)) {
		/* Remote domain is converting source pointer to ID */
		return (uint32_t)(uintptr_t)source_data;
	} else {
		return (source_data != NULL) ?
			(IS_ENABLED(CONFIG_LOG_RUNTIME_FILTERING) ?
				log_dynamic_source_id(source_data) :
				log_const_source_id(source_data)) :
			 UNKNOWN_LOG_SOURCE;
	}
}


static void logger_process(const struct log_backend *const backend, union log_msg_generic *msg)
{
	if (backend != &log_nrf_cloud_backend) {
		return;
	}
	int level = log_msg_get_level(&msg->log);

	prevent_self_logging();

	if (IS_ENABLED(CONFIG_DATE_TIME) && !starting_timestamp) {
		date_time_now(&starting_timestamp);
	}

	if (!enabled || (level > nrf_cloud_log_level)) {
		/* Filter logs here, in case runtime filtering is off, and the cloud changed
		 * the log level in the shadow.
		 */
		return;
	}

	void *source_data = (void *)log_msg_get_source(&msg->log);

	context.src_id = get_source_id(source_data);
	/* Prevent locally-generated logs from being self-logged, which runs away. */
	if (context.src_id == self_source_id) {
		//atomic_clear_bit(&in_logger_proc, 0);
		return;
	}
	context.src_name = context.src_id != UNKNOWN_LOG_SOURCE ?
			   log_source_name_get(context.dom_id, context.src_id) : NULL;
	context.level = level;
	context.dom_id = log_msg_get_domain(&msg->log);
	context.dom_name = log_domain_name_get(context.dom_id);
	context.ts = log_msg_get_timestamp(&msg->log) + starting_timestamp;
	context.sequence = log_sequence++;

	log_format_func_t log_output_func = log_format_func_t_get(log_format_current);

	log_output_func(&log_nrf_cloud_output, &msg->log, log_output_flags);
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
	if ((log_type == LOG_OUTPUT_TEXT) || (log_type == LOG_OUTPUT_DICT)) {
		log_format_current = log_type;
		return 0;
	}
	return -ENOTSUP;
}

static void logger_notify(const struct log_backend *const backend, enum log_backend_evt event,
		       union log_backend_evt_arg *arg)
{
	if (backend != &log_nrf_cloud_backend) {
		return;
	}
	if (event == LOG_BACKEND_EVT_PROCESS_THREAD_DONE) {
		/* Not sure how helpful this is... */
		send_ring_buffer();

		LOG_INF("Logged lines:%u, bytes:%u; buf bytes:%u; sent lines:%u, sent bytes:%u",
			stats.lines_rendered, stats.bytes_rendered,
			ring_buf_size_get(&log_nrf_cloud_rb),
			stats.lines_sent, stats.bytes_sent);
	}
}

static int send_ring_buffer(void)
{
	int err;
	int ret;
	struct nrf_cloud_tx_data output = {
		.qos = MQTT_QOS_0_AT_MOST_ONCE,
		.topic_type = (log_format_current == LOG_OUTPUT_TEXT) ?
			       NRF_CLOUD_TOPIC_BULK : NRF_CLOUD_TOPIC_BIN
	};
	uint32_t stored;

	if ((num_msgs != 0) && (log_format_current == LOG_OUTPUT_TEXT)) {
		ring_buf_put(&log_nrf_cloud_rb, "]", 1);
	}
	stored = ring_buf_size_get(&log_nrf_cloud_rb);
	output.data.len = ring_buf_get_claim(&log_nrf_cloud_rb,
					     (uint8_t **)&output.data.ptr,
					     stored);
	if (output.data.len != stored) {
		LOG_WRN("Capacity:%u, free:%u, stored:%u, claimed:%u",
			ring_buf_capacity_get(&log_nrf_cloud_rb),
			ring_buf_space_get(&log_nrf_cloud_rb),
			stored, output.data.len);
	}

	if (IS_ENABLED(CONFIG_NRF_CLOUD_MQTT)) {
		err = nrf_cloud_send(&output);
	}
	else if (IS_ENABLED(CONFIG_NRF_CLOUD_REST)) {
		/* send to d2c topic, unless bulk is true, in which case d2c/bulk is used */
		err = nrf_cloud_rest_send_device_message(context.rest_ctx,
							 context.device_id,
							 output.data.ptr, false, NULL);
	} else {
		err = -ENODEV;
	}
	if (!err) {
		stats.lines_sent += num_msgs;
		stats.bytes_sent += output.data.len;
	}

	ret = ring_buf_get_finish(&log_nrf_cloud_rb, output.data.len);
	num_msgs = 0;
	ring_buf_reset(&log_nrf_cloud_rb);

	if (ret) {
		LOG_ERR("Error finishing ring buffer: %d", ret);
		err = ret;
	}
	return err;
}

static int logger_out(uint8_t *buf, size_t size, void *ctx)
{
	ARG_UNUSED(ctx);
	int err = 0;
	struct nrf_cloud_data data;
	size_t orig_size = size;
	uint32_t stored;

	if (!size) {
		return 0;
	}

#if defined(EXTRA_DEBUG)
	LOG_DBG("dom_id:%d, dom_name:%s, src_id:%u, src_name:%s, lvl:%d, ts:%u, seq:%u",
		context.dom_id, context.dom_name ? context.dom_name : "?",
		context.src_id, context.src_name ? context.src_name : "?",
		context.level, context.ts, context.sequence);
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

	err = nrf_cloud_encode_log(&context, buf, size, &data, log_format_current);
	if (err) {
		LOG_ERR("Error encoding log: %d", err);
		goto end;
	}

	stats.lines_rendered++;
	stats.bytes_rendered += data.len;
	if (log_format_current == LOG_OUTPUT_TEXT) {
		LOG_DBG("Log sent: %s", (const char *)data.ptr);
	} else {
		LOG_HEXDUMP_DBG(data.ptr, data.len, "DICT log");
	}

	do {
		/* If there is enough room for this rendering, store it and leave. */
		if (ring_buf_space_get(&log_nrf_cloud_rb) > (data.len + 2)) {

			if ((num_msgs == 0) && (log_format_current == LOG_OUTPUT_TEXT)) {
				ring_buf_put(&log_nrf_cloud_rb, "[", 1);
			}
			stored = ring_buf_put(&log_nrf_cloud_rb, data.ptr, data.len);
			if (stored != data.len) {
				LOG_WRN("Stored:%u, put:%u", stored, data.len);
			}
			num_msgs++;
			if (log_format_current == LOG_OUTPUT_TEXT) {
				cJSON_free((void *)data.ptr);
			} else {
				k_free((void *)data.ptr);
			}
			/* Stored rendered output in ring buffer, so we can exit now */
			break;
		}

		/* Low on space, so send everything. */
		err = send_ring_buffer();
	} while (!err);

	if (err) {
		LOG_ERR("Error sending log: %d", err);
	}
end:
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

