# iTouch x86_64 / K93OS-x86 - build system
# Toolchain: musl-gcc -static for everything that runs in the guest.
# zlib + mbedTLS are vendored (built by scripts/build-deps.sh) and linked static.

TOP     := $(CURDIR)
BUILD   := $(TOP)/build
GBIN    := $(BUILD)/bin
GDEV    := $(BUILD)/dev
GEN     := $(BUILD)/gen
FIXT    := $(TOP)/fixtures
UAPI    := $(BUILD)/uapi

CC      := musl-gcc
CFLAGS  := -static -Os -g -Wall -Wextra -Wno-unused-parameter -I$(TOP)/userspace/include -I$(UAPI)
LDFLAGS := -static

ZDIR    := $(TOP)/third_party/_build/zlib
MDIR    := $(TOP)/third_party/_build/mbedtls
QDIR    := $(TOP)/third_party/_build/quickjs
QJSA    := $(QDIR)/libquickjs.a
SDIR    := $(TOP)/third_party/_build/stb
ZLIBA   := $(ZDIR)/libz.a
MBEDLIB := $(MDIR)/build/library
CFLAGS  += -I$(ZDIR) -I$(MDIR)/include -I$(QDIR)/quickjs-2025-04-26 -I$(SDIR)
LDLIBS_Z := $(ZLIBA)
LDLIBS_TLS := $(MBEDLIB)/libmbedtls.a $(MBEDLIB)/libmbedx509.a $(MBEDLIB)/libmbedcrypto.a

