#
# Makefile for application layer
#
include $(PROJECT_CONFIG_FILE)
include $(COMPILATION_RULES)

_CELESTIA_ROOT := $(abspath $(dir $(PROJECT_CONFIG_FILE))/../..)
# Rootfs staging paths (inlined; build.sh exports SOC_PLATFORM_BUILD_DIR/ROOTFS_DIR).
REL_SOC_PLATFORM_DIR ?= system/$(CONFIG_SOC_PLATFORM)/$(CONFIG_SOC_SDK_VERSION)
ifndef SOC_PLATFORM_BUILD_DIR
PRODUCT_NAME ?= $(notdir $(patsubst %/,%,$(dir $(PROJECT_CONFIG_FILE))))
_CELESTIA_ROOT ?= $(abspath $(dir $(PROJECT_CONFIG_FILE))/../..)
SOC_PLATFORM_BUILD_DIR := $(_CELESTIA_ROOT)/out/$(PRODUCT_NAME)/build/$(REL_SOC_PLATFORM_DIR)
endif
ROOTFS_DIR ?= $(SOC_PLATFORM_BUILD_DIR)/rootfs

# Runtime .so staging (packaging reads this tree; never burntool/install/lib):
#   ROOTFS_USR_LIB        = out/<product>/build/system/.../rootfs/utils/usr/lib
#   ROOTFS_USR_LIB_MIRROR = out/.../rootfs/usr/lib (usr.sqsh4 source; optional mirror at install)
# install_app_main copies framework/hardware/third_party .so into ROOTFS_USR_LIB only;
# build_rootfs.sh merges utils/usr/lib -> rootfs/usr/lib for flash /usr/lib.
FRAMEWORK_SO_INSTALL_DIR ?= $(FRAMEWORK_INSTALL_DIR)/lib
# Rootfs staging: out/<product>/build/system/.../rootfs (inlined above; build.sh exports ROOTFS_DIR).
ROOTFS_USR_LIB ?= $(ROOTFS_DIR)/utils/usr/lib
ROOTFS_USR_LIB_MIRROR ?= $(ROOTFS_DIR)/usr/lib
APP_DEPLOY_THIRD_PARTY_SO := $(CONFIG_APP_DEPLOY_THIRD_PARTY_SO)
APP_DEPLOY_CORE_SO_TO_USR_LIB := $(if $(strip $(CONFIG_APP_DEPLOY_CORE_SO_TO_USR_LIB)),$(CONFIG_APP_DEPLOY_CORE_SO_TO_USR_LIB),y)
THIRD_PARTY_SO_ALLOWLIST := $(CONFIG_THIRD_PARTY_SO_ALLOWLIST)

MODULE_ROOT ?= $(shell pwd)
BUILD_DIR ?= $(MODULE_ROOT)/build
BUILD_APPLICATION_DIR ?= $(BUILD_DIR)
BIN_DIR := $(BUILD_DIR)/bin

.DEFAULT_GOAL := all

# Build app_main last so it can link with volc_engine, nova, uvc libs already installed
obj-$(CONFIG_APP_CLOUD_ALIYUN) += cloud/aliyun
obj-$(CONFIG_APP_CLOUD_TUYA) += cloud/tuya
obj-$(CONFIG_APP_CLOUD_VOLC) += cloud/volc_engine
obj-$(CONFIG_APP_CLOUD_MIKE) += cloud/mike
GUI_SUBDIR := tablet
ifneq ($(CONFIG_APP_PACKAGES_MODULE),)
GUI_SUBDIR := $(CONFIG_APP_PACKAGES_MODULE)
endif
# weather service before gui: packages/wearable (clock) and packages/tablet include weather_service.h,
# so its header must be installed before the gui modules compile.
obj-$(CONFIG_APP_WEATHER) += services/weather/
# studio (PC sync tool daemon) has no dependents; keep it next to weather.
obj-$(CONFIG_APP_STUDIO) += services/studio/
obj-$(CONFIG_APP_PACKAGES_TABLET) += packages/$(GUI_SUBDIR)/
obj-$(CONFIG_APP_PACKAGES_WEARABLE) += packages/wearable/
obj-$(CONFIG_APP_PACKAGES_IPCAMERA) += packages/ipcamera/
obj-$(CONFIG_APP_UVC) += services/uvc/
obj-$(CONFIG_APP_MAIN_ENTRY) += app_main/

