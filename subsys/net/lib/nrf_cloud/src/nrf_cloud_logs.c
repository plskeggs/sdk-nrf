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
#include <zephyr/sys/base64.h>
#include <date_time.h>
#include "nrf_cloud_fsm.h"
#include "nrf_cloud_mem.h"
#include "nrf_cloud_codec.h"
#include "nrf_cloud_transport.h"
#include "nrf_cloud_logger_context.h"
#include <net/nrf_cloud_rest.h>
#include <net/nrf_cloud_logs.h>

#define LOCAL_LOG_NAME nrf_cloud_logs
LOG_MODULE_REGISTER(LOCAL_LOG_NAME, CONFIG_NRF_CLOUD_LOGGER_LOG_LEVEL);

static bool enabled;
static int64_t starting_timestamp;
static atomic_t log_sequence;
static int nrf_cloud_log_level = CONFIG_NRF_CLOUD_LOGS_LEVEL;

#if defined(CONFIG_NRF_CLOUD_LOGS)

#define JSON_FMT1 "{\"b64\":\""
#define JSON_FMT2 "\"}"

#if defined(CONFIG_NRF_CLOUD_LOG_DICT_JSON)
#define RING_BUF_SIZE (((CONFIG_NRF_CLOUD_LOG_RING_BUF_SIZE * 3) / 4) - 10)
#else
#define RING_BUF_SIZE CONFIG_NRF_CLOUD_LOG_RING_BUF_SIZE
#endif

/** Special value indicating the source of this log entry could not be determined */
#define UNKNOWN_LOG_SOURCE UINT32_MAX

static void logger_init(const struct log_backend *const backend);
static void logger_process(const struct log_backend *const backend, union log_msg_generic *msg);
static void logger_dropped(const struct log_backend *const backend, uint32_t cnt);
static void logger_panic(const struct log_backend *const backend);
static int logger_is_ready(const struct log_backend *const backend);
static int logger_format_set(const struct log_backend *const backend, uint32_t log_type);
static void logger_notify(const struct log_backend *const backend, enum log_backend_evt event,
		       union log_backend_evt_arg *arg);
static int logger_out(uint8_t *buf, size_t size, void *ctx);

static const struct log_backend_api logger_api = {
	.init		= logger_init,
	.format_set	= logger_format_set,
	.process	= logger_process,
	.panic		= logger_panic,
	.dropped	= logger_dropped,
	.is_ready	= logger_is_ready,
	.notify		= logger_notify
};

static struct nrf_cloud_log_stats {
	/** Total number of lines logged */
	uint32_t lines_rendered;
	/** Total number of bytes (before TLS) logged */
	uint32_t bytes_rendered;
	/** Total number of lines sent */
	uint32_t lines_sent;
	/** Total number of bytes (before TLS) sent */
	uint32_t bytes_sent;
} stats;

static uint8_t log_buf[CONFIG_NRF_CLOUD_LOG_BUF_SIZE + 1];
static uint32_t log_format_current = CONFIG_LOG_BACKEND_NRF_CLOUD_OUTPUT_DEFAULT;
static uint32_t log_output_flags = LOG_OUTPUT_FLAG_CRLF_NONE;
static uint32_t self_source_id;
static int num_msgs;

/* Information about a log message is stored in the context by the logger_process backend function,
 * then used by the logger_out function when encoding messages for transport.
 */
struct nrf_cloud_log_context context;

BUILD_ASSERT(CONFIG_NRF_CLOUD_LOG_BUF_SIZE < CONFIG_NRF_CLOUD_LOG_RING_BUF_SIZE,
	     "Ring buffer size must be larger than log buffer size");
LOG_BACKEND_DEFINE(log_nrf_cloud_backend, logger_api, false);
/* Reduce reported log_buf size by 1 so we can null terminate */
LOG_OUTPUT_DEFINE(log_nrf_cloud_output, logger_out, log_buf, (sizeof(log_buf) - 1));
RING_BUF_DECLARE(log_nrf_cloud_rb, RING_BUF_SIZE);

