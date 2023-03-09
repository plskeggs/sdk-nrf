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

/** @brief Set log level at or below that should be sent to the cloud.
 *         Set log_level to 0 to disable logging.
 *         See zephyr/include/zephyr/logging/log_core.h.
 *
 * @param log_level The log level to go to the cloud.
 */
void nrf_cloud_log_control_set(int log_level);

/** @brief Get current log level to be sent to nRF Cloud.
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

#if defined(NRF_CLOUD_MQTT)
int nrf_cloud_logs_send(int log_level, const char *fmt, ...);
#elif defined(NRF_CLOUD_REST)
int nrf_cloud_rest_logs_send(struct nrf_cloud_rest_context *ctx, const char *dev_id,
			     int log_level, const char *fmt, ...);
#endif

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* NRF_CLOUD_LOGS_H_ */