.PHONY: all clean install_app_main $(obj-y) _ensure_application_build_dir

# Defer creating out/.../build/application until we actually run application build steps
# (avoid parse-time $(shell mkdir) side effects; submodules still mkdir their own BUILD_DIR as needed).
_ensure_application_build_dir:
	@mkdir -p "$(BUILD_APPLICATION_DIR)"

all: _ensure_application_build_dir build_modules test install_app_main

# When CONFIG_APP_MAIN_ENTRY=y, packaged main exe must come from app_main (overwrite tests copy)
install_app_main:
	@if [ "$(CONFIG_APP_MAIN_ENTRY)" = "y" ] && [ -n "$(ROOTFS_DIR)" ] && [ -d "$(ROOTFS_DIR)" ]; then \
		APP_MAIN_TARGET="$(CONFIG_APP_MAIN_TARGET)"; \
		RUNTIME_CFG_DIR="$(dir $(PROJECT_CONFIG_FILE))system/$(CONFIG_SOC_PLATFORM)/$(CONFIG_SOC_SDK_VERSION)/board/$(CONFIG_BOARD)/rootfs/utils/etc/config"; \
		if [ ! -d "$$RUNTIME_CFG_DIR" ]; then RUNTIME_CFG_DIR="$(dir $(PROJECT_CONFIG_FILE))config"; fi; \
		[ -z "$$APP_MAIN_TARGET" ] && APP_MAIN_TARGET=ipc_main; \
		if [ -f "$(BUILD_APPLICATION_DIR)/app_main/$$APP_MAIN_TARGET" ]; then \
			cp -f "$(BUILD_APPLICATION_DIR)/app_main/$$APP_MAIN_TARGET" "$(ROOTFS_DIR)/"; \
			echo "Installed app_main $$APP_MAIN_TARGET to rootfs for packaging"; \
			if [ "$(CONFIG_APP_MAIN_DEFER_HEAVY_SO)" = "y" ]; then \
				cp -f "$(BUILD_APPLICATION_DIR)/app_main/$$APP_MAIN_TARGET" "$(ROOTFS_DIR)/ipc_home"; \
				echo "Installed Scheme B alias ipc_home (-> $$APP_MAIN_TARGET)"; \
			fi; \
		fi; \
		GUI_CFG="$$RUNTIME_CFG_DIR/app_config.ini"; \
		if [ ! -f "$$GUI_CFG" ]; then GUI_CFG="$$RUNTIME_CFG_DIR/app_gui_config.ini"; fi; \
		if [ -f "$$GUI_CFG" ] && [ -d "$(ROOTFS_DIR)" ]; then \
			mkdir -p "$(ROOTFS_DIR)/utils/etc/config"; \
			cp -f "$$GUI_CFG" "$(ROOTFS_DIR)/utils/etc/config/app_config.ini"; \
			echo "Installed app_config.ini from $$GUI_CFG"; \
		fi; \
		mkdir -p "$(ROOTFS_USR_LIB)" "$(ROOTFS_USR_LIB_MIRROR)"; \
		if [ "$(APP_DEPLOY_THIRD_PARTY_SO)" = "y" ]; then \
			if [ -d "$(THIRD_PARTY_INSTALL_DIR)/iniparser/lib" ]; then \
				find "$(THIRD_PARTY_INSTALL_DIR)/iniparser/lib" -maxdepth 1 -name 'libiniparser.so*' -exec cp -af {} "$(ROOTFS_USR_LIB)/" \; 2>/dev/null || true; \
			fi; \
			if [ -d "$(THIRD_PARTY_INSTALL_DIR)/cjson/lib" ]; then \
				find "$(THIRD_PARTY_INSTALL_DIR)/cjson/lib" -maxdepth 1 -name 'libcjson.so*' -exec cp -af {} "$(ROOTFS_USR_LIB)/" \; 2>/dev/null || true; \
			fi; \
			if [ -d "$(THIRD_PARTY_INSTALL_DIR)/curl/lib" ]; then \
				find "$(THIRD_PARTY_INSTALL_DIR)/curl/lib" -maxdepth 1 -name 'libcurl.so*' -exec cp -af {} "$(ROOTFS_USR_LIB)/" \; 2>/dev/null || true; \
			fi; \
			if [ -d "$(THIRD_PARTY_INSTALL_DIR)/zlib/lib" ]; then \
				find "$(THIRD_PARTY_INSTALL_DIR)/zlib/lib" -maxdepth 1 -name 'libz.so*' -exec cp -af {} "$(ROOTFS_USR_LIB)/" \; 2>/dev/null || true; \
			fi; \
			if [ -d "$(THIRD_PARTY_INSTALL_DIR)/zxing/lib" ]; then \
				find "$(THIRD_PARTY_INSTALL_DIR)/zxing/lib" -maxdepth 1 -name 'libzxing.so*' -exec cp -af {} "$(ROOTFS_USR_LIB)/" \; 2>/dev/null || true; \
			fi; \
			if [ -d "$(THIRD_PARTY_INSTALL_DIR)/openssl/lib" ]; then \
				find "$(THIRD_PARTY_INSTALL_DIR)/openssl/lib" -maxdepth 1 -name 'libcrypto.so*' -exec cp -af {} "$(ROOTFS_USR_LIB)/" \; 2>/dev/null || true; \
				find "$(THIRD_PARTY_INSTALL_DIR)/openssl/lib" -maxdepth 1 -name 'libssl.so*' -exec cp -af {} "$(ROOTFS_USR_LIB)/" \; 2>/dev/null || true; \
			fi; \
			if [ -d "$(THIRD_PARTY_INSTALL_DIR)/iconv/lib" ]; then \
				find "$(THIRD_PARTY_INSTALL_DIR)/iconv/lib" -maxdepth 1 -name 'libiconv.so*' -exec cp -af {} "$(ROOTFS_USR_LIB)/" \; 2>/dev/null || true; \
			fi; \
		else \
			echo "Skip full third-party .so deploy (APP_DEPLOY_THIRD_PARTY_SO=n), apply allowlist: $(THIRD_PARTY_SO_ALLOWLIST)"; \
			for mod in $(THIRD_PARTY_SO_ALLOWLIST); do \
				if [ -d "$(THIRD_PARTY_INSTALL_DIR)/$$mod/lib" ]; then \
					find "$(THIRD_PARTY_INSTALL_DIR)/$$mod/lib" -maxdepth 1 -name 'lib*.so*' -exec cp -af {} "$(ROOTFS_USR_LIB)/" \; 2>/dev/null || true; \
					echo "Installed third_party/$$mod .so -> $(ROOTFS_USR_LIB)"; \
				fi; \
			done; \
		fi; \
		for mod in freetype libpng; do \
			if [ -d "$(THIRD_PARTY_INSTALL_DIR)/$$mod/lib" ]; then \
				find "$(THIRD_PARTY_INSTALL_DIR)/$$mod/lib" -maxdepth 1 -name 'lib*.so*' -exec cp -af {} "$(ROOTFS_USR_LIB)/" \; 2>/dev/null || true; \
			fi; \
		done; \
		if [ "$(CONFIG_THIRD_PARTY_ZXING)" = "y" ] && { [ "$(CONFIG_APP_NEBULA)" = "y" ] || [ "$(CONFIG_APP_PACKAGES_IPCAMERA)" = "y" ]; }; then \
			if [ -d "$(THIRD_PARTY_INSTALL_DIR)/zxing/lib" ]; then \
				find "$(THIRD_PARTY_INSTALL_DIR)/zxing/lib" -maxdepth 1 -name 'libzxing.so*' -exec cp -af {} "$(ROOTFS_USR_LIB)/" \; 2>/dev/null || true; \
				echo "Installed libzxing.so for ipcamera/nebula runtime"; \
			else \
				echo "ERROR: libzxing.so required but missing under $(THIRD_PARTY_INSTALL_DIR)/zxing/lib"; exit 1; \
			fi; \
		fi; \
		if [ "$(CONFIG_APP_PACKAGES_IPCAMERA)" = "y" ] && [ -f "$(APPLICATION_INSTALL_DIR)/lib/libipc_cam_app.so" ]; then \
			cp -af "$(APPLICATION_INSTALL_DIR)/lib/libipc_cam_app.so" "$(ROOTFS_USR_LIB)/"; \
			echo "Installed libipc_cam_app.so to $(ROOTFS_USR_LIB)"; \
		fi; \
		if [ "$(CONFIG_APP_IPCAMERA_MIKE_CAM)" = "y" ] && [ -f "$(APPLICATION_INSTALL_DIR)/lib/libmike_cam_app.so" ]; then \
			cp -af "$(APPLICATION_INSTALL_DIR)/lib/libmike_cam_app.so" "$(ROOTFS_USR_LIB)/"; \
			echo "Installed libmike_cam_app.so to $(ROOTFS_USR_LIB)"; \
		fi; \
		if [ -f "$(ROOTFS_USR_LIB)/libz.so" ] && [ ! -e "$(ROOTFS_USR_LIB)/libz.so.1" ]; then \
			ln -sf libz.so "$(ROOTFS_USR_LIB)/libz.so.1"; \
		fi; \
		if [ -d "$(SYSTEM_CORE_INSTALL_DIR)/lib" ]; then \
			find "$(SYSTEM_CORE_INSTALL_DIR)/lib" -maxdepth 1 -name 'libsystem_core.so*' -exec cp -af {} "$(ROOTFS_USR_LIB)/" \; 2>/dev/null || true; \
		fi; \
		if [ -d "$(FRAMEWORK_SO_INSTALL_DIR)/framework" ]; then \
			find "$(FRAMEWORK_SO_INSTALL_DIR)/framework" -maxdepth 1 -name 'libnebula_protocol.so*' -exec cp -af {} "$(ROOTFS_USR_LIB)/" \; 2>/dev/null || true; \
			find "$(FRAMEWORK_SO_INSTALL_DIR)/framework" -maxdepth 1 -name 'librtsp_protocol.so*' -exec cp -af {} "$(ROOTFS_USR_LIB)/" \; 2>/dev/null || true; \
			find "$(FRAMEWORK_SO_INSTALL_DIR)/framework" -maxdepth 1 -name 'libruntime.so*' -exec cp -af {} "$(ROOTFS_USR_LIB)/" \; 2>/dev/null || true; \
		fi; \
		find "$(FRAMEWORK_SO_INSTALL_DIR)" -maxdepth 1 -name 'libnebula_protocol.so*' -exec cp -af {} "$(ROOTFS_USR_LIB)/" \; 2>/dev/null || true; \
		find "$(FRAMEWORK_SO_INSTALL_DIR)" -maxdepth 1 -name 'librtsp_protocol.so*' -exec cp -af {} "$(ROOTFS_USR_LIB)/" \; 2>/dev/null || true; \
		find "$(FRAMEWORK_SO_INSTALL_DIR)" -maxdepth 1 -name 'libruntime.so*' -exec cp -af {} "$(ROOTFS_USR_LIB)/" \; 2>/dev/null || true; \
		if [ "$(APP_DEPLOY_CORE_SO_TO_USR_LIB)" = "y" ]; then \
			if [ -d "$(FRAMEWORK_SO_INSTALL_DIR)" ]; then \
				find "$(FRAMEWORK_SO_INSTALL_DIR)" -maxdepth 1 -name 'lib*.so*' ! -name 'liblvgl.so*' \
					-exec cp -af {} "$(ROOTFS_USR_LIB)/" \; 2>/dev/null || true; \
			fi; \
			if [ -d "$(HARDWARE_INSTALL_DIR)/lib" ]; then \
				find "$(HARDWARE_INSTALL_DIR)/lib" -maxdepth 1 -name 'lib*.so*' -exec cp -af {} "$(ROOTFS_USR_LIB)/" \; 2>/dev/null || true; \
			fi; \
			if [ -d "$(SYSTEM_CORE_INSTALL_DIR)/lib" ]; then \
				find "$(SYSTEM_CORE_INSTALL_DIR)/lib" -maxdepth 1 -name 'lib*.so*' -exec cp -af {} "$(ROOTFS_USR_LIB)/" \; 2>/dev/null || true; \
			fi; \
			if [ -d "$(SOC_PLATFORM_INSTALL_DIR)/lib" ]; then \
				find "$(SOC_PLATFORM_INSTALL_DIR)/lib" -maxdepth 1 -name 'libakaudiofilter.so*' -exec cp -af {} "$(ROOTFS_USR_LIB)/" \; 2>/dev/null || true; \
			fi; \
			cp -af "$(ROOTFS_USR_LIB)"/lib*.so* "$(ROOTFS_USR_LIB_MIRROR)/" 2>/dev/null || true; \
			rm -f "$(ROOTFS_USR_LIB)"/liblvgl.so* "$(ROOTFS_USR_LIB_MIRROR)"/liblvgl.so* 2>/dev/null || true; \
			echo "Mirrored $(ROOTFS_USR_LIB) -> $(ROOTFS_USR_LIB_MIRROR) for usr.sqsh4"; \
		fi; \
		echo "Installed Celestia runtime .so to $(ROOTFS_USR_LIB) (flash /usr/lib via packaging; not burntool/install/lib)"; \
		if [ "$(CONFIG_APP_TABLET_VOLC_ENGINE)" = "y" ] && [ -f "$(APPLICATION_INSTALL_DIR)/lib/libvolc_engine_svc.so" ]; then \
			cp -af "$(APPLICATION_INSTALL_DIR)/lib/libvolc_engine_svc.so" "$(ROOTFS_USR_LIB)/"; \
			echo "Installed libvolc_engine_svc.so to $(ROOTFS_USR_LIB)"; \
		fi; \
		if [ "$(CONFIG_APP_WEATHER)" = "y" ] && [ -f "$(APPLICATION_INSTALL_DIR)/lib/libweather_service.so" ]; then \
			cp -af "$(APPLICATION_INSTALL_DIR)/lib/libweather_service.so" "$(ROOTFS_USR_LIB)/"; \
			echo "Installed libweather_service.so to $(ROOTFS_USR_LIB)"; \
		fi; \
		if [ "$(CONFIG_APP_STUDIO)" = "y" ] && [ -f "$(APPLICATION_INSTALL_DIR)/lib/libstudio.so" ]; then \
			cp -af "$(APPLICATION_INSTALL_DIR)/lib/libstudio.so" "$(ROOTFS_USR_LIB)/"; \
			echo "Installed libstudio.so to $(ROOTFS_USR_LIB)"; \
		fi; \
	fi

