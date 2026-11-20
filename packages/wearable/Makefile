#
# Makefile for application/packages/wearable module
#
include $(COMPILATION_RULES)
FRAMEWORK_OR_PROJECT := $(or $(FRAMEWORK_CONFIG_FILE),$(PROJECT_CONFIG_FILE))
include $(FRAMEWORK_OR_PROJECT)

MODULE_ROOT ?= $(shell pwd)
MODULE_ROOT_NORM := $(subst \,/,$(MODULE_ROOT))
_CELESTIA_ROOT := $(abspath $(dir $(PROJECT_CONFIG_FILE))/../..)
# Rootfs staging paths (inlined; build.sh exports SOC_PLATFORM_BUILD_DIR/ROOTFS_DIR).
REL_SOC_PLATFORM_DIR ?= system/$(CONFIG_SOC_PLATFORM)/$(CONFIG_SOC_SDK_VERSION)
ifndef SOC_PLATFORM_BUILD_DIR
PRODUCT_NAME ?= $(notdir $(patsubst %/,%,$(dir $(PROJECT_CONFIG_FILE))))
_CELESTIA_ROOT ?= $(abspath $(dir $(PROJECT_CONFIG_FILE))/../..)
SOC_PLATFORM_BUILD_DIR := $(_CELESTIA_ROOT)/out/$(PRODUCT_NAME)/build/$(REL_SOC_PLATFORM_DIR)
endif
ROOTFS_DIR ?= $(SOC_PLATFORM_BUILD_DIR)/rootfs

LAUNCHER_DIR := $(MODULE_ROOT)/launcher
GUI_COMMON_DIR := $(if $(strip $(SRC_APPLICATION_DIR)),$(SRC_APPLICATION_DIR)/packages/common,$(abspath $(MODULE_ROOT)/../common))
BUILD_DIR ?= $(MODULE_ROOT)/build
LAUNCHER_OBJ_DIR := $(BUILD_DIR)/launcher_obj
GUI_FONT_OBJ := $(LAUNCHER_OBJ_DIR)/gui_common/gui_font.o
GUI_COMMON_OBJ := $(LAUNCHER_OBJ_DIR)/gui_common/gui_common.o
APP_SO_BUILD_DIR := $(BUILD_DIR)/app_sos

ENABLED_APP_DIRS :=
ENABLED_APP_IDS :=
ifeq ($(CONFIG_APP_WEARABLE_CAMERA),y)
ENABLED_APP_DIRS += $(MODULE_ROOT)/camera
ENABLED_APP_IDS += camera
endif
ifeq ($(CONFIG_APP_WEARABLE_DASHCAM),y)
ENABLED_APP_DIRS += $(MODULE_ROOT)/dashcam
ENABLED_APP_IDS += dashcam
endif

APP_SO_FILE_camera := libcamera_app.so
APP_SO_FILE_dashcam := libdashcam_app.so
APP_SO_TARGETS :=
ifeq ($(CONFIG_APP_WEARABLE_CAMERA),y)
APP_SO_TARGETS += $(APP_SO_BUILD_DIR)/camera/lib/$(APP_SO_FILE_camera)
endif
ifeq ($(CONFIG_APP_WEARABLE_DASHCAM),y)
APP_SO_TARGETS += $(APP_SO_BUILD_DIR)/dashcam/lib/$(APP_SO_FILE_dashcam)
endif

# Luna-only power-key daemon (long-press shutdown); not built for Nova.
POWER_KEYD_DIR := $(MODULE_ROOT)/power_keyd
POWER_KEYD_SRC := $(POWER_KEYD_DIR)/luna_power_keyd.c
POWER_KEYD_BIN := luna_power_keyd
POWER_KEYD_BUILD := $(BUILD_DIR)/$(POWER_KEYD_BIN)
ROOTFS_USR_SBIN := $(ROOTFS_DIR)/utils/usr/sbin
POWER_KEYD_TARGETS :=
ifeq ($(CONFIG_APP_LUNA_POWER_KEYD),y)
POWER_KEYD_TARGETS += $(POWER_KEYD_BUILD)
endif

