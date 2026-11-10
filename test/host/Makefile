# Host (PC) unit tests for the pure logic of the project.
#
#   make -C test/host            # build every test binary
#   make -C test/host check      # build and run them all
#   make -C test/host check TEST=paradox_frame   # one suite
#
# Tests must not include any ESP-IDF header: anything that needs the RTOS,
# UART driver or NVS belongs in a device test (test/device) instead.

IDF_PATH ?= $(HOME)/Source/esp-idf
UNITY_DIR := $(IDF_PATH)/components/unity/unity/src

ifeq ($(wildcard $(UNITY_DIR)/unity.c),)
$(error Unity not found in $(UNITY_DIR). Set IDF_PATH to your ESP-IDF checkout)
endif

# Sanitizers are opt-in because they need libasan/libubsan to be installed. On
# rpm-ostree images (Fedora Atomic) the system gcc ships the flags but not the
# runtime libraries; point CC at a toolchain that has them, e.g.
#   make -C test/host check SAN=1 CC=gcc-16
#   make -C test/host check SAN=thread CC=gcc-16
SAN ?= 0
ifeq ($(SAN),1)
  SAN_FLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer
else ifeq ($(SAN),thread)
  SAN_FLAGS := -fsanitize=thread -fno-omit-frame-pointer
else
  SAN_FLAGS :=
endif

CC ?= gcc
APP_MAIN_DIR := ../../apps/paradox1738/main
CFLAGS := -std=gnu17 -g -O1 -Wall -Wextra -Werror -Wno-unused-parameter \
          -Iinclude -I$(APP_MAIN_DIR) -I$(UNITY_DIR) -pthread $(SAN_FLAGS)
LDFLAGS := -pthread $(SAN_FLAGS)

BUILD := build

# Unity is vendored in ESP-IDF; we compile it from source rather than relying
# on a system package so the version matches what the device tests use.
UNITY_SRCS := $(UNITY_DIR)/unity.c
UNITY_OBJS := $(patsubst $(UNITY_DIR)/%.c,$(BUILD)/unity/%.o,$(UNITY_SRCS))

SRC_SRCS := $(wildcard src/*.c)
SRC_OBJS := $(patsubst src/%.c,$(BUILD)/src/%.o,$(SRC_SRCS))

# Pure logic lives in the firmware tree so host tests exercise the exact code
# that ships. paradox_proto.c must stay free of ESP-IDF headers (checked below).
PURE_SRCS := $(APP_MAIN_DIR)/paradox_proto.c $(APP_MAIN_DIR)/paradox_config.c \
             $(APP_MAIN_DIR)/paradox_state.c
PURE_OBJS := $(patsubst $(APP_MAIN_DIR)/%.c,$(BUILD)/pure/%.o,$(PURE_SRCS))

# The recorded-panel table is firmware data, but paradox_sim.c only includes
# paradox1738.h, so the host suite can validate it too (test_simframes.c).
SIM_CFLAGS := $(CFLAGS) -DCONFIG_PARADOX_SIMULATOR
SIM_SRCS := $(APP_MAIN_DIR)/paradox_sim.c
SIM_OBJS := $(patsubst $(APP_MAIN_DIR)/%.c,$(BUILD)/sim/%.o,$(SIM_SRCS))

TEST_SRCS := $(wildcard test_*.c)
TEST_BINS := $(patsubst test_%.c,$(BUILD)/%,$(TEST_SRCS))

# Not matched by the test_*.c glob, so it never runs in `check`. `check-fail`
# builds it to prove a red run is reported loudly rather than silently ignored.
FAIL_SAMPLE := $(BUILD)/_fail

.PHONY: all check check-fail clean
all: $(TEST_BINS)

# Every suite provides its own main(), so each test file is its own binary.
$(BUILD)/%: test_%.c $(UNITY_OBJS) $(SRC_OBJS) $(PURE_OBJS) $(SIM_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) $^ -o $@ $(LDFLAGS)

# The simulator table is only declared under CONFIG_PARADOX_SIMULATOR, so the
# suite that reads it needs the same define as paradox_sim.o.
$(BUILD)/simframes: EXTRA_CFLAGS := -DCONFIG_PARADOX_SIMULATOR

$(BUILD)/unity/%.o: $(UNITY_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) -std=gnu17 -g -O1 -I$(UNITY_DIR) -c $< -o $@

$(BUILD)/pure/%.o: $(APP_MAIN_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/sim/%.o: $(APP_MAIN_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(SIM_CFLAGS) -MMD -MP -c $< -o $@

$(FAIL_SAMPLE): samples/deliberate_failure.c $(UNITY_OBJS)
	@mkdir -p $(dir $@)
	$(CC) -std=gnu17 -g -O1 -I$(UNITY_DIR) $^ -o $@

$(BUILD)/src/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

ifneq ($(MAKECMDGOALS),clean)
-include $(BUILD)/src/*.d $(BUILD)/pure/*.d $(BUILD)/sim/*.d
endif

# T1.5 (static half): the raw panel buffer is 4 bytes with no terminator, so no
# conversion may treat it as a C string. Every %s in the UART sources must be
# fed by one of the name() helpers (which return literals), never by the buffer.
# The helper argument usually sits on the continuation line, so each %s is
# checked together with the two lines that follow it.
UART_SRCS := $(APP_MAIN_DIR)/paradox1738.c $(APP_MAIN_DIR)/paradox_proto.c \
             $(APP_MAIN_DIR)/paradox_sim.c

uart-log-check:
	@bad=""; for f in $(UART_SRCS); do \
		bad="$$bad$$(awk '{ L[NR] = $$0 } \
			END { for (i = 1; i <= NR; i++) { \
				if (L[i] !~ /%s/) continue; \
				if (L[i] ~ /^[[:space:]]*(\*|\/\/|\/\*)/) continue; \
				stmt = L[i] " " L[i+1] " " L[i+2]; \
				if (stmt !~ /paradox_(event|class|category)_name/) \
					printf "%s:%d: %s\n", FILENAME, i, L[i] } }' $$f)"; \
	done; \
	if [ -n "$$bad" ]; then \
		echo "uart-log-check: FAIL - %s not fed by a name() helper (T1.5):"; \
		printf '%s\n' "$$bad"; exit 1; \
	fi; \
	echo "uart-log-check: PASS (no %s applied to the raw panel buffer)"

# Unity exits with the number of failed assertions, so the exit status gates.
check: all uart-log-check
	@status=0; \
	for t in $(if $(TEST),$(BUILD)/$(TEST),$(TEST_BINS)); do \
		echo "== $$t"; \
		$$t || status=1; \
	done; \
	if [ $$status -eq 0 ]; then echo "host tests: PASS"; else echo "host tests: FAIL"; fi; \
	exit $$status

# T0.2: a red test must be loud. Fails this target if the sample passes, if it
# exits zero, or if its output does not name the file and line of the assert.
check-fail: $(FAIL_SAMPLE)
	@out=$$($(FAIL_SAMPLE) 2>&1); status=$$?; \
	echo "$$out"; \
	if [ $$status -eq 0 ]; then echo "check-fail: FAIL - deliberate failure exited 0"; exit 1; fi; \
	if ! echo "$$out" | grep -q 'deliberate_failure.c:[0-9]\+[:*]'; then \
		echo "check-fail: FAIL - output does not name file and line"; exit 1; fi; \
	echo "check-fail: PASS (harness reports failures with location, exit $$status)"

clean:
	rm -rf $(BUILD)
