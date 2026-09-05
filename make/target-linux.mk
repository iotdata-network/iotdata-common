# target-linux.mk — shared native (gcc) build rules for the linux example / tool projects.
#
# A project Makefile sets the variables below, then includes this file (after config.mk):
#   TARGET          output binary                                   [required]
#   MAIN            the single translation unit to compile          [required]
#   SOURCES         prerequisites (rebuild on change)               [required]
#   CFLAGS LDFLAGS LIBS   the compile + link flags                  [required]
#   CC              compiler                                        [default: gcc]
#   FORMAT          clang-format binary                            [default: clang-format-19]
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
FORMAT ?= clang-format-19

TEST_TARGET    ?= $(TARGET)_test
TEST_LIBS      ?= $(LIBS)
SOURCES_ALL    ?= $(MAIN) $(if $(TEST_MAIN),$(TEST_MAIN)) $(SOURCES_TARGET)
TARGETS_ALL    ?= $(TARGET) $(if $(TEST_MAIN),$(TEST_TARGET))

.DEFAULT_GOAL := all
.PHONY: all clean format

all: $(TARGET)

$(TARGET): $(MAIN) $(SOURCES)
	$(CC) $(CFLAGS) -o $(TARGET) $(MAIN) $(LDFLAGS) $(LIBS)

clean:
	rm -f $(TARGETS_ALL)
format:
	$(FORMAT) -i $(SOURCES_ALL)

# --- optional unit test -------------------------------------------------------------------------
ifdef TEST_MAIN
.PHONY: test
$(TEST_TARGET): $(TEST_MAIN) $(SOURCES)
	$(CC) $(CFLAGS) -o $(TEST_TARGET) $(TEST_MAIN) $(LDFLAGS) $(TEST_LIBS)
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