CSRCS = $(shell find $(LAUNCHER_DIR) -maxdepth 2 -name *.c)
CXXSRCS = $(shell find $(LAUNCHER_DIR) -maxdepth 2 -name *.cpp)
OBJS := $(patsubst $(MODULE_ROOT)/%.c,$(LAUNCHER_OBJ_DIR)/%.o,$(CSRCS))
OBJS += $(patsubst $(MODULE_ROOT)/%.cpp,$(LAUNCHER_OBJ_DIR)/%.o,$(CXXSRCS))
OBJS += $(GUI_FONT_OBJ) $(GUI_COMMON_OBJ)
ifeq ($(CONFIG_APP_WEARABLE_CAMERA),y)
TFCARD_OBJ := $(LAUNCHER_OBJ_DIR)/camera/wearable_camera_tfcard.o
OBJS += $(TFCARD_OBJ)
endif

RESOURCE_INSTALL_DIR := $(ROOTFS_DIR)/utils/usr
APPS_INSTALL_DIR := $(ROOTFS_DIR)/utils/usr/apps

# Prefer system/core source headers so new APIs (e.g. thread_set_current_name) resolve before out/install is refreshed.
SYSTEM_CORE_SRC := $(if $(strip $(SRC_SYSTEM_CORE_DIR)),$(SRC_SYSTEM_CORE_DIR),$(abspath $(MODULE_ROOT)/../../../system/core))
INCLUDE += -I$(SYSTEM_CORE_SRC)/include

INCLUDE += -I$(FRAMEWORK_INSTALL_DIR)/include
INCLUDE += -I$(FRAMEWORK_INSTALL_DIR)/include/ui
INCLUDE += -I$(FRAMEWORK_INSTALL_DIR)/include/gateway
INCLUDE += -I$(FRAMEWORK_INSTALL_DIR)/include/runtime
INCLUDE += -I$(FRAMEWORK_INSTALL_DIR)/include/services/multimedia
INCLUDE += -I$(FRAMEWORK_INSTALL_DIR)/include/services/storage
INCLUDE += -I$(FRAMEWORK_INSTALL_DIR)/include/services/network
INCLUDE += -I$(FRAMEWORK_INSTALL_DIR)/include/services/device
INCLUDE += -I$(FRAMEWORK_INSTALL_DIR)/include/services/notification
INCLUDE += -I$(SYSTEM_CORE_INSTALL_DIR)/include
INCLUDE += -I$(SOC_PLATFORM_INSTALL_DIR)/include
INCLUDE += -I$(HARDWARE_INSTALL_DIR)/include
INCLUDE += -I$(THIRD_PARTY_INSTALL_DIR)/freetype/include/freetype2
INCLUDE += -I$(THIRD_PARTY_INSTALL_DIR)/lvgl/include
INCLUDE += -I$(THIRD_PARTY_INSTALL_DIR)/lvgl/include/src/libs/freetype
INCLUDE += -I$(THIRD_PARTY_INSTALL_DIR)/iniparser/include
INCLUDE += -I$(THIRD_PARTY_INSTALL_DIR)/cjson/include
INCLUDE += -I$(PROJECT_DIR)/include
PROJECT_GEN_INCLUDE_DIR ?= $(PROJECT_DIR)/include
INCLUDE += -I$(PROJECT_GEN_INCLUDE_DIR)
# Prefer packages/common source headers over out/install/application/include (e.g. gui_camera.h).
INCLUDE += -I$(GUI_COMMON_DIR)/include
INCLUDE += -I$(APPLICATION_INSTALL_DIR)/include
INCLUDE += -I$(APPLICATION_INSTALL_DIR)/include/services/weather
# application/app_main/app_main.h (used by wearable_entry.c, etc.)
APP_MAIN_SRC_DIR := $(if $(strip $(SRC_APPLICATION_DIR)),$(SRC_APPLICATION_DIR)/app_main,$(abspath $(MODULE_ROOT)/../../app_main))
INCLUDE += -I$(APP_MAIN_SRC_DIR)
INCLUDE += -I$(LAUNCHER_DIR)
INCLUDE += -I$(FRAMEWORK_INSTALL_DIR)/include/ui/common
INCLUDE += $(patsubst %,-I%,$(ENABLED_APP_DIRS))

CFLAGS += -W
CFLAGS += -ffunction-sections -fdata-sections
CFLAGS += $(INCLUDE)

ifeq ($(CONFIG_FW_NETWORK), y)
CFLAGS += -DCONFIG_NETWORK_SUPPORTED=1
	ifeq ($(CONFIG_FW_NETWORK_WIFI), y)
	CFLAGS += -DCONFIG_NETWORK_WLAN=1
	CFLAGS += -DCONFIG_FW_NETWORK_WIFI=1
	endif
