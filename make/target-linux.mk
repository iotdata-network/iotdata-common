# target-linux.mk — shared native (gcc) build rules for the linux example / tool projects.
#
# A project Makefile sets the variables below, then includes this file (after config.mk):
#   TARGET          output binary                                   [required]
#   MAIN            the single translation unit to compile          [required]
#   SOURCES         prerequisites (rebuild on change)               [required]
#   SOURCES_EXTRA   vendored .c files to compile and link in        [default: empty]
#   CFLAGS LDFLAGS LIBS   the compile + link flags                  [required]
#   CC              compiler                                        [default: gcc]
#   FORMAT          clang-format binary                   [default: clang-format-19, from config.mk]
#   SOURCES_TARGET  the project's OWN sources, for `format`         [default: empty -> deps not reformatted]
#   SOURCES_ALL     files `format` rewrites            [default: $(MAIN) [$(TEST_MAIN)] $(SOURCES_TARGET)]
#   TARGETS_ALL     files `clean` removes             [default: $(TARGET) [$(TEST_TARGET)]]
#
# Optional unit test (only when TEST_MAIN is set):
#   TEST_MAIN       the test translation unit
#   TEST_TARGET     test binary                                     [default: $(TARGET)_test]
#   TEST_LIBS       test link libs                                  [default: $(LIBS)]
#
# Optional apt dev-deps (only when PACKAGES_DEV is set):
#   PACKAGES_DEV              packages for install-dev / remove-dev (+ :armhf variants)
#   PACKAGES_DEV_CROSS_ARMHF  extra cross toolchain package         [default: gcc-arm-linux-gnueabihf]
#
# Optional systemd service install (only when INSTALL is set):
#   INSTALL         installed name (bin + /etc/default + <name>.service)
#   INSTALL_DIR_BIN/CFG/SRV   install dirs                          [defaults: /usr/local/bin, /etc/default, /etc/systemd/system]
#   CFG_SRC         the .cfg to install         [default: $(TARGET).$(IOTDATA_HOST).cfg if present, else $(TARGET).cfg]

CC     ?= gcc

# Version identity, the same three variables the esp32 rule uses (target-esp32.mk), so a project
# declares itself the same way whichever platform it targets. NAME is the name the node REPORTS --
# distinct from TARGET, the binary name. VERSION is the release line, hand-set at a release point.
# The stamp is regenerated on EVERY invocation, so a build cannot inherit an earlier one's; pass a
# fixed IOTDATA_NODE_VERSION_STAMP for a reproducible binary.
NAME           ?= $(TARGET)
VERSION        ?= 0.0.0
VERSION_STAMP  ?= $(shell date -u +%Y%m%d%H%M)
CFLAGS_DEFINES += -DIOTDATA_NODE_VERSION_APP='"$(NAME)"' -DIOTDATA_NODE_VERSION_SEMVER='"$(VERSION)"' -DIOTDATA_NODE_VERSION_STAMP='"$(VERSION_STAMP)"'

# Staging, the same shape the esp32 rule uses: the deployment's release tree, one subtree per
# platform, one manifest format. The root comes from $(IOTDATA_DEPLOYMENT) (config.mk), which has no
# default -- a release belongs to a site. See iotdata-platform/tools/ota-stage.
_TARGET_LINUX_MK_DIR := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
OTA_STAGE      ?= $(_TARGET_LINUX_MK_DIR)/../tools/ota-stage/ota-stage
STAGE_ROOT     ?= $(IOTDATA_DEPLOYMENT_RELEASE)
STAGE_ARGS     ?=
# `make stage STAGE_FORCE=1` -> --force: restage over a release already staged under this name and
# version. 0, no and false are off. Spelled the same as the esp32 rule's, and for the reason given
# there: FORCE alone is a build-side target name in that file, and one word meaning two things is
# not worth the keystrokes it saves.
STAGE_FORCE    ?=
_STAGE_FORCE    = $(if $(filter-out 0 no false,$(STAGE_FORCE)),--force)
# The oldest loader this build needs. Means little on linux today -- it is carried so an OTA blob
# from a linux build has the same fields as one from an esp32 build, and so an esp32 gateway later
# slots in without the format gaining a platform-conditional.
BOOTLOADER_MIN ?= 1

TEST_TARGET    ?= $(TARGET)_test
TEST_LIBS      ?= $(LIBS)
SOURCES_ALL    ?= $(MAIN) $(if $(TEST_MAIN),$(TEST_MAIN)) $(SOURCES_TARGET)
TARGETS_ALL    ?= $(TARGET) $(TARGET).buildinfo $(if $(TEST_MAIN),$(TEST_TARGET)) $(TARGET).d $(if $(TEST_MAIN),$(TEST_TARGET).d) \
                  $(SOURCES_EXTRA_OBJS) $(SOURCES_EXTRA_OBJS:.o=.d)

