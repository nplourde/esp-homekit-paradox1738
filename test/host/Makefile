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

# Sanitizers are opt-in because they need libasan/libubsan to be installed.
SAN ?= 0
ifeq ($(SAN),1)
  SAN_FLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer
else
  SAN_FLAGS :=
endif

CC ?= gcc
CFLAGS := -std=gnu17 -g -O1 -Wall -Wextra -Werror -Wno-unused-parameter \
          -Iinclude -I$(UNITY_DIR) $(SAN_FLAGS)
LDFLAGS := $(SAN_FLAGS)

BUILD := build

# Unity is vendored in ESP-IDF; we compile it from source rather than relying
# on a system package so the version matches what the device tests use.
UNITY_SRCS := $(UNITY_DIR)/unity.c
UNITY_OBJS := $(patsubst $(UNITY_DIR)/%.c,$(BUILD)/unity/%.o,$(UNITY_SRCS))

SRC_SRCS := $(wildcard src/*.c)
SRC_OBJS := $(patsubst src/%.c,$(BUILD)/src/%.o,$(SRC_SRCS))

TEST_SRCS := $(wildcard test_*.c)
TEST_BINS := $(patsubst test_%.c,$(BUILD)/%,$(TEST_SRCS))

# Not matched by the test_*.c glob, so it never runs in `check`. `check-fail`
# builds it to prove a red run is reported loudly rather than silently ignored.
FAIL_SAMPLE := $(BUILD)/_fail

.PHONY: all check check-fail clean
all: $(TEST_BINS)

# Every suite provides its own main(), so each test file is its own binary.
$(BUILD)/%: test_%.c $(UNITY_OBJS) $(SRC_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BUILD)/unity/%.o: $(UNITY_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) -std=gnu17 -g -O1 -I$(UNITY_DIR) -c $< -o $@

$(FAIL_SAMPLE): samples/deliberate_failure.c $(UNITY_OBJS)
	@mkdir -p $(dir $@)
	$(CC) -std=gnu17 -g -O1 -I$(UNITY_DIR) $^ -o $@

$(BUILD)/src/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

ifneq ($(MAKECMDGOALS),clean)
-include $(BUILD)/src/*.d
endif

# Unity exits with the number of failed assertions, so the exit status gates.
check: all
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
