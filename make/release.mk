# ---------------------------------------------------------------------------------------------
# release.mk -- package and bundle a staged release, for one platform subtree of a release tree
# (a deployment's iotdata-deploy/<deployment>/release/<platform>).
#
# Lives here with the other shared rules rather than in the release tree: that tree holds built
# artifacts, and a Makefile is not one. Each platform subtree includes it from its own directory,
# which is what makes bin/, pkg/ and ota/ resolve without any of them being named here.
#
# A staged release in bin/ is five loose files. This wraps one into a single archive that unpacks
# to a DIRECTORY OF THE SAME NAME, so the far end can point esp32-tool straight at it:
#
#     make package RELEASE=relay-0.9.8
#     scp pkg/relay-0.9.8.tar.xz box:
#     ssh box 'tar xf relay-0.9.8.tar.xz'
#     esp32-tool deploy -b relay-0.9.8 -f relay-0.9.8
#
# That is the whole point of the bundle: one file over a mobile or wifi link, and a full cable
# flash at the other end with nothing to reassemble by hand.
#
# WHAT GOES IN comes from the manifest, not from a glob, so a release packages exactly the files it
# declares — a stray .bin left in bin/ by a half-finished copy cannot ride along unnoticed. An esp32
# release is the whole flash set; a linux release is one executable; the manifest says which.
#
# ARCHIVES ARE REPRODUCIBLE: sorted entries, no owner, and every file's mtime pinned to the
# release's own staged_utc, so packaging the same release twice gives identical bytes and a bundle
# can be checked against the tree it came from rather than taken on trust.
#
#     make list                      what is staged
#     make package RELEASE=<name>    all three transport formats
#     make txz|tgz|zip RELEASE=<name>  just one
#     make ota-target  RELEASE=<name> [OTA_FORM=delta OTA_BASE=0.9.8 OTA_CODEC=deflate OTA_FLAGS=downgrade-okay]
#     make ota-targets RELEASE=<name> [OTA_BASE=0.9.8]   the whole matrix
#     make clean                     remove pkg/ and ota/
#     make tools                     what this pipeline needs, and what is missing
#     make tools-install             build and install the ones we can
#
# OTA BLOBS ARE A DIFFERENT THING FROM THE BUNDLES ABOVE. A bundle is every partition, compressed for
# a cable flash at the far end. A blob is the app alone, with a kvr header asserting what it needs --
# one file, over the air, defined by iotdata-common/include/iotdata_node_content_ota.h.
# ---------------------------------------------------------------------------------------------

# COMPRESSION IS AT MAXIMUM, deliberately and not by much. On a ~250KB payload xz -9 and the -6
# default produce the same 135,144 bytes, because even -6's dictionary already dwarfs the input --
# -9 is set for the intent and because it is the one that wins if a release ever gets large.
# Measured and rejected: -e is 64 bytes WORSE on binary this dense, and -T0 buys nothing at this
# size while making the output depend on the packaging box's core count, which would cost the
# byte-identical property above for nothing. gzip and zip are already at their own maximum.
XZ      := xz -9
# GZIP    := gzip -n9
GZIP    := zopfli -i5000
ZIP     := zip -qrX9

PLATFORM := $(notdir $(CURDIR))
BIN     := bin
PKG     := pkg
RELEASE ?=

MANIFEST = $(BIN)/$(RELEASE).json
STAGE    = $(PKG)/.stage/$(RELEASE)
# the files the release declares, and the instant it was staged — both straight out of the manifest
# From iotdata.files, which BOTH platform shapes have -- flash_files is an esp32-only thing, and
# reading it here is what quietly broke packaging for a linux release.
FILES    = $$(python3 -c "import json;print(' '.join('$(BIN)/'+f for f in sorted(json.load(open('$(MANIFEST)'))['iotdata']['files'])))")
WANT     = $$(python3 -c "import json;print(len(json.load(open('$(MANIFEST)'))['iotdata']['files'])+1)")
STAMP    = $$(python3 -c "import json;print(json.load(open('$(MANIFEST)'))['iotdata']['staged_utc'])")

_REL_MK_DIR := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
OTA       := $(_REL_MK_DIR)/../tools/ota-bundle/ota-bundle
OTA_DIR   := ota
OTA_FORM  ?= full
OTA_CODEC ?= deflate
OTA_FLAGS ?=
OTA_BASE  ?=
# --base belongs only to a delta: the bundler refuses it on a full image rather than ignore it.
_OTA_ARGS  = -d . -r $(RELEASE) $(if $(OTA_FLAGS),--flags $(OTA_FLAGS))
_OTA_BASE  = $(if $(OTA_BASE),--base $(OTA_BASE))

.PHONY: help list package txz tgz zip tree clean ota-target ota-targets tools tools-install _check _tools_quiet _report

