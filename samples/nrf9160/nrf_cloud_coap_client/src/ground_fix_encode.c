/*
 * Generated using zcbor version 0.6.0
 * https://github.com/NordicSemiconductor/zcbor
 * Generated with a --default-max-qty of 3
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "zcbor_encode.h"
#include "ground_fix_encode.h"

#if DEFAULT_MAX_QTY != 3
#error "The type file was generated with a different default_max_qty than this file"
#endif

static bool encode_repeated_cell_earfcn(zcbor_state_t *state, const struct cell_earfcn *input);
static bool encode_repeated_cell_adv(zcbor_state_t *state, const struct cell_adv *input);
static bool encode_repeated_ncell_rsrp(zcbor_state_t *state, const struct ncell_rsrp *input);
static bool encode_repeated_ncell_rsrq(zcbor_state_t *state, const struct ncell_rsrq *input);
static bool encode_repeated_ncell_timeDiff(zcbor_state_t *state, const struct ncell_timeDiff *input);
static bool encode_ncell(zcbor_state_t *state, const struct ncell *input);
static bool encode_repeated_cell_rsrp(zcbor_state_t *state, const struct cell_rsrp *input);
static bool encode_repeated_cell_rsrq(zcbor_state_t *state, const struct cell_rsrq *input);
static bool encode_cell(zcbor_state_t *state, const struct cell *input);
static bool encode_lte_ar(zcbor_state_t *state, const struct lte_ar *input);
static bool encode_repeated_ground_fix_req_lte(zcbor_state_t *state, const struct ground_fix_req_lte *input);
static bool encode_repeated_ap_age(zcbor_state_t *state, const struct ap_age *input);
static bool encode_repeated_ap_signalStrength(zcbor_state_t *state, const struct ap_signalStrength *input);
static bool encode_repeated_ap_channel(zcbor_state_t *state, const struct ap_channel *input);
static bool encode_repeated_ap_ssid(zcbor_state_t *state, const struct ap_ssid *input);
static bool encode_ap(zcbor_state_t *state, const struct ap *input);
static bool encode_wifi_ob(zcbor_state_t *state, const struct wifi_ob *input);
static bool encode_repeated_ground_fix_req_wifi(zcbor_state_t *state, const struct ground_fix_req_wifi *input);
static bool encode_ground_fix_req(zcbor_state_t *state, const struct ground_fix_req *input);


static bool encode_repeated_cell_earfcn(
		zcbor_state_t *state, const struct cell_earfcn *input)
{
	zcbor_print("%s\r\n", __func__);
	struct zcbor_string tmp_str;

	bool tmp_result = ((((zcbor_tstr_encode(state, ((tmp_str.value = (uint8_t *)"earfcn", tmp_str.len = sizeof("earfcn") - 1, &tmp_str)))))
	&& (zcbor_uint32_encode(state, (&(*input)._cell_earfcn)))));

	if (!tmp_result)
		zcbor_trace();

	return tmp_result;
}

static bool encode_repeated_cell_adv(
		zcbor_state_t *state, const struct cell_adv *input)
{
	zcbor_print("%s\r\n", __func__);
	struct zcbor_string tmp_str;

	bool tmp_result = ((((zcbor_tstr_encode(state, ((tmp_str.value = (uint8_t *)"adv", tmp_str.len = sizeof("adv") - 1, &tmp_str)))))
	&& (zcbor_uint32_encode(state, (&(*input)._cell_adv)))));

	if (!tmp_result)
		zcbor_trace();

	return tmp_result;
}

static bool encode_repeated_ncell_rsrp(
		zcbor_state_t *state, const struct ncell_rsrp *input)
{
	zcbor_print("%s\r\n", __func__);
	struct zcbor_string tmp_str;

	bool tmp_result = ((((zcbor_tstr_encode(state, ((tmp_str.value = (uint8_t *)"rsrp", tmp_str.len = sizeof("rsrp") - 1, &tmp_str)))))
	&& (zcbor_int32_encode(state, (&(*input)._ncell_rsrp)))));

	if (!tmp_result)
		zcbor_trace();

	return tmp_result;
}

static bool encode_repeated_ncell_rsrq(
		zcbor_state_t *state, const struct ncell_rsrq *input)
{
	zcbor_print("%s\r\n", __func__);
	struct zcbor_string tmp_str;

	bool tmp_result = ((((zcbor_tstr_encode(state, ((tmp_str.value = (uint8_t *)"rsrq", tmp_str.len = sizeof("rsrq") - 1, &tmp_str)))))
	&& (zcbor_float64_encode(state, (&(*input)._ncell_rsrq)))));

	if (!tmp_result)
		zcbor_trace();

	return tmp_result;
}

static bool encode_repeated_ncell_timeDiff(
		zcbor_state_t *state, const struct ncell_timeDiff *input)
{
	zcbor_print("%s\r\n", __func__);
	struct zcbor_string tmp_str;

	bool tmp_result = ((((zcbor_tstr_encode(state, ((tmp_str.value = (uint8_t *)"timeDiff", tmp_str.len = sizeof("timeDiff") - 1, &tmp_str)))))
	&& (zcbor_int32_encode(state, (&(*input)._ncell_timeDiff)))));

	if (!tmp_result)
		zcbor_trace();

	return tmp_result;
}

static bool encode_ncell(
		zcbor_state_t *state, const struct ncell *input)
{
	zcbor_print("%s\r\n", __func__);
	struct zcbor_string tmp_str;

	bool tmp_result = (((zcbor_map_start_encode(state, 5) && (((((zcbor_tstr_encode(state, ((tmp_str.value = (uint8_t *)"earfcn", tmp_str.len = sizeof("earfcn") - 1, &tmp_str)))))
	&& (zcbor_uint32_encode(state, (&(*input)._ncell_earfcn))))
	&& (((zcbor_tstr_encode(state, ((tmp_str.value = (uint8_t *)"pci", tmp_str.len = sizeof("pci") - 1, &tmp_str)))))
	&& (zcbor_uint32_encode(state, (&(*input)._ncell_pci))))
	&& zcbor_present_encode(&((*input)._ncell_rsrp_present), (zcbor_encoder_t *)encode_repeated_ncell_rsrp, state, (&(*input)._ncell_rsrp))
	&& zcbor_present_encode(&((*input)._ncell_rsrq_present), (zcbor_encoder_t *)encode_repeated_ncell_rsrq, state, (&(*input)._ncell_rsrq))
	&& zcbor_present_encode(&((*input)._ncell_timeDiff_present), (zcbor_encoder_t *)encode_repeated_ncell_timeDiff, state, (&(*input)._ncell_timeDiff))) || (zcbor_list_map_end_force_encode(state), false)) && zcbor_map_end_encode(state, 5))));

	if (!tmp_result)
		zcbor_trace();

	return tmp_result;
}

static bool encode_repeated_cell_rsrp(
		zcbor_state_t *state, const struct cell_rsrp *input)
{
	zcbor_print("%s\r\n", __func__);
	struct zcbor_string tmp_str;

	bool tmp_result = ((((zcbor_tstr_encode(state, ((tmp_str.value = (uint8_t *)"rsrp", tmp_str.len = sizeof("rsrp") - 1, &tmp_str)))))
	&& (zcbor_int32_encode(state, (&(*input)._cell_rsrp)))));

	if (!tmp_result)
		zcbor_trace();

	return tmp_result;
}

static bool encode_repeated_cell_rsrq(
		zcbor_state_t *state, const struct cell_rsrq *input)
{
	zcbor_print("%s\r\n", __func__);
	struct zcbor_string tmp_str;

	bool tmp_result = ((((zcbor_tstr_encode(state, ((tmp_str.value = (uint8_t *)"rsrq", tmp_str.len = sizeof("rsrq") - 1, &tmp_str)))))
	&& (zcbor_float64_encode(state, (&(*input)._cell_rsrq)))));

	if (!tmp_result)
		zcbor_trace();

	return tmp_result;
}

static bool encode_cell(
		zcbor_state_t *state, const struct cell *input)
{
	zcbor_print("%s\r\n", __func__);
	struct zcbor_string tmp_str;

	bool tmp_result = (((zcbor_map_start_encode(state, 9) && (((((zcbor_tstr_encode(state, ((tmp_str.value = (uint8_t *)"mcc", tmp_str.len = sizeof("mcc") - 1, &tmp_str)))))
	&& (zcbor_uint32_encode(state, (&(*input)._cell_mcc))))
	&& (((zcbor_tstr_encode(state, ((tmp_str.value = (uint8_t *)"mnc", tmp_str.len = sizeof("mnc") - 1, &tmp_str)))))
	&& (zcbor_uint32_encode(state, (&(*input)._cell_mnc))))
	&& (((zcbor_tstr_encode(state, ((tmp_str.value = (uint8_t *)"eci", tmp_str.len = sizeof("eci") - 1, &tmp_str)))))
	&& (zcbor_uint32_encode(state, (&(*input)._cell_eci))))
	&& (((zcbor_tstr_encode(state, ((tmp_str.value = (uint8_t *)"tac", tmp_str.len = sizeof("tac") - 1, &tmp_str)))))
	&& (zcbor_uint32_encode(state, (&(*input)._cell_tac))))
	&& zcbor_present_encode(&((*input)._cell_earfcn_present), (zcbor_encoder_t *)encode_repeated_cell_earfcn, state, (&(*input)._cell_earfcn))
	&& zcbor_present_encode(&((*input)._cell_adv_present), (zcbor_encoder_t *)encode_repeated_cell_adv, state, (&(*input)._cell_adv))
	&& (((zcbor_tstr_encode(state, ((tmp_str.value = (uint8_t *)"nmr", tmp_str.len = sizeof("nmr") - 1, &tmp_str)))))
	&& (zcbor_list_start_encode(state, 5) && ((zcbor_multi_encode_minmax(0, 5, &(*input)._cell_nmr_ncells_count, (zcbor_encoder_t *)encode_ncell, state, (&(*input)._cell_nmr_ncells), sizeof(struct ncell))) || (zcbor_list_map_end_force_encode(state), false)) && zcbor_list_end_encode(state, 5)))
	&& zcbor_present_encode(&((*input)._cell_rsrp_present), (zcbor_encoder_t *)encode_repeated_cell_rsrp, state, (&(*input)._cell_rsrp))
	&& zcbor_present_encode(&((*input)._cell_rsrq_present), (zcbor_encoder_t *)encode_repeated_cell_rsrq, state, (&(*input)._cell_rsrq))) || (zcbor_list_map_end_force_encode(state), false)) && zcbor_map_end_encode(state, 9))));

	if (!tmp_result)
		zcbor_trace();

	return tmp_result;
}

static bool encode_lte_ar(
		zcbor_state_t *state, const struct lte_ar *input)
{
	zcbor_print("%s\r\n", __func__);

	bool tmp_result = (((zcbor_list_start_encode(state, 5) && ((zcbor_multi_encode_minmax(1, 5, &(*input)._lte_ar__cell_count, (zcbor_encoder_t *)encode_cell, state, (&(*input)._lte_ar__cell), sizeof(struct cell))) || (zcbor_list_map_end_force_encode(state), false)) && zcbor_list_end_encode(state, 5))));

	if (!tmp_result)
		zcbor_trace();

	return tmp_result;
}

static bool encode_repeated_ground_fix_req_lte(
		zcbor_state_t *state, const struct ground_fix_req_lte *input)
{
	zcbor_print("%s\r\n", __func__);
	struct zcbor_string tmp_str;

	bool tmp_result = ((((zcbor_tstr_encode(state, ((tmp_str.value = (uint8_t *)"lte", tmp_str.len = sizeof("lte") - 1, &tmp_str)))))
	&& (encode_lte_ar(state, (&(*input)._ground_fix_req_lte)))));

	if (!tmp_result)
		zcbor_trace();

	return tmp_result;
}

static bool encode_repeated_ap_age(
		zcbor_state_t *state, const struct ap_age *input)
{
	zcbor_print("%s\r\n", __func__);
	struct zcbor_string tmp_str;

	bool tmp_result = ((((zcbor_tstr_encode(state, ((tmp_str.value = (uint8_t *)"age", tmp_str.len = sizeof("age") - 1, &tmp_str)))))
	&& (zcbor_uint32_encode(state, (&(*input)._ap_age)))));

	if (!tmp_result)
		zcbor_trace();

	return tmp_result;
}

static bool encode_repeated_ap_signalStrength(
		zcbor_state_t *state, const struct ap_signalStrength *input)
{
	zcbor_print("%s\r\n", __func__);
	struct zcbor_string tmp_str;

	bool tmp_result = ((((zcbor_tstr_encode(state, ((tmp_str.value = (uint8_t *)"signalStrength", tmp_str.len = sizeof("signalStrength") - 1, &tmp_str)))))
	&& (zcbor_int32_encode(state, (&(*input)._ap_signalStrength)))));

	if (!tmp_result)
		zcbor_trace();

	return tmp_result;
}

static bool encode_repeated_ap_channel(
		zcbor_state_t *state, const struct ap_channel *input)
{
	zcbor_print("%s\r\n", __func__);
	struct zcbor_string tmp_str;

	bool tmp_result = ((((zcbor_tstr_encode(state, ((tmp_str.value = (uint8_t *)"channel", tmp_str.len = sizeof("channel") - 1, &tmp_str)))))
	&& (zcbor_uint32_encode(state, (&(*input)._ap_channel)))));

	if (!tmp_result)
		zcbor_trace();

	return tmp_result;
}

static bool encode_repeated_ap_ssid(
		zcbor_state_t *state, const struct ap_ssid *input)
{
	zcbor_print("%s\r\n", __func__);
	struct zcbor_string tmp_str;

	bool tmp_result = ((((zcbor_tstr_encode(state, ((tmp_str.value = (uint8_t *)"ssid", tmp_str.len = sizeof("ssid") - 1, &tmp_str)))))
	&& (zcbor_tstr_encode(state, (&(*input)._ap_ssid)))));

	if (!tmp_result)
		zcbor_trace();

	return tmp_result;
}

static bool encode_ap(
		zcbor_state_t *state, const struct ap *input)
{
	zcbor_print("%s\r\n", __func__);
	struct zcbor_string tmp_str;

	bool tmp_result = (((zcbor_map_start_encode(state, 5) && (((((zcbor_tstr_encode(state, ((tmp_str.value = (uint8_t *)"macAddress", tmp_str.len = sizeof("macAddress") - 1, &tmp_str)))))
	&& (zcbor_tstr_encode(state, (&(*input)._ap_macAddress))))
	&& zcbor_present_encode(&((*input)._ap_age_present), (zcbor_encoder_t *)encode_repeated_ap_age, state, (&(*input)._ap_age))
	&& zcbor_present_encode(&((*input)._ap_signalStrength_present), (zcbor_encoder_t *)encode_repeated_ap_signalStrength, state, (&(*input)._ap_signalStrength))
	&& zcbor_present_encode(&((*input)._ap_channel_present), (zcbor_encoder_t *)encode_repeated_ap_channel, state, (&(*input)._ap_channel))
	&& zcbor_present_encode(&((*input)._ap_ssid_present), (zcbor_encoder_t *)encode_repeated_ap_ssid, state, (&(*input)._ap_ssid))) || (zcbor_list_map_end_force_encode(state), false)) && zcbor_map_end_encode(state, 5))));

	if (!tmp_result)
		zcbor_trace();

	return tmp_result;
}

static bool encode_wifi_ob(
		zcbor_state_t *state, const struct wifi_ob *input)
{
	zcbor_print("%s\r\n", __func__);
	struct zcbor_string tmp_str;

	bool tmp_result = (((zcbor_map_start_encode(state, 1) && (((((zcbor_tstr_encode(state, ((tmp_str.value = (uint8_t *)"accessPoints", tmp_str.len = sizeof("accessPoints") - 1, &tmp_str)))))
	&& (zcbor_list_start_encode(state, 20) && ((zcbor_multi_encode_minmax(2, 20, &(*input)._wifi_ob_accessPoints__ap_count, (zcbor_encoder_t *)encode_ap, state, (&(*input)._wifi_ob_accessPoints__ap), sizeof(struct ap))) || (zcbor_list_map_end_force_encode(state), false)) && zcbor_list_end_encode(state, 20)))) || (zcbor_list_map_end_force_encode(state), false)) && zcbor_map_end_encode(state, 1))));

	if (!tmp_result)
		zcbor_trace();

	return tmp_result;
}

static bool encode_repeated_ground_fix_req_wifi(
		zcbor_state_t *state, const struct ground_fix_req_wifi *input)
{
	zcbor_print("%s\r\n", __func__);
	struct zcbor_string tmp_str;

	bool tmp_result = ((((zcbor_tstr_encode(state, ((tmp_str.value = (uint8_t *)"wifi", tmp_str.len = sizeof("wifi") - 1, &tmp_str)))))
	&& (encode_wifi_ob(state, (&(*input)._ground_fix_req_wifi)))));

	if (!tmp_result)
		zcbor_trace();

	return tmp_result;
}

static bool encode_ground_fix_req(
		zcbor_state_t *state, const struct ground_fix_req *input)
{
	zcbor_print("%s\r\n", __func__);

	bool tmp_result = (((zcbor_map_start_encode(state, 2) && ((zcbor_present_encode(&((*input)._ground_fix_req_lte_present), (zcbor_encoder_t *)encode_repeated_ground_fix_req_lte, state, (&(*input)._ground_fix_req_lte))
	&& zcbor_present_encode(&((*input)._ground_fix_req_wifi_present), (zcbor_encoder_t *)encode_repeated_ground_fix_req_wifi, state, (&(*input)._ground_fix_req_wifi))) || (zcbor_list_map_end_force_encode(state), false)) && zcbor_map_end_encode(state, 2))));

	if (!tmp_result)
		zcbor_trace();

	return tmp_result;
}



int cbor_encode_ground_fix_req(
		uint8_t *payload, size_t payload_len,
		const struct ground_fix_req *input,
		size_t *payload_len_out)
{
	zcbor_state_t states[7];

	zcbor_new_state(states, sizeof(states) / sizeof(zcbor_state_t), payload, payload_len, 1);

	bool ret = encode_ground_fix_req(states, input);

	if (ret && (payload_len_out != NULL)) {
		*payload_len_out = MIN(payload_len,
				(size_t)states[0].payload - (size_t)payload);
	}

	if (!ret) {
		int err = zcbor_pop_error(states);

		zcbor_print("Return error: %d\r\n", err);
		return (err == ZCBOR_SUCCESS) ? ZCBOR_ERR_UNKNOWN : err;
	}
	return ZCBOR_SUCCESS;
}
