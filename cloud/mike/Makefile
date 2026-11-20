# SPDX-License-Identifier: Apache-2.0
#
# Install Xiaomi MIKE SDK headers + static libs for mike_cam to link against.
# Vendor drop lives in a versioned subdirectory (currently v5.3.4).
include $(COMPILATION_RULES)
include $(or $(FRAMEWORK_CONFIG_FILE),$(PROJECT_CONFIG_FILE))

MODULE_ROOT ?= $(shell pwd)
MIKE_SDK_VER ?= v5.3.4.25
MIKE_DIR := $(MODULE_ROOT)/$(MIKE_SDK_VER)

.PHONY: all clean install

all: install

install:
	@mkdir -p "$(APPLICATION_INSTALL_DIR)/include/mike"
	@mkdir -p "$(APPLICATION_INSTALL_DIR)/lib/mike"
	@if [ -d "$(MIKE_DIR)/include" ]; then cp -f "$(MIKE_DIR)"/include/*.h "$(APPLICATION_INSTALL_DIR)/include/mike/"; fi
	@if [ -d "$(MIKE_DIR)/static_lib" ]; then cp -f "$(MIKE_DIR)"/static_lib/*.a "$(APPLICATION_INSTALL_DIR)/lib/mike/" 2>/dev/null || true; fi
	@echo "Installed MIKE $(MIKE_SDK_VER) headers and static libs"

clean:
	@echo "cloud/mike: nothing to clean"
