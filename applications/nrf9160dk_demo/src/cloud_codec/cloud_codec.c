/*
 * Copyright (c) 2019 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <zephyr.h>
#include <zephyr/types.h>
#include <net/cloud.h>
#include <sys/util.h>

#include "cJSON.h"
#include "cJSON_os.h"
#include "cloud_codec.h"

#include <logging/log.h>
LOG_MODULE_REGISTER(cloud_codec, CONFIG_NRF9160DK_DEMO_LOG_LEVEL);

#define CMD_GROUP_KEY_STR "messageType"
#define CMD_CHAN_KEY_STR "appId"
#define CMD_DATA_TYPE_KEY_STR "data"

#define DISABLE_SEND_INTERVAL_VAL 0
#define MIN_INTERVAL_VAL_SECONDS 5

struct cmd {
	const char *const key;
	union {
		enum cloud_cmd_group group;
		enum cloud_channel channel;
		enum cloud_cmd_type type;
	};

	struct cmd *children;
	size_t num_children;
};

enum sensor_chan_cfg_item_type {
	SENSOR_CHAN_CFG_ITEM_TYPE__BEGIN,

	SENSOR_CHAN_CFG_ITEM_TYPE_SEND_ENABLE =
		SENSOR_CHAN_CFG_ITEM_TYPE__BEGIN,
	SENSOR_CHAN_CFG_ITEM_TYPE_THRESH_LOW_VALUE,
	SENSOR_CHAN_CFG_ITEM_TYPE_THRESH_LOW_ENABLE,
	SENSOR_CHAN_CFG_ITEM_TYPE_THRESH_HIGH_VALUE,
	SENSOR_CHAN_CFG_ITEM_TYPE_THRESH_HIGH_ENABLE,

	SENSOR_CHAN_CFG_ITEM_TYPE__END
};

struct sensor_chan_cfg {
	double value[SENSOR_CHAN_CFG_ITEM_TYPE__END];
};

struct cloud_sensor_chan_cfg {
	enum cloud_channel chan;
	struct sensor_chan_cfg cfg;
};

#define CMD_ARRAY(...) ((struct cmd[]){ __VA_ARGS__ })

#define CMD_NEW_TYPE(_type) \
	{ \
		.key = CMD_DATA_TYPE_KEY_STR, .type = _type, \
	}

#define CMD_NEW_CHAN(_chan, _children) \
	{ \
		.key = CMD_CHAN_KEY_STR, .channel = _chan, \
		.children = _children, .num_children = ARRAY_SIZE(_children), \
	}

#define CMD_NEW_GROUP(_var_name, _group, _children) \
	struct cmd _var_name = { \
		.key = CMD_GROUP_KEY_STR, \
		.group = _group, \
		.children = _children, \
		.num_children = ARRAY_SIZE(_children), \
	}

static CMD_NEW_GROUP(group_cfg_set, CLOUD_CMD_GROUP_CFG_SET, CMD_ARRAY(
		CMD_NEW_CHAN(CLOUD_CHANNEL_HUMID, CMD_ARRAY(
			CMD_NEW_TYPE(CLOUD_CMD_ENABLE),
			CMD_NEW_TYPE(CLOUD_CMD_THRESHOLD_HIGH),
			CMD_NEW_TYPE(CLOUD_CMD_THRESHOLD_LOW)
			)
		),
		CMD_NEW_CHAN(CLOUD_CHANNEL_AIR_PRESS, CMD_ARRAY(
			CMD_NEW_TYPE(CLOUD_CMD_ENABLE),
			CMD_NEW_TYPE(CLOUD_CMD_THRESHOLD_HIGH),
			CMD_NEW_TYPE(CLOUD_CMD_THRESHOLD_LOW)
			)
		),
		CMD_NEW_CHAN(CLOUD_CHANNEL_TEMP, CMD_ARRAY(
			CMD_NEW_TYPE(CLOUD_CMD_ENABLE),
			CMD_NEW_TYPE(CLOUD_CMD_THRESHOLD_HIGH),
			CMD_NEW_TYPE(CLOUD_CMD_THRESHOLD_LOW)
			)
		),
		CMD_NEW_CHAN(CLOUD_CHANNEL_AIR_QUAL, CMD_ARRAY(
			CMD_NEW_TYPE(CLOUD_CMD_ENABLE),
			CMD_NEW_TYPE(CLOUD_CMD_THRESHOLD_HIGH),
			CMD_NEW_TYPE(CLOUD_CMD_THRESHOLD_LOW)
			)
		),
		CMD_NEW_CHAN(CLOUD_CHANNEL_GPS, CMD_ARRAY(
			CMD_NEW_TYPE(CLOUD_CMD_ENABLE),
			CMD_NEW_TYPE(CLOUD_CMD_INTERVAL)
			)
		),
		CMD_NEW_CHAN(CLOUD_CHANNEL_LIGHT_SENSOR, CMD_ARRAY(
			CMD_NEW_TYPE(CLOUD_CMD_INTERVAL)
			)
		),
		CMD_NEW_CHAN(CLOUD_CHANNEL_LIGHT_RED, CMD_ARRAY(
			CMD_NEW_TYPE(CLOUD_CMD_ENABLE),
			CMD_NEW_TYPE(CLOUD_CMD_THRESHOLD_HIGH),
			CMD_NEW_TYPE(CLOUD_CMD_THRESHOLD_LOW)
			)
		),
		CMD_NEW_CHAN(CLOUD_CHANNEL_LIGHT_GREEN, CMD_ARRAY(
			CMD_NEW_TYPE(CLOUD_CMD_ENABLE),
			CMD_NEW_TYPE(CLOUD_CMD_THRESHOLD_HIGH),
			CMD_NEW_TYPE(CLOUD_CMD_THRESHOLD_LOW)
			)
		),
		CMD_NEW_CHAN(CLOUD_CHANNEL_LIGHT_BLUE, CMD_ARRAY(
			CMD_NEW_TYPE(CLOUD_CMD_ENABLE),
			CMD_NEW_TYPE(CLOUD_CMD_THRESHOLD_HIGH),
			CMD_NEW_TYPE(CLOUD_CMD_THRESHOLD_LOW)
			)
		),
		CMD_NEW_CHAN(CLOUD_CHANNEL_LIGHT_IR, CMD_ARRAY(
			CMD_NEW_TYPE(CLOUD_CMD_ENABLE),
			CMD_NEW_TYPE(CLOUD_CMD_THRESHOLD_HIGH),
			CMD_NEW_TYPE(CLOUD_CMD_THRESHOLD_LOW)
			)
		),
		CMD_NEW_CHAN(CLOUD_CHANNEL_LED, CMD_ARRAY(
			CMD_NEW_TYPE(CLOUD_CMD_COLOR),
			CMD_NEW_TYPE(CLOUD_CMD_ENABLE),
			CMD_NEW_TYPE(CLOUD_CMD_STATE),
			)
		),
		CMD_NEW_CHAN(CLOUD_CHANNEL_ENVIRONMENT, CMD_ARRAY(
			CMD_NEW_TYPE(CLOUD_CMD_INTERVAL)
			)
		),
	)
);

static CMD_NEW_GROUP(group_get, CLOUD_CMD_GROUP_GET, CMD_ARRAY(
		CMD_NEW_CHAN(CLOUD_CHANNEL_LTE_LINK_RSRP, CMD_ARRAY(
			CMD_NEW_TYPE(CLOUD_CMD_EMPTY)
			)
		),
		CMD_NEW_CHAN(CLOUD_CHANNEL_DEVICE_INFO, CMD_ARRAY(
			CMD_NEW_TYPE(CLOUD_CMD_EMPTY)
			)
		)
	)
);

static CMD_NEW_GROUP(group_data, CLOUD_CMD_GROUP_DATA, CMD_ARRAY(
		CMD_NEW_CHAN(CLOUD_CHANNEL_ASSISTED_GPS, CMD_ARRAY(
			CMD_NEW_TYPE(CLOUD_CMD_MODEM_PARAM)
			)
		),
	)
);

static CMD_NEW_GROUP(group_command, CLOUD_CMD_GROUP_COMMAND, CMD_ARRAY(
		CMD_NEW_CHAN(CLOUD_CHANNEL_MODEM, CMD_ARRAY(
			CMD_NEW_TYPE(CLOUD_CMD_DATA_STRING)
			)
		),
	)
);

struct cmd *cmd_groups[] = { &group_cfg_set, &group_get, &group_data,
			      &group_command };
static cloud_cmd_cb_t cloud_command_cb;
struct cloud_command cmd_parsed;

static const char *const channel_type_str[] = {
	[CLOUD_CHANNEL_GPS] = CLOUD_CHANNEL_STR_GPS,
	[CLOUD_CHANNEL_FLIP] = CLOUD_CHANNEL_STR_FLIP,
	[CLOUD_CHANNEL_IMPACT] = "",
	[CLOUD_CHANNEL_BUTTON] = CLOUD_CHANNEL_STR_BUTTON,
	[CLOUD_CHANNEL_PIN] = "",
	[CLOUD_CHANNEL_LED] = CLOUD_CHANNEL_STR_LED,
	[CLOUD_CHANNEL_BUZZER] = "",
	[CLOUD_CHANNEL_ENVIRONMENT] = CLOUD_CHANNEL_STR_ENVIRONMENT,
	[CLOUD_CHANNEL_TEMP] = CLOUD_CHANNEL_STR_TEMP,
	[CLOUD_CHANNEL_HUMID] = CLOUD_CHANNEL_STR_HUMID,
	[CLOUD_CHANNEL_AIR_PRESS] = CLOUD_CHANNEL_STR_AIR_PRESS,
	[CLOUD_CHANNEL_AIR_QUAL] = CLOUD_CHANNEL_STR_AIR_QUAL,
	[CLOUD_CHANNEL_LTE_LINK_RSRP] = CLOUD_CHANNEL_STR_LTE_LINK_RSRP,
	[CLOUD_CHANNEL_DEVICE_INFO] = CLOUD_CHANNEL_STR_DEVICE_INFO,
	[CLOUD_CHANNEL_LIGHT_SENSOR] = CLOUD_CHANNEL_STR_LIGHT_SENSOR,
	[CLOUD_CHANNEL_LIGHT_RED] = CLOUD_CHANNEL_STR_LIGHT_RED,
	[CLOUD_CHANNEL_LIGHT_GREEN] = CLOUD_CHANNEL_STR_LIGHT_GREEN,
	[CLOUD_CHANNEL_LIGHT_BLUE] = CLOUD_CHANNEL_STR_LIGHT_BLUE,
	[CLOUD_CHANNEL_LIGHT_IR] = CLOUD_CHANNEL_STR_LIGHT_IR,
	[CLOUD_CHANNEL_ASSISTED_GPS] = CLOUD_CHANNEL_STR_ASSISTED_GPS,
	[CLOUD_CHANNEL_MODEM] = CLOUD_CHANNEL_STR_MODEM,
	[CLOUD_CHANNEL_MSG] = CLOUD_CHANNEL_STR_MSG,
};
BUILD_ASSERT(ARRAY_SIZE(channel_type_str) == CLOUD_CHANNEL__TOTAL);

static const char *const cmd_group_str[] = {
	[CLOUD_CMD_GROUP_HELLO] = CLOUD_CMD_GROUP_STR_HELLO,
	[CLOUD_CMD_GROUP_START] = CLOUD_CMD_GROUP_STR_START,
	[CLOUD_CMD_GROUP_STOP] = CLOUD_CMD_GROUP_STR_STOP,
	[CLOUD_CMD_GROUP_INIT] = CLOUD_CMD_GROUP_STR_INIT,
	[CLOUD_CMD_GROUP_GET] = CLOUD_CMD_GROUP_STR_GET,
	[CLOUD_CMD_GROUP_STATUS] = CLOUD_CMD_GROUP_STR_STATUS,
	[CLOUD_CMD_GROUP_DATA] = CLOUD_CMD_GROUP_STR_DATA,
	[CLOUD_CMD_GROUP_OK] = CLOUD_CMD_GROUP_STR_OK,
	[CLOUD_CMD_GROUP_CFG_SET] = CLOUD_CMD_GROUP_STR_CFG_SET,
	[CLOUD_CMD_GROUP_CFG_GET] = CLOUD_CMD_GROUP_STR_CFG_GET,
	[CLOUD_CMD_GROUP_COMMAND] = CLOUD_CMD_GROUP_STR_COMMAND,
};
BUILD_ASSERT(ARRAY_SIZE(cmd_group_str) == CLOUD_CMD_GROUP__TOTAL);

static const char *const cmd_type_str[] = {
	[CLOUD_CMD_EMPTY] = CLOUD_CMD_TYPE_STR_EMPTY,
	[CLOUD_CMD_ENABLE] = CLOUD_CMD_TYPE_STR_ENABLE,
	[CLOUD_CMD_THRESHOLD_HIGH] = CLOUD_CMD_TYPE_STR_THRESH_HI,
	[CLOUD_CMD_THRESHOLD_LOW] = CLOUD_CMD_TYPE_STR_THRESH_LO,
	[CLOUD_CMD_INTERVAL] = CLOUD_CMD_TYPE_STR_INTERVAL,
	[CLOUD_CMD_COLOR] = CLOUD_CMD_TYPE_STR_COLOR,
	[CLOUD_CMD_MODEM_PARAM] = CLOUD_CMD_TYPE_STR_MODEM_PARAM,
	[CLOUD_CMD_DATA_STRING] = CLOUD_CMD_TYPE_STR_DATA_STRING,
	[CLOUD_CMD_STATE] = CLOUD_CMD_TYPE_STR_STATE,
};
BUILD_ASSERT(ARRAY_SIZE(cmd_type_str) == CLOUD_CMD__TOTAL);

static struct cloud_sensor_chan_cfg sensor_cfg[] = {
	{ .chan = CLOUD_CHANNEL_TEMP,
	  .cfg = { .value = {
		  [SENSOR_CHAN_CFG_ITEM_TYPE_SEND_ENABLE] = true } } },
	{ .chan = CLOUD_CHANNEL_HUMID,
	  .cfg = { .value = {
		  [SENSOR_CHAN_CFG_ITEM_TYPE_SEND_ENABLE] = true } } },
	{ .chan = CLOUD_CHANNEL_AIR_PRESS,
	  .cfg = { .value = {
		  [SENSOR_CHAN_CFG_ITEM_TYPE_SEND_ENABLE] = true } } },
	{ .chan = CLOUD_CHANNEL_AIR_QUAL,
	  .cfg = { .value = {
		  [SENSOR_CHAN_CFG_ITEM_TYPE_SEND_ENABLE] = true } } },
	{ .chan = CLOUD_CHANNEL_LIGHT_RED,
	  .cfg = { .value = {
		  [SENSOR_CHAN_CFG_ITEM_TYPE_SEND_ENABLE] = true } } },
	{ .chan = CLOUD_CHANNEL_LIGHT_GREEN,
	  .cfg = { .value = {
		  [SENSOR_CHAN_CFG_ITEM_TYPE_SEND_ENABLE] = true } } },
	{ .chan = CLOUD_CHANNEL_LIGHT_BLUE,
	  .cfg = { .value = {
		  [SENSOR_CHAN_CFG_ITEM_TYPE_SEND_ENABLE] = true } } },
	{ .chan = CLOUD_CHANNEL_LIGHT_IR,
	  .cfg = { .value = {
		  [SENSOR_CHAN_CFG_ITEM_TYPE_SEND_ENABLE] = true } } }
};

static int cloud_cmd_handle_sensor_set_chan_cfg(struct cloud_command
						const *const cmd);

static int cloud_set_chan_cfg_item(const enum cloud_channel channel,
				   const enum sensor_chan_cfg_item_type type,
				   const double value);

static int json_add_obj(cJSON *parent, const char *str, cJSON *item)
{
	cJSON_AddItemToObject(parent, str, item);

	return 0;
}

static int json_add_str(cJSON *parent, const char *str, const char *item)
{
	cJSON *json_str;

	json_str = cJSON_CreateString(item);
	if (json_str == NULL) {
		return -ENOMEM;
	}

	return json_add_obj(parent, str, json_str);
}

static cJSON *json_object_decode(cJSON *obj, const char *str)
{
	return obj ? cJSON_GetObjectItem(obj, str) : NULL;
}

static bool json_value_string_compare(cJSON *obj, const char *const str)
{
	char *json_str = cJSON_GetStringValue(obj);

	if ((json_str == NULL) || (str == NULL)) {
		return false;
	}

	return (strcmp(json_str, str) == 0);
}

int cloud_encode_data(const struct cloud_channel_data *channel,
		      const enum cloud_cmd_group group,
		      struct cloud_msg *output)
{
	int ret;

	if (channel == NULL || channel->data.buf == NULL ||
	    channel->data.len == 0 || output == NULL ||
	    group >= CLOUD_CMD_GROUP__TOTAL) {
		return -EINVAL;
	}

	cJSON *root_obj = cJSON_CreateObject();

	if (root_obj == NULL) {
		cJSON_Delete(root_obj);
		return -ENOMEM;
	}

	ret = json_add_str(root_obj, CMD_CHAN_KEY_STR,
			   channel_type_str[channel->type]);
	ret += json_add_str(root_obj, CMD_GROUP_KEY_STR, cmd_group_str[group]);
	ret += json_add_str(root_obj, CMD_DATA_TYPE_KEY_STR, channel->data.buf);
	if (ret != 0) {
		cJSON_Delete(root_obj);
		return -ENOMEM;
	}

	char *buffer;

	buffer = cJSON_PrintUnformatted(root_obj);
	cJSON_Delete(root_obj);

	output->buf = buffer;
	output->len = strlen(buffer);

	return 0;
}

int cloud_encode_digital_twin_data(const struct cloud_channel_data *channel,
				   struct cloud_msg *output)
{
	int ret = 0;
	char *buffer;

	__ASSERT_NO_MSG(channel != NULL);
	__ASSERT_NO_MSG(channel->data.buf != NULL);
	__ASSERT_NO_MSG(channel->data.len != 0);
	__ASSERT_NO_MSG(output != NULL);

	cJSON *root_obj = cJSON_CreateObject();
	cJSON *state_obj = cJSON_CreateObject();
	cJSON *reported_obj = cJSON_CreateObject();
	char dev_str[] = CLOUD_CHANNEL_STR_DEVICE_INFO;
	const char *channel_type;

	if (root_obj == NULL || state_obj == NULL || reported_obj == NULL) {
		cJSON_Delete(root_obj);
		cJSON_Delete(state_obj);
		cJSON_Delete(reported_obj);
		return -ENOMEM;
	}

	/* Workaround for deleting "DEVICE" objects (with uppercase key) if
	 * it already exists in the digital twin.
	 * The cloud implementations expect the key to be lowercase "device",
	 * but that would duplicate the information and needlessly increase
	 * the size of the digital twin document if the "DEVICE" is not
	 * deleted at the same time.
	 */
	if (channel->type == CLOUD_CHANNEL_DEVICE_INFO) {
		cJSON *dummy_obj = cJSON_CreateNull();

		if (dummy_obj == NULL) {
			/* Dummy creation failed, but we'll let it do so
			 * silently as it's not a functionally critical error.
			 */
		} else {
			ret += json_add_obj(reported_obj, dev_str, dummy_obj);
		}

		/* Convert to lowercase for shadow */
		for (int i = 0; dev_str[i]; ++i) {
			dev_str[i] = tolower(dev_str[i]);
		}
		channel_type = dev_str;
	} else {
		channel_type = channel_type_str[channel->type];
	}

	ret += json_add_obj(reported_obj, channel_type,
			   (cJSON *)channel->data.buf);
	ret += json_add_obj(state_obj, "reported", reported_obj);
	ret += json_add_obj(root_obj, "state", state_obj);

	if (ret != 0) {
		cJSON_Delete(root_obj);
		cJSON_Delete(state_obj);
		cJSON_Delete(reported_obj);
		return -EAGAIN;
	}

	buffer = cJSON_PrintUnformatted(root_obj);
	cJSON_Delete(root_obj);

	output->buf = buffer;
	output->len = strlen(buffer);

	return 0;
}

