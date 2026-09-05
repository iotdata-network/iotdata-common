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

# Hostname we're building on
IOTDATA_HOST ?= $(shell hostname -s)

# Source root holding all the iotdata-* repos: explicit override, else <apex>/src.
IOTDATA_SRC ?= $(IOTDATA_APEX)/src
# Force absolute. A project may set IOTDATA_SRC=../.. (fine for native gcc -I from the project dir),
# but the esp32 CMake component build resolves relative include dirs from main/ (one level deeper)
# and would double them (…/iotdata-example/iotdata-example/include). Absolute paths avoid that and
# work for both. $(abspath) resolves against the invoking Makefile's directory.
IOTDATA_SRC := $(abspath $(IOTDATA_SRC))

# Repo roots — every iotdata repo is a sibling under $(IOTDATA_SRC).
IOTDATA_SRC_LIBRARY ?= $(IOTDATA_SRC)/iotdata-library
IOTDATA_SRC_DEPEND  ?= $(IOTDATA_SRC)/iotdata-depend
IOTDATA_SRC_COMMON  ?= $(IOTDATA_SRC)/iotdata-common
IOTDATA_SRC_DEVICE  ?= $(IOTDATA_SRC)/iotdata-device
IOTDATA_SRC_EXAMPLE ?= $(IOTDATA_SRC)/iotdata-example

# iotdata-example internal sub-paths.
#   IOTDATA_SRC_EXAMPLE_SIMULATOR -> simulator sources
# (the variant-suite header is now canonical at IOTDATA_VARIANT_HEADER, below.)
IOTDATA_SRC_EXAMPLE_SIMULATOR ?= $(IOTDATA_SRC_EXAMPLE)/simulator

# The iotdata library's own sources — list in a project's SOURCES for rebuild-on-change + format.
# (iotdata.c is #included by the unity-build apps, so this is a prerequisite list, not a link list.)
IOTDATA_SRC_LIBRARY_SOURCES ?= \
    $(IOTDATA_SRC_LIBRARY)/iotdata_mesh.h \
    $(IOTDATA_SRC_LIBRARY)/iotdata.h \
    $(IOTDATA_SRC_LIBRARY)/iotdata.c

# The variant-suite the apps compile against, as a FILE path. Injected as -DIOTDATA_VARIANT so the
# committed wrapper iotdata-common/include/iotdata_variant.h #includes it; override per project or
# per host to swap the variant set. No default is pulled in if unset -> the build breaks explicitly.
IOTDATA_VARIANT ?= $(IOTDATA_SRC_COMMON)/include/iotdata_variant_weather_station.h

# Shared native-build compiler flags. A Makefile uses these and may extend them, either
# inline (CFLAGS_COMMON = $(IOTDATA_CFLAGS_COMMON) -Wextra-local) or by appending after the
# include (IOTDATA_CFLAGS_COMMON += -Wextra-local). ?= so a project may pre-set to replace.
IOTDATA_CFLAGS_COMMON ?= \
    -Wfloat-conversion -Werror=float-conversion \
    -Wall -Wextra -Werror -Wpedantic \
    -Wstrict-prototypes -Wold-style-definition \
    -Wcast-align -Wcast-qual -Wconversion \
    -Wfloat-equal -Wformat=2 -Wformat-security \
    -Winit-self -Wjump-misses-init \
    -Wlogical-op -Wmissing-include-dirs \
    -Wnested-externs -Wpointer-arith \
    -Wredundant-decls -Wshadow \
    -Wstrict-overflow=2 -Wswitch-default \
    -Wunreachable-code -Wunused \
    -Wwrite-strings \
    -Wdouble-promotion \
    -Wnull-dereference \
    -Wduplicated-cond \
    -Wduplicated-branches \
    -Wrestrict \
    -Wstringop-overflow \
    -Wundef \
    -Wvla \
    -Wno-duplicated-branches
IOTDATA_CFLAGS_OPT ?= -O3

# Link deps of the iotdata library itself: cjson (JSON encode/decode) + m (float math). A Makefile
# appends its own, e.g. LIBS = $(IOTDATA_LIBS_COMMON) -lmosquitto -lpthread. LDFLAGS is a slot for
# shared linker options (none by default); extend locally the same way.
IOTDATA_LIBS_COMMON    ?= -lcjson -lm
IOTDATA_LDFLAGS_COMMON ?=
