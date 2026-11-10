#include "paradox_config.h"

#include <stdio.h>
#include <string.h>

#define CONFIG_BLOB_HEADER_SIZE 1u
#define CONFIG_ZONE_RECORD_SIZE \
    (1u + 1u + PARADOX_CONFIG_ZONE_NAME_SIZE)
#define CONFIG_V0_BLOB_SIZE \
    (CONFIG_BLOB_HEADER_SIZE + PARADOX_MAX_ZONES * CONFIG_ZONE_RECORD_SIZE + 1u)
#define CONFIG_V1_BLOB_SIZE \
    (CONFIG_V0_BLOB_SIZE + PARADOX_CONFIG_WEB_AUTH_HASH_SIZE)

static bool service_is_valid(paradox_hk_service_t service)
{
    return service >= PARADOX_HK_SERVICE_CONTACT &&
           service <= PARADOX_HK_SERVICE_LEAK;
}

void paradox_config_defaults(paradox_config_t *config)
{
    if (config == NULL) {
        return;
    }

    memset(config, 0, sizeof(*config));
    for (size_t i = 0; i < PARADOX_MAX_ZONES; i++) {
        config->zones[i].enabled = true;
        config->zones[i].service = PARADOX_HK_SERVICE_CONTACT;
        (void)snprintf(config->zones[i].name, sizeof(config->zones[i].name),
                       "Zone %u", (unsigned)(i + 1));
    }
}

paradox_config_result_t paradox_config_validate(const paradox_config_t *config)
{
    if (config == NULL) {
        return PARADOX_CONFIG_INVALID_ARGUMENT;
    }

    for (size_t i = 0; i < PARADOX_MAX_ZONES; i++) {
        const paradox_zone_config_t *zone = &config->zones[i];
        if (!service_is_valid(zone->service)) {
            return PARADOX_CONFIG_INVALID_VALUE;
        }

        const char *end = memchr(zone->name, '\0', sizeof(zone->name));
        if (end == NULL) {
            return PARADOX_CONFIG_NAME_TOO_LONG;
        }
        if (end == zone->name) {
            return PARADOX_CONFIG_NAME_EMPTY;
        }
    }
    return PARADOX_CONFIG_OK;
}

static paradox_config_result_t zone_for_update(
    paradox_config_t *config, int zone_id, paradox_zone_config_t **out_zone)
{
    if (config == NULL || out_zone == NULL) {
        return PARADOX_CONFIG_INVALID_ARGUMENT;
    }
    if (zone_id < 1 || zone_id > PARADOX_MAX_ZONES) {
        return PARADOX_CONFIG_ZONE_OUT_OF_RANGE;
    }
    *out_zone = &config->zones[zone_id - 1];
    return PARADOX_CONFIG_OK;
}

paradox_config_result_t paradox_config_set_zone_enabled(
    paradox_config_t *config, int zone_id, bool enabled)
{
    paradox_zone_config_t *zone;
    paradox_config_result_t result = zone_for_update(config, zone_id, &zone);
    if (result == PARADOX_CONFIG_OK) {
        zone->enabled = enabled;
    }
    return result;
}

paradox_config_result_t paradox_config_set_zone_service(
    paradox_config_t *config, int zone_id, paradox_hk_service_t service)
{
    paradox_zone_config_t *zone;
    paradox_config_result_t result = zone_for_update(config, zone_id, &zone);
    if (result != PARADOX_CONFIG_OK) {
        return result;
    }
    if (!service_is_valid(service)) {
        return PARADOX_CONFIG_INVALID_VALUE;
    }
    zone->service = service;
    return PARADOX_CONFIG_OK;
}

paradox_config_result_t paradox_config_set_zone_name(
    paradox_config_t *config, int zone_id, const char *name)
{
    paradox_zone_config_t *zone;
    paradox_config_result_t result = zone_for_update(config, zone_id, &zone);
    if (result != PARADOX_CONFIG_OK) {
        return result;
    }
    if (name == NULL) {
        return PARADOX_CONFIG_INVALID_ARGUMENT;
    }
    size_t length = strnlen(name, PARADOX_CONFIG_ZONE_NAME_SIZE);
    if (length == 0) {
        return PARADOX_CONFIG_NAME_EMPTY;
    }
    if (length > PARADOX_CONFIG_ZONE_NAME_MAX) {
        return PARADOX_CONFIG_NAME_TOO_LONG;
    }
    memcpy(zone->name, name, length + 1);
    memset(zone->name + length + 1, 0,
           sizeof(zone->name) - length - 1);
    return PARADOX_CONFIG_OK;
}