static int cloud_decode_modem_params(cJSON *const data_obj,
			  struct cloud_command_modem_params *const params)
{
	cJSON *blob;
	cJSON *checksum;

	if ((data_obj == NULL) || (params == NULL)) {
		return -EINVAL;
	}

	if (!cJSON_IsObject(data_obj)) {
		return -ESRCH;
	}

	blob = json_object_decode(data_obj, MODEM_PARAM_BLOB_KEY_STR);
	params->blob = cJSON_GetStringValue(blob);

	checksum = json_object_decode(data_obj, MODEM_PARAM_CHECKSUM_KEY_STR);
	params->checksum = cJSON_GetStringValue(checksum);

	return (((params->blob == NULL) || (params->checksum == NULL)) ?
			-ESRCH : 0);
}

static int cloud_cmd_parse_type(const struct cmd *const type_cmd,
				cJSON *type_obj,
				struct cloud_command *const parsed_cmd)
{
	int err;
	cJSON *decoded_obj = NULL;

	if ((type_cmd == NULL) || (parsed_cmd == NULL)) {
		return -EINVAL;
	}

	if (type_obj != NULL) {
		/* Data string type does not require additional decoding */
		if (type_cmd->type != CLOUD_CMD_DATA_STRING) {
			decoded_obj = json_object_decode(type_obj,
					cmd_type_str[type_cmd->type]);

			if (!decoded_obj) {
				return -ENOENT; /* Command not found */
			}
		}

		switch (type_cmd->type) {
		case CLOUD_CMD_ENABLE: {
			if (cJSON_IsNull(decoded_obj)) {
				parsed_cmd->data.sv.state =
					CLOUD_CMD_STATE_FALSE;
			} else if (cJSON_IsBool(decoded_obj)) {
				parsed_cmd->data.sv.state =
					cJSON_IsTrue(decoded_obj) ?
						CLOUD_CMD_STATE_TRUE :
						CLOUD_CMD_STATE_FALSE;
			} else {
				return -ESRCH;
			}

			break;
		}
		case CLOUD_CMD_INTERVAL:
		case CLOUD_CMD_THRESHOLD_LOW:
		case CLOUD_CMD_THRESHOLD_HIGH: {
			if (cJSON_IsNull(decoded_obj)) {
				parsed_cmd->data.sv.state =
					CLOUD_CMD_STATE_FALSE;
			} else if (cJSON_IsNumber(decoded_obj)) {
				parsed_cmd->data.sv.state =
					CLOUD_CMD_STATE_UNDEFINED;
				parsed_cmd->data.sv.value =
					decoded_obj->valuedouble;
			} else {
				return -ESRCH;
			}

			break;
		}
		case CLOUD_CMD_COLOR: {
			if (cJSON_GetStringValue(decoded_obj) == NULL) {
				return -ESRCH;
			}

			parsed_cmd->data.sv.value = (double)strtol(
				cJSON_GetStringValue(decoded_obj), NULL, 16);

			break;
		}
		case CLOUD_CMD_STATE: {
			if (cJSON_IsNull(decoded_obj)) {
				parsed_cmd->data.sv.state =
					CLOUD_CMD_STATE_FALSE;
				LOG_WRN("CLOUD_CMD_STATE is NULL");
			} else if (cJSON_IsArray(decoded_obj)) {
				int len = cJSON_GetArraySize(decoded_obj);
				int i;
				cJSON *entry;
				u32_t value = 0;

				LOG_INF("is array of %d entries", len);

				for (i = 0; i < MIN(len, 4); i++) {
					entry = cJSON_GetArrayItem(decoded_obj,
						i);
					if (cJSON_IsNumber(entry)) {
						LOG_INF("entry %d=%d",
							i, entry->valueint);
						value |= entry->valueint <<
							(8 * i);
					} else {
						LOG_WRN("entry %d invalid", i);
					}
				}
				parsed_cmd->data.sv.value = (double)value;
			}
			break;
		}
		case CLOUD_CMD_MODEM_PARAM: {
			err = cloud_decode_modem_params(decoded_obj,
							&parsed_cmd->data.mp);

			if (err) {
				return err;
			}

			break;
		}
		case CLOUD_CMD_DATA_STRING:
			parsed_cmd->data.data_string =
				cJSON_GetStringValue(type_obj);
			if (parsed_cmd->data.data_string == NULL) {
				return -ESRCH;
			}
			break;
		case CLOUD_CMD_EMPTY:
		default: {
			return -ENOTSUP;
		}
		}
	} else if (type_cmd->type != CLOUD_CMD_EMPTY) {
		/* Only the empty cmd type can have no data */
		return -EINVAL;
	}

