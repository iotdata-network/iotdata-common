# target-esp32.mk — shared ESP-IDF (idf.py) build rules for the esp32 projects.
#
# A project Makefile sets the variables below, then includes this file:
#   PLATFORM      target chip, e.g. esp32c3                 [required]
#   NAME          the app's name, as it REPORTS itself       [default: app]
#   VERSION       release line, hand-set at a release point   [default: 0.0.0]
#   SOURCES       files whose change should force a rebuild   [default: main/app.c]
#   DEVICE        serial port for flash / monitor           [default: /dev/ttyACM0]
#   BUILDER_DEFS  extra `idf.py -D...` cache entries         [optional]
#
# `make release` builds a lean field / OTA image into $(RELEASE_DIR) (default
# build_release/): ESP_LOG compiled out and assertions silenced, so the format
# strings drop out of flash. Kept entirely separate from the verbose build/.
#
# Source paths come from iotdata-common/make/config.mk — include that first so
# BUILDER_DEFS can reference the SRC_* variables.

BUILDER      ?= idf.py
# NAME is the app's own name -- what it reports in VERSION, and what a human calls it. It is NOT
# the source path (every project's entry point is main/app.c) and NOT the binary name (that comes
# from project() in CMakeLists.txt, and those names are arbitrary: sensor_depth_snow,
# sensor_tsa_test). Conflating the three is what made this inconsistent.
NAME         ?= app
SOURCES      ?= main/app.c
DEVICE       ?= /dev/ttyACM0
# The stamp is regenerated on EVERY invocation, so a build can never inherit an earlier one's --
# and because it is a build INPUT rather than something the compiler invents, it stays compatible
# with CONFIG_APP_REPRODUCIBLE_BUILD, which leaves esp_app_desc_t.date/.time empty by design (pass
# a fixed IOTDATA_VERSION_STAMP for a reproducible binary).
VERSION      ?= 0.0.0
VERSION_STAMP ?= $(shell date -u +%Y%m%d%H%M)

BUILDER_DEFS ?=
BUILDER_CFGS ?= build/build.ninja
BUILDER_DEFS += \
    -DIOTDATA_VERSION_APP=$(NAME) \
    -DIOTDATA_VERSION_SEMVER=$(VERSION) \
    -DIOTDATA_VERSION_STAMP=$(VERSION_STAMP)

# The binary IDF actually writes, read out of CMakeLists.txt rather than guessed: project() names
# do not follow from NAME (sds is sensor_depth_snow), and a TARGET that names a file which never
# appears means `make` can never report itself up to date.
PROJECT      ?= $(shell sed -n 's/^project(\(.*\))/\1/p' CMakeLists.txt)
TARGET       ?= build/$(PROJECT).bin

# release image: the dev sdkconfig.defaults is the single source of truth; the release
# config is SYNTHESISED from it on the fly (see the `release` rule) into a separate dir.
SDKCONFIG_DEFAULTS ?= sdkconfig.defaults
RELEASE_DIR        ?= build_release
RELEASE_DEFAULTS   ?= $(RELEASE_DIR)/sdkconfig.defaults

# The common IDF config baseline lives beside this makefile. On first build a project's local
# sdkconfig.defaults is SEEDED from it (only when absent — never overwritten), with an optional
# per-host sdkconfig.$(IOTDATA_HOST).defaults appended. Delete the local copy to re-seed after
# the baseline changes. Keeps every esp32 project on one consistent config; host tweaks stay local.
_TARGET_ESP32_MK_DIR       := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
SDKCONFIG_COMMON           ?= $(_TARGET_ESP32_MK_DIR)/sdkconfig.defaults
# Optional TRACKED per-project overlay (committed in the project dir); layered ON TOP of the
# common baseline and BELOW the per-host file. For settings that are project-specific but not
# host-specific, e.g. a solar board's brownout level. Named to dodge the sdkconfig.*.defaults
# gitignore so it stays tracked.
SDKCONFIG_PROJECT_DEFAULTS ?= sdkconfig.defaults.project
# Optional LOCAL per-host overlay (gitignored); layered last so host tweaks win.
SDKCONFIG_HOST_DEFAULTS    ?= sdkconfig.$(IOTDATA_HOST).defaults

.DEFAULT_GOAL := all
.PHONY: all upload debug monitor size setup clean fullclean format release release-size release-upload

all: $(TARGET)

$(TARGET): $(SOURCES) $(BUILDER_CFGS)
	$(BUILDER) $(BUILDER_DEFS) build

