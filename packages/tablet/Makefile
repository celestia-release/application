#
# Makefile for application/packages/tablet module (shared by anypi_nova & anypi_nova_lite)
#
# This file carries NO app-specific information. Apps are discovered from the
# directory tree itself:
#   - any folder with an app.json is a candidate app;
#   - apps are enabled by default; product config.mk opts out with
#     CONFIG_APP_TABLET_<ID> = n (so a newly generated app needs no edits
#     outside its own directory);
#   - the .so name comes from the app's own app.json (entry_so basename);
#   - per-app build tweaks (extra include dirs, link libs, extra objects,
#     filtered sources) live in the app's own app.mk.
#
include $(COMPILATION_RULES)
# Use FRAMEWORK_CONFIG_FILE when building from application layer, else PROJECT_CONFIG_FILE
FRAMEWORK_OR_PROJECT := $(or $(FRAMEWORK_CONFIG_FILE),$(PROJECT_CONFIG_FILE))
include $(FRAMEWORK_OR_PROJECT)

PROJ = app_gui
MODULE_ROOT ?= $(shell pwd)
MODULE_ROOT_NORM := $(subst \,/,$(MODULE_ROOT))
SRC_DIR := $(MODULE_ROOT)
ROOTFS_DIR ?= $(SOC_PLATFORM_DIR)/rootfs
LAUNCHER_DIR := $(MODULE_ROOT)/launcher