	/* Validate interval value */
	if ((type_cmd->type == CLOUD_CMD_INTERVAL) &&
	    (parsed_cmd->data.sv.state == CLOUD_CMD_STATE_UNDEFINED)) {
		if (parsed_cmd->data.sv.value == DISABLE_SEND_INTERVAL_VAL) {
			parsed_cmd->data.sv.state = CLOUD_CMD_STATE_FALSE;
		} else if (parsed_cmd->data.sv.value <
				   MIN_INTERVAL_VAL_SECONDS) {
			parsed_cmd->data.sv.value = MIN_INTERVAL_VAL_SECONDS;
		}
	}

	parsed_cmd->type = type_cmd->type;

	return 0;
}

static int cloud_search_cmd(cJSON *root_obj)
{
	int ret;
	struct cmd *group	= NULL;
	struct cmd *chan	= NULL;
	struct cmd *type	= NULL;
	cJSON *group_obj;
	cJSON *channel_obj;
	cJSON *type_obj;

	if (root_obj == NULL) {
		return -EINVAL;
	}

	for (int i = 0; i < ARRAY_SIZE(cmd_groups); ++i) {
		group_obj = json_object_decode(root_obj, cmd_groups[i]->key);

		if ((group_obj != NULL) &&
			(json_value_string_compare(group_obj,
					cmd_group_str[cmd_groups[i]->group]))) {
			group = cmd_groups[i];
			break;
		}
	}

	if (group == NULL) {
		LOG_WRN("messageType not found");
		return -ENOTSUP;
	}

	cmd_parsed.group = group->group;

	for (size_t j = 0; j < group->num_children; ++j) {
		channel_obj =
			json_object_decode(root_obj, group->children[j].key);

		if ((channel_obj != NULL) &&
		    (json_value_string_compare(
			    channel_obj,
			    channel_type_str[group->children[j].channel]))) {
			chan = &group->children[j];
			break;
		}
	}

	if (chan == NULL) {
		LOG_WRN("appId not found");
		return -ENOTSUP;
	}

	cmd_parsed.channel = chan->channel;

	for (size_t k = 0; k < chan->num_children; ++k) {

		type = &chan->children[k];
		type_obj = json_object_decode(root_obj, type->key);

		ret = cloud_cmd_parse_type(type, type_obj, &cmd_parsed);
		if (ret == -ENOENT) {
			type = NULL;
			continue;
		} else if (ret != 0) {
			LOG_ERR("Unhandled cmd format for %s, %s, error %d",
				log_strdup(cmd_group_str[group->group]),
				log_strdup(channel_type_str[chan->channel]),
				ret);
			continue;
		}

		LOG_INF("Found cmd %s, %s, %s\n",
			log_strdup(cmd_group_str[cmd_parsed.group]),
			log_strdup(channel_type_str[cmd_parsed.channel]),
			log_strdup(cmd_type_str[cmd_parsed.type]));

		/* Handle cfg commands */
		(void)cloud_cmd_handle_sensor_set_chan_cfg(&cmd_parsed);

		if (cloud_command_cb) {
			cloud_command_cb(&cmd_parsed);
		}
	}

	if (type == NULL) {
		LOG_WRN("data type not found");
		return -ENOTSUP;
	}

	return 0;
}

