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
#include "cbor_xcode.h"

#if DEFAULT_MAX_QTY != 5
#error "The type file was generated with a different default_max_qty than this file"
#endif

static bool encode_ncell(zcbor_state_t *state, const struct ncell *input);

static bool encode_cell(zcbor_state_t *state, const struct cell *input);

static bool encode_lte(zcbor_state_t *state, const struct lte *input);

static bool encode_ap(zcbor_state_t *state, const struct ap *input);

static bool encode_wifi(zcbor_state_t *state, const struct wifi *input);

static bool encode_groundfix(zcbor_state_t *state, const struct groundfix *input);


static bool encode_ncell(
		zcbor_state_t *state, const struct ncell *input)
{
	zcbor_print("%s\r\n", __func__);

	bool tmp_result = (((zcbor_list_start_encode(state, 5) && ((((zcbor_uint32_encode(state, (&(*input)._ncell_earfcn))))
	&& ((zcbor_uint32_encode(state, (&(*input)._ncell_pci))))
	&& zcbor_present_encode(&((*input)._ncell_rsrp_present), (zcbor_encoder_t *)zcbor_int32_encode, state, (&(*input)._ncell_rsrp))
	&& zcbor_present_encode(&((*input)._ncell_rsrq_present), (zcbor_encoder_t *)zcbor_float64_encode, state, (&(*input)._ncell_rsrq))
	&& zcbor_present_encode(&((*input)._ncell_timeDiff_present), (zcbor_encoder_t *)zcbor_int32_encode, state, (&(*input)._ncell_timeDiff))) || (zcbor_list_map_end_force_encode(state), false)) && zcbor_list_end_encode(state, 5))));

	if (!tmp_result)
		zcbor_trace();

	return tmp_result;
}


static bool encode_cell(
		zcbor_state_t *state, const struct cell *input)
{
	zcbor_print("%s\r\n", __func__);

	bool tmp_result = (((zcbor_list_start_encode(state, 9) && ((((zcbor_int32_encode(state, (&(*input)._cell_mcc))))
	&& ((zcbor_int32_encode(state, (&(*input)._cell_mnc))))
	&& ((zcbor_uint32_encode(state, (&(*input)._cell_eci))))
	&& ((zcbor_uint32_encode(state, (&(*input)._cell_tac))))
	&& zcbor_present_encode(&((*input)._cell_earfcn_present), (zcbor_encoder_t *)zcbor_uint32_encode, state, (&(*input)._cell_earfcn))
	&& zcbor_present_encode(&((*input)._cell_adv_present), (zcbor_encoder_t *)zcbor_uint32_encode, state, (&(*input)._cell_adv))
	&& ((zcbor_list_start_encode(state, CONFIG_LTE_NEIGHBOR_CELLS_MAX) && ((zcbor_multi_encode_minmax(0, CONFIG_LTE_NEIGHBOR_CELLS_MAX, &(*input)._cell_nmr_ncells_count, (zcbor_encoder_t *)encode_ncell, state, (&(*input)._cell_nmr_ncells), sizeof(struct ncell))) || (zcbor_list_map_end_force_encode(state), false)) && zcbor_list_end_encode(state, CONFIG_LTE_NEIGHBOR_CELLS_MAX)))
	&& zcbor_present_encode(&((*input)._cell_rsrp_present), (zcbor_encoder_t *)zcbor_int32_encode, state, (&(*input)._cell_rsrp))
	&& zcbor_present_encode(&((*input)._cell_rsrq_present), (zcbor_encoder_t *)zcbor_float64_encode, state, (&(*input)._cell_rsrq))) || (zcbor_list_map_end_force_encode(state), false)) && zcbor_list_end_encode(state, 9))));

	if (!tmp_result)
		zcbor_trace();

	return tmp_result;
}


static bool encode_lte(
		zcbor_state_t *state, const struct lte *input)
{
	zcbor_print("%s\r\n", __func__);

	bool tmp_result = (((zcbor_list_start_encode(state, 5) && ((zcbor_multi_encode_minmax(1, 5, &(*input)._lte__cell_count, (zcbor_encoder_t *)encode_cell, state, (&(*input)._lte__cell), sizeof(struct cell))) || (zcbor_list_map_end_force_encode(state), false)) && zcbor_list_end_encode(state, 5))));

	if (!tmp_result)
		zcbor_trace();

	return tmp_result;
}


static bool encode_ap(
		zcbor_state_t *state, const struct ap *input)
{
	zcbor_print("%s\r\n", __func__);

	bool tmp_result = (((zcbor_list_start_encode(state, 6) && ((((zcbor_tstr_encode(state, (&(*input)._ap_mac))))
	&& zcbor_present_encode(&((*input)._ap_age_present), (zcbor_encoder_t *)zcbor_uint32_encode, state, (&(*input)._ap_age))
	&& zcbor_present_encode(&((*input)._ap_freq_present), (zcbor_encoder_t *)zcbor_uint32_encode, state, (&(*input)._ap_freq))
	&& zcbor_present_encode(&((*input)._ap_rssi_present), (zcbor_encoder_t *)zcbor_int32_encode, state, (&(*input)._ap_rssi))
	&& zcbor_present_encode(&((*input)._ap_ch_present), (zcbor_encoder_t *)zcbor_uint32_encode, state, (&(*input)._ap_ch))
	&& zcbor_present_encode(&((*input)._ap_ssid_present), (zcbor_encoder_t *)zcbor_tstr_encode, state, (&(*input)._ap_ssid))) || (zcbor_list_map_end_force_encode(state), false)) && zcbor_list_end_encode(state, 6))));

	if (!tmp_result)
		zcbor_trace();

	return tmp_result;
}


static bool encode_wifi(
		zcbor_state_t *state, const struct wifi *input)
{
	zcbor_print("%s\r\n", __func__);

	bool tmp_result = (((zcbor_list_start_encode(state, 60) && ((zcbor_multi_encode_minmax(2, 60, &(*input)._wifi__ap_count, (zcbor_encoder_t *)encode_ap, state, (&(*input)._wifi__ap), sizeof(struct ap))) || (zcbor_list_map_end_force_encode(state), false)) && zcbor_list_end_encode(state, 60))));

	if (!tmp_result)
		zcbor_trace();

	return tmp_result;
}


static bool encode_groundfix(
		zcbor_state_t *state, const struct groundfix *input)
{
	zcbor_print("%s\r\n", __func__);

	bool tmp_result = (((zcbor_list_start_encode(state, 6) && ((zcbor_multi_encode_minmax(0, 1, &(*input)._groundfix__lte_count, (zcbor_encoder_t *)encode_lte, state, (&(*input)._groundfix__lte), sizeof(struct lte))
	&& zcbor_multi_encode_minmax(0, 1, &(*input)._groundfix__wifi_count, (zcbor_encoder_t *)encode_wifi, state, (&(*input)._groundfix__wifi), sizeof(struct wifi))) || (zcbor_list_map_end_force_encode(state), false)) && zcbor_list_end_encode(state, 6))));

	if (!tmp_result)
		zcbor_trace();

	return tmp_result;
}



int cbor_encode_groundfix(
		uint8_t *payload, size_t payload_len,
		const struct groundfix *input,
		size_t *payload_len_out)
{
	zcbor_state_t states[7];

	zcbor_new_state(states, sizeof(states) / sizeof(zcbor_state_t), payload, payload_len, 1);

	bool ret = encode_groundfix(states, input);

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
