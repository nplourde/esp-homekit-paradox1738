# Top-level convenience targets. ESP-IDF's own build still happens through
# idf.py; this Makefile only chains the checks that gate each milestone.
#
#   make check        # host tests + analyzer self-test + both device builds (T0.2)
#   make check-fail   # prove a red test is reported loudly (T0.2)
#   make host-test    # host unit tests only
#   make build        # idf.py build only
#   make build-sim    # idf.py build with the recorded-panel simulator (T0.4)
#   make build-selftest # idf.py build with the link init/deinit self-test (T1.6)
#   make build-configtest # idf.py build with the NVS config self-test (M2)
#   make smoke        # reset + boot-log smoke test (T0.3); FLASH=1 to flash first
#   make soak         # 24 h soak logger over the web API (T8.4); HOST= required
#   make soak-selftest # soak logger's reboot detector, no device needed

APP          := apps/paradox1738
HOST_TEST_DIR := test/host
PORT         ?= /dev/ttyUSB0
HOST         ?=
INTERVAL     ?= 60
DURATION     ?= 86400
OUT          ?= soak.csv

# IDF_PATH must be exported for idf.py; point it at the local checkout.
export IDF_PATH ?= $(HOME)/Source/esp-idf

.PHONY: check check-fail host-test build build-sim build-selftest build-configtest flash monitor smoke smoke-selftest soak soak-selftest clean distclean

check: host-test smoke-selftest soak-selftest build build-sim
	@echo "check: PASS"

host-test:
	@$(MAKE) -C $(HOST_TEST_DIR) check

# Builds samples/deliberate_failure.c, which asserts 1 == 2. Must exit non-zero
# and print the failing file and line, otherwise the harness is lying to us.
check-fail:
	@$(MAKE) -C $(HOST_TEST_DIR) check-fail

build:
	@cd $(APP) && idf.py build

# T0.4: the same sources must also build with the recorded-panel simulator on.
# A separate SDKCONFIG is required: sdkconfig.defaults is only consulted when
# sdkconfig is (re)generated, so sharing one file would silently ignore the overlay.
build-sim:
	@cd $(APP) && idf.py -B build-sim \
		-D SDKCONFIG=sdkconfig.simulator \
		-D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.simulator.defaults" build

# T1.6: same sources, but app_main() runs paradox1738_link_selftest() at boot.
build-selftest:
	@cd $(APP) && idf.py -B build-selftest \
		-D SDKCONFIG=sdkconfig.selftest \
		-D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.selftest.defaults" build

# M2: isolated NVS namespace; verifies persistence and v0/corrupt fallbacks.
build-configtest:
	@cd $(APP) && idf.py -B build-configtest \
		-D SDKCONFIG=sdkconfig.configtest \
		-D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.configtest.defaults" build

flash:
	@cd $(APP) && idf.py -p $(PORT) flash

monitor:
	@cd $(APP) && idf.py -p $(PORT) monitor

# Hardware smoke test: resets the board and checks the boot log. Add FLASH=1 to
# build and flash first, which overwrites whatever firmware is currently running.
smoke:
	@test/device/smoke.sh --port $(PORT) $(if $(FLASH),--flash)

# The analyzer half of T0.3, runnable with no board attached.
smoke-selftest:
	@./test/device/smoke.sh --self-test

# T8.4: poll heap/stack/decoder counters on a schedule for a day and catch a
# silent reboot. Password comes from PARADOX_WEB_PASSWORD or a prompt, never a
# command-line argument (it would land in shell history and in `ps`).
#   make soak HOST=192.168.4.30
soak:
	@test -n "$(HOST)" || { echo "soak: HOST=<device address> is required"; exit 2; }
	@test/device/soak.sh --host "$(HOST)" --interval $(INTERVAL) \
		--duration $(DURATION) --out "$(OUT)"

# The reboot detector of the soak logger, runnable with no device attached.
soak-selftest:
	@./test/device/soak.sh --self-test

clean:
	@$(MAKE) -C $(HOST_TEST_DIR) clean

distclean: clean
	@rm -rf $(APP)/build $(APP)/managed_components $(APP)/dependencies.lock