$(BUILDER_CFGS): | $(SDKCONFIG_DEFAULTS)
	$(BUILDER) $(BUILDER_DEFS) set-target $(PLATFORM)

# Seed the local sdkconfig.defaults from the common baseline the first time only: a target with
# no prerequisites runs its recipe just when the file is missing, so an existing (possibly edited)
# local copy is left untouched. Appended after it: the optional per-host overrides. It is an
# ORDER-ONLY prerequisite of the configure step above, so it is created before `set-target` reads
# it, but its mtime never triggers a spurious reconfigure.
$(SDKCONFIG_DEFAULTS):
	@test -f "$(SDKCONFIG_COMMON)" || { echo "target-esp32: missing common baseline $(SDKCONFIG_COMMON)"; exit 1; }
	@cp "$(SDKCONFIG_COMMON)" "$@"
	@src="common baseline"; \
	if [ -f "$(SDKCONFIG_PROJECT_DEFAULTS)" ]; then \
	    printf '\n# --- project overrides appended from %s ---\n' "$(SDKCONFIG_PROJECT_DEFAULTS)" >> "$@"; \
	    cat "$(SDKCONFIG_PROJECT_DEFAULTS)" >> "$@"; \
	    src="$$src + $(SDKCONFIG_PROJECT_DEFAULTS)"; \
	fi; \
	if [ -f "$(SDKCONFIG_HOST_DEFAULTS)" ]; then \
	    printf '\n# --- host overrides appended from %s ---\n' "$(SDKCONFIG_HOST_DEFAULTS)" >> "$@"; \
	    cat "$(SDKCONFIG_HOST_DEFAULTS)" >> "$@"; \
	    src="$$src + $(SDKCONFIG_HOST_DEFAULTS)"; \
	fi; \
	echo "target-esp32: seeded $@ from $$src"

# Lean field / OTA image, into a SEPARATE $(RELEASE_DIR) so it never touches build/.
# ESP_LOG is stripped at COMPILE time (drops the format strings from flash — ~18%
# smaller in practice) and assertions are silenced. The release sdkconfig.defaults is
# SYNTHESISED from $(SDKCONFIG_DEFAULTS): the log / assertion / target lines are removed
# and re-set. It is a full derivation, NOT a layered overlay, because a layered overlay
# cannot flip the LOG_MAXIMUM_LEVEL choice (an ESP-IDF quirk) and that is the switch that
# actually removes the strings. Regenerated every run; the source of truth stays the one
# committed $(SDKCONFIG_DEFAULTS). Version stamping is a later TODO.
release: | $(SDKCONFIG_DEFAULTS)
	@test -f $(SDKCONFIG_DEFAULTS) || { echo "release: no $(SDKCONFIG_DEFAULTS) to derive from"; exit 1; }
	@mkdir -p $(RELEASE_DIR)
	@grep -vE '^CONFIG_(IDF_TARGET|LOG_MAXIMUM_LEVEL|LOG_DEFAULT_LEVEL|COMPILER_OPTIMIZATION_ASSERTION)' $(SDKCONFIG_DEFAULTS) > $(RELEASE_DEFAULTS)
	@printf '\n# --- release overrides (generated by make release; do not edit) ---\n' >> $(RELEASE_DEFAULTS)
	@printf 'CONFIG_IDF_TARGET="$(PLATFORM)"\n'                     >> $(RELEASE_DEFAULTS)
	@printf 'CONFIG_LOG_DEFAULT_LEVEL_NONE=y\n'                     >> $(RELEASE_DEFAULTS)
	@printf 'CONFIG_LOG_MAXIMUM_LEVEL_NONE=y\n'                     >> $(RELEASE_DEFAULTS)
	@printf 'CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_SILENT=y\n'    >> $(RELEASE_DEFAULTS)
	$(BUILDER) -B $(RELEASE_DIR) -D SDKCONFIG=$(RELEASE_DIR)/sdkconfig -D SDKCONFIG_DEFAULTS=$(RELEASE_DEFAULTS) $(BUILDER_DEFS) build
	@echo ""
	@echo "release image: $$(ls $(RELEASE_DIR)/*.bin 2>/dev/null)"
release-size:
	$(BUILDER) -B $(RELEASE_DIR) size
release-upload:
	$(BUILDER) -B $(RELEASE_DIR) flash -p $(DEVICE)

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
	rm -rf $(RELEASE_DIR)
	rm -f $(SDKCONFIG_DEFAULTS) sdkconfig sdkconfig.old
format:
	clang-format-19 -i $(SOURCES_TARGET)
