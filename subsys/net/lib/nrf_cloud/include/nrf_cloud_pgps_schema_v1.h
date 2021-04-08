/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr.h>

#ifndef NRF_CLOUD_PGPS_SCHEMA_V1_H_
#define NRF_CLOUD_PGPS_SCHEMA_V1_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "nrf_cloud_agps_schema_v1.h"

#define NRF_CLOUD_PGPS_BIN_SCHEMA_VERSION		(1)
#define NRF_CLOUD_PGPS_PREDICTION_HEADER ((int)NRF_CLOUD_AGPS_LAST + 1)

#define NRF_CLOUD_PGPS_BIN_SCHEMA_VERSION_INDEX		(0)
#define NRF_CLOUD_PGPS_BIN_SCHEMA_VERSION_SIZE		(1)
#define NRF_CLOUD_PGPS_BIN_TYPE_OFFSET			(0)
#define NRF_CLOUD_PGPS_BIN_TYPE_SIZE			(1)
#define NRF_CLOUD_PGPS_BIN_COUNT_OFFSET			(1)
#define NRF_CLOUD_PGPS_BIN_COUNT_SIZE			(2)

#define NRF_CLOUD_PGPS_NUM_SV				(32U)

struct nrf_cloud_pgps_system_time {
	uint16_t date_day;
	uint32_t time_full_s;
	uint16_t time_frac_ms;
	uint32_t sv_mask;
	uint32_t pad; /* placeholder where sv_tow[32] is for AGPS; not here though */
} __packed;

struct nrf_cloud_pgps_prediction {
	uint8_t time_type;
	uint16_t time_count;
	struct nrf_cloud_pgps_system_time time;
	int8_t schema_version;	/* not from cloud */
	uint8_t ephemeris_type;
	uint16_t ephemeris_count;
	struct nrf_cloud_agps_ephemeris ephemerii[NRF_CLOUD_PGPS_NUM_SV];
	uint32_t sentinel;	/* not from cloud */
} __packed;

struct nrf_cloud_pgps_header {
	int8_t schema_version;
	int8_t array_type;
	int16_t num_items;
	int16_t prediction_count;
	int16_t prediction_size;
	int16_t prediction_period_min;
	int16_t gps_day;
	int32_t gps_time_of_day;
	struct nrf_cloud_pgps_prediction predictions[];
} __packed;

struct agps_header {
	uint8_t type;
	uint16_t count;
	uint8_t data[];
} __packed;

struct pgps_sys_time {
	uint8_t schema_version;
	uint8_t type;
	uint16_t count;
	struct nrf_cloud_agps_system_time time;
} __packed;

struct pgps_location {
	uint8_t schema_version;
	uint8_t type;
	uint16_t count;
	struct nrf_cloud_agps_location location;
} __packed;

#ifdef __cplusplus
}
#endif

#endif /* NRF_CLOUD_AGPS_SCHEMA_V1_H_ */