# WHAT WAS ACTUALLY INCLUDED, asked of the compiler rather than listed by hand.
#
# SOURCES is a hand-kept list and it had drifted: the gateway's own iotdata_gateway_conf.h was not
# in it, so editing the config table rebuilt NOTHING and `make test` then ran an old binary against
# new headers and passed. A test that goes green because it did not rebuild is worse than a failing
# one, and this is a header-only codebase -- almost every change is to a file no list mentions.
#
# -MMD writes the real prerequisites beside the binary; -MP adds a phony target for each header so
# that DELETING or renaming one is not a hard error the next time round. SOURCES stays as the
# first-build prerequisite and for `format`; from the second build on, the .d file is the truth.
CFLAGS_DEPEND  ?= -MMD -MP
-include $(TARGET).d
$(if $(TEST_MAIN),$(eval -include $(TEST_TARGET).d))

# --- SOURCES_EXTRA: vendored C that has to be COMPILED, not just included -----------------------
#
# Some seams are a header over a real library: d_module_hpatch.h is HPatchLite's two entry points,
# and those live in iotdata-depend/hpatchlite/hpatch_lite.c. A project naming that file here gets
# it built and linked. The esp32 side says the same thing in its own dialect -- the CMakeLists
# carry it in idf_component_register's SRCS -- so both platforms declare the dependency rather
# than one of them getting it by luck.
#
# AN OBJECT EACH, AND NOT TWO .c FILES ON THE LINK LINE, WHICH IS THE WHOLE POINT OF THIS BLOCK.
# `gcc -MMD -MF one.d -o bin main.c vendor.c` writes the dependencies of the LAST source only:
# the .d collapses to `bin: vendor.c` and every header the project actually includes vanishes out
# of it. That is precisely the drift the note above describes, re-introduced by the mechanism meant
# to be convenient -- `make test` would go green against stale headers again. Compiling each extra
# source to its own object with its own .d keeps $(MAIN) the only .c on its command line, so its
# depfile stays complete.
#
# THEY ARE BUILT WITH THE PROJECT'S FLAGS, deliberately: hpatch_lite.c passes the full -Werror
# -Wconversion -Wpedantic set unchanged, which is worth knowing about a vendored dependency. A
# future one that cannot take them needs its own rule and a note saying which flag it failed --
# not a blanket relaxation here that would quietly cover this file too.
#
# NOT in SOURCES_ALL, so `format` never rewrites them: they are somebody else's code, carried at a
# recorded upstream commit, and reformatting would make the next vendor bump a conflict.
SOURCES_EXTRA  ?=
SOURCES_EXTRA_OBJS = $(foreach _s,$(SOURCES_EXTRA),$(notdir $(basename $(_s))).o)

define _TARGET_LINUX_EXTRA_RULE
$(notdir $(basename $(1))).o: $(1) $$(MAKEFILE_LIST)
	$$(CC) $$(CFLAGS) $$(CFLAGS_DEPEND) -MF $(notdir $(basename $(1))).d -c $(1) -o $$@
-include $(notdir $(basename $(1))).d
endef
$(foreach _s,$(SOURCES_EXTRA),$(eval $(call _TARGET_LINUX_EXTRA_RULE,$(_s))))

.DEFAULT_GOAL := all
.PHONY: all clean format stage

all: $(TARGET)

## ---- stage: into the deployment's release tree, as <name>-<version> --------
## The platform subtree is derived from the binary's own ELF header, so a cross-built aarch64
## artefact cannot be staged into the x86_64 tree by a forgotten variable.
stage: $(TARGET)
	@$(IOTDATA_DEPLOYMENT_ASSERT)
	@test "$(VERSION)" != "0.0.0" || { echo "stage: VERSION is still the 0.0.0 default -- set it at the release point" >&2; exit 1; }
	@test -d "$(STAGE_ROOT)" || { echo "stage: $(STAGE_ROOT) not found -- deployment '$(IOTDATA_DEPLOYMENT)' has no release tree yet; mkdir it (and give each platform subtree the one-line Makefile that includes iotdata-platform/make/release.mk)" >&2; exit 1; }
	@test -z "$(FORCE)" || echo "stage: FORCE=$(FORCE) is not the restage switch -- you want STAGE_FORCE=1" >&2
	@test -z "$(_STAGE_FORCE)" || echo "stage: FORCING over $(NAME)-$(VERSION) -- re-cut anything in ota/ that came from it" >&2
	$(OTA_STAGE) -d $(STAGE_ROOT) -n $(NAME) --version $(VERSION) --app $(TARGET) --bootloader-min $(BOOTLOADER_MIN) $(STAGE_ARGS) $(_STAGE_FORCE)