static int send_ring_buffer(void);

static void logger_init(const struct log_backend *const backend)
{
	static bool initialized;
	uint32_t actual_level;

	if ((backend != &log_nrf_cloud_backend) || initialized) {
		return;
	}
	initialized = true;
	(void)nrf_cloud_codec_init(NULL);

	self_source_id = log_source_id_get(STRINGIFY(LOCAL_LOG_NAME));
	actual_level = log_filter_set(&log_nrf_cloud_backend,
				      Z_LOG_LOCAL_DOMAIN_ID, self_source_id, 0);
	if (actual_level != 0) {
		LOG_WRN("Unable to filter self-generated logs");
	}

	if (IS_ENABLED(CONFIG_DATE_TIME) && !starting_timestamp) {
		date_time_now(&starting_timestamp);
	}

	LOG_DBG("self_source_id:%u, actual_level:%u", self_source_id, actual_level);
	LOG_DBG("domain name:%s, num domains:%u, num sources:%u",
		log_domain_name_get(Z_LOG_LOCAL_DOMAIN_ID),
		log_domains_count(),
		log_src_cnt_get(Z_LOG_LOCAL_DOMAIN_ID));
}

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
	int level;
	log_format_func_t log_output_func;

	if ((backend != &log_nrf_cloud_backend) || !enabled) {
		return;
	}

	/* Filter logs here, in case runtime filtering is off, and the cloud changed
	 * the log level in the shadow.
	 */
	level = log_msg_get_level(&msg->log);
	if (level > nrf_cloud_log_level) {
		return;
	}

	/* Prevent locally-generated logs from being self-logged, which runs away. */
	context.src_id = get_source_id((void *)log_msg_get_source(&msg->log));
	if (context.src_id == self_source_id) {
		return;
	}

	context.src_name = context.src_id != UNKNOWN_LOG_SOURCE ?
			   log_source_name_get(context.dom_id, context.src_id) : NULL;
	context.level = level;
	context.dom_id = log_msg_get_domain(&msg->log);
	context.ts = log_output_timestamp_to_us(log_msg_get_timestamp(&msg->log)) / 1000U +
		     starting_timestamp;
	context.sequence = log_sequence;

	log_output_func = log_format_func_t_get(log_format_current);
	log_output_func(&log_nrf_cloud_output, &msg->log, log_output_flags);
}

static void logger_dropped(const struct log_backend *const backend, uint32_t cnt)
{
	if (backend == &log_nrf_cloud_backend) {
		log_output_dropped_process(&log_nrf_cloud_output, cnt);
	}
}

static void logger_panic(const struct log_backend *const backend)
{
	if (backend == &log_nrf_cloud_backend) {
		log_output_flush(&log_nrf_cloud_output);
	}
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
		/* Flush our transmission buffer */
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
	uint8_t *base64_buf = NULL;

	/* The bulk topic requires the multiple JSON messages to be placed in
	 * a JSON array. Close the array then send it.
	 */
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
		stored = output.data.len;
	}

	if ((log_format_current == LOG_OUTPUT_DICT) &&
	    IS_ENABLED(CONFIG_NRF_CLOUD_LOG_DICT_JSON)) {
		/* Convert to base64 and wrap in JSON */
		size_t olen;
		size_t buflen;

		/* Determine buffer size needed -- olen will include 1 byte for null terminator */
		(void)base64_encode(NULL, 0, &olen, output.data.ptr, output.data.len);
		buflen = strlen(JSON_FMT1) + olen + strlen(JSON_FMT2);

		base64_buf = k_calloc(buflen, 1);
		if (base64_buf == NULL) {
			err = -ENOMEM;
			goto cleanup;
		}

		memcpy(base64_buf, JSON_FMT1, strlen(JSON_FMT1));
		err = base64_encode(&base64_buf[strlen(JSON_FMT1)], olen, &olen,
				    output.data.ptr, output.data.len);
		if (err) {
			goto cleanup;
		}
		memcpy(&base64_buf[strlen(JSON_FMT1) + olen], JSON_FMT2, strlen(JSON_FMT2));
		output.data.ptr = base64_buf;
		output.data.len = buflen - 1;
	} /* Else send ring buffer contents as is -- JSON or raw binary. */

	if (IS_ENABLED(CONFIG_NRF_CLOUD_MQTT)) {
		err = nrf_cloud_send(&output);
	} else if (IS_ENABLED(CONFIG_NRF_CLOUD_REST)) {
		err = nrf_cloud_rest_send_device_message(context.rest_ctx,
							 context.device_id,
							 output.data.ptr, true, NULL);
	} else {
		err = -ENODEV;
	}
	if (!err) {
		stats.lines_sent += num_msgs;
		stats.bytes_sent += output.data.len;
	}

