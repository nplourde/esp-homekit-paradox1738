/*
 * M2: defaults, validation, stable config encoding and schema migration.
 */
#include <unity.h>

#include "unity_compat.h"
#include "paradox_config.h"

#include <stdio.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

void test_defaults_cover_all_zones(void)
{
    paradox_config_t config;
    paradox_config_defaults(&config);

    TEST_ASSERT_EQUAL_INT(PARADOX_CONFIG_OK, paradox_config_validate(&config));
    TEST_ASSERT_FALSE(config.panel_debug_enabled);
    for (int i = 0; i < PARADOX_MAX_ZONES; i++) {
        TEST_ASSERT_TRUE(config.zones[i].enabled);
        TEST_ASSERT_EQUAL_INT(PARADOX_HK_SERVICE_CONTACT,
                              config.zones[i].service);
        char expected[PARADOX_CONFIG_ZONE_NAME_SIZE];
        (void)snprintf(expected, sizeof(expected), "Zone %d", i + 1);
        TEST_ASSERT_EQUAL_STRING(expected, config.zones[i].name);
    }
}

void test_mutators_validate_service_and_name_values(void)
{
    paradox_config_t config;
    paradox_config_defaults(&config);

    TEST_ASSERT_EQUAL_INT(PARADOX_CONFIG_OK,
                          paradox_config_set_zone_service(
                              &config, 7, PARADOX_HK_SERVICE_MOTION));
    TEST_ASSERT_EQUAL_INT(PARADOX_CONFIG_INVALID_VALUE,
                          paradox_config_set_zone_service(
                              &config, 7, (paradox_hk_service_t)99));
    TEST_ASSERT_EQUAL_INT(PARADOX_CONFIG_OK,
                          paradox_config_set_zone_enabled(&config, 12, false));
    TEST_ASSERT_FALSE(config.zones[11].enabled);

    TEST_ASSERT_EQUAL_INT(PARADOX_CONFIG_NAME_EMPTY,
                          paradox_config_set_zone_name(&config, 3, ""));
    TEST_ASSERT_EQUAL_INT(PARADOX_CONFIG_INVALID_ARGUMENT,
                          paradox_config_set_zone_name(&config, 3, NULL));

    char too_long[PARADOX_CONFIG_ZONE_NAME_MAX + 2];
    memset(too_long, 'x', sizeof(too_long) - 1);
    too_long[sizeof(too_long) - 1] = '\0';
    TEST_ASSERT_EQUAL_INT(PARADOX_CONFIG_NAME_TOO_LONG,
                          paradox_config_set_zone_name(&config, 3, too_long));
    char max_length[PARADOX_CONFIG_ZONE_NAME_SIZE];
    memset(max_length, 'y', PARADOX_CONFIG_ZONE_NAME_MAX);
    max_length[PARADOX_CONFIG_ZONE_NAME_MAX] = '\0';
    TEST_ASSERT_EQUAL_INT(PARADOX_CONFIG_OK,
                          paradox_config_set_zone_name(&config, 3, max_length));
    TEST_ASSERT_EQUAL_UINT(PARADOX_CONFIG_ZONE_NAME_MAX,
                           strlen(config.zones[2].name));
    TEST_ASSERT_EQUAL_INT(PARADOX_CONFIG_OK,
                          paradox_config_set_zone_name(&config, 3, "Workshop"));
    TEST_ASSERT_EQUAL_STRING("Workshop", config.zones[2].name);
}

void test_config_encoder_round_trips_valid_config(void)
{
    paradox_config_t expected;
    paradox_config_defaults(&expected);
    TEST_ASSERT_EQUAL_INT(PARADOX_CONFIG_OK,
                          paradox_config_set_zone_name(&expected, 3, "Workshop"));
    TEST_ASSERT_EQUAL_INT(PARADOX_CONFIG_OK,
                          paradox_config_set_zone_service(
                              &expected, 7, PARADOX_HK_SERVICE_MOTION));
    TEST_ASSERT_EQUAL_INT(PARADOX_CONFIG_OK,
                          paradox_config_set_zone_enabled(&expected, 12, false));
    TEST_ASSERT_EQUAL_INT(PARADOX_CONFIG_OK,
                          paradox_config_set_panel_debug(&expected, true));

    uint8_t hash[PARADOX_CONFIG_WEB_AUTH_HASH_SIZE];
    memset(hash, 0x5a, sizeof(hash));
    TEST_ASSERT_EQUAL_INT(PARADOX_CONFIG_OK,
                          paradox_config_set_web_auth_hash(
                              &expected, hash, sizeof(hash)));

    uint8_t blob[PARADOX_CONFIG_BLOB_MAX_SIZE];
    size_t blob_size = 0;
    TEST_ASSERT_TRUE(paradox_config_encode(&expected, blob, sizeof(blob),
                                           &blob_size));
    TEST_ASSERT_EQUAL_SIZE(PARADOX_CONFIG_BLOB_MAX_SIZE, blob_size);

    paradox_config_t actual;
    bool migrated = true;
    TEST_ASSERT_TRUE(paradox_config_decode(blob, blob_size, &actual, &migrated));
    TEST_ASSERT_FALSE(migrated);
    TEST_ASSERT_EQUAL_INT(PARADOX_CONFIG_OK, paradox_config_validate(&actual));
    TEST_ASSERT_EQUAL_MEMORY(&expected, &actual, sizeof(expected));
}