endif

# app_product_config.h enables the camera GUI (APP_GUI_CAMERA=1) only when
# CONFIG_APP_WEARABLE_CAMERA is a defined preprocessor macro. config.mk sets it and the
# generated build/project/config.h carries it; sources include
# config.h explicitly, so there is no longer a config.h name collision
# with system/core's iniparser config.h. These explicit -D passes are kept as a
# harmless belt-and-suspenders (config.h #ifndef-guards each macro).
ifeq ($(CONFIG_APP_WEARABLE_CAMERA), y)
CFLAGS += -DCONFIG_APP_WEARABLE_CAMERA=1
endif

# Dashcam switches (kept explicit; config.h also defines them via include).
# Sub-switches are nested under the master so they only take effect when
# CONFIG_APP_WEARABLE_DASHCAM=y (mirrors cfg_master_off_skip in build.sh).
ifeq ($(CONFIG_APP_WEARABLE_DASHCAM), y)
CFLAGS += -DCONFIG_APP_WEARABLE_DASHCAM=1
ifeq ($(CONFIG_APP_WEARABLE_DASHCAM_PREVIEW), y)
CFLAGS += -DCONFIG_APP_WEARABLE_DASHCAM_PREVIEW=1
endif
ifeq ($(CONFIG_APP_WEARABLE_DASHCAM_AUTOSTART), y)
CFLAGS += -DCONFIG_APP_WEARABLE_DASHCAM_AUTOSTART=1
endif
endif

# luna camera sources (e.g. wearable_camera_streams.c) include config.h and use
# CONFIG_LCD_HOR_RES/VER_RES directly. Pass them from config.mk as well so they resolve.
ifneq ($(strip $(CONFIG_LCD_HOR_RES)),)
CFLAGS += -DCONFIG_LCD_HOR_RES=$(CONFIG_LCD_HOR_RES)
endif
ifneq ($(strip $(CONFIG_LCD_VER_RES)),)
CFLAGS += -DCONFIG_LCD_VER_RES=$(CONFIG_LCD_VER_RES)
endif

$(shell mkdir -p $(BUILD_DIR))

.PHONY: all target app_sos power_keyd clean install

all: target app_sos power_keyd install

target: $(OBJS)
	@echo "Compiled $(words $(OBJS)) object files for packages/wearable"

$(LAUNCHER_OBJ_DIR)/%.o: $(MODULE_ROOT)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDE) -c $< -o $@

$(LAUNCHER_OBJ_DIR)/%.o: $(MODULE_ROOT)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDE) -c $< -o $@

$(GUI_FONT_OBJ): $(GUI_COMMON_DIR)/gui_font.c $(GUI_COMMON_DIR)/include/gui_font.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(GUI_COMMON_OBJ): $(GUI_COMMON_DIR)/gui_common.c $(GUI_COMMON_DIR)/include/gui_common.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

ifeq ($(CONFIG_APP_WEARABLE_CAMERA),y)
$(TFCARD_OBJ): $(MODULE_ROOT)/camera/wearable_camera_tfcard.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDE) -c $< -o $@
endif

app_sos: $(APP_SO_TARGETS)

power_keyd: $(POWER_KEYD_TARGETS)

ifeq ($(CONFIG_APP_LUNA_POWER_KEYD),y)
# Standalone executable: strip freestanding flags inherited from GUI .so builds.
POWER_KEYD_CFLAGS := $(filter-out -nostdlib -fno-builtin,$(CFLAGS))
POWER_KEYD_LIBS := $(shell find $(THIRD_PARTY_INSTALL_DIR)/cjson/lib -maxdepth 1 -name "*.a" 2>/dev/null)
$(POWER_KEYD_BUILD): $(POWER_KEYD_SRC)
	@mkdir -p $(dir $@)
	$(CC) $(POWER_KEYD_CFLAGS) -Wall -O2 -o $@ $< $(POWER_KEYD_LIBS) -lc -lgcc
endif

$(APP_SO_BUILD_DIR)/camera/obj/%.o: $(MODULE_ROOT_NORM)/camera/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fPIC -I$(GUI_COMMON_DIR)/include -c $< -o $@

