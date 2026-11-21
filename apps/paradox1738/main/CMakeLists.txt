idf_component_register(SRCS ./app_main.c ./web_server.c
                            ./paradox1738.c ./paradox_proto.c
                            ./paradox_config.c ./paradox_config_store.c
                            ./paradox_state.c ./paradox_event_log.c
                            ./paradox_raw_tap.c
                            ./paradox_json_guard.c ./paradox_same_origin.c ./paradox_audit.c
                            ./paradox_sim.c
                       INCLUDE_DIRS "."
                       REQUIRES app_hap_setup_payload app_wifi button driver
                                app_update esp_app_format esp_hap_apple_profiles
                                esp_hap_core
                                esp_http_server esp_system esp_timer
                                esp_wifi json mbedtls nvs_flash spi_flash)