static int cloud_search_config(cJSON * const root_obj)
{
	struct cmd const *const group = &group_cfg_set;
	cJSON *state_obj = NULL;
	cJSON *config_obj = NULL;

	if (root_obj == NULL) {
		return -EINVAL;
	}

	/* A delta update will have state */
	state_obj = cJSON_GetObjectItem(root_obj, "state");
	config_obj = cJSON_DetachItemFromObject(
		state_obj ? state_obj : root_obj, "config");

	if (config_obj == NULL) {
		return 0;
	}

	/* Search all channels */
	for (size_t ch = 0; ch < group->num_children; ++ch) {
		struct cloud_command found_config_item = {
				.group = CLOUD_CMD_GROUP_CFG_SET
			};

		cJSON *channel_obj = json_object_decode(
			config_obj,
			channel_type_str[group->children[ch].channel]);

		if (channel_obj == NULL) {
			continue;
		}

		struct cmd *chan = &group->children[ch];

		found_config_item.channel = chan->channel;

		/* Search channel's config types */
		for (size_t type = 0; type < chan->num_children; ++type) {
			int ret = cloud_cmd_parse_type(&chan->children[type],
						   channel_obj,
						   &found_config_item);

			if (ret == -ENOENT) {
				continue;
			} else if (ret != 0) {
				LOG_ERR("Unhandled cfg format for %s, error %d",
					log_strdup(channel_type_str[
						chan->channel]),
					ret);
				continue;
			}

			LOG_INF("[%s:%d] Found cfg item %s, %s\n", __func__,
				__LINE__,
				log_strdup(channel_type_str[
					found_config_item.channel]),
				log_strdup(cmd_type_str[
					found_config_item.type]));

			/* Handle cfg commands */
			(void)cloud_cmd_handle_sensor_set_chan_cfg(
				&found_config_item);

			if (cloud_command_cb) {
				cloud_command_cb(&found_config_item);
			}
		}
	}

	/* Config was detached, must be deleted */
	cJSON_Delete(config_obj);

	return 0;
}