# Candidate apps: every folder here that carries an app.json.
APP_DIRS := $(shell for d in $(MODULE_ROOT)/*; do if [ -f "$$d/app.json" ]; then echo "$$d"; fi; done)
tablet_uc = $(shell echo $(1) | tr '[:lower:]' '[:upper:]')
ALL_APP_IDS := $(sort $(foreach d,$(APP_DIRS),$(notdir $(d))))
# Enabled apps: every candidate app is built unless the product config.mk opts
# out with CONFIG_APP_TABLET_<ID> = n. Default-on keeps a newly generated app
# self-contained in its own directory (no config.mk / Makefile edits needed).
ENABLED_APP_IDS := $(strip $(foreach id,$(ALL_APP_IDS),$(if $(filter n,$(CONFIG_APP_TABLET_$(call tablet_uc,$(id)))),,$(id))))
ENABLED_APP_DIRS := $(foreach id,$(ENABLED_APP_IDS),$(MODULE_ROOT)/$(id))

BUILD_DIR ?= $(MODULE_ROOT)/build
LAUNCHER_OBJ_DIR := $(BUILD_DIR)/launcher_obj
APP_SO_BUILD_DIR := $(BUILD_DIR)/app_sos

# Basename of entry_so from the app's own app.json (lib/libfoo_app.so -> libfoo_app.so),
# with a lib<id>_app.so convention fallback.
app_so_base = $(shell sed -n 's/.*"entry_so"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' $(MODULE_ROOT)/$(1)/app.json 2>/dev/null | sed 's|.*/||')
app_so_name = $(if $(call app_so_base,$(1)),$(call app_so_base,$(1)),lib$(1)_app.so)

# Apps with root sources get compile+link rules generated here. Apps with no
# root *.c are external: either a prebuilt runtime package (install falls back
# to the lib/ shipped in the app dir) or an external build delegating from its
# own app.mk (which appends its own APP_SO_TARGETS entry).
APP_SO_TARGETS := $(foreach id,$(ENABLED_APP_IDS),$(if $(wildcard $(MODULE_ROOT_NORM)/$(id)/*.c),$(APP_SO_BUILD_DIR)/$(id)/lib/$(call app_so_name,$(id)),))

# Launcher .c live in launcher/; bitmap fonts only under res/font/ (not legacy fonts/).
# app_main drops */launcher/fonts/* at link time, so those objects never define lv_font_home_*.
CSRCS = $(shell find $(LAUNCHER_DIR) -maxdepth 2 -name *.c)
CSRCS := $(filter-out $(wildcard $(LAUNCHER_DIR)/fonts/*.c),$(CSRCS))
CSRCS += $(wildcard $(LAUNCHER_DIR)/res/font/lv_font_home_*.c)
# When linked into ipc_main (library mode), exclude tablet_main.c to avoid duplicate main symbol
ifeq ($(TABLET_EXCLUDE_MAIN),1)
CSRCS := $(filter-out $(LAUNCHER_DIR)/tablet_main.c,$(CSRCS))
endif

# In SO-based app system, app entry symbols live in per-app shared libraries.
# The launcher carries no direct tablet_gui / app-level dependencies.

# Exclude demo modules that are disabled in app_globle_config.h.
# These exclusions read CONFIG_* values exported from config.mk / app_globle_config.h.
# Format: CONFIG_APP_TABLET_LV_DEMO_XXX=n in config.mk disables the corresponding .c file.
# LV_DEMO_QWEN: not present in app_globle_config.h (=0), exclude unless explicitly enabled.
ifneq ($(CONFIG_APP_TABLET_LV_DEMO_QWEN),y)
CSRCS := $(filter-out $(LAUNCHER_DIR)/tablet_qwen.c,$(CSRCS))
endif
# LV_DEMO_MUSIC / VIDEO: off in app_globle_config.h; no corresponding .c but guard here for safety.
ifneq ($(CONFIG_APP_TABLET_LV_DEMO_MUSIC),y)
CSRCS := $(filter-out $(LAUNCHER_DIR)/tablet_music.c,$(CSRCS))
endif
ifneq ($(CONFIG_APP_TABLET_LV_DEMO_VIDEO),y)
CSRCS := $(filter-out $(LAUNCHER_DIR)/tablet_video.c,$(CSRCS))
endif

CXXSRCS = $(shell find $(LAUNCHER_DIR) -maxdepth 2 -name *.cpp)

OBJS := $(patsubst $(MODULE_ROOT)/%.c,$(LAUNCHER_OBJ_DIR)/%.o,$(CSRCS))
OBJS += $(patsubst $(MODULE_ROOT)/%.cpp,$(LAUNCHER_OBJ_DIR)/%.o,$(CXXSRCS))

RESOURCE_INSTALL_DIR := $(ROOTFS_DIR)/utils/usr
DATA_RESOURCE_INSTALL_DIR := $(ROOTFS_DIR)/utils/usr/data
APPS_INSTALL_DIR := $(ROOTFS_DIR)/utils/usr/apps

# No static library target - only compile objects and install headers/resources
TARGET := $(OBJS)

# Prefer system/core source headers so new APIs resolve before out/install is refreshed.
SYSTEM_CORE_SRC := $(if $(strip $(SRC_SYSTEM_CORE_DIR)),$(SRC_SYSTEM_CORE_DIR),$(abspath $(MODULE_ROOT)/../../../system/core))
INCLUDE += -I$(SYSTEM_CORE_SRC)/include

INCLUDE += -I$(FRAMEWORK_INSTALL_DIR)/include
INCLUDE += -I$(FRAMEWORK_INSTALL_DIR)/include/gateway
INCLUDE += -I$(FRAMEWORK_INSTALL_DIR)/include/runtime
INCLUDE += -I$(SYSTEM_CORE_INSTALL_DIR)/include
INCLUDE += -I$(FRAMEWORK_INSTALL_DIR)/include/services/multimedia
INCLUDE += -I$(SOC_PLATFORM_INSTALL_DIR)/include
INCLUDE += -I$(HARDWARE_INSTALL_DIR)/include
INCLUDE += -I$(FRAMEWORK_INSTALL_DIR)/include/services/device
INCLUDE += -I$(FRAMEWORK_INSTALL_DIR)/include/ui
INCLUDE += -I$(FRAMEWORK_INSTALL_DIR)/include/services/network
INCLUDE += -I$(FRAMEWORK_INSTALL_DIR)/include/rtsp/rtsp_server
INCLUDE += -I$(FRAMEWORK_INSTALL_DIR)/include/services/notification
INCLUDE += -I$(THIRD_PARTY_INSTALL_DIR)/freetype/include/freetype2
INCLUDE += -I$(THIRD_PARTY_INSTALL_DIR)/lvgl/include
INCLUDE += -I$(THIRD_PARTY_INSTALL_DIR)/lvgl/include/src/libs/freetype
INCLUDE += -I$(THIRD_PARTY_INSTALL_DIR)/iniparser/include
INCLUDE += -I$(THIRD_PARTY_INSTALL_DIR)/cjson/include
INCLUDE += -I$(THIRD_PARTY_INSTALL_DIR)/tslib/include
INCLUDE += -I$(THIRD_PARTY_INSTALL_DIR)/zxing/include
INCLUDE += -I$(PROJECT_DIR)/include
PROJECT_GEN_INCLUDE_DIR ?= $(PROJECT_DIR)/include
INCLUDE += -I$(PROJECT_GEN_INCLUDE_DIR)
# project/<product>/include/config.h holds CONFIG_* macros. system/core INI API is
# system_config.h (renamed to avoid colliding with this product config.h).
INCLUDE += -I$(APPLICATION_INSTALL_DIR)/include
INCLUDE += -I$(LAUNCHER_DIR)
INCLUDE += $(patsubst %,-I%,$(ENABLED_APP_DIRS))
INCLUDE += -I$(SRC_APPLICATION_DIR)/app_main
# volc_engine_app.h for LV_DEMO_VOLC (event_callback_menu_volc)
INCLUDE += -I$(SRC_APPLICATION_DIR)/cloud/volc_engine/include

CFLAGS += -W
# Per-symbol sections: required for --gc-sections in launcher to strip unused nova code.
CFLAGS += -ffunction-sections -fdata-sections
# Force-include the generated config.h so every nova source sees CONFIG_* macros
# globally (some entry sources don't include it explicitly). system/core's iniparser
# API lives in system_config.h, so there is no name collision with this project config.h.
CFLAGS += -include $(PROJECT_GEN_INCLUDE_DIR)/config.h

# Align with project config: CONFIG_FW_NETWORK* (fallback to CONFIG_NETWORK/CONFIG_WLAN/CONFIG_ETHERNET)
ifeq ($(CONFIG_FW_NETWORK), y)
CFLAGS += -DCONFIG_NETWORK_SUPPORTED=1
	ifeq ($(CONFIG_FW_NETWORK_WIFI), y)
	CFLAGS += -DCONFIG_NETWORK_WLAN=1
	CFLAGS += -DCONFIG_FW_NETWORK_WIFI=1
	else ifeq ($(CONFIG_FW_NETWORK_ETHERNET), y)
	CFLAGS += -DCONFIG_NETWORK_ETHERNET=1
	endif
else ifeq ($(CONFIG_NETWORK), y)
CFLAGS += -DCONFIG_NETWORK_SUPPORTED=1
	ifeq ($(CONFIG_WLAN), y)
	CFLAGS += -DCONFIG_NETWORK_WLAN=1
	CFLAGS += -DCONFIG_FW_NETWORK_WIFI=1
	else ifeq ($(CONFIG_ETHERNET), y)
	CFLAGS += -DCONFIG_NETWORK_ETHERNET=1
	endif
endif

# Project CFLAGS from config.mk (CONFIG_PROJECT_CFLAGS, USB storage tab, etc.)
CFLAGS += $(CONFIG_PROJECT_CFLAGS)

# Weather service header (libweather_service.a linked via app_main); gate matches CONFIG_APP_WEATHER
ifeq ($(CONFIG_APP_WEATHER), y)
INCLUDE += -I$(SRC_APPLICATION_DIR)/services/weather/include
INCLUDE += -I$(APPLICATION_INSTALL_DIR)/include/services/weather
endif

CFLAGS += $(INCLUDE)

# Per-app build tweaks (extra include dirs, link libs, extra objects, filtered
# sources) load from each enabled app's own app.mk. Keep this file generic.
-include $(foreach id,$(ENABLED_APP_IDS),$(MODULE_ROOT)/$(id)/app.mk)

$(shell mkdir -p $(BUILD_DIR))

.PHONY: all target app_sos clean launcher_prune_legacy_font_objs

all: target app_sos install

# Stale launcher/fonts/*.o are excluded from ipc_main and duplicate res/font symbols if linked elsewhere.
launcher_prune_legacy_font_objs:
	@$(RM) -rf $(LAUNCHER_OBJ_DIR)/launcher/fonts

target: launcher_prune_legacy_font_objs $(OBJS)
	@if [ -z "$(OBJS)" ]; then \
		echo "ERROR: No object files found! Check source files in $(SRC_DIR)"; \
		echo "CSRCS: $(CSRCS)"; \
		echo "CXXSRCS: $(CXXSRCS)"; \
		exit 1; \
	fi
	@echo "Compiled $(words $(OBJS)) object files for packages/tablet"

$(LAUNCHER_OBJ_DIR)/%.o: $(MODULE_ROOT)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDE) -c $< -o $@

$(LAUNCHER_OBJ_DIR)/%.o: $(MODULE_ROOT)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDE) -c $< -o $@

# Build shared libraries for each independent app
app_sos: $(APP_SO_TARGETS)

# Compile app sources into per-app SO object directories (PIC). A GNU-make
# pattern may carry only one '%', so a two-stem generic rule cannot split
# app id and source name — each enabled app gets an explicit rule generated
# from the directory scan below instead. Apps needing extra include dirs set
# the target-specific APP_CFLAGS_EXTRA in their app.mk.

$(APP_SO_BUILD_DIR)/%/obj/tablet_common.o: $(LAUNCHER_DIR)/tablet_common.c $(LAUNCHER_DIR)/tablet_common.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fPIC -c $< -o $@

$(APP_SO_BUILD_DIR)/%/obj/tablet_font.o: $(LAUNCHER_DIR)/tablet_font.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fPIC -c $< -o $@

# One link rule per enabled app, generated from the app directory. Extra link
# libs, extra objects, and filtered sources are declared in the app's app.mk
# as <ID>_SHLIB_LIBS / <ID>_APP_EXTRA_OBJS / <ID>_SRCS_FILTER_OUT. Everything
# expands immediately during eval: all inputs (app.mk vars included) are
# defined before this line, so the rule needs no intermediate variables.
define TABLET_APP_SO_RULE
# Parse-time sanity: an app with entry_so must be one of
#   - source-built:     root *.c sources present (or <ID>_APP_EXTRA_OBJS);
#   - prebuilt package: ships its own lib/<entry_so basename> (install stages it);
#   - external build:   its app.mk declares <ID>_APP_EXTERNAL_BUILD and owns
#                        the build + APP_SO_TARGETS entry.
# Anything else would "link" an SO holding only launcher common objects,
# lacking app_entry_*, and show "app entry not found" on the device — fail
# here instead of shipping that broken SO silently.
$(if $(call app_so_base,$(1)),\
  $(if $(strip $(wildcard $(MODULE_ROOT_NORM)/$(1)/*.c)$($(call tablet_uc,$(1))_APP_EXTRA_OBJS)$(wildcard $(MODULE_ROOT_NORM)/$(1)/lib/$(call app_so_base,$(1)))$($(call tablet_uc,$(1))_APP_EXTERNAL_BUILD)),,\
    $(error tablet app '$(1)' has entry_so=$(call app_so_base,$(1)) but no .c sources, no prebuilt lib/$(call app_so_base,$(1)), no $(call tablet_uc,$(1))_APP_EXTRA_OBJS, and no $(call tablet_uc,$(1))_APP_EXTERNAL_BUILD; refusing to link an SO without app_entry_*)))
$(if $(wildcard $(MODULE_ROOT_NORM)/$(1)/*.c),\
$(APP_SO_BUILD_DIR)/$(1)/obj/%.o: $(MODULE_ROOT_NORM)/$(1)/%.c
	@mkdir -p $$(dir $$@)
	$(CC) $(CFLAGS) -fPIC $$(APP_CFLAGS_EXTRA) -I$(LAUNCHER_DIR) -c $$< -o $$@
$(APP_SO_BUILD_DIR)/$(1)/lib/$(call app_so_name,$(1)): $(patsubst $(MODULE_ROOT_NORM)/$(1)/%.c,$(APP_SO_BUILD_DIR)/$(1)/obj/%.o,$(strip $(filter-out $($(call tablet_uc,$(1))_SRCS_FILTER_OUT),$(wildcard $(MODULE_ROOT_NORM)/$(1)/*.c)))) $($(call tablet_uc,$(1))_APP_EXTRA_OBJS) $(APP_SO_BUILD_DIR)/$(1)/obj/tablet_common.o $(APP_SO_BUILD_DIR)/$(1)/obj/tablet_font.o
	@mkdir -p $$(dir $$@)
	$(CXX) $(CXXFLAGS) $$^ -shared -fPIC -o $$@ $(LDFLAGS) $$($(call tablet_uc,$(1))_SHLIB_LIBS)
,)
endef
$(foreach id,$(ENABLED_APP_IDS),$(eval $(call TABLET_APP_SO_RULE,$(id))))

install:
	@mkdir -p $(APPLICATION_INSTALL_DIR)/include
	@mkdir -p $(APPLICATION_INSTALL_DIR)/temp
	@echo "Installing headers from packages/tablet"
	@find $(MODULE_ROOT) -type d -name "include" | xargs -i cp -rf {} $(APPLICATION_INSTALL_DIR)/temp/
	@mv $(APPLICATION_INSTALL_DIR)/temp/include/* $(APPLICATION_INSTALL_DIR)/include/ 2>/dev/null || true
	@rm -rf $(APPLICATION_INSTALL_DIR)/temp
	@mkdir -p $(RESOURCE_INSTALL_DIR)/gui/font
	@if [ -d $(LAUNCHER_DIR)/res/font ]; then \
		cp -rf $(LAUNCHER_DIR)/res/font/* $(RESOURCE_INSTALL_DIR)/gui/font; \
	fi
	@# Launcher wallpaper + weather icons → /usr/gui/image (flash) and/or install pack source.
	@# Layout: res/image/<WxH>/{icon,weather}/... flattened to gui/image/{icon,weather}/...
	@if [ -d $(LAUNCHER_DIR)/res/image ]; then \
		res_tag="$(CONFIG_LCD_HOR_RES)x$(CONFIG_LCD_VER_RES)"; \
		mkdir -p $(RESOURCE_INSTALL_DIR)/gui/image; \
		if [ -d $(LAUNCHER_DIR)/res/image/$$res_tag ]; then \
			cp -rf $(LAUNCHER_DIR)/res/image/$$res_tag/. $(RESOURCE_INSTALL_DIR)/gui/image/; \
			echo "Installed launcher images from res/image/$$res_tag -> $(RESOURCE_INSTALL_DIR)/gui/image"; \
		else \
			cp -rf $(LAUNCHER_DIR)/res/image/. $(RESOURCE_INSTALL_DIR)/gui/image/; \
			echo "Installed launcher images from res/image -> $(RESOURCE_INSTALL_DIR)/gui/image"; \
		fi; \
	fi
	@if [ -n "$(ENABLED_APP_DIRS)" ]; then \
		mkdir -p $(APPS_INSTALL_DIR); \
		for d in $(ENABLED_APP_DIRS); do \
			app_id=$$(basename $$d); \
			rm -rf $(APPLICATION_INSTALL_DIR)/$$app_id; \
			mkdir -p $(APPLICATION_INSTALL_DIR)/$$app_id/lib; \
			if [ -f $$d/app.json ]; then cp -f $$d/app.json $(APPLICATION_INSTALL_DIR)/$$app_id/; fi; \
			if [ -d $$d/res ]; then cp -rf $$d/res $(APPLICATION_INSTALL_DIR)/$$app_id/; fi; \
			so_file=""; \
			if [ -f $$d/app.json ]; then \
				so_file=`sed -n 's/.*"entry_so"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' $$d/app.json | sed 's|.*/||'`; \
			fi; \
			[ -n "$$so_file" ] || so_file="lib$${app_id}_app.so"; \
			if [ ! -f $(APP_SO_BUILD_DIR)/$$app_id/lib/$$so_file ] && [ -f $$d/lib/$$so_file ]; then \
				mkdir -p $(APP_SO_BUILD_DIR)/$$app_id/lib; \
				cp -f $$d/lib/$$so_file $(APP_SO_BUILD_DIR)/$$app_id/lib/; \
			fi; \
			if [ -f $(APP_SO_BUILD_DIR)/$$app_id/lib/$$so_file ]; then cp -f $(APP_SO_BUILD_DIR)/$$app_id/lib/$$so_file $(APPLICATION_INSTALL_DIR)/$$app_id/lib/; fi; \
			if [ -d $(APP_SO_BUILD_DIR)/$$app_id/data ]; then cp -rf $(APP_SO_BUILD_DIR)/$$app_id/data/. $(APPLICATION_INSTALL_DIR)/$$app_id/; fi; \
			find $(APPLICATION_INSTALL_DIR)/$$app_id -type f \( -name "*.c" -o -name "*.h" \) -delete 2>/dev/null || true; \
			rm -rf $(APPS_INSTALL_DIR)/$$app_id; \
			mkdir -p $(APPS_INSTALL_DIR)/$$app_id; \
			mkdir -p ~/nfs/application/apps/$$app_id; \
			if [ -f $$d/app.json ]; then cp -f $$d/app.json $(APPS_INSTALL_DIR)/$$app_id/; cp -f $$d/app.json ~/nfs/application/apps/$$app_id/; fi; \
			if [ -d $$d/res ]; then cp -rf $$d/res $(APPS_INSTALL_DIR)/$$app_id/; cp -rf $$d/res ~/nfs/application/apps/$$app_id/; fi; \
			if [ -d $(APP_SO_BUILD_DIR)/$$app_id/lib ]; then cp -rf $(APP_SO_BUILD_DIR)/$$app_id/lib $(APPS_INSTALL_DIR)/$$app_id/; cp -rf $(APP_SO_BUILD_DIR)/$$app_id/lib ~/nfs/application/apps/$$app_id/; fi; \
			if [ -d $(APP_SO_BUILD_DIR)/$$app_id/data ]; then cp -rf $(APP_SO_BUILD_DIR)/$$app_id/data/. $(APPS_INSTALL_DIR)/$$app_id/; cp -rf $(APP_SO_BUILD_DIR)/$$app_id/data/. ~/nfs/application/apps/$$app_id/; fi; \
			find $(APPS_INSTALL_DIR)/$$app_id -type f \( -name "*.c" -o -name "*.h" \) -delete 2>/dev/null || true; \
		done; \
	fi

clean:
	$(RM) -rf $(OBJS)
	$(RM) -rf $(LAUNCHER_OBJ_DIR)/launcher/fonts
	$(RM) -rf $(BUILD_DIR)
