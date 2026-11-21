# SPDX-License-Identifier: BSD-3-Clause
# L8 e-paper dashboard firmware on the cc2530-zigbee stack (git submodule cc2530-zigbee).
.DEFAULT_GOAL := test
STACK := cc2530-zigbee
PYTHON ?= python3
LLVM_TOOLS ?= ../llvm-mcs51-build/bin
LLVM_SRC ?= ../llvm-project
FIRMWARE_OUT ?= build/firmware
HOST_DEFINES := -DCC2530_CHIP_SENSE -DCC2530_ZDO_DESCRIPTORS -DCC2530_APP_QUIET -DCC2530_APP_MGMT_LQI
TESTS := test_display test_display_579 test_zcl_dashboard
# The 5.79" (L5) display: the same host test sources with EPD_PANEL_579.
test_display_579_DEFINES := -DEPD_PANEL_579
# Linked with test_zcl_dashboard; the stack's temperature codec cross-checks
# the server's reads.
test_zcl_dashboard_SOURCES := src/zcl_dashboard.c boards/lg_esl29.c stack:src/zcl_temperature.c \
	stack:src/zcl_dispatch.c stack:src/zcl_write.c stack:src/zcl_attributes.c stack:src/zcl_frame.c \
	stack:src/zcl_value.c

.PHONY: test $(TESTS) firmware art clean

$(STACK)/Makefile:
	git submodule update --init $(STACK)

# Host tests (plain and ASan/UBSan) of the application against the stack.
test: $(TESTS)

$(TESTS): | $(STACK)/Makefile
	$(MAKE) -C $(STACK) test-app-host APP_DIR=$(CURDIR) BUILD=$(CURDIR)/build/$@ \
		APP_HOST_TEST=tests/$@.c APP_HOST_DEFINES="$(HOST_DEFINES) $($@_DEFINES)" APP_HOST_SOURCES="$($@_SOURCES)"

# LLVM MCS-51 firmware (Intel HEX in $(FIRMWARE_OUT)/firmware.hex); PANEL=579
# builds the 5.79" (L5) image from build/app-579 (tools/stage_panel.py) into
# build/firmware-579.
ifeq ($(PANEL),579)
FIRMWARE_APP := $(CURDIR)/build/app-579
FIRMWARE_OUT_PANEL := build/firmware-579
else
FIRMWARE_APP := $(CURDIR)
FIRMWARE_OUT_PANEL := $(FIRMWARE_OUT)
endif
firmware: | $(STACK)/Makefile
ifeq ($(PANEL),579)
	$(PYTHON) tools/stage_panel.py $(FIRMWARE_APP)
endif
	$(PYTHON) $(STACK)/tools/llvm/firmware.py --tools $(LLVM_TOOLS) --llvm-src $(LLVM_SRC) \
		--output $(FIRMWARE_OUT_PANEL) --board external --image join --variant app --app $(FIRMWARE_APP)

# Regenerates include/display_art.h and tests/display_golden.h (FONTS: the
# u8g2 tools/font/bdf directory), include/display_art_l5.h and
# tests/display_golden_l5.h (DEJAVU: the directory of DejaVuSans.ttf and
# DejaVuSans-Bold.ttf).
art:
	$(PYTHON) tools/epd/l8_art.py --fonts $(FONTS)
	$(PYTHON) tools/epd/l5_art.py --dejavu $(DEJAVU)

clean:
	rm -rf build
