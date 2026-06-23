# target-esp32.mk — shared ESP-IDF (idf.py) build rules for the esp32 projects.
#
# A project Makefile sets the variables below, then includes this file:
#   PLATFORM      target chip, e.g. esp32c3                 [required]
#   NAME          app name; TARGET = build/$(NAME).bin      [default: app]
#   SOURCES       files whose change should force a rebuild  [default: main/$(NAME).c]
#   DEVICE        serial port for flash / monitor           [default: /dev/ttyACM0]
#   BUILDER_DEFS  extra `idf.py -D...` cache entries         [optional]
#
# Source paths come from iotdata-common/make/config.mk — include that first so
# BUILDER_DEFS can reference the SRC_* variables.

BUILDER      ?= idf.py
NAME         ?= app
SOURCES      ?= main/$(NAME).c
DEVICE       ?= /dev/ttyACM0
BUILDER_DEFS ?=
BUILDER_CFGS ?= build/build.ninja
TARGET       ?= build/$(NAME).bin

.DEFAULT_GOAL := all
.PHONY: all upload debug monitor size setup clean fullclean format

all: $(TARGET)

$(TARGET): $(SOURCES) $(BUILDER_CFGS)
	$(BUILDER) $(BUILDER_DEFS) build

$(BUILDER_CFGS):
	$(BUILDER) set-target $(PLATFORM)

upload:
	$(BUILDER) flash -p $(DEVICE)
debug:
	$(BUILDER) flash -p $(DEVICE) monitor
monitor:
	$(BUILDER) -p $(DEVICE) monitor
size:
	$(BUILDER) size
setup:
	@echo "try . $(IDF_PATH)/export.sh"
clean:
	$(BUILDER) clean
fullclean:
	$(BUILDER) fullclean
format:
	clang-format -i $(SOURCES)