void test_v0_config_migrates_and_keeps_existing_values(void)
{
    paradox_config_t expected;
    paradox_config_defaults(&expected);
    TEST_ASSERT_EQUAL_INT(PARADOX_CONFIG_OK,
                          paradox_config_set_zone_name(&expected, 3, "Workshop"));
    TEST_ASSERT_EQUAL_INT(PARADOX_CONFIG_OK,
                          paradox_config_set_zone_service(
                              &expected, 7, PARADOX_HK_SERVICE_MOTION));
    TEST_ASSERT_EQUAL_INT(PARADOX_CONFIG_OK,
                          paradox_config_set_zone_enabled(&expected, 12, false));
    TEST_ASSERT_EQUAL_INT(PARADOX_CONFIG_OK,
                          paradox_config_set_panel_debug(&expected, true));

    const size_t v0_size = PARADOX_CONFIG_BLOB_MAX_SIZE -
                          PARADOX_CONFIG_WEB_AUTH_HASH_SIZE;
    uint8_t old_blob[PARADOX_CONFIG_BLOB_MAX_SIZE];
    size_t offset = 0;
    old_blob[offset++] = 0;
    for (size_t i = 0; i < PARADOX_MAX_ZONES; i++) {
        const paradox_zone_config_t *zone = &expected.zones[i];
        old_blob[offset++] = zone->enabled ? 1u : 0u;
        old_blob[offset++] = (uint8_t)zone->service;
        memcpy(old_blob + offset, zone->name, sizeof(zone->name));
        offset += sizeof(zone->name);
    }
    old_blob[offset++] = expected.panel_debug_enabled ? 1u : 0u;
    TEST_ASSERT_EQUAL_SIZE(v0_size, offset);

    paradox_config_t actual;
    bool migrated = false;
    TEST_ASSERT_TRUE(paradox_config_decode(old_blob, v0_size, &actual, &migrated));
    TEST_ASSERT_TRUE(migrated);
    TEST_ASSERT_EQUAL_STRING("Workshop", actual.zones[2].name);
    TEST_ASSERT_EQUAL_INT(PARADOX_HK_SERVICE_MOTION, actual.zones[6].service);
    TEST_ASSERT_FALSE(actual.zones[11].enabled);
    TEST_ASSERT_TRUE(actual.panel_debug_enabled);
    for (size_t i = 0; i < sizeof(actual.web_auth_hash); i++) {
        TEST_ASSERT_EQUAL_UINT8(0, actual.web_auth_hash[i]);
    }
}

void test_decoder_rejects_bad_schema_and_invalid_values(void)
{
    paradox_config_t config;
    uint8_t blob[PARADOX_CONFIG_BLOB_MAX_SIZE] = {0};
    bool migrated = false;

    blob[0] = PARADOX_CONFIG_SCHEMA_VERSION + 1;
    TEST_ASSERT_FALSE(paradox_config_decode(
        blob, sizeof(blob), &config, &migrated));

    paradox_config_defaults(&config);
    TEST_ASSERT_EQUAL_INT(PARADOX_CONFIG_INVALID_VALUE,
                          paradox_config_set_zone_service(
                              &config, 2, (paradox_hk_service_t)99));
    config.zones[0].service = (paradox_hk_service_t)99;
    TEST_ASSERT_EQUAL_INT(PARADOX_CONFIG_INVALID_VALUE,
                          paradox_config_validate(&config));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_defaults_cover_all_zones);
    RUN_TEST(test_mutators_validate_service_and_name_values);
    RUN_TEST(test_config_encoder_round_trips_valid_config);
    RUN_TEST(test_v0_config_migrates_and_keeps_existing_values);
    RUN_TEST(test_decoder_rejects_bad_schema_and_invalid_values);
    return UNITY_END();
}
