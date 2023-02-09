/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef NRF_CLOUD_LOGS_H_
#define NRF_CLOUD_LOGS_H_

/** @file nrf_cloud_logs.h
 * @brief Module to provide nRF Cloud logging support to nRF9160 SiP.
 */

#include <zephyr/logging/log.h>

#ifdef __cplusplus
extern "C" {
#endif

struct nrf_cloud_rest_context;

/** @defgroup nrf_cloud_logs nRF Cloud Logs
 * @{
 */

/** Special value indicating the source of this log entry could not be determined */
#define UNKNOWN_LOG_SOURCE UINT32_MAX

/** @brief Data associated with each log entry */
struct nrf_cloud_log_context
{
	/** In a multi-core system, the source of the log message */
	int dom_id;
	/** Name of the domain that generated the log */
	const char *dom_name;
	/** fixed or dynamic source information */
	uint32_t src_id;
	/** When not using runtime filtering, this is the name of the source */
	const char *src_name;
	/** The criticality of the log entry */
	int level;
	/** The time at which the log entry was generated */
	log_timestamp_t ts_ms;
	/** Monotonically increasing sequence number */
	unsigned int sequence;
	/** When using REST, this points to the context structure */
	void *rest_ctx;
	/** When using REST, this is the device_id making the REST connection */
	const char device_id[NRF_CLOUD_CLIENT_ID_MAX_LEN + 1];
	/** Total number of lines logged */
	uint32_t lines_logged;
	/** Total number of bytes (before TLS) logged */
	uint32_t bytes_logged;
};

/** Special value indicating this is an nRF Cloud binary format */
#define NRF_CLOUD_BINARY_MAGIC 0x4346526e /* 'nRFC' in little-endian order */

/** Format identifier for remainder of this binary blob */
#define NRF_CLOUD_DICT_LOG_FMT 0x0001

/** @brief Header preceeding binary blobs so nRF Cloud can
 *  process them in correct order using ts_ms and sequence fields.
 */
struct nrf_cloud_bin_hdr
{
	/** Special marker value indicating this binary blob is a supported type */
	uint32_t magic;
	/** Value indicating the service format, such as a dictionary-based log */
	uint16_t format;
	/** The time at which the log entry was generated */
	log_timestamp_t ts_ms;
	/** Monotonically increasing sequence number */
	uint32_t sequence;
} __packed;

/** @brief Set criticality of logs that should be sent to the cloud.
 *         Set log_level to 0 to disable logging.
 *
 * @param log_level The criticality of the logs to go to the cloud.
 */
void nrf_cloud_log_control_set(int log_level);

/** @brief Get current criticality of logs to be sent to nRF Cloud.
 * @retval Current log level, if CONFIG_NRF_CLOUD_LOGS is enabled, otherwise
 *         0, indicating logs are disabled.
 */
int nrf_cloud_log_control_get(void);

/** @brief Tell REST-based logger the REST context and device_id for later
 *         use when outputting a log.
 *
 *  @param[in] ctx REST context to use.
 *  @param[in] dev_id Device ID to send message on behalf of.
 */
void nrf_cloud_rest_log_context_set(struct nrf_cloud_rest_context *ctx, const char *dev_id);

/** @brief Enable or disable logging to the cloud.
 *  @param[in] enable Set true to send logs to the cloud, false to disable.
 */
void nrf_cloud_log_enable(bool enable);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* NRF_CLOUD_LOGS_H_ */
