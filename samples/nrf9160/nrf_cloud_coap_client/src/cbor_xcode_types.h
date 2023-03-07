/*
 * Generated using zcbor version 0.6.0
 * https://github.com/NordicSemiconductor/zcbor
 * Generated with a --default-max-qty of 3
 */

#ifndef CBOR_XCODE_TYPES_H__
#define CBOR_XCODE_TYPES_H__

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "zcbor_encode.h"

/** Which value for --default-max-qty this file was created with.
 *
 *  The define is used in the other generated file to do a build-time
 *  compatibility check.
 *
 *  See `zcbor --help` for more information about --default-max-qty
 */
#define DEFAULT_MAX_QTY 5

struct ncell {

	uint32_t _ncell_earfcn;

	uint32_t _ncell_pci;

	int32_t _ncell_rsrp;

	uint_fast32_t _ncell_rsrp_present;

	double _ncell_rsrq;

	uint_fast32_t _ncell_rsrq_present;

	int32_t _ncell_timeDiff;

	uint_fast32_t _ncell_timeDiff_present;

};



struct cell {

	int32_t _cell_mcc;

	int32_t _cell_mnc;

	uint32_t _cell_eci;

	uint32_t _cell_tac;

	uint32_t _cell_earfcn;

	uint_fast32_t _cell_earfcn_present;

	uint32_t _cell_adv;

	uint_fast32_t _cell_adv_present;

	struct ncell _cell_nmr_ncells[CONFIG_LTE_NEIGHBOR_CELLS_MAX];

	uint_fast32_t _cell_nmr_ncells_count;

	int32_t _cell_rsrp;

	uint_fast32_t _cell_rsrp_present;

	double _cell_rsrq;

	uint_fast32_t _cell_rsrq_present;

};



struct lte {

	struct cell _lte__cell[5];

	uint_fast32_t _lte__cell_count;

};



struct ap {

	struct zcbor_string _ap_mac;

	uint32_t _ap_age;

	uint_fast32_t _ap_age_present;

	uint32_t _ap_freq;

	uint_fast32_t _ap_freq_present;

	int32_t _ap_rssi;

	uint_fast32_t _ap_rssi_present;

	uint32_t _ap_ch;

	uint_fast32_t _ap_ch_present;

	struct zcbor_string _ap_ssid;

	uint_fast32_t _ap_ssid_present;

};



struct wifi {

	struct ap _wifi__ap[60];

	uint_fast32_t _wifi__ap_count;

};



struct groundfix {

	struct lte _groundfix__lte[1];

	uint_fast32_t _groundfix__lte_count;

	struct wifi _groundfix__wifi[1];

	uint_fast32_t _groundfix__wifi_count;

};


#endif /* CBOR_XCODE_TYPES_H__ */
