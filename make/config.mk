# config.mk — shared build configuration for all iotdata repos.
#
# Single source of truth for where the iotdata sources live. Included by each
# repo's Makefiles (and, for esp32 builds, pushed through to CMake) via a sibling-
# relative path, e.g.:
#   iotdata-example/Makefile                 -> include ../iotdata-common/make/config.mk
#   iotdata-example/simulator/Makefile       -> include ../../iotdata-common/make/config.mk
#   iotdata-device/tsa/test_esp32c3/Makefile -> include ../../../iotdata-common/make/config.mk
#
# Resolution order for the source root:
#   1. $(IOTDATA_SRC)        — explicit override (environment or `make IOTDATA_SRC=...`)
#   2. $(IOTDATA_APEX)/src
# IOTDATA_APEX defaults to this tree's apex, derived from THIS file's own location
# (it lives at $(IOTDATA_APEX)/src/iotdata-common/make), so everything self-locates
# with no hardcoded paths. Set IOTDATA_APEX or IOTDATA_SRC (env or command line) to
# relocate.
#
# NB: keep comments on their own lines — a trailing "# ..." after a value would
# leave the spaces before the # in the variable, corrupting the path.

# Directory of this file (.../iotdata-common/make), captured at include time.
_IOTDATA_MK_DIR := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))

# Apex of the iotdata tree (three levels up: make -> iotdata-common -> src -> apex).
IOTDATA_APEX ?= $(abspath $(_IOTDATA_MK_DIR)/../../..)

# Source root holding all the iotdata-* repos: explicit override, else <apex>/src.
IOTDATA_SRC ?= $(IOTDATA_APEX)/src

# Repo roots — every iotdata repo is a sibling under $(IOTDATA_SRC).
IOTDATA_SRC_LIBRARY ?= $(IOTDATA_SRC)/iotdata-library
IOTDATA_SRC_DEPEND  ?= $(IOTDATA_SRC)/iotdata-depend
IOTDATA_SRC_COMMON  ?= $(IOTDATA_SRC)/iotdata-common
IOTDATA_SRC_DEVICE  ?= $(IOTDATA_SRC)/iotdata-device
IOTDATA_SRC_EXAMPLE ?= $(IOTDATA_SRC)/iotdata-example

# iotdata-example internal sub-paths.
#   IOTDATA_SRC_EXAMPLE_COMMON    -> iotdata_variant_suite.h
#   IOTDATA_SRC_EXAMPLE_SIMULATOR -> simulator sources
IOTDATA_SRC_EXAMPLE_COMMON    ?= $(IOTDATA_SRC_EXAMPLE)/iotdata
IOTDATA_SRC_EXAMPLE_SIMULATOR ?= $(IOTDATA_SRC_EXAMPLE)/simulator
