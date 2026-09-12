##
## Host tests for the shared headers in iotdata-common/include.
##
## They live here because the headers belong to no single project: their consumers are ESP-IDF
## applications a host toolchain cannot build, so this is the only place they get compiled with
## warnings on and actually run.
##

IOTDATA_SRC          = ../..
IOTDATA_SRC_LIBRARY  = $(IOTDATA_SRC)/iotdata-library
IOTDATA_SRC_COMMON   = $(IOTDATA_SRC)/iotdata-common
IOTDATA_SRC_EXAMPLE  = $(IOTDATA_SRC)/iotdata-example

## a variant has to be named: the library includes no default by design
IOTDATA_VARIANT ?= $(abspath $(IOTDATA_SRC_COMMON)/include/iotdata_variant_weather_station.h)

TEST_CC     ?= gcc
TEST_CFLAGS ?= -std=c11 -Wall -Wextra -Werror -Wconversion -Wshadow \
               -I$(IOTDATA_SRC_LIBRARY) -I$(IOTDATA_SRC_COMMON)/include -I$(IOTDATA_SRC_EXAMPLE)/include \
               -DIOTDATA_VARIANT='"$(IOTDATA_VARIANT)"' -DIOTDATA_NO_JSON
TEST_DIR    ?= build-test

.PHONY: test test-clean

test:
	@mkdir -p $(TEST_DIR)
	@$(TEST_CC) $(TEST_CFLAGS) test_node_endpoint.c $(IOTDATA_SRC_LIBRARY)/iotdata.c -o $(TEST_DIR)/test_node_endpoint -lm
	@$(TEST_DIR)/test_node_endpoint
	@$(TEST_CC) $(TEST_CFLAGS) -DPLATFORM_LINUX -DIOTDATA_VERSION_APP='"testapp"' -DIOTDATA_VERSION_SEMVER='"1.2.3"' -DIOTDATA_VERSION_STAMP='"$(shell date -u +%Y%m%d%H%M)"' test_version.c -o $(TEST_DIR)/test_version -lm
	@$(TEST_DIR)/test_version
	@$(TEST_CC) $(TEST_CFLAGS) test_format.c -o $(TEST_DIR)/test_format
	@$(TEST_DIR)/test_format
	@$(TEST_CC) $(TEST_CFLAGS) test_module_buffers.c -o $(TEST_DIR)/test_module_buffers
	@$(TEST_DIR)/test_module_buffers
	@$(TEST_CC) $(TEST_CFLAGS) -D_GNU_SOURCE -pthread test_module_buffers_locked.c -o $(TEST_DIR)/test_module_buffers_locked
	@$(TEST_DIR)/test_module_buffers_locked

test-clean:
	@rm -rf $(TEST_DIR)