help:
	@# the leading comment block, however long it grows -- not a line range that drifts from it
	@sed -n '1,/^$$/{s/^# \?//; p;}' $(_REL_MK_DIR)/release.mk

# ONE SHELL, deliberately. `exit 0` on a recipe line of its own ends only THAT line's shell -- make
# runs the next line regardless, and the loop then iterates over the unexpanded glob and dies inside
# python. An empty bin/ is the normal state of a deployment's release tree on the day it is created,
# so the nothing-staged case is the one that has to read well.
list:
	@if ! ls $(BIN)/*.json >/dev/null 2>&1; then echo "nothing staged in $(BIN)/"; else \
	  for m in $(BIN)/*.json; do \
	    python3 -c "import json,sys;m=json.load(open(sys.argv[1]))['iotdata'];\
print('  %-28s %-10s %9d B  %s' % (m['name']+'-'+m['version'], m.get('platform') or m.get('chip','?'), m['ota']['size'] if m.get('ota') else 0, m['staged_utc']))" $$m; \
	  done; fi

# ---- the tools this pipeline needs ---------------------------------------
#
# WHY THIS IS HERE AND NOT IN A README. Packaging a release and bundling an OTA blob reach for six
# different programs, and until one is missing nobody thinks about them. The failure modes are not
# equal either: a missing `zopfli` costs a few bytes on one of three archive formats, while a missing
# `hdiffi` means every delta bundle fails -- and `ota-bundle` can only report that at the moment
# somebody wanted the blob. Asked up front, it is a list; discovered later, it is a hunt.
#
# hdiffi IS THE ODD ONE OUT. The rest come from a distro; it is built from source, because it is the
# generator for the patch format iotdata_node_content_ota.h's DELTA form is defined in and the device
# side of it is vendored at a known commit in iotdata-depend/hpatchlite. `tools-install` builds the
# matching generator so the two ends cannot drift -- see that directory's README.
HPATCH_REPO ?= https://github.com/sisong/HPatchLite
HPATCH_WORK ?= /tmp/hpatchlite-build
BINDIR      ?= /usr/local/bin

tools:
	@echo "release pipeline tools:"
	@if command -v python3 >/dev/null 2>&1; then echo "  ok      python3   $$(python3 --version 2>&1 | cut -d' ' -f2)"; \
	  else echo "  MISSING python3   everything: the manifest is read with it"; fi
	@if command -v node >/dev/null 2>&1; then echo "  ok      node      $$(node --version)"; \
	  else echo "  MISSING node      ota-target, ota-targets: ota-bundle is node"; fi
	@if command -v hdiffi >/dev/null 2>&1; then echo "  ok      hdiffi    $$(hdiffi 2>&1 | head -1 | awk '{print $$NF}')"; \
	  else echo "  MISSING hdiffi    DELTA bundles only  --  make tools-install"; fi
	@for t in tar xz zip; do \
	  if command -v $$t >/dev/null 2>&1; then echo "  ok      $$t"; else echo "  MISSING $$t       package, $$t archives"; fi; \
	done
	@if command -v zopfli >/dev/null 2>&1; then echo "  ok      zopfli"; \
	  else echo "  MISSING zopfli    tgz is smaller with it; gzip -n9 is the fallback"; fi

# Only hdiffi: the rest are a distro's business and guessing a package manager here would be worse
# than saying what is missing. Needs a C++ toolchain, cmake-free -- its own Makefile does the work.
tools-install:
	@command -v git >/dev/null 2>&1 || { echo "tools-install: needs git" >&2; exit 1; }
	@echo "tools-install: hdiffi + hpatchi from $(HPATCH_REPO)"
	@rm -rf "$(HPATCH_WORK)"
	@git clone --depth 1 --recursive -q "$(HPATCH_REPO)" "$(HPATCH_WORK)"
	@$(MAKE) -s -C "$(HPATCH_WORK)" -j4 >/dev/null
	@install -m 0755 "$(HPATCH_WORK)/hdiffi" "$(HPATCH_WORK)/hpatchi" "$(BINDIR)/"
	@rm -rf "$(HPATCH_WORK)"
	@echo "  installed $(BINDIR)/hdiffi $(BINDIR)/hpatchi  ($$(hdiffi 2>&1 | head -1))"
	@echo "  the vendored applier it must match: iotdata-depend/hpatchlite/README.md"

# A DELTA CANNOT BE CUT WITHOUT IT, so the one target that needs hdiffi says so itself rather than
# letting ota-bundle fail three steps later with a shell error.
_tools_quiet:
	@command -v hdiffi >/dev/null 2>&1 || { echo "ota: a delta needs hdiffi, which is not on PATH -- 'make tools' explains, 'make tools-install' fixes it" >&2; exit 1; }

_check:
	@test -n "$(RELEASE)" || { echo "package: set RELEASE=<name-version>, e.g. make package RELEASE=relay-0.9.8   ('make list' shows what is staged)" >&2; exit 1; }
	@test -f "$(MANIFEST)" || { echo "package: no release '$(RELEASE)' in $(BIN)/ — 'make list' shows what is staged" >&2; exit 1; }

# One tree, built once, archived as many ways as asked for.
tree: _check
	@rm -rf "$(STAGE)"; mkdir -p "$(STAGE)" "$(PKG)"
	@cp -p $(MANIFEST) $(FILES) "$(STAGE)/"
	@# A bundle missing a binary still tars, still transfers, and only fails at the far end with a
	@# board in hand -- so count what landed against what the manifest declared, here.
	@got=$$(ls -1 "$(STAGE)" | wc -l); want=$(WANT); 	 test "$$got" -eq "$$want" || { echo "package: staged $$got files, manifest declares $$want -- is $(BIN)/ complete?" >&2; exit 1; }
	@touch -d "$(STAMP)" "$(STAGE)"/* "$(STAGE)"

txz: tree
	@tar --sort=name --owner=0 --group=0 --numeric-owner -C $(PKG)/.stage -cf - $(RELEASE) | $(XZ) > $(PKG)/$(RELEASE).tar.xz
	@$(MAKE) --no-print-directory _report F=$(RELEASE).tar.xz

tgz: tree
	@tar --sort=name --owner=0 --group=0 --numeric-owner -C $(PKG)/.stage -cf - $(RELEASE) | $(GZIP) > $(PKG)/$(RELEASE).tar.gz
	@$(MAKE) --no-print-directory _report F=$(RELEASE).tar.gz

zip: tree
	@rm -f $(PKG)/$(RELEASE).zip
	@cd $(PKG)/.stage && $(ZIP) ../$(RELEASE).zip $(RELEASE)
	@$(MAKE) --no-print-directory _report F=$(RELEASE).zip

# sha256 beside each bundle: the manifest already proves the CONTENTS after unpacking, this catches
# a truncated transfer before anyone unpacks it.
_report:
	@cd $(PKG) && sha256sum $(F) > $(F).sha256
	@raw=$$(python3 -c "import json;m=json.load(open('$(MANIFEST)'))['iotdata'];print(sum(f['size'] for f in m['files'].values()))"); \
	 got=$$(stat -c %s $(PKG)/$(F)); \
	 echo "  $(F)  $$got B  ($$((100*$$got/$$raw))% of $$raw B raw)"

## The closing hint is platform-aware because the two ends differ: an esp32 bundle is flashed by a
## tool that reads the manifest, a linux bundle is a binary somebody installs. Printing the esp32
## line for a linux release would be telling you to do something that cannot work.
package: txz tgz zip
	@echo ""
	@echo "  at the far end:"
	@echo "    tar xf $(RELEASE).tar.xz   # or: unzip $(RELEASE).zip"
	@if python3 -c "import json,sys; sys.exit(0 if 'flash_files' in json.load(open('$(MANIFEST)')) else 1)"; then \
	   echo "    esp32-tool deploy -b $(RELEASE) -f $(RELEASE)"; \
	 else \
	   echo "    $(RELEASE)/$(RELEASE).app   # install it however this host installs things"; \
	 fi

## ---- ota: one blob, or the matrix ----------------------------------------
ota-target: _check $(if $(filter delta,$(OTA_FORM)),_tools_quiet)
	@$(OTA) $(_OTA_ARGS) --form $(OTA_FORM) --codec $(OTA_CODEC) $(if $(filter delta,$(OTA_FORM)),$(_OTA_BASE))

## full with and without compression always; the delta pair only when a base is named, because a
## delta against nothing is not a thing and a silent skip would hide the omission.
##
## delta+none IS EXPECTED TO BE USELESS, and is cut anyway. hdiffi emits a byte-level difference
## stream that is about the size of the image -- measured at 389300 bytes against 389266 for the
## whole uncompressed image, so the "delta" is LARGER than the thing it is a delta of. That is not a
## fault: FORM and CODEC are independent by design, the patch is the difference and the codec is what
## squeezes it, and the same pair with deflate is 639 bytes. It stays in the matrix because seeing
## both is what makes that split obvious; it is not a thing to deploy.
ota-targets: _check $(if $(OTA_BASE),_tools_quiet)
	@$(OTA) $(_OTA_ARGS) --form full --codec none
	@$(OTA) $(_OTA_ARGS) --form full --codec deflate
	@if [ -n "$(OTA_BASE)" ]; then \
	   $(OTA) $(_OTA_ARGS) --form delta --codec none $(_OTA_BASE); \
	   $(OTA) $(_OTA_ARGS) --form delta --codec deflate $(_OTA_BASE); \
	 else echo "  (no OTA_BASE given -- skipping the delta pair)"; fi

clean:
	@rm -rf $(PKG) $(OTA_DIR)
	@echo "removed $(PKG)/ and $(OTA_DIR)/"