paradox_config_result_t paradox_config_set_panel_debug(
    paradox_config_t *config, bool enabled)
{
    if (config == NULL) {
        return PARADOX_CONFIG_INVALID_ARGUMENT;
    }
    config->panel_debug_enabled = enabled;
    return PARADOX_CONFIG_OK;
}

paradox_config_result_t paradox_config_set_web_auth_hash(
    paradox_config_t *config, const uint8_t *hash, size_t hash_size)
{
    if (config == NULL || (hash == NULL && hash_size != 0)) {
        return PARADOX_CONFIG_INVALID_ARGUMENT;
    }
    if (hash_size != 0 && hash_size != sizeof(config->web_auth_hash)) {
        return PARADOX_CONFIG_INVALID_VALUE;
    }
    if (hash_size == 0) {
        memset(config->web_auth_hash, 0, sizeof(config->web_auth_hash));
    } else {
        memcpy(config->web_auth_hash, hash, hash_size);
    }
    return PARADOX_CONFIG_OK;
}

size_t paradox_config_blob_max_size(void)
{
    return CONFIG_V1_BLOB_SIZE;
}

bool paradox_config_encode(const paradox_config_t *config, uint8_t *blob,
                           size_t blob_capacity, size_t *blob_size)
{
    if (blob == NULL || blob_size == NULL ||
        paradox_config_validate(config) != PARADOX_CONFIG_OK ||
        blob_capacity < CONFIG_V1_BLOB_SIZE) {
        return false;
    }

    uint8_t *cursor = blob;
    *cursor++ = PARADOX_CONFIG_SCHEMA_VERSION;
    for (size_t i = 0; i < PARADOX_MAX_ZONES; i++) {
        const paradox_zone_config_t *zone = &config->zones[i];
        *cursor++ = zone->enabled ? 1u : 0u;
        *cursor++ = (uint8_t)zone->service;
        size_t name_length = strlen(zone->name);
        memset(cursor, 0, sizeof(zone->name));
        memcpy(cursor, zone->name, name_length + 1);
        cursor += sizeof(zone->name);
    }
    *cursor++ = config->panel_debug_enabled ? 1u : 0u;
    memcpy(cursor, config->web_auth_hash, sizeof(config->web_auth_hash));
    *blob_size = CONFIG_V1_BLOB_SIZE;
    return true;
}

static bool decode_zone_records(const uint8_t **cursor, const uint8_t *end,
                                paradox_config_t *config)
{
    for (size_t i = 0; i < PARADOX_MAX_ZONES; i++) {
        if ((size_t)(end - *cursor) < CONFIG_ZONE_RECORD_SIZE) {
            return false;
        }

        uint8_t enabled = *(*cursor)++;
        uint8_t service = *(*cursor)++;
        if (enabled > 1u || service > PARADOX_HK_SERVICE_LEAK) {
            return false;
        }
        paradox_zone_config_t *zone = &config->zones[i];
        zone->enabled = enabled != 0;
        zone->service = (paradox_hk_service_t)service;
        memcpy(zone->name, *cursor, sizeof(zone->name));
        *cursor += sizeof(zone->name);
    }
    return true;
}

bool paradox_config_decode(const uint8_t *blob, size_t blob_size,
                           paradox_config_t *config, bool *migrated)
{
    if (blob == NULL || config == NULL || migrated == NULL ||
        blob_size < CONFIG_BLOB_HEADER_SIZE) {
        return false;
    }

    paradox_config_defaults(config);
    *migrated = false;
    uint8_t version = blob[0];
    if ((version == 0u && blob_size != CONFIG_V0_BLOB_SIZE) ||
        (version == PARADOX_CONFIG_SCHEMA_VERSION &&
         blob_size != CONFIG_V1_BLOB_SIZE) ||
        (version != 0u && version != PARADOX_CONFIG_SCHEMA_VERSION)) {
        return false;
    }

    const uint8_t *cursor = blob + CONFIG_BLOB_HEADER_SIZE;
    const uint8_t *end = blob + blob_size;
    if (!decode_zone_records(&cursor, end, config) ||
        cursor == end || *cursor > 1u) {
        return false;
    }
    config->panel_debug_enabled = *cursor++ != 0;

    if (version == 0u) {
        *migrated = true;
    } else {
        if ((size_t)(end - cursor) != sizeof(config->web_auth_hash)) {
            return false;
        }
        memcpy(config->web_auth_hash, cursor, sizeof(config->web_auth_hash));
    }
    return paradox_config_validate(config) == PARADOX_CONFIG_OK;
}
