#ifndef PARADOX_CONFIG_H
#define PARADOX_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "paradox_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PARADOX_CONFIG_SCHEMA_VERSION 2u
#define PARADOX_CONFIG_ZONE_NAME_MAX 64u
#define PARADOX_CONFIG_ZONE_NAME_SIZE (PARADOX_CONFIG_ZONE_NAME_MAX + 1u)
#define PARADOX_CONFIG_WEB_AUTH_SALT_SIZE 16u
#define PARADOX_CONFIG_WEB_AUTH_HASH_SIZE 32u
#define PARADOX_CONFIG_BLOB_MAX_SIZE \
    (1u + PARADOX_MAX_ZONES * (1u + 1u + PARADOX_CONFIG_ZONE_NAME_SIZE) + \
     1u + PARADOX_CONFIG_WEB_AUTH_SALT_SIZE + \
     PARADOX_CONFIG_WEB_AUTH_HASH_SIZE + 1u)

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
    /* A false password_set flag means the hash is for a temporary bootstrap password. */
    uint8_t web_auth_salt[PARADOX_CONFIG_WEB_AUTH_SALT_SIZE];
    uint8_t web_auth_hash[PARADOX_CONFIG_WEB_AUTH_HASH_SIZE];
    bool web_auth_password_set;
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

/* Request-shape rules, kept out of the HTTP layer so they can be tested
 * without esp_http_server. These decide what a zone edit may say; whether the
 * edit is then stored is paradox_config_update_zone's job, and it applies a
 * multi-field edit to a candidate copy so a rejected field leaves the stored
 * configuration untouched. */

/* Digits-only zone index as it appears in /api/zones/{n}: ASCII digits, no
 * sign, no whitespace, no leading zeros except a bare "0", and 1-16. Stricter
 * than strtol(), which would also accept " 12" and "+12". */
bool paradox_config_zone_index_from_text(const char *text, int *zone_id);

/* The HomeKit service name used by GET /api/zones; NULL for an out-of-range
 * service value, which never comes out of a decoded config. */
const char *paradox_config_service_name(paradox_hk_service_t service);

/* The exact-lowercase service name a PUT may send, or false to reject. */
bool paradox_config_service_from_name(const char *name,
                                      paradox_hk_service_t *service);

/* Field names a zone-edit body may contain: name, type, enabled. */
bool paradox_config_zone_field_is_known(const char *field);
paradox_config_result_t paradox_config_update_zone(
    paradox_config_t *config, int zone_id, const char *name, bool update_name,
    int service, bool update_service, bool enabled, bool update_enabled);
paradox_config_result_t paradox_config_set_zone_service(
    paradox_config_t *config, int zone_id, paradox_hk_service_t service);
paradox_config_result_t paradox_config_set_zone_name(
    paradox_config_t *config, int zone_id, const char *name);
paradox_config_result_t paradox_config_set_panel_debug(
    paradox_config_t *config, bool enabled);
paradox_config_result_t paradox_config_set_web_auth(
    paradox_config_t *config, const uint8_t *salt, size_t salt_size,
    const uint8_t *hash, size_t hash_size, bool password_set);

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
