/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef NRF_CLOUD_PGPS_H_
#define NRF_CLOUD_PGPS_H_

/** @file nrf_cloud_pgps.h
 * @brief Module to provide nRF Cloud P-GPS support to nRF9160 SiP.
 */

#include <zephyr.h>
#include <drivers/gps.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @defgroup nrf_cloud_pgps nRF Cloud P-GPS
 * @{
 */

struct nrf_cloud_pgps_prediction;

/** @brief Cell-based location request type */
struct gps_pgps_request {
	uint16_t prediction_count;
	uint16_t prediction_period_min;
	uint16_t gps_day;
	uint32_t gps_time_of_day;
} __packed;

#define EAPPROXIMATE	8000 /* current time unknown; using first prediction */
#define ELOADING	8001 /* not found but loading in progress */
#define NRF_CLOUD_PGPS_EMPTY_EPHEM_HEALTH (0xff)

enum nrf_cloud_pgps_event {
	PGPS_EVT_INIT,
	PGPS_EVT_UNAVAILABLE,
	PGPS_EVT_LOADING,
	PGPS_EVT_AVAILABLE,
	PGPS_EVT_READY
};

typedef void (*pgps_event_handler_t)(enum nrf_cloud_pgps_event event,
				     struct nrf_cloud_pgps_prediction *p);

/**@brief Update storage of the most recent known location, in modem-specific
 * normalized format (int32_t).
 * Current time is also stored.
 * Normalization:
 *   (latitude / 90.0) * (1 << 23)
 *   (longitude / 360.0) * (1 << 24)
 *
 * @param latitude Current latitude normalized.
 * @param longitude Current longitude in normalized.
 */
void nrf_cloud_set_location_normalized(int32_t latitude, int32_t longitude);

/**@brief Update the storage of the most recent known location in degrees.
 * This will be injected along with the current time and relevant predicted
 * ephemerii to the GPS unit in order to get the fastest possible fix.
 * Current time is also stored.
 *
 * @param latitude Current latitude in degrees.
 * @param longitude Current longitude in degrees.
 */
void nrf_cloud_set_location(double latitude, double longitude);

/**@brief Update the storage of the leap second offset between GPS time
 * and UTC.  Correct value for 2021, and default value, is 18.
 *
 * @param leap_seconds Offset in seconds.
 */
void nrf_cloud_set_leap_seconds(int leap_seconds);

/**@brief Schedule a callback when prediction for current time is
 * available.  Callback could be immediate, if data already stored in
 * Flash, or later, after loading from the cloud.
 *
 * @return 0 if scheduled successfully, or negative error code if
 * could not send request to cloud.
 */
int nrf_cloud_notify_prediction(void);

/**@brief Tries to find an appropriate GPS prediction for the current time.
 *
 * @param prediction Pointer to a pointer to a prediction; the pointer at
 * this pointer will be modified to point to the prediction if the return value
 * is 0 or EAPPROXIMATE.
 *
 * @return 0..NumPredictions-1 if successful; EAPPROXIMATE if current time
 * not known well; -ETIMEDOUT if all predictions stored are expired;
 * -EINVAL if prediction for the current time is invalid.
 */
int nrf_cloud_find_prediction(struct nrf_cloud_pgps_prediction **prediction);

/**@brief Requests specified P-GPS data from nRF Cloud.
 *
 * @param request Pointer to structure containing specified P-GPS data to be
 * requested.
 *
 * @return 0 if successful, otherwise a (negative) error code.
 */
int nrf_cloud_pgps_request(const struct gps_pgps_request *request);

/**@brief Requests all available P-GPS data from nRF Cloud.
 *
 * @return 0 if successful, otherwise a (negative) error code.
 */
int nrf_cloud_pgps_request_all(void);

/**@brief Processes binary P-GPS data received from nRF Cloud.
 *
 * @param buf Pointer to data received from nRF Cloud.
 * @param buf_len Buffer size of data to be processed.
 *
 * @return 0 if successful, otherwise a (negative) error code.
 */
int nrf_cloud_pgps_process(const char *buf, size_t buf_len);

/**@brief Injects binary P-GPS data to the modem.
 *
 * @param p Pointer to a prediction.
 * @param request Which assistance elements the modem needs.
 * @param socket Pointer to GNSS socket to which P-GPS data will be injected.
 * If NULL, the nRF9160 GPS driver is used to inject the data.
 *
 * @return 0 if successful, otherwise a (negative) error code.
 */
int nrf_cloud_pgps_inject(struct nrf_cloud_pgps_prediction *p,
			  struct gps_agps_request *request,
			  const int *socket);

/**@brief Find out of PGPS update is in progress
 *
 * @return true if request sent but loading not yet completed.
 */
bool nrf_cloud_pgps_loading(void);

/**@brief Download more predictions if it is time.
 *
 * @return 0 if successful, otherwise a (negative) error code.
 */
int nrf_cloud_pgps_preemptive_updates(void);

/**@brief Validate the predictions in flash; if invalid or expired,
 * will request a download of the latest. If some are missing, will resume
 * download.
 *
 * @return 0 if valid or request begun; nonzero on error.
 */
int nrf_cloud_pgps_init(pgps_event_handler_t handler);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* NRF_CLOUD_AGPS_H_ */
