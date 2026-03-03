# Provided for convenience; delegates immediately to cmake

BUILD ?= build

cmake_bool = $(if $(filter 1 on,$(1)),ON,$(if $(filter 0 off,$(1)),OFF,$(1)))

cmake_build := -DSAN=$(call cmake_bool,$(SAN)) \
	-DASAN=$(call cmake_bool,$(ASAN)) \
	-DUBSAN=$(call cmake_bool,$(UBSAN)) \
	-DTSAN=$(call cmake_bool,$(TSAN))

ifeq ($(V),1)
cmake_verbose := --verbose
endif

targets = cot-test cot-test-threads cot-test-io ctconsensus \
	rpcg-client rpcg-server

all:
	cmake -B $(BUILD) $(cmake_build)
	cmake --build $(BUILD) $(cmake_verbose)

clean:
	rm -rf $(BUILD) .cache

$(targets):
	cmake -B $(BUILD) $(cmake_build)
	cmake --build $(BUILD) --target $@ $(cmake_verbose)

$(targets:%=$(BUILD)/%): $(BUILD)/%:
	cmake -B $(BUILD) $(cmake_build)
	cmake --build $(BUILD) --target $* $(cmake_verbose)

test: cot-test cot-test-threads cot-test-io ctconsensus
	$(BUILD)/cot-test
	$(BUILD)/cot-test-threads
	$(BUILD)/cot-test-io
	$(BUILD)/ctconsensus -q -R 10000

.PHONY: all clean test $(targets) $(targets:%=$(BUILD)/%)
