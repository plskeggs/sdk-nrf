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
	const void *source;
	/** When not using runtime filtering, this is the name of the source */
	const char *src_name;
	/** Monotonically increasing sequence number */
	unsigned int sequence;
};

/** @brief Set criticality of logs that should be sent to the cloud.
 *         Set log_level to 0 to disable logging.
 *
 * @param log_level The criticality of the logs to go to the cloud.
 */
void nrf_cloud_log_control(int log_level);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* NRF_CLOUD_LOGS_H_ */
