/*
 * Copyright (c) 2022 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/**
 * @brief PHY init config parameters. These are passed to phy at init.
 */

#ifndef _PHY_RF_PARAMS_H_
#define _PHY_RF_PARAMS_H_

#define NRF_WIFI_RF_PARAMS_SIZE 200
#define NRF_WIFI_RF_PARAMS_CONF_SIZE 42
#define NRF_WIFI_DEF_RF_PARAMS "0000000000002C00000000000000004C3C443C3C3C4444440000000050EC000000000000000000000000214365003F032424001000002800323500000CF0080A7D8105010071630300EED501001F6F00003B350100F52E0000E35E0000B7B6000066EFFEFFB5F60000896200007A840200E28FFCFF08080808040A140100000000A1A10178000000080050003B020726181818181A120A140E0600"



#define NRF_WIFI_PHY_CALIB_FLAG_RXDC 1
#define NRF_WIFI_PHY_CALIB_FLAG_TXDC 2
#define NRF_WIFI_PHY_CALIB_FLAG_TXPOW 0
#define NRF_WIFI_PHY_CALIB_FLAG_TXIQ 8
#define NRF_WIFI_PHY_CALIB_FLAG_RXIQ 16
#define NRF_WIFI_PHY_CALIB_FLAG_DPD  32

#define NRF_WIFI_PHY_SCAN_CALIB_FLAG_RXDC (1<<16)
#define NRF_WIFI_PHY_SCAN_CALIB_FLAG_TXDC (2<<16)
#define NRF_WIFI_PHY_SCAN_CALIB_FLAG_TXPOW (0<<16)
#define NRF_WIFI_PHY_SCAN_CALIB_FLAG_TXIQ (0<<16)
#define NRF_WIFI_PHY_SCAN_CALIB_FLAG_RXIQ (0<<16)
#define NRF_WIFI_PHY_SCAN_CALIB_FLAG_DPD (0<<16)

#define NRF_WIFI_DEF_PHY_CALIB (NRF_WIFI_PHY_CALIB_FLAG_RXDC |\
				NRF_WIFI_PHY_CALIB_FLAG_TXDC |\
				NRF_WIFI_PHY_CALIB_FLAG_RXIQ |\
				NRF_WIFI_PHY_CALIB_FLAG_TXIQ |\
				NRF_WIFI_PHY_CALIB_FLAG_TXPOW |\
				NRF_WIFI_PHY_CALIB_FLAG_DPD |\
				NRF_WIFI_PHY_SCAN_CALIB_FLAG_RXDC |\
				NRF_WIFI_PHY_SCAN_CALIB_FLAG_TXDC |\
				NRF_WIFI_PHY_SCAN_CALIB_FLAG_RXIQ |\
				NRF_WIFI_PHY_SCAN_CALIB_FLAG_TXIQ |\
				NRF_WIFI_PHY_SCAN_CALIB_FLAG_TXPOW |\
				NRF_WIFI_PHY_SCAN_CALIB_FLAG_DPD)

/* Temperature based calibration params */
#define NRF_WIFI_DEF_PHY_TEMP_CALIB (NRF_WIFI_PHY_CALIB_FLAG_RXDC |\
				     NRF_WIFI_PHY_CALIB_FLAG_TXDC |\
				     NRF_WIFI_PHY_CALIB_FLAG_RXIQ |\
				     NRF_WIFI_PHY_CALIB_FLAG_TXIQ |\
				     NRF_WIFI_PHY_CALIB_FLAG_TXPOW |\
				     NRF_WIFI_PHY_CALIB_FLAG_DPD)


#define NRF_WIFI_TEMP_CALIB_PERIOD (1024 * 1024) /* micro seconds */
#define NRF_WIFI_TEMP_CALIB_THRESHOLD (40)
#define NRF_WIFI_TEMP_CALIB_ENABLE 0

/* Battery voltage changes base calibrations and voltage thresholds */
#define NRF_WIFI_DEF_PHY_VBAT_CALIB (NRF_WIFI_PHY_CALIB_FLAG_DPD)
#define NRF_WIFI_VBAT_VERYLOW (3) /* Corresponds to (2.5+3*0.07)=2.71V */
#define NRF_WIFI_VBAT_LOW  (6)  /* Correspond to (2.5+6*0.07)=2.92V */
#define NRF_WIFI_VBAT_HIGH (12) /* Correspond to (2.5+12*0.07)=3.34V */


#endif