int cloud_decode_command(char const *input)
{
	cJSON *root_obj = NULL;
	int err;

	if (input == NULL) {
		return -EINVAL;
	}

	root_obj = cJSON_Parse(input);
	if (root_obj == NULL) {
		LOG_ERR("[%s:%d] Unable to parse input", __func__, __LINE__);
		return -ENOENT;
	}

	err = cloud_search_cmd(root_obj);
	if (!err) {
		cloud_search_config(root_obj);
	}

	cJSON_Delete(root_obj);

	return err;
}

int cloud_decode_init(cloud_cmd_cb_t cb)
{
	cloud_command_cb = cb;

	return 0;
}

static int sensor_chan_cfg_set_item(struct sensor_chan_cfg *const cfg,
				  const enum sensor_chan_cfg_item_type type,
				  const double value)
{
	if ((type < SENSOR_CHAN_CFG_ITEM_TYPE__BEGIN) ||
	    (type >= SENSOR_CHAN_CFG_ITEM_TYPE__END) || (cfg == NULL)) {
		return -EINVAL;
	}

	cfg->value[type] = value;

	return 0;
}

static bool sensor_chan_cfg_is_send_allowed(const struct sensor_chan_cfg
						*const cfg,
						const double sensor_value)
{
	if ((cfg == NULL) ||
	    (!cfg->value[SENSOR_CHAN_CFG_ITEM_TYPE_SEND_ENABLE])) {
		return false;
	}

	if (((cfg->value[SENSOR_CHAN_CFG_ITEM_TYPE_THRESH_LOW_ENABLE]) &&
	     (sensor_value <
	      cfg->value[SENSOR_CHAN_CFG_ITEM_TYPE_THRESH_LOW_VALUE]))) {
		return true;
	}

	if (((cfg->value[SENSOR_CHAN_CFG_ITEM_TYPE_THRESH_HIGH_ENABLE]) &&
	     (sensor_value >
	      cfg->value[SENSOR_CHAN_CFG_ITEM_TYPE_THRESH_HIGH_VALUE]))) {
		return true;
	}

	return (!cfg->value[SENSOR_CHAN_CFG_ITEM_TYPE_THRESH_LOW_ENABLE] &&
		!cfg->value[SENSOR_CHAN_CFG_ITEM_TYPE_THRESH_HIGH_ENABLE]);
}