LIBK93_SRC := $(wildcard $(TOP)/userspace/libk93/*.c) $(GEN)/font8x16.c $(GEN)/icons.c
LIBK93_OBJ := $(patsubst $(TOP)/userspace/libk93/%.c,$(BUILD)/obj/%.o,$(filter $(TOP)/userspace/libk93/%,$(LIBK93_SRC))) $(GEN)/font8x16.o $(GEN)/icons.o

# ---------------------------------------------------------------- apps
APPS := springboard settings files terminal installer installtohd \
        notes calendar contacts music photos camera diagnostics safari \
        store youtube maps gamecenter trustme adsheet calculator ibooks \
        voicememos mail
APP_BINS := $(patsubst %,$(GBIN)/%,$(APPS))

.PHONY: all clean deps font rootfs initrd disk run qemu-test selfcheck hostselfcheck fixtures uapi

all: uapi deps font $(GBIN)/init $(GBIN)/launchd $(APP_BINS) $(GBIN)/k93selfcheck \
     $(GBIN)/k93dmgtool $(GBIN)/k93ipatool $(GBIN)/k93smc

# musl's toolchain has no kernel UAPI headers; copy the host's (kernel-headers
# package) into the build tree so <linux/fb.h>/<linux/input.h> resolve.
uapi: $(UAPI)/.stamp
$(UAPI)/.stamp:
	@mkdir -p $(UAPI)
	cp -r /usr/include/linux /usr/include/asm /usr/include/asm-generic $(UAPI)/
	ln -sfn /usr/include/mtd $(UAPI)/mtd
	# DRM/KMS uapi (Fedora's kernel-headers package omits it; vendor headers)
	mkdir -p $(UAPI)/linux
	cp $(TOP)/third_party/linux/include/uapi/drm/drm.h \
	   $(TOP)/third_party/linux/include/uapi/drm/drm_mode.h $(UAPI)/linux/
	# ALSA PCM UAPI (for the clean-room sound client)
	cp $(TOP)/third_party/linux/include/uapi/sound/asound.h $(UAPI)/linux/
	@touch $@

# ---------------------------------------------------------------- deps
deps: $(ZLIBA) $(MBEDLIB)/libmbedtls.a $(QJSA) $(SDIR)/stb_image.h

$(ZLIBA):
	@echo ">> building zlib (static, musl)"
	@bash $(TOP)/scripts/build-deps.sh zlib

$(MBEDLIB)/libmbedtls.a:
	@echo ">> building mbedTLS (static, musl)"
	@bash $(TOP)/scripts/build-deps.sh mbedtls

$(QJSA):
	@echo ">> building QuickJS (static, musl)"
	@bash $(TOP)/scripts/build-deps.sh quickjs

$(SDIR)/stb_image.h:
	@echo ">> fetching stb_image.h"
	@bash $(TOP)/scripts/build-deps.sh stb

# ---------------------------------------------------------------- objects
$(GEN)/font8x16.c: $(TOP)/tools/genfont.py $(TOP)/third_party/linux/lib/fonts/font_8x16.c
	@mkdir -p $(GEN)
	python3 $(TOP)/tools/genfont.py $(TOP)/third_party/linux/lib/fonts/font_8x16.c $@

$(BUILD)/obj/%.o: $(TOP)/userspace/libk93/%.c | $(UAPI)/.stamp
	@mkdir -p $(BUILD)/obj
	$(CC) $(CFLAGS) -c -o $@ $<

$(GEN)/font8x16.o: $(GEN)/font8x16.c | $(UAPI)/.stamp
	$(CC) $(CFLAGS) -c -o $@ $<

$(GEN)/icons.c: $(TOP)/tools/genicons.py
	@mkdir -p $(GEN) $(TOP)/assets/icons
	python3 $(TOP)/tools/genicons.py $(TOP)/assets/icons $@

$(GEN)/icons.o: $(GEN)/icons.c | $(UAPI)/.stamp
	$(CC) $(CFLAGS) -c -o $@ $<

LIBK93 := $(BUILD)/libk93.a
# `ar rcs` updates members in place, so make can wrongly consider the archive
# fresh; force the recipe to run and re-archive every member
$(LIBK93): $(LIBK93_OBJ)
	@rm -f $@
	ar rcs $@ $^

$(GBIN)/init: $(TOP)/userspace/sbin/init.c $(LIBK93)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $< $(LIBK93) $(LDFLAGS)

$(GBIN)/launchd: $(TOP)/userspace/sbin/launchd.c $(LIBK93)
	$(CC) $(CFLAGS) -o $@ $< $(LIBK93) $(LDFLAGS)

# the app binaries must relink when libk93 objects change; make only tracks the
# .a file, so the objects have to be explicit prerequisites here too
$(GBIN)/%: $(TOP)/userspace/apps/%/main.c $(LIBK93) $(LIBK93_OBJ)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $< $(LIBK93) $(LDFLAGS) $(LDLIBS_Z)

$(GBIN)/k93selfcheck: $(TOP)/userspace/tests/selfcheck.c $(LIBK93)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $< $(LIBK93) $(LDFLAGS) $(LDLIBS_Z) $(LDLIBS_TLS) -lm

$(GBIN)/k93dmgtool: $(TOP)/userspace/tools/dmgtool.c $(LIBK93)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $< $(LIBK93) $(LDFLAGS) $(LDLIBS_Z)

$(GBIN)/k93ipatool: $(TOP)/userspace/tools/ipatool.c $(LIBK93)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $< $(LIBK93) $(LDFLAGS) $(LDLIBS_Z)

$(GBIN)/k93smc: $(TOP)/userspace/tools/smc.c $(LIBK93)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $< $(LIBK93) $(LDFLAGS)

SAFARI_SRC := $(TOP)/userspace/apps/safari/main.c \
              $(TOP)/userspace/apps/safari/render.c \
              $(TOP)/userspace/apps/safari/js.c
$(GBIN)/safari: $(SAFARI_SRC) $(LIBK93) $(LDLIBS_TLS) $(QJSA) $(SDIR)/stb_image.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $(SAFARI_SRC) $(LIBK93) $(LDFLAGS) \
	    $(LDLIBS_Z) $(LDLIBS_TLS) $(QJSA) -lm

# ---------------------------------------------------------------- fixtures/test
fixtures:
	python3 $(TOP)/tools/mkfixtures.py $(FIXT)

hostselfcheck: deps font $(GBIN)/k93selfcheck fixtures
	K93_HOST=1 $(GBIN)/k93selfcheck $(FIXT)

selfcheck: hostselfcheck

# ---------------------------------------------------------------- rootfs/image
rootfs: all
	bash $(TOP)/scripts/mkrootfs.sh

initrd: rootfs
	bash $(TOP)/scripts/mkinitrd.sh

disk: initrd
	bash $(TOP)/scripts/mkdisk.sh

qemu-test: disk
	bash $(TOP)/scripts/qemu-test.sh

run: disk
	bash $(TOP)/scripts/qemu-run.sh

clean:
	rm -rf $(BUILD)