$(APP_SO_BUILD_DIR)/camera/obj/gui_common.o: $(GUI_COMMON_DIR)/gui_common.c $(GUI_COMMON_DIR)/include/gui_common.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fPIC -c $< -o $@

$(APP_SO_BUILD_DIR)/camera/obj/gui_camera.o: $(GUI_COMMON_DIR)/gui_camera.c $(GUI_COMMON_DIR)/include/gui_camera.h
	@mkdir -p $(dir $@)
	$(CC) -I$(GUI_COMMON_DIR)/include $(CFLAGS) -fPIC -c $< -o $@

$(APP_SO_BUILD_DIR)/camera/obj/gui_font.o: $(GUI_COMMON_DIR)/gui_font.c $(GUI_COMMON_DIR)/include/gui_font.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fPIC -c $< -o $@

ifeq ($(CONFIG_APP_WEARABLE_CAMERA),y)
WEARABLE_CAMERA_APP_SRCS := $(filter-out $(MODULE_ROOT_NORM)/camera/wearable_camera_tfcard.c,$(wildcard $(MODULE_ROOT_NORM)/camera/*.c))
WEARABLE_CAMERA_APP_OBJS := $(patsubst $(MODULE_ROOT_NORM)/camera/%.c,$(APP_SO_BUILD_DIR)/camera/obj/%.o,$(WEARABLE_CAMERA_APP_SRCS))
$(APP_SO_BUILD_DIR)/camera/lib/$(APP_SO_FILE_camera): $(WEARABLE_CAMERA_APP_OBJS) $(APP_SO_BUILD_DIR)/camera/obj/gui_common.o $(APP_SO_BUILD_DIR)/camera/obj/gui_camera.o $(APP_SO_BUILD_DIR)/camera/obj/gui_font.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $^ -shared -fPIC -o $@ $(LDFLAGS)
endif

$(APP_SO_BUILD_DIR)/dashcam/obj/%.o: $(MODULE_ROOT_NORM)/dashcam/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fPIC -I$(GUI_COMMON_DIR)/include -c $< -o $@

$(APP_SO_BUILD_DIR)/dashcam/obj/gui_common.o: $(GUI_COMMON_DIR)/gui_common.c $(GUI_COMMON_DIR)/include/gui_common.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fPIC -c $< -o $@

$(APP_SO_BUILD_DIR)/dashcam/obj/gui_font.o: $(GUI_COMMON_DIR)/gui_font.c $(GUI_COMMON_DIR)/include/gui_font.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fPIC -c $< -o $@

ifeq ($(CONFIG_APP_WEARABLE_DASHCAM),y)
# Resolve gateway rtsp_protocol symbols at link time; dlopen(libdashcam_app) needs
# DT_NEEDED or relocation fails on musl/uClibc ld.
DASHCAM_SO_LDFLAGS := -L$(FRAMEWORK_INSTALL_DIR)/lib -lrtsp_protocol -Wl,-rpath,/usr/lib
WEARABLE_DASHCAM_APP_SRCS := $(wildcard $(MODULE_ROOT_NORM)/dashcam/*.c)
WEARABLE_DASHCAM_APP_OBJS := $(patsubst $(MODULE_ROOT_NORM)/dashcam/%.c,$(APP_SO_BUILD_DIR)/dashcam/obj/%.o,$(WEARABLE_DASHCAM_APP_SRCS))
DASHCAM_SO_EXTRA_OBJS :=
ifeq ($(CONFIG_APP_WEARABLE_DASHCAM_PREVIEW),y)
DASHCAM_SO_EXTRA_OBJS += $(APP_SO_BUILD_DIR)/dashcam/obj/gui_camera.o
endif
$(APP_SO_BUILD_DIR)/dashcam/lib/$(APP_SO_FILE_dashcam): $(WEARABLE_DASHCAM_APP_OBJS) $(APP_SO_BUILD_DIR)/dashcam/obj/gui_common.o $(APP_SO_BUILD_DIR)/dashcam/obj/gui_font.o $(DASHCAM_SO_EXTRA_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $^ -shared -fPIC -o $@ $(LDFLAGS) $(DASHCAM_SO_LDFLAGS)
endif

$(APP_SO_BUILD_DIR)/dashcam/obj/gui_camera.o: $(GUI_COMMON_DIR)/gui_camera.c $(GUI_COMMON_DIR)/include/gui_camera.h
	@mkdir -p $(dir $@)
	$(CC) -I$(GUI_COMMON_DIR)/include $(CFLAGS) -fPIC -c $< -o $@

install:
	@mkdir -p $(APPLICATION_INSTALL_DIR)/include
	@if [ -f $(GUI_COMMON_DIR)/include/gui_font.h ]; then \
		cp -f $(GUI_COMMON_DIR)/include/gui_font.h $(APPLICATION_INSTALL_DIR)/include/; \
	fi
	@if [ -f $(GUI_COMMON_DIR)/include/gui_common.h ]; then \
		cp -f $(GUI_COMMON_DIR)/include/gui_common.h $(APPLICATION_INSTALL_DIR)/include/; \
	fi
	@if [ -f $(GUI_COMMON_DIR)/include/gui_camera.h ]; then \
		cp -f $(GUI_COMMON_DIR)/include/gui_camera.h $(APPLICATION_INSTALL_DIR)/include/; \
	fi
	@mkdir -p $(APPLICATION_INSTALL_DIR)/temp
	@find $(MODULE_ROOT) -type d -name "include" | xargs -i cp -rf {} $(APPLICATION_INSTALL_DIR)/temp/
	@mv $(APPLICATION_INSTALL_DIR)/temp/include/* $(APPLICATION_INSTALL_DIR)/include/ 2>/dev/null || true
	@rm -rf $(APPLICATION_INSTALL_DIR)/temp
	@if [ -f $(MODULE_ROOT)/camera/res/icon.png ]; then \
		mkdir -p $(RESOURCE_INSTALL_DIR)/gui/image/apps; \
		cp -f $(MODULE_ROOT)/camera/res/icon.png $(RESOURCE_INSTALL_DIR)/gui/image/apps/camera.png; \
	fi
	@if [ "$(CONFIG_APP_WEARABLE_DASHCAM)" = "y" ] && [ -f $(MODULE_ROOT)/dashcam/res/icon.png ]; then \
		mkdir -p $(RESOURCE_INSTALL_DIR)/gui/image/apps; \
		cp -f $(MODULE_ROOT)/dashcam/res/icon.png $(RESOURCE_INSTALL_DIR)/gui/image/apps/dashcam.png; \
	fi
	@# Weather icons (same assets as Nova tablet launcher) → /usr/gui/image/weather/
	@# weather_service uses paths like "weather/0_2x.png"; gui_image_set_src resolves under RES_IMAGE_PATH.
	@if [ "$(CONFIG_APP_WEATHER)" = "y" ]; then \
		res_tag="$(CONFIG_LCD_HOR_RES)x$(CONFIG_LCD_VER_RES)"; \
		weather_src=""; \
		if [ -d $(MODULE_ROOT)/launcher/res/image/$$res_tag/weather ]; then \
			weather_src="$(MODULE_ROOT)/launcher/res/image/$$res_tag/weather"; \
		elif [ -d $(MODULE_ROOT)/launcher/res/image/weather ]; then \
			weather_src="$(MODULE_ROOT)/launcher/res/image/weather"; \
		elif [ -d $(GUI_COMMON_DIR)/../tablet/launcher/res/image/$$res_tag/weather ]; then \
			weather_src="$(GUI_COMMON_DIR)/../tablet/launcher/res/image/$$res_tag/weather"; \
		elif [ -d $(GUI_COMMON_DIR)/../tablet/launcher/res/image/800x480/weather ]; then \
			weather_src="$(GUI_COMMON_DIR)/../tablet/launcher/res/image/800x480/weather"; \
		elif [ -d $(GUI_COMMON_DIR)/../tablet/launcher/res/image/1024x600/weather ]; then \
			weather_src="$(GUI_COMMON_DIR)/../tablet/launcher/res/image/1024x600/weather"; \
		fi; \
		if [ -n "$$weather_src" ]; then \
			mkdir -p $(RESOURCE_INSTALL_DIR)/gui/image/weather; \
			cp -rf $$weather_src/. $(RESOURCE_INSTALL_DIR)/gui/image/weather/; \
			echo "Installed weather icons from $$weather_src -> $(RESOURCE_INSTALL_DIR)/gui/image/weather"; \
		else \
			echo "WARN: CONFIG_APP_WEATHER=y but no weather icon directory found"; \
		fi; \
	fi
	@# Simple 240x240 wallpaper (not Nova clock dial) → /usr/gui/image/clock/
	@if [ -f $(MODULE_ROOT)/launcher/res/image/clock/wallpaper.png ]; then \
		mkdir -p $(RESOURCE_INSTALL_DIR)/gui/image/clock; \
		cp -f $(MODULE_ROOT)/launcher/res/image/clock/wallpaper.png \
			$(RESOURCE_INSTALL_DIR)/gui/image/clock/wallpaper.png; \
		rm -f $(RESOURCE_INSTALL_DIR)/gui/image/clock/clock_bg.png; \
		echo "Installed clock wallpaper -> $(RESOURCE_INSTALL_DIR)/gui/image/clock/"; \
	else \
		echo "WARN: wearable wallpaper.png missing under launcher/res/image/clock/"; \
	fi
	@# Status icons (WiFi etc.) → /usr/gui/image/status/
	@if [ -d $(MODULE_ROOT)/launcher/res/image/status ]; then \
		mkdir -p $(RESOURCE_INSTALL_DIR)/gui/image/status; \
		cp -rf $(MODULE_ROOT)/launcher/res/image/status/. \
			$(RESOURCE_INSTALL_DIR)/gui/image/status/; \
		echo "Installed status icons -> $(RESOURCE_INSTALL_DIR)/gui/image/status/"; \
	fi
	@if [ -n "$(ENABLED_APP_DIRS)" ]; then \
		mkdir -p $(APPS_INSTALL_DIR); \
		for d in $(ENABLED_APP_DIRS); do \
			app_id=$$(basename $$d); \
			rm -rf $(APPLICATION_INSTALL_DIR)/$$app_id; \
			mkdir -p $(APPLICATION_INSTALL_DIR)/$$app_id/lib; \
			if [ -f $$d/app.json ]; then cp -f $$d/app.json $(APPLICATION_INSTALL_DIR)/$$app_id/; fi; \
			if [ -d $$d/res ]; then cp -rf $$d/res $(APPLICATION_INSTALL_DIR)/$$app_id/; fi; \
			if [ "$$app_id" = "camera" ] && [ -f $(APP_SO_BUILD_DIR)/$$app_id/lib/$(APP_SO_FILE_camera) ]; then \
				cp -f $(APP_SO_BUILD_DIR)/$$app_id/lib/$(APP_SO_FILE_camera) $(APPLICATION_INSTALL_DIR)/$$app_id/lib/; fi; \
			if [ "$$app_id" = "dashcam" ] && [ -f $(APP_SO_BUILD_DIR)/$$app_id/lib/$(APP_SO_FILE_dashcam) ]; then \
				cp -f $(APP_SO_BUILD_DIR)/$$app_id/lib/$(APP_SO_FILE_dashcam) $(APPLICATION_INSTALL_DIR)/$$app_id/lib/; fi; \
			find $(APPLICATION_INSTALL_DIR)/$$app_id -type f \( -name "*.c" -o -name "*.h" \) -delete 2>/dev/null || true; \
			rm -rf $(APPS_INSTALL_DIR)/$$app_id; \
			mkdir -p $(APPS_INSTALL_DIR)/$$app_id; \
			if [ -f $$d/app.json ]; then cp -f $$d/app.json $(APPS_INSTALL_DIR)/$$app_id/; fi; \
			if [ -d $$d/res ]; then cp -rf $$d/res $(APPS_INSTALL_DIR)/$$app_id/; fi; \
			if [ -d $(APP_SO_BUILD_DIR)/$$app_id/lib ]; then cp -rf $(APP_SO_BUILD_DIR)/$$app_id/lib $(APPS_INSTALL_DIR)/$$app_id/; fi; \
		done; \
	fi
	@if [ "$(CONFIG_APP_LUNA_POWER_KEYD)" = "y" ] && [ -f $(POWER_KEYD_BUILD) ]; then \
		mkdir -p $(ROOTFS_USR_SBIN); \
		cp -f $(POWER_KEYD_BUILD) $(ROOTFS_USR_SBIN)/$(POWER_KEYD_BIN); \
		chmod 755 $(ROOTFS_USR_SBIN)/$(POWER_KEYD_BIN); \
		echo "Installed $(POWER_KEYD_BIN) -> $(ROOTFS_USR_SBIN)/"; \
	fi

clean:
	$(RM) -rf $(OBJS)
	$(RM) -rf $(BUILD_DIR)