build_modules:
	@echo "Building application modules"
	@for dir in $(obj-y); do \
		if [ -d "$(SRC_APPLICATION_DIR)/$$dir" ]; then \
			echo "Building application module: $$dir"; \
			EXTRA_VAR=""; \
			[ "$$dir" = "packages/tablet/" ] && EXTRA_VAR="TABLET_EXCLUDE_MAIN=1"; \
			[ "$$dir" = "packages/wearable/" ] && EXTRA_VAR="TABLET_EXCLUDE_MAIN=1"; \
			MODULE_GOAL=all; \
			if [ "$(CONFIG_APP_MAIN_OBJECTS_ONLY)" = "y" ] && [ "$$dir" = "app_main/" ]; then \
				MODULE_GOAL=aos_objects; \
				echo "app_main: objects only (final link is the project image)"; \
			fi; \
			$(MAKE) -C $(SRC_APPLICATION_DIR)/$$dir \
				SRC_DIR=$(SRC_APPLICATION_DIR)/$$dir \
				BUILD_DIR=$(BUILD_APPLICATION_DIR)/$$dir \
				BUILD_APPLICATION_DIR=$(BUILD_APPLICATION_DIR) \
				ROOTFS_DIR=$(ROOTFS_DIR) \
				SRC_APPLICATION_DIR=$(SRC_APPLICATION_DIR) \
				BUILD_FRAMEWORK_DIR=$(BUILD_FRAMEWORK_DIR) \
				SRC_FRAMEWORK_DIR=$(SRC_FRAMEWORK_DIR) \
				FRAMEWORK_DIR=$(FRAMEWORK_DIR) \
				BUILD_SYSTEM_CORE_DIR=$(BUILD_SYSTEM_CORE_DIR) \
				SRC_SYSTEM_CORE_DIR=$(SRC_SYSTEM_CORE_DIR) \
				SYSTEM_CORE_INSTALL_DIR=$(SYSTEM_CORE_INSTALL_DIR) \
				FRAMEWORK_INSTALL_DIR=$(FRAMEWORK_INSTALL_DIR) \
				FRAMEWORK_SO_INSTALL_DIR=$(FRAMEWORK_SO_INSTALL_DIR) \
				APPLICATION_INSTALL_DIR=$(APPLICATION_INSTALL_DIR) \
				HARDWARE_INSTALL_DIR=$(HARDWARE_INSTALL_DIR) \
				HARDWARE_DIR=$(HARDWARE_DIR) \
				THIRD_PARTY_INSTALL_DIR=$(THIRD_PARTY_INSTALL_DIR) \
				SOC_PLATFORM_INSTALL_DIR=$(SOC_PLATFORM_INSTALL_DIR) \
				FRAMEWORK_CONFIG_FILE=$(PROJECT_CONFIG_FILE) \
				PROJECT_CONFIG_FILE=$(PROJECT_CONFIG_FILE) \
				SOC_PLATFORM_DIR=$(SOC_PLATFORM_DIR) \
				SOC_PLATFORM_BUILD_DIR=$(SOC_PLATFORM_BUILD_DIR) \
				ROOTFS_DIR=$(ROOTFS_DIR) \
				COMPILATION_RULES=$(COMPILATION_RULES) \
				CROSS_TOOLCHAIN=$(CROSS_TOOLCHAIN) \
				$$EXTRA_VAR $$MODULE_GOAL; \
			if [ $$? -ne 0 ]; then exit 1; fi; \
		fi; \
	done