static int cloud_set_chan_cfg_item(const enum cloud_channel channel,
			  const enum sensor_chan_cfg_item_type type,
			  const double value)
{
	for (int i = 0; i < ARRAY_SIZE(sensor_cfg); ++i) {
		if (sensor_cfg[i].chan == channel) {
			return sensor_chan_cfg_set_item(&sensor_cfg[i].cfg,
							type, value);
		}
	}

	return -ENOTSUP;
}

bool cloud_is_send_allowed(const enum cloud_channel channel, const double value)
{
	for (int i = 0; i < ARRAY_SIZE(sensor_cfg); ++i) {
		if (sensor_cfg[i].chan == channel) {
			return sensor_chan_cfg_is_send_allowed(
						     &sensor_cfg[i].cfg, value);
		}
	}

	return false;
}

static int cloud_cmd_handle_sensor_set_chan_cfg(struct cloud_command
						const *const cmd)
{
	int err = -ENOTSUP;

	if ((cmd == NULL) || (cmd->group != CLOUD_CMD_GROUP_CFG_SET)) {
		return -EINVAL;
	}

	switch (cmd->type) {
	case CLOUD_CMD_ENABLE:
		err = cloud_set_chan_cfg_item(
			cmd->channel,
			SENSOR_CHAN_CFG_ITEM_TYPE_SEND_ENABLE,
			(cmd->data.sv.state == CLOUD_CMD_STATE_TRUE));
		break;
	case CLOUD_CMD_THRESHOLD_HIGH:
		if (cmd->data.sv.state == CLOUD_CMD_STATE_UNDEFINED) {
			err = cloud_set_chan_cfg_item(
				cmd->channel,
				SENSOR_CHAN_CFG_ITEM_TYPE_THRESH_HIGH_VALUE,
				cmd->data.sv.value);
			cloud_set_chan_cfg_item(
				cmd->channel,
				SENSOR_CHAN_CFG_ITEM_TYPE_THRESH_HIGH_ENABLE,
				true);

		} else {
			err = cloud_set_chan_cfg_item(
				cmd->channel,
				SENSOR_CHAN_CFG_ITEM_TYPE_THRESH_HIGH_ENABLE,
				(cmd->data.sv.state == CLOUD_CMD_STATE_TRUE));
		}
		break;
	case CLOUD_CMD_THRESHOLD_LOW:
		if (cmd->data.sv.state == CLOUD_CMD_STATE_UNDEFINED) {
			err = cloud_set_chan_cfg_item(
				cmd->channel,
				SENSOR_CHAN_CFG_ITEM_TYPE_THRESH_LOW_VALUE,
				cmd->data.sv.value);
			cloud_set_chan_cfg_item(
				cmd->channel,
				SENSOR_CHAN_CFG_ITEM_TYPE_THRESH_LOW_ENABLE,
				true);

		} else {
			err = cloud_set_chan_cfg_item(
				cmd->channel,
				SENSOR_CHAN_CFG_ITEM_TYPE_THRESH_LOW_ENABLE,
				(cmd->data.sv.state == CLOUD_CMD_STATE_TRUE));
		}
		break;
	default:
		break;
	}

	return err;
}

const char *cloud_get_group_name(const enum cloud_cmd_group group)
{
	if (((int)group >= 0) && (group < CLOUD_CMD_GROUP__TOTAL)) {
		return cmd_group_str[group];
	} else {
		return "unknown cmd group";
	}
}

const char *cloud_get_channel_name(const enum cloud_channel channel)
{
	if (((int)channel >= 0) && (channel < CLOUD_CHANNEL__TOTAL)) {
		return channel_type_str[channel];
	} else {
		return "unknown cmd channel";
	}
}

const char *cloud_get_type_name(const enum cloud_cmd_type type)
{
	if (((int)type >= 0) && (type < CLOUD_CMD__TOTAL)) {
		return cmd_type_str[type];
	} else {
		return "unknown cmd type";
	}
}
