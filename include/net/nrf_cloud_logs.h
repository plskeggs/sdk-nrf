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

/** @brief Data associated with each log entry */
struct nrf_cloud_log_context
{
	/** In a multi-core system, the source of the log message */
	int domain;
	/** The criticality of the log entry */
	int level;
	/** The time at which the log entry was generated */
	log_timestamp_t ts_ms;
	/** fixed or dynamic source information */
	uint32_t source;
	/** When not using runtime filtering, this is the name of the source */
	const char *src_name;
	/** Monotonically increasing sequence number */
	unsigned int sequence;
	/** When using REST, this points to the context structure */
	void *rest_ctx;
	/** When using REST, this is the device_id making the REST connection */
	const char device_id[NRF_CLOUD_CLIENT_ID_MAX_LEN + 1];
};

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
