##
## esp32-tool — install the single script onto a host that has boards plugged into it.
##
## The point of this tool is that such a host needs NO toolchain and NO repo: one python file, and
## two python packages it can get from its own distro. So installing is a copy, and the interesting
## part is the dependency check -- which reports per FEATURE, because the tool is useful long
## before everything is present. `monitor` needs pyserial; `flash` needs esptool; only `--pull`
## needs rsync/ssh. A host that will never flash does not need esptool, and should not be told it
## is broken for lacking it.
##
##   make check                     what is present, and the exact command to fix what is not
##   make deps                      install the missing python packages
##   make install                   copy to the bin directory (PREFIX=, BINDIR=, DESTDIR= all honoured)
##   make install-remote HOST=box   copy to a remote host and check its dependencies
##   make uninstall
##

TOOL    := esp32-tool
PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin
DESTDIR ?=
PYTHON  ?= python3
HOST    ?=

.PHONY: all help check deps install uninstall install-remote clean

all: help

help:
	@sed -n 's/^## \{0,1\}//p' $(firstword $(MAKEFILE_LIST))
	@echo "  target: $(DESTDIR)$(BINDIR)/$(TOOL)"
	@echo
	@$(MAKE) --no-print-directory check

# ----------------------------------------------------------------------------------------------

# Each line: is it there, what stops working without it, and what to type. Only python3 is fatal:
# without it nothing runs at all, whereas the rest degrade one subcommand at a time.
check:
	@ok=0; \
	if command -v $(PYTHON) >/dev/null 2>&1; then \
		echo "  ok      $(PYTHON)          $$($(PYTHON) --version 2>&1)"; \
	else \
		echo "  MISSING $(PYTHON)          everything -- install python3"; ok=1; \
	fi; \
	if $(PYTHON) -c "import serial" >/dev/null 2>&1; then \
		echo "  ok      pyserial         $$($(PYTHON) -c 'import serial; print(serial.__version__)' 2>/dev/null)"; \
	else \
		echo "  MISSING pyserial         monitor, command, deploy  --  make deps"; \
	fi; \
	if $(PYTHON) -m esptool version >/dev/null 2>&1; then \
		echo "  ok      esptool          $$($(PYTHON) -m esptool version 2>/dev/null | head -1)"; \
	elif command -v esptool.py >/dev/null 2>&1 || command -v esptool >/dev/null 2>&1; then \
		echo "  ok      esptool          (on PATH, not importable -- fine)"; \
	else \
		echo "  MISSING esptool          flash, deploy, diag       --  make deps"; \
	fi; \
	for t in rsync ssh; do \
		if command -v $$t >/dev/null 2>&1; then echo "  ok      $$t"; \
		else echo "  MISSING $$t             --pull only"; fi; \
	done; \
	if [ -x "$(DESTDIR)$(BINDIR)/$(TOOL)" ]; then echo "  ok      installed        $(DESTDIR)$(BINDIR)/$(TOOL)"; \
	else echo "  --      not installed   make install"; fi; \
	exit $$ok

# ----------------------------------------------------------------------------------------------

# Debian marks its python EXTERNALLY-MANAGED (PEP 668), so a plain `pip install` refuses rather
# than quietly fighting apt. The distro package is therefore tried first and is the better answer
# where it exists; pip --user is the fallback, and --break-system-packages only as a last resort
# and only after saying so.
deps:
	@need=""; \
	$(PYTHON) -c "import serial" >/dev/null 2>&1 || need="$$need pyserial"; \
	{ $(PYTHON) -m esptool version >/dev/null 2>&1 || command -v esptool.py >/dev/null 2>&1 || command -v esptool >/dev/null 2>&1; } || need="$$need esptool"; \
	if [ -z "$$need" ]; then echo "dependencies: nothing missing"; exit 0; fi; \
	echo "dependencies: installing:$$need"; \
	for pkg in $$need; do \
		case $$pkg in pyserial) apt_pkg=python3-serial ;; esptool) apt_pkg=esptool ;; esac; \
		if command -v apt-get >/dev/null 2>&1 && apt-cache show $$apt_pkg >/dev/null 2>&1; then \
			echo "  $$pkg: apt-get install -y $$apt_pkg"; \
			$(SUDO) apt-get install -y $$apt_pkg && continue; \
		fi; \
		echo "  $$pkg: $(PYTHON) -m pip install --user $$pkg"; \
		$(PYTHON) -m pip install --user $$pkg && continue; \
		echo "  $$pkg: pip refused (PEP 668 externally-managed?); retrying with --break-system-packages"; \
		$(PYTHON) -m pip install --user --break-system-packages $$pkg; \
	done; \
	$(MAKE) --no-print-directory check

SUDO := $(shell [ "$$(id -u)" = 0 ] || command -v sudo 2>/dev/null)

# ----------------------------------------------------------------------------------------------

install: $(TOOL)
	@install -d $(DESTDIR)$(BINDIR)
	@install -m 0755 $(TOOL) $(DESTDIR)$(BINDIR)/$(TOOL)
	@echo "installed $(DESTDIR)$(BINDIR)/$(TOOL)"
	@$(MAKE) --no-print-directory check

uninstall:
	@rm -f $(DESTDIR)$(BINDIR)/$(TOOL)
	@echo "removed $(DESTDIR)$(BINDIR)/$(TOOL)"

# The whole point: the board host gets one file. No repo, no checkout, no toolchain. The check runs
# THERE, because what matters is what that host is missing, not what this one has.
install-remote:
	@[ -n "$(HOST)" ] || { echo "usage: make install-remote HOST=[user@]host [BINDIR=/usr/local/bin]"; exit 1; }
	@echo "--> $(HOST):$(BINDIR)/$(TOOL)"
	@scp -q $(TOOL) $(HOST):/tmp/$(TOOL).new
	@ssh $(HOST) 'sudo install -d $(BINDIR) && sudo install -m 0755 /tmp/$(TOOL).new $(BINDIR)/$(TOOL) && rm -f /tmp/$(TOOL).new && $(BINDIR)/$(TOOL) --help >/dev/null 2>&1 && echo "  installed, runs"'
	@echo "--> checking dependencies on $(HOST)"
	@ssh $(HOST) 'command -v python3 >/dev/null && echo "  ok      python3" || echo "  MISSING python3"; \
	             python3 -c "import serial" 2>/dev/null && echo "  ok      pyserial" || echo "  MISSING pyserial   (monitor/command/deploy)"; \
	             { python3 -m esptool version >/dev/null 2>&1 || command -v esptool.py >/dev/null 2>&1 || command -v esptool >/dev/null 2>&1; } && echo "  ok      esptool" || echo "  MISSING esptool    (flash/deploy/diag)"'

clean:
	@rm -rf __pycache__
