/*
 * Generated using zcbor version 0.5.1
 * https://github.com/NordicSemiconductor/zcbor
 * Generated with a --default-max-qty of 3
 */

#ifndef MSG_ENCODE_TYPES_H__
#define MSG_ENCODE_TYPES_H__

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
#define DEFAULT_MAX_QTY 10

struct pvt_spd {
	double _pvt_spd;
};

struct pvt_hdg {
	double _pvt_hdg;
};

struct pvt_alt {
	double _pvt_alt;
};

struct pvt {
	double _pvt_lat;
	double _pvt_lng;
	double _pvt_acc;
	struct pvt_spd _pvt_spd;
	uint_fast32_t _pvt_spd_present;
	struct pvt_hdg _pvt_hdg;
	uint_fast32_t _pvt_hdg_present;
	struct pvt_alt _pvt_alt;
	uint_fast32_t _pvt_alt_present;
};

struct message_out_ts {
	uint64_t _message_out_ts;
};

struct message_out {
	struct zcbor_string _message_out_appId;
	union {
		struct zcbor_string _message_out_data_tstr;
		double _message_out_data_float;
		int32_t _message_out_data_int;
		struct pvt _message_out_data__pvt;
	};
	enum {
		_message_out_data_tstr,
		_message_out_data_float,
		_message_out_data_int,
		_message_out_data__pvt,
	} _message_out_data_choice;
	struct message_out_ts _message_out_ts;
	uint_fast32_t _message_out_ts_present;
};


#endif /* MSG_ENCODE_TYPES_H__ */