test:
	@if [ -d "$(SRC_APPLICATION_DIR)/tests" ]; then \
		$(MAKE) -C $(SRC_APPLICATION_DIR)/tests \
			BUILD_DIR=$(BUILD_APPLICATION_DIR)/tests \
			BUILD_APPLICATION_DIR=$(BUILD_APPLICATION_DIR) \
			ROOTFS_DIR=$(ROOTFS_DIR) \
			SYSTEM_CORE_INSTALL_DIR=$(SYSTEM_CORE_INSTALL_DIR) \
			FRAMEWORK_INSTALL_DIR=$(FRAMEWORK_INSTALL_DIR) \
			APPLICATION_INSTALL_DIR=$(APPLICATION_INSTALL_DIR) \
			HARDWARE_INSTALL_DIR=$(HARDWARE_INSTALL_DIR) \
			THIRD_PARTY_INSTALL_DIR=$(THIRD_PARTY_INSTALL_DIR) \
			SOC_PLATFORM_INSTALL_DIR=$(SOC_PLATFORM_INSTALL_DIR) \
			SOC_PLATFORM_DIR=$(SOC_PLATFORM_DIR) \
			SOC_PLATFORM_BUILD_DIR=$(SOC_PLATFORM_BUILD_DIR) \
			ROOTFS_DIR=$(ROOTFS_DIR) \
			CROSS_TOOLCHAIN=$(CROSS_TOOLCHAIN) \
			COMPILATION_RULES=$(COMPILATION_RULES) \
			PROJECT_CONFIG_FILE=$(PROJECT_CONFIG_FILE); \
	fi

clean:
	@echo "Cleaning application build files..."
	@rm -rf $(BUILD_DIR)
	@for dir in $(obj-y); do \
		if [ -d "$(SRC_APPLICATION_DIR)/$$dir" ]; then \
			$(MAKE) -C $(SRC_APPLICATION_DIR)/$$dir clean BUILD_DIR=$(BUILD_APPLICATION_DIR)/$$dir 2>/dev/null || true; \
		fi; \
	done
	@find $(MODULE_ROOT) -name "*.o" -type f -delete 2>/dev/null || true
	@find $(MODULE_ROOT) -name "*.d" -type f -delete 2>/dev/null || true