# MAKEFILE_LIST is a prerequisite because the makefiles carry the compile flags -- NAME, VERSION and
# the rest -- and make has no other way to notice that one of them changed. Without it, bumping
# VERSION at a release point would leave the binary reporting the old one, which is exactly the
# kind of quiet lie the version report exists to prevent.
$(TARGET): $(MAIN) $(SOURCES) $(SOURCES_EXTRA_OBJS) $(MAKEFILE_LIST)
	$(CC) $(CFLAGS) $(CFLAGS_DEPEND) -MF $(TARGET).d -o $(TARGET) $(MAIN) $(SOURCES_EXTRA_OBJS) $(LDFLAGS) $(LIBS)
	@# THE STAMP MUST SURVIVE THE BUILD. VERSION_STAMP is a fresh `date -u` every invocation, so by
	@# the time anything stages this binary the value compiled into it is unrecoverable -- unless the
	@# build writes it down here. esp32 gets this free from CMakeCache.txt; linux has to say it.
	@printf 'name=%s\nversion=%s\nstamp=%s\n' '$(NAME)' '$(VERSION)' '$(VERSION_STAMP)' > $(TARGET).buildinfo

clean:
	rm -f $(TARGETS_ALL)
format:
	$(FORMAT) -i $(SOURCES_ALL)

# --- optional unit test -------------------------------------------------------------------------
ifdef TEST_MAIN
.PHONY: test
$(TEST_TARGET): $(TEST_MAIN) $(SOURCES) $(SOURCES_EXTRA_OBJS) $(MAKEFILE_LIST)
	$(CC) $(CFLAGS) $(CFLAGS_DEPEND) -MF $(TEST_TARGET).d -o $(TEST_TARGET) $(TEST_MAIN) $(SOURCES_EXTRA_OBJS) $(LDFLAGS) $(TEST_LIBS)
test: $(TEST_TARGET)
	./$(TEST_TARGET)
endif

# --- optional apt dev dependencies --------------------------------------------------------------
ifdef PACKAGES_DEV
PACKAGES_DEV_ARMHF       ?= $(addsuffix :armhf,$(PACKAGES_DEV))
PACKAGES_DEV_CROSS_ARMHF ?= gcc-arm-linux-gnueabihf
.PHONY: install-dev remove-dev install-dev-armhf remove-dev-armhf
install-dev:
	apt install -y $(PACKAGES_DEV)
remove-dev:
	apt purge -y $(PACKAGES_DEV)
install-dev-armhf:
	dpkg --add-architecture armhf
	apt update
	apt install -y $(PACKAGES_DEV_CROSS_ARMHF) $(PACKAGES_DEV_ARMHF)
remove-dev-armhf:
	apt purge -y $(PACKAGES_DEV_CROSS_ARMHF) $(PACKAGES_DEV_ARMHF)
	dpkg --remove-architecture armhf
	apt update
endif

# --- optional systemd service install -----------------------------------------------------------
ifdef INSTALL
INSTALL_DIR_BIN ?= /usr/local/bin
INSTALL_DIR_CFG ?= /etc/default
INSTALL_DIR_SRV ?= /etc/systemd/system
CFG_SRC         ?= $(if $(wildcard $(TARGET).$(IOTDATA_HOST).cfg),$(TARGET).$(IOTDATA_HOST).cfg,$(TARGET).cfg)
define install_service_systemd
	-systemctl stop $(2) 2>/dev/null || true
	-systemctl disable $(2) 2>/dev/null || true
	install -m 644 $(1).service $(INSTALL_DIR_SRV)/$(2).service
	systemctl daemon-reload
	systemctl enable $(2)
	systemctl start $(2) || echo "Warning: Failed to start $(2)"
endef
.PHONY: install install_target install_default install_service restart
install_target: $(TARGET)
	install -m 755 $(TARGET) $(INSTALL_DIR_BIN)/$(INSTALL)
install_default: $(CFG_SRC)
	@echo "installing config from $(CFG_SRC)"
	install -m 644 $(CFG_SRC) $(INSTALL_DIR_CFG)/$(INSTALL)
install_service: $(TARGET).service
	$(call install_service_systemd,$(TARGET),$(INSTALL))
install: install_target install_default install_service
restart:
	systemctl restart $(INSTALL)
endif
