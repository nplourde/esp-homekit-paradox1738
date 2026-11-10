#ifndef PARADOX_CONFIG_H
#define PARADOX_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "paradox_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PARADOX_CONFIG_SCHEMA_VERSION 1u
#define PARADOX_CONFIG_ZONE_NAME_MAX 64u
#define PARADOX_CONFIG_ZONE_NAME_SIZE (PARADOX_CONFIG_ZONE_NAME_MAX + 1u)
#define PARADOX_CONFIG_WEB_AUTH_HASH_SIZE 32u
#define PARADOX_CONFIG_BLOB_MAX_SIZE \
    (1u + PARADOX_MAX_ZONES * (1u + 1u + PARADOX_CONFIG_ZONE_NAME_SIZE) + \
     1u + PARADOX_CONFIG_WEB_AUTH_HASH_SIZE)

typedef enum {
    PARADOX_HK_SERVICE_CONTACT = 0,
    PARADOX_HK_SERVICE_MOTION,
    PARADOX_HK_SERVICE_SMOKE,
    PARADOX_HK_SERVICE_LEAK,
} paradox_hk_service_t;

typedef struct {
    bool enabled;
    paradox_hk_service_t service;
    char name[PARADOX_CONFIG_ZONE_NAME_SIZE];
} paradox_zone_config_t;

typedef struct {
    paradox_zone_config_t zones[PARADOX_MAX_ZONES];
    bool panel_debug_enabled;
    /* All zeroes means no web-auth hash has been configured. */
    uint8_t web_auth_hash[PARADOX_CONFIG_WEB_AUTH_HASH_SIZE];
} paradox_config_t;

typedef enum {
    PARADOX_CONFIG_OK = 0,
    PARADOX_CONFIG_INVALID_ARGUMENT,
    PARADOX_CONFIG_ZONE_OUT_OF_RANGE,
    PARADOX_CONFIG_INVALID_VALUE,
    PARADOX_CONFIG_NAME_EMPTY,
    PARADOX_CONFIG_NAME_TOO_LONG,
} paradox_config_result_t;

void paradox_config_defaults(paradox_config_t *config);
paradox_config_result_t paradox_config_validate(const paradox_config_t *config);
paradox_config_result_t paradox_config_set_zone_enabled(
    paradox_config_t *config, int zone_id, bool enabled);
paradox_config_result_t paradox_config_set_zone_service(
    paradox_config_t *config, int zone_id, paradox_hk_service_t service);
paradox_config_result_t paradox_config_set_zone_name(
    paradox_config_t *config, int zone_id, const char *name);
paradox_config_result_t paradox_config_set_panel_debug(
    paradox_config_t *config, bool enabled);
paradox_config_result_t paradox_config_set_web_auth_hash(
    paradox_config_t *config, const uint8_t *hash, size_t hash_size);

/* Stable byte encoding used by the NVS adapter and host-side codec tests. */
size_t paradox_config_blob_max_size(void);
bool paradox_config_encode(const paradox_config_t *config, uint8_t *blob,
                           size_t blob_capacity, size_t *blob_size);
bool paradox_config_decode(const uint8_t *blob, size_t blob_size,
                           paradox_config_t *config, bool *migrated);

#ifdef __cplusplus
}
#endif

#endif