cleanup:
	if (base64_buf) {
		k_free(base64_buf);
	}
	ret = ring_buf_get_finish(&log_nrf_cloud_rb, stored);
	ring_buf_reset(&log_nrf_cloud_rb);
	num_msgs = 0;

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
	int extra;

	if (!size) {
		return 0;
	}

	if (log_format_current == LOG_OUTPUT_TEXT) {
		if ((buf >= log_buf) && (&buf[size] <= &log_buf[CONFIG_NRF_CLOUD_LOG_BUF_SIZE])) {
		} else {
			printk("buf %p..%p is not inside our log_buf %p..%p\n",
				buf, &buf[size], log_buf,
				&log_buf[CONFIG_NRF_CLOUD_LOG_BUF_SIZE]);
			return orig_size;
		}
		buf[size] = '\0'; /* Our log_buf has 1 extra byte so this can be done safely */

		extra = 2;
		if (context.src_name) {
			int len = strlen(context.src_name);

			/* We want to pass the log source as a separate field for cloud filtering.
			 * Because the Zephyr logging subsystem prefixes the log text with
			 * 'src_name: ', remove it before encoding the log.
			 */
			if ((strncmp(context.src_name, (const char *)buf, MIN(size, len)) == 0) &&
			    ((len + 2) < size) && (buf[len] == ':') && (buf[len + 1] == ' ')) {
				buf += len + 2;
				size -= len + 2;
			}
		}
		err = nrf_cloud_encode_log(&context, buf, size, &data);
		if (err) {
			LOG_ERR("Error encoding log: %d", err);
			goto end;
		}
		log_sequence++;
	} else {
		extra = sizeof(struct nrf_cloud_bin_hdr);
		data.ptr = buf;
		data.len = size;
	}

	stats.lines_rendered++;
	stats.bytes_rendered += data.len;

	do {
		/* If there is enough room for this rendering, store it and leave. */
		if (ring_buf_space_get(&log_nrf_cloud_rb) > (data.len + extra)) {

			if (num_msgs == 0) {
				/* Insert start of buffer marker */
				if (log_format_current == LOG_OUTPUT_TEXT) {
					/* Open JSON array */
					ring_buf_put(&log_nrf_cloud_rb, "[", 1);
				} else {
					struct nrf_cloud_bin_hdr hdr;

					hdr.magic = NRF_CLOUD_BINARY_MAGIC;
					hdr.format = NRF_CLOUD_DICT_LOG_FMT;
					hdr.ts = context.ts;
					hdr.sequence = context.sequence;
					log_sequence++;
					ring_buf_put(&log_nrf_cloud_rb,
						     (const uint8_t *)&hdr, sizeof(hdr));
				}
			}
			stored = ring_buf_put(&log_nrf_cloud_rb, data.ptr, data.len);
			if (stored != data.len) {
				LOG_WRN("Stored:%u, put:%u", stored, data.len);
			}
			num_msgs++;
			if (log_format_current == LOG_OUTPUT_TEXT) {
				cJSON_free((void *)data.ptr);
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

/**
 * @brief These functions provide a simple logging interface as an alternative
 * to using the full logger backend above.
 */
#if defined(CONFIG_NRF_CLOUD_MQTT)
int nrf_cloud_logs_send(int log_level, const char *fmt, ...)
#elif defined(CONFIG_NRF_CLOUD_REST)
int nrf_cloud_rest_logs_send(struct nrf_cloud_rest_context *ctx, const char *dev_id,
			     int log_level, const char *fmt, ...)
#endif
{
	va_list ap;
	int err = 0;

#if defined(CONFIG_NRF_CLOUD_LOGS)

	/* Cloud logging is enabled, so send it through the main logging system. */
	va_start(ap, fmt);
	z_log_msg_runtime_vcreate(Z_LOG_LOCAL_DOMAIN_ID, NULL, log_level,
				  NULL, 0, Z_LOG_MSG2_CBPRINTF_FLAGS(0), fmt, ap);
	va_end(ap);
	return 0;

#else

	/* Cloud logging is disabled, so construct a temporary cloud
	 * logging context needed by nrf_cloud_encode_log().
	 */
	char buf[CONFIG_NRF_CLOUD_LOG_BUF_SIZE];
	struct nrf_cloud_log_context context = {0};
	struct nrf_cloud_tx_data output = {
		.qos = MQTT_QOS_0_AT_MOST_ONCE,
		.topic_type = NRF_CLOUD_TOPIC_MESSAGE
	};

	/* Do we want any level filtering in this simple logging configuration? */
	if (log_level > nrf_cloud_log_level) {
		return 0;
	}

	va_start(ap, fmt);
	vsnprintk(buf, CONFIG_NRF_CLOUD_LOG_BUF_SIZE - 1, fmt, ap);
	va_end(ap);

#if defined(CONFIG_NRF_CLOUD_REST)
	strncpy(context.device_id, dev_id, NRF_CLOUD_CLIENT_ID_MAX_LEN);
	context.rest_ctx = ctx;
#endif
	context.level = log_level;
	context.sequence = log_sequence++;
	context.src_name = "nrf_cloud_logs";

	if (IS_ENABLED(CONFIG_DATE_TIME)) {
		if (!starting_timestamp) {
			date_time_now(&starting_timestamp);
		}
		context.ts = k_uptime_get() + starting_timestamp;
	} else {
		context.ts = 0;
	}

	err = nrf_cloud_encode_log(&context, buf, strlen(buf), &output.data);
	if (err) {
		return err;
	}

	if (IS_ENABLED(CONFIG_NRF_CLOUD_MQTT)) {
		err = nrf_cloud_send(&output);
	} else if (IS_ENABLED(CONFIG_NRF_CLOUD_REST)) {
		err = nrf_cloud_rest_send_device_message(context.rest_ctx,
							 context.device_id,
							 output.data.ptr, true, NULL);
	}

#endif /* CONFIG_NRF_CLOUD_LOGS */

	return err;
}

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

void nrf_cloud_log_control_set(int level)
{
	if (nrf_cloud_log_level != level) {
		LOG_DBG("Changing log level from:%d to:%d", nrf_cloud_log_level, level);
		nrf_cloud_log_level = level;
		nrf_cloud_log_enable(level != 0);
	}
}

int nrf_cloud_log_control_get(void)
{
	return nrf_cloud_log_level;
}

void nrf_cloud_log_enable(bool enable)
{
	if (enable != enabled) {
#if defined(CONFIG_NRF_CLOUD_LOGS)
		if (enable) {
			logger_init(&log_nrf_cloud_backend);
			log_backend_enable(&log_nrf_cloud_backend, NULL, nrf_cloud_log_level);
		} else {
			log_backend_disable(&log_nrf_cloud_backend);
		}
#endif
		enabled = enable;
		LOG_DBG("enabled = %d", enabled);
	}
}
