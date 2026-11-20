# btdisk — a USB stick for the car radio that plays the phone's Bluetooth audio
#
#   make              list the targets
#   make test         unit tests + the real-time radio simulation, on this PC
#   make check-image  the virtual disk through fsck.fat and 7-Zip
#   make cross        build for the Pi Zero W with the image's SDK
#   make image        the read-only Yocto image (DEV=1: with Wi-Fi and ssh)
#   make flash DISK=/dev/sdX
#   make bench-test   end to end on the bench's Pi Zero W (dummy_hcd host, a fake phone)
#
# Plain poky + bitbake, no kas: the layers are git clones (make layers) and the build dir's conf
# files are generated. BOARD picks the hardware; yocto/boards/$(BOARD).mk says what that means,
# yocto/conf/boards/$(BOARD).conf holds its bitbake settings.

SHELL := /bin/bash
.DEFAULT_GOAL := help

APP    := app
BUILD  := $(APP)/build

BOARD  ?= rpi0w
YOCTO  := yocto
LAYERS := $(YOCTO)/layers
BOARDS := $(patsubst $(YOCTO)/boards/%.mk,%,$(wildcard $(YOCTO)/boards/*.mk))
ifeq ($(filter $(BOARD),$(BOARDS)),)
  $(error BOARD=$(BOARD) is not one of: $(BOARDS))
endif
include $(YOCTO)/boards/$(BOARD).mk
YB     := $(YOCTO)/build-$(BOARD)
DEPLOY := $(YB)/tmp/deploy/images/$(MACHINE)
DEV    ?=
IMAGE  := btdisk-image
BB      = set -e && export MACHINE=$(MACHINE) && \
          . $(LAYERS)/poky/oe-init-build-env $(CURDIR)/$(YB) >/dev/null &&

# The cross build uses the image's SDK (make sdk), or any SDK for the same target given by SDK=.
SDK     ?= $(firstword $(wildcard $(YOCTO)/sdk/$(BOARD) ../bt-developement-bench/sdk/$(BOARD)))
SDK_ENV  = $(SDK)/environment-setup-$(SDK_TARGET)
XBUILD  := $(APP)/build-$(BOARD)

# The board to deploy to and the bench used by bench-test.
TARGET  ?= root@btdisk.local
BENCH   ?= btbench-rpi
# Where `make layers` clones from: upstream, or a directory of existing clones (LAYER_MIRROR=
# ../bt-developement-bench/yocto/layers) to work offline and fast.
LAYER_MIRROR ?=
PHONE   ?= bench
SOURCE  ?= btbench-rpi5
DISK    ?=

SSH_OPTS := -o ControlMaster=auto -o ControlPath=/tmp/ssh-btdisk-%C -o ControlPersist=60 \
            -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR
SCRATCH  := /tmp/btdisk-check

BOLD := \033[1m
DIM  := \033[2m
OFF  := \033[0m

## ─── develop ─────────────────────────────────────────────────────────────────

.PHONY: build
build: $(BUILD)/CMakeCache.txt ## Compile btdisk, its tests and the test tools for this machine
	@cmake --build $(BUILD) -j$$(nproc)

$(BUILD)/CMakeCache.txt:
	@cmake -S $(APP) -B $(BUILD) -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON >/dev/null

.PHONY: test
test: build ## Unit tests and the real-time radio simulation (about 20 s)
	@ctest --test-dir $(BUILD) --output-on-failure

.PHONY: check-image
check-image: build ## The virtual disk (FAT32 and FAT16) checked by fsck.fat and listed by 7-Zip
	@mkdir -p $(SCRATCH)
	@for fat in fat32 fat16; do \
	  printf "[usb]\nfat = $$fat\n[player]\nfile_mb = 64\nwav_info = true\n" > $(SCRATCH)/$$fat.conf; \
	  $(BUILD)/btdisk -c $(SCRATCH)/$$fat.conf --dump-image $(SCRATCH)/$$fat-disk.img 2>/dev/null; \
	  dd if=$(SCRATCH)/$$fat-disk.img of=$(SCRATCH)/$$fat-part.img bs=512 skip=2048 conv=sparse status=none; \
	  echo -e "$(BOLD)$$fat$(OFF)"; fsck.fat -n $(SCRATCH)/$$fat-part.img | tail -1; \
	  7z l $(SCRATCH)/$$fat-part.img | sed -n '/^----/,/^----/p' | sed -n '2,4p'; \
	done

.PHONY: clean
clean: ## Remove the local builds (FULL=1 also the Yocto tmp)
	@rm -rf $(BUILD) $(APP)/build-*
ifdef FULL
	@rm -rf $(YB)/tmp
endif

## ─── board ───────────────────────────────────────────────────────────────────

.PHONY: cross
cross: ## Build btdisk and the test tools for the board with the SDK (SDK=path)
	@[ -f "$(SDK_ENV)" ] || { echo "No SDK at '$(SDK)': make sdk, or SDK=<dir>"; exit 1; }
	@. $(SDK_ENV) && cmake -S $(APP) -B $(XBUILD) -DCMAKE_TOOLCHAIN_FILE=$$OE_CMAKE_TOOLCHAIN_FILE \
	    -DCMAKE_BUILD_TYPE=Release -DBTDISK_TESTS=ON >/dev/null && env -u MAKEFLAGS cmake --build $(XBUILD) -j$$(nproc) | grep -E "error|warning" ; \
	  for b in btdisk radio-play btdisk-tests; do $$STRIP -o $(XBUILD)/$$b.stripped $(XBUILD)/$$b; done
	@ls -la $(XBUILD)/*.stripped

.PHONY: deploy
deploy: cross ## Install the new btdisk on a running stick (DEV image) and restart it (TARGET=)
	@scp $(SSH_OPTS) -q $(XBUILD)/btdisk.stripped $(TARGET):/tmp/btdisk.new
	@ssh $(SSH_OPTS) $(TARGET) 'mount -o remount,rw / && install -m 0755 /tmp/btdisk.new /usr/bin/btdisk && \
	    mount -o remount,ro / && systemctl restart btdisk && systemctl is-active btdisk'

.PHONY: logs
logs: ## Follow btdisk's log on the stick (TARGET=)
	@ssh $(SSH_OPTS) $(TARGET) journalctl -fu btdisk -u bluetooth

## ─── bench ───────────────────────────────────────────────────────────────────

# The bench's Pi Zero W is the same hardware as the stick. Its own kernel enumerates the stick
# through dummy_hcd (no cable), mounts it and reads it like a radio; a fake phone on this PC
# streams a tone per track and answers next/previous. PipeWire on the board is stopped for the
# run (btdisk is the A2DP sink) and everything is put back afterwards.
.PHONY: bench-modules
bench-modules: ## Build usb_f_fs.ko + dummy_hcd.ko for the bench kernel (once per bench kernel)
	@tools/bench-modules.sh $(BENCH)

.PHONY: bench-test
bench-test: build cross ## End to end on the bench Pi (BENCH=, PHONE=bench|real|fake|tone, SOURCE=): phone -> btdisk -> USB -> radio
	@PHONE=$(PHONE) SOURCE=$(SOURCE) tools/bench-test.sh $(BENCH) $(XBUILD) $(BUILD)

## ─── image ───────────────────────────────────────────────────────────────────

.PHONY: layers
layers: ## Clone the pinned layers into yocto/layers (LAYER_MIRROR= to clone from local copies)
	@set -e; source <(sed -n 's/^\([A-Z_]*\) *= *\(.*\)/\1=\2/p' $(YOCTO)/conf/versions.conf); \
	clone() { name=$$1 url=$$2 rev=$$3 src=$$2; \
	  [ -z "$(LAYER_MIRROR)" ] || src=$(LAYER_MIRROR)/$$name; \
	  if [ ! -d $(LAYERS)/$$name ]; then \
	    git clone -q $$src $(LAYERS)/$$name; \
	    git -C $(LAYERS)/$$name remote set-url origin $$url; fi; \
	  git -C $(LAYERS)/$$name checkout -q $$rev; \
	  echo "$$name $$(git -C $(LAYERS)/$$name log --oneline -1)"; }; \
	clone poky $$POKY_GIT $$POKY_REV; \
	clone meta-raspberrypi $$META_RASPBERRYPI_GIT $$META_RASPBERRYPI_REV

$(YB)/conf/bblayers.conf: $(YOCTO)/conf/bblayers.conf.sample
	@mkdir -p $(YB)/conf
	@sed 's|@LAYERS@|    $(CURDIR)/$(LAYERS)/poky/meta \\\n    $(CURDIR)/$(LAYERS)/poky/meta-poky \\\n    $(CURDIR)/$(LAYERS)/meta-raspberrypi \\\n    $(CURDIR)/$(YOCTO)/meta-btdisk \\|' $< > $@

.PHONY: build-conf
build-conf: $(YB)/conf/bblayers.conf
	@[ -f $(YB)/conf/local.conf ] || cp $(YOCTO)/conf/local.conf.sample $(YB)/conf/local.conf
	@[ -f $(YB)/conf/site.conf ] || [ ! -f $(YOCTO)/conf/site.conf ] || ln -sf ../../conf/site.conf $(YB)/conf/site.conf
	@printf '# Written by the Makefile for BOARD=$(BOARD) DEV=$(DEV).\nrequire $(CURDIR)/$(YOCTO)/conf/boards/$(BOARD).conf\nBTDISK_DEV = "$(if $(DEV),1,0)"\n' > $(YB)/conf/auto.conf.new
	@cmp -s $(YB)/conf/auto.conf.new $(YB)/conf/auto.conf && rm $(YB)/conf/auto.conf.new || mv $(YB)/conf/auto.conf.new $(YB)/conf/auto.conf

.PHONY: configure
configure: ## Point the Yocto caches at shared directories (DL=, SSTATE=), written to yocto/conf/site.conf
	@printf '# Written by make configure. Per machine, not tracked.\nDL_DIR ?= "%s"\nSSTATE_DIR ?= "%s"\n' \
	    "$${DL:-$$HOME/yocto/downloads}" "$${SSTATE:-$$HOME/yocto/sstate-cache}" > $(YOCTO)/conf/site.conf
	@cat $(YOCTO)/conf/site.conf

.PHONY: image
image: build-conf ## Build the read-only image (DEV=1: the development variant with Wi-Fi and ssh)
	@# By process name, so the check never matches its own shell's command line.
	@if ps -eo comm=,args= | awk '$$1 ~ /^(bitbake|bitbake-server|python3)$$/ && /bitbake/ {f=1} END {exit !f}'; then \
	  echo "Another bitbake is running: one Yocto build at a time."; exit 1; fi
	@$(BB) bitbake $(IMAGE)
	@ls -la $(DEPLOY)/$(IMAGE)-$(MACHINE).rootfs.wic.bz2

.PHONY: sdk
sdk: build-conf ## Build and install the SDK the cross build uses (yocto/sdk/$(BOARD))
	@$(BB) bitbake $(IMAGE) -c populate_sdk
	@sh $$(ls -t $(YB)/tmp/deploy/sdk/*.sh | head -1) -y -d $(CURDIR)/$(YOCTO)/sdk/$(BOARD) >/dev/null
	@echo "SDK in $(YOCTO)/sdk/$(BOARD)"

.PHONY: bitbake
bitbake: build-conf ## Any bitbake command: make bitbake ARGS="-c menuconfig virtual/kernel"
	@$(BB) bitbake $(ARGS)

## ─── flash ───────────────────────────────────────────────────────────────────

.PHONY: flash
flash: ## Write the image to an SD card: make flash DISK=/dev/sdX (needs sudo)
	@[ -n "$(DISK)" ] || { echo "DISK=/dev/sdX please"; lsblk -dpo NAME,SIZE,MODEL,TRAN | grep -E 'usb|mmc'; exit 1; }
	@[ "$$(lsblk -dno RM $(DISK))" = "1" ] || [ "$$(lsblk -dno TRAN $(DISK))" = "usb" ] || \
	  { echo "$(DISK) is not a removable disk"; exit 1; }
	@for p in $$(lsblk -lno NAME,MOUNTPOINT $(DISK) | awk '$$2 != "" {print "/dev/"$$1}'); do sudo umount $$p; done
	@if command -v bmaptool >/dev/null; then \
	  sudo bmaptool copy $(DEPLOY)/$(IMAGE)-$(MACHINE).rootfs.wic.bz2 $(DISK); \
	else bzcat $(DEPLOY)/$(IMAGE)-$(MACHINE).rootfs.wic.bz2 | sudo dd of=$(DISK) bs=4M conv=fsync status=progress; fi
	@sync && echo -e "$(BOLD)done$(OFF): plug the Pi's USB port (not PWR) into the radio"


.PHONY: help
help:
	@echo -e "$(BOLD)btdisk$(OFF)  $(DIM)a USB stick for the car radio that plays the phone's Bluetooth audio (BOARD=$(BOARD))$(OFF)\n"
	@awk 'BEGIN {FS = ":.*## "} \
	     /^## ─/ { gsub(/## /,""); printf "\n\033[2m%s\033[0m\n", $$0; next } \
	     /^[a-zA-Z_-]+:.*?## / { printf "  \033[1m%-14s\033[0m %s\n", $$1, $$2 }' $(MAKEFILE_LIST)
	@echo -e "\n$(DIM)Vars:  BOARD=$(BOARD)  DEV=1  TARGET=$(TARGET)  BENCH=$(BENCH)  DISK=/dev/sdX  SDK=dir  LAYER_MIRROR=dir$(OFF)\n"
