#
# Makefile for application/app_main - main executable (replaces tests as entry when CONFIG_APP_MAIN_ENTRY=y)
# TARGET name from CONFIG_APP_MAIN_TARGET (project config.mk).
#
# Resolve this file's dir before any includes (later MAKEFILE_LIST lastword may not be this file).
_APP_MAIN_MK_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
_CELESTIA_ROOT ?= $(abspath $(_APP_MAIN_MK_DIR)/../..)

# Standalone `make -C application/app_main` defaults (full build exports these via scripts/build.sh).
PRODUCT_NAME ?= anypi_luna
PROJECT_CONFIG_FILE ?= $(_CELESTIA_ROOT)/project/$(PRODUCT_NAME)/config.mk
COMPILATION_RULES ?= $(_CELESTIA_ROOT)/system/AnyCloud39AV100_V2/V2.04/rules.mk
INSTALL_DIR ?= $(_CELESTIA_ROOT)/out/$(PRODUCT_NAME)/install
FRAMEWORK_INSTALL_DIR ?= $(INSTALL_DIR)/framework
SYSTEM_CORE_INSTALL_DIR ?= $(INSTALL_DIR)/system/core
HARDWARE_INSTALL_DIR ?= $(INSTALL_DIR)/hardware
THIRD_PARTY_INSTALL_DIR ?= $(INSTALL_DIR)/third_party
APPLICATION_INSTALL_DIR ?= $(INSTALL_DIR)/application
SOC_PLATFORM_INSTALL_DIR ?= $(INSTALL_DIR)/system/AnyCloud39AV100_V2/V2.04
SRC_APPLICATION_DIR ?= $(_CELESTIA_ROOT)/application
SRC_DIR ?= $(_CELESTIA_ROOT)
BUILD_APPLICATION_DIR ?= $(_CELESTIA_ROOT)/out/$(PRODUCT_NAME)/build/application
SOC_PLATFORM_DIR ?= $(_CELESTIA_ROOT)/system/AnyCloud39AV100_V2/V2.04
PROJECT_DIR ?= $(_CELESTIA_ROOT)/project/$(PRODUCT_NAME)

include $(COMPILATION_RULES)
include $(PROJECT_CONFIG_FILE)

_CELESTIA_ROOT := $(abspath $(dir $(PROJECT_CONFIG_FILE))/../..)
# Rootfs staging paths (inlined; build.sh exports SOC_PLATFORM_BUILD_DIR/ROOTFS_DIR).
REL_SOC_PLATFORM_DIR ?= system/$(CONFIG_SOC_PLATFORM)/$(CONFIG_SOC_SDK_VERSION)
ifndef SOC_PLATFORM_BUILD_DIR
PRODUCT_NAME ?= $(notdir $(patsubst %/,%,$(dir $(PROJECT_CONFIG_FILE))))
_CELESTIA_ROOT ?= $(abspath $(dir $(PROJECT_CONFIG_FILE))/../..)
SOC_PLATFORM_BUILD_DIR := $(_CELESTIA_ROOT)/out/$(PRODUCT_NAME)/build/$(REL_SOC_PLATFORM_DIR)
endif
ROOTFS_DIR ?= $(SOC_PLATFORM_BUILD_DIR)/rootfs

MODULE_ROOT ?= $(shell pwd)
BUILD_DIR ?= $(MODULE_ROOT)/build
# Parent passes BUILD_DIR as .../app_main/ (obj-y trailing slash); goals use .../app_main/foo.o.
BUILD_DIR := $(patsubst %/,%,$(strip $(BUILD_DIR)))
# Framework-built .so under install (e.g. out/install/framework/lib); deployed to rootfs /usr/lib with board libs.
FRAMEWORK_SO_INSTALL_DIR ?= $(FRAMEWORK_INSTALL_DIR)/lib
ROOTFS_USR_LIB ?= $(ROOTFS_DIR)/utils/usr/lib
# Mirror (some packaging flows read rootfs/usr directly).
ROOTFS_USR_LIB_MIRROR ?= $(ROOTFS_DIR)/usr/lib
ROOTFS_USR_SBIN ?= $(ROOTFS_DIR)/utils/usr/sbin
ROOTFS_ETC_CONFIG ?= $(ROOTFS_DIR)/utils/etc/config

# Main binary name: prefer CONFIG_APP_MAIN_TARGET from project/<product>/config.mk.
# If unset, derive from project directory name: anypi_<suffix> → <suffix>_main
#   e.g. anypi_luna → luna_main, anypi_luna_dev → luna_dev_main, anypi_nova → nova_main
_PROJECT_NAME := $(notdir $(patsubst %/,%,$(dir $(abspath $(PROJECT_CONFIG_FILE)))))
_DEFAULT_MAIN_TARGET := $(patsubst anypi_%,%_main,$(_PROJECT_NAME))
ifeq ($(strip $(CONFIG_APP_MAIN_TARGET)),)
CONFIG_APP_MAIN_TARGET := $(_DEFAULT_MAIN_TARGET)
endif
TARGET ?= $(CONFIG_APP_MAIN_TARGET)
# Optimize knobs are controlled by project config.mk.
APP_MAIN_MINIMAL_LINK := $(CONFIG_APP_MAIN_MINIMAL_LINK)
APP_MAIN_SIZE_OPT := $(if $(strip $(CONFIG_APP_MAIN_SIZE_OPT)),$(CONFIG_APP_MAIN_SIZE_OPT),y)
APP_MAIN_DYNAMIC_CORE_SO := $(if $(strip $(CONFIG_APP_MAIN_DYNAMIC_CORE_SO)),$(CONFIG_APP_MAIN_DYNAMIC_CORE_SO),y)
APP_MAIN_EMBED_FFMPEG := $(CONFIG_APP_MAIN_EMBED_FFMPEG)
# Diagnostic: set to the .so name (e.g. liblvgl.so) to temporarily hide that single DSO
# and link its .a instead, verifying whether it causes "hidden symbol referenced by DSO".
# Set to "all" to test every framework/system_core/hardware DSO one-by-one automatically.
# Set to "n" (default) to skip verification.
APP_MAIN_VERIFY_DSO ?= n
ifeq ($(strip $(CONFIG_APP_MAIN_TARGET)),)
$(error CONFIG_APP_MAIN_TARGET is not set in $(PROJECT_CONFIG_FILE) and could not derive from $(_PROJECT_NAME))
endif
ifeq ($(TARGET),)
TARGET := $(CONFIG_APP_MAIN_TARGET)
endif

# Config artifacts directory (same path used by application/Makefile install); varies with PROJECT_CONFIG_FILE
PROJECT_CONFIG_DIR ?= $(dir $(PROJECT_CONFIG_FILE))config

SOURCES = main.c app_config.c app_main.c app_main_config.c
# Nova Montserrat trim (28/48 only): alias missing sizes so old plugin SOs can dlopen.
# Headless products (e.g. luna_dev) build without LVGL — skip this file.
ifeq ($(CONFIG_FW_GUI_LVGL),y)
SOURCES += app_main_montserrat_aliases.c
else ifeq ($(CONFIG_THIRD_PARTY_LVGL),y)
SOURCES += app_main_montserrat_aliases.c
endif
# Scheme B: wifi_event_* trampolines when -lnetwork is omitted from the link line.
ifeq ($(CONFIG_APP_MAIN_DEFER_HEAVY_SO),y)
SOURCES += app_main_net_stubs.c
# FreeType/libpng: not for Home; dlopen after Home (trampolines resolve link).
SOURCES += app_main_defer_gui_stubs.c
ifeq ($(CONFIG_APP_WEATHER),y)
# Weather: not linked as .a (would pull libcurl DT_NEEDED); stub dlopens libweather_service.so.
SOURCES += app_main_weather_stubs.c
ifneq ($(SRC_APPLICATION_DIR),)
INCLUDE += -I$(SRC_APPLICATION_DIR)/services/weather/include
else
INCLUDE += -I$(MODULE_ROOT)/../services/weather/include
endif
INCLUDE += -I$(APPLICATION_INSTALL_DIR)/include/services/weather
endif
else
# tablet_entry always calls app_main_load_deferred_gui_libs(); provide no-op when defer is off.
SOURCES += app_main_defer_gui_noop.c
endif
OBJS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(SOURCES))

GUI_MODULE :=
ifneq ($(CONFIG_APP_PACKAGES_MODULE),)
GUI_MODULE := $(CONFIG_APP_PACKAGES_MODULE)
else ifeq ($(CONFIG_APP_PACKAGES_TABLET), y)
GUI_MODULE := tablet
else ifeq ($(CONFIG_APP_PACKAGES_WEARABLE), y)
GUI_MODULE := wearable
# Legacy aliases from pre-packages layout (gui/nova, gui/luna)
else ifeq ($(CONFIG_APP_GUI_NOVA), y)
GUI_MODULE := tablet
else ifeq ($(CONFIG_APP_GUI_LUNA), y)
GUI_MODULE := wearable
else ifeq ($(CONFIG_APP_GUI_MODULE), nova)
GUI_MODULE := tablet
else ifeq ($(CONFIG_APP_GUI_MODULE), luna)
GUI_MODULE := wearable
endif
ifneq ($(GUI_MODULE),)
GUI_BUILD_DIR := $(BUILD_APPLICATION_DIR)/packages/$(GUI_MODULE)
ifneq ($(SRC_APPLICATION_DIR),)
INCLUDE += -I$(SRC_APPLICATION_DIR)/packages/$(GUI_MODULE)/launcher
else
INCLUDE += -I$(MODULE_ROOT)/../packages/$(GUI_MODULE)/launcher
endif
endif

LIBS = $(shell find $(THIRD_PARTY_INSTALL_DIR)/cjson/lib -maxdepth 1 -name "*.a" 2>/dev/null)
ifneq ($(APP_MAIN_DYNAMIC_CORE_SO),y)
LIBS += $(shell find $(SYSTEM_CORE_INSTALL_DIR)/lib -maxdepth 1 -name "libsystem_core.a" 2>/dev/null)
LIBS += $(shell find $(FRAMEWORK_INSTALL_DIR)/lib -maxdepth 1 -name "libruntime.a" 2>/dev/null)
LIBS += $(shell find $(FRAMEWORK_INSTALL_DIR)/lib -maxdepth 1 -name "libnotification.a" 2>/dev/null)
endif
# FFmpeg in ipc_main: default off when core is dynamic .so (decode lives in libmultimedia.so).
# Set CONFIG_APP_MAIN_EMBED_FFMPEG=y to force static ffmpeg into ipc_main.
FFMPEG_STATIC_IN_IPC := y
ifeq ($(APP_MAIN_DYNAMIC_CORE_SO),y)
ifneq ($(APP_MAIN_EMBED_FFMPEG),y)
FFMPEG_STATIC_IN_IPC := n
endif
endif
ifeq ($(FFMPEG_STATIC_IN_IPC),y)
ifneq ($(wildcard $(THIRD_PARTY_INSTALL_DIR)/ffmpeg/lib/libavutil.a),)
LIBS += $(THIRD_PARTY_INSTALL_DIR)/ffmpeg/lib/libavutil.a
endif
ifneq ($(wildcard $(THIRD_PARTY_INSTALL_DIR)/ffmpeg/lib/libavcodec.a),)
LIBS += $(THIRD_PARTY_INSTALL_DIR)/ffmpeg/lib/libavcodec.a
endif
ifneq ($(wildcard $(THIRD_PARTY_INSTALL_DIR)/ffmpeg/lib/libavformat.a),)
LIBS += $(THIRD_PARTY_INSTALL_DIR)/ffmpeg/lib/libavformat.a
endif
ifneq ($(wildcard $(THIRD_PARTY_INSTALL_DIR)/ffmpeg/lib/libswscale.a),)
LIBS += $(THIRD_PARTY_INSTALL_DIR)/ffmpeg/lib/libswscale.a
endif
ifneq ($(wildcard $(THIRD_PARTY_INSTALL_DIR)/ffmpeg/lib/libswresample.a),)
LIBS += $(THIRD_PARTY_INSTALL_DIR)/ffmpeg/lib/libswresample.a
endif
endif
# LVGL / graphics / freetype: same as Nova — resolved via framework + third_party static archives
# in the dynamic-core LIBS block below (not a Luna-only -llvgl link line).
ifneq ($(APP_MAIN_DYNAMIC_CORE_SO),y)
LIBS += $(shell find $(FRAMEWORK_INSTALL_DIR)/lib -maxdepth 1 -name "libmultimedia.a" 2>/dev/null)
endif
ifneq ($(APP_MAIN_DYNAMIC_CORE_SO),y)
ifneq ($(APP_MAIN_MINIMAL_LINK),y)
LIBS += $(shell find $(FRAMEWORK_INSTALL_DIR)/lib -maxdepth 3 -name "*.a" 2>/dev/null | grep -v libmultimedia.a | grep -v libruntime.a | grep -v libnotification.a)
LIBS += $(shell find $(HARDWARE_INSTALL_DIR)/lib -maxdepth 3 -name "*.a" 2>/dev/null)
LIBS += $(shell find $(APPLICATION_INSTALL_DIR)/lib -maxdepth 3 -name "*.a" 2>/dev/null)
endif
endif
# Volc RTC SDK (~10MB): only link into ipc_main when NOT deferring heavy SOs.
# With DEFER_HEAVY_SO, libvolc_engine_svc.so DT_NEEDED pulls VolcEngineRTCLite on Volc page open.
ifeq ($(CONFIG_APP_CLOUD_VOLC), y)
ifneq ($(CONFIG_APP_MAIN_DEFER_HEAVY_SO),y)
LIBS += $(shell find $(APPLICATION_INSTALL_DIR)/lib -maxdepth 3 -name "libVolcEngineRTCLite.so" 2>/dev/null)
endif
endif
ifneq ($(APP_MAIN_DYNAMIC_CORE_SO),y)
LIBS += $(shell find $(SOC_PLATFORM_INSTALL_DIR)/lib -maxdepth 3 -name "*.a" 2>/dev/null)
ifneq ($(APP_MAIN_MINIMAL_LINK),y)
LIBS += $(shell find $(THIRD_PARTY_INSTALL_DIR) -maxdepth 3 -name "*.a" 2>/dev/null | grep -v cjson | grep -v ffmpeg)
endif
else
# Dynamic ipc_main: static .a only for GUI/third-party; core modules already via -lhardware -lnetwork …
ifneq ($(APP_MAIN_MINIMAL_LINK),y)
LIBS += $(shell find $(FRAMEWORK_INSTALL_DIR)/lib -maxdepth 3 -name "*.a" 2>/dev/null | grep -v libmultimedia.a | grep -v libruntime.a | grep -v libnotification.a | grep -v libnetwork.a | grep -v libstorage.a)
LIBS += $(shell find $(HARDWARE_INSTALL_DIR)/lib -maxdepth 3 -name "*.a" 2>/dev/null | grep -v libhardware.a)
LIBS += $(shell find $(APPLICATION_INSTALL_DIR)/lib -maxdepth 3 -name "*.a" 2>/dev/null)
LIBS += $(shell find $(SOC_PLATFORM_INSTALL_DIR)/lib -maxdepth 3 -name "*.a" 2>/dev/null)
LIBS += $(shell find $(THIRD_PARTY_INSTALL_DIR) -maxdepth 3 -name "*.a" 2>/dev/null | grep -v cjson | grep -v ffmpeg)
endif
endif

# Some vendor SDK archives export TLS / hidden symbols that are unsafe to fold
# into the final executable when the same functionality is also referenced by
# DSOs like libhardware.so. Link those as shared-library deps instead.
APP_MAIN_TLS_UNSAFE_STATIC_LIBS := $(SOC_PLATFORM_INSTALL_DIR)/lib/libakaudiofilter.a
# Use the absolute path only to detect the available input file. Pass its
# directory and basename to ld so DT_NEEDED never contains the build host path.
APP_MAIN_AKAUDIOFILTER_LIB_DIR := $(SOC_PLATFORM_INSTALL_DIR)/lib
APP_MAIN_AKAUDIOFILTER_FILE := $(firstword $(wildcard \
	$(APP_MAIN_AKAUDIOFILTER_LIB_DIR)/libakaudiofilter.so \
	$(APP_MAIN_AKAUDIOFILTER_LIB_DIR)/libakaudiofilter.so.*))

# Drop test archives and duplicate paths (long duplicate lists can crash old collect2/ld on uClibc).
LIBS := $(sort $(LIBS))
LIBS := $(filter-out %pngtest.a %_test.a,$(LIBS))
LIBS := $(filter-out $(APP_MAIN_TLS_UNSAFE_STATIC_LIBS),$(LIBS))
# libwebsockets mbedtls backend embeds OpenSSL API shims (SSL_*/ERR_*). Static-linking
# libwebsockets.a next to real libssl/libcrypto causes multiple-definition at ipc_main link.
# Prefer shared libwebsockets.so (deploy via CONFIG_THIRD_PARTY_SO_ALLOWLIST).
LIBS := $(filter-out %/libwebsockets/lib/libwebsockets.a %/libwebsockets.a,$(LIBS))
ifeq ($(APP_MAIN_DYNAMIC_CORE_SO),y)
# Same symbol must not come from both a pulled-in .a member and a -l*.so (e.g. wpa_ctrl_* in libhardware).
LIBS := $(filter-out %/libhardware.a %/libnetwork.a %/libstorage.a %/libmultimedia.a %/libruntime.a %/libnotification.a %/libsystem_core.a,$(LIBS))
endif
# Scheme B: Home needs LVGL in-process (static liblvgl.a). FreeType/libpng do NOT
# affect Home first frame — omit their .a and resolve via dlopen trampolines
# (app_main_defer_gui_stubs.c). Never DT_NEEDED liblvgl/libfreetype for deferral.
# curl / Volc RTC / weather: must NOT be DT_NEEDED by ipc_main (boot silent ~1.5s was
# dominated by libVolcEngineRTCLite.so ~10MB + libcurl). Weather → stub+dlopen;
# Volc → libvolc_engine_svc.so; curl rides with those .so.
ifeq ($(CONFIG_APP_MAIN_DEFER_HEAVY_SO),y)
LIBS := $(filter-out %/libfreetype.a %/freetype/lib/libfreetype.a,$(LIBS))
LIBS := $(filter-out %/libpng.a %/libpng/lib/libpng.a %/libpng16.a,$(LIBS))
# Prefer one LVGL archive: framework (core+ports). Drop third_party duplicate.
LIBS := $(filter-out %/third_party/lvgl/lib/liblvgl.a %/lvgl/lib/liblvgl.a,$(LIBS))
LIBS := $(filter-out %/libcurl.a %/curl/lib/libcurl.a %/libcurl.so %/libcurl.so.%,$(LIBS))
LIBS := $(filter-out %/libmbedtls.a %/libmbedcrypto.a %/libmbedx509.a \
	%/mbedtls/lib/libmbedtls.a %/mbedtls/lib/libmbedcrypto.a %/mbedtls/lib/libmbedx509.a,$(LIBS))
LIBS := $(filter-out %/libweather_service.a,$(LIBS))
LIBS := $(filter-out %/libVolcEngineRTCLite.so %/libVolcEngineRTCLite.so.%,$(LIBS))
LDFLAGS += -Wl,-rpath-link,$(THIRD_PARTY_INSTALL_DIR)/curl/lib
LDFLAGS += -Wl,-rpath-link,$(THIRD_PARTY_INSTALL_DIR)/mbedtls/lib
LDFLAGS += -Wl,-rpath-link,$(THIRD_PARTY_INSTALL_DIR)/zlib/lib
$(info [app_main] DEFER_HEAVY_SO: no curl/Volc/weather on ipc_main NEEDED; FT/png via stubs)
endif

# Static libnetwork.a embeds wifi_cred_crypto (mbedtls AES/base64). Dynamic network.so
# already DT_NEEDED libmbedcrypto; static consumers must link it after DEFER filters.
ifneq ($(APP_MAIN_DYNAMIC_CORE_SO),y)
LIBS += $(wildcard $(THIRD_PARTY_INSTALL_DIR)/mbedtls/lib/libmbedcrypto.a)
LDFLAGS += -Wl,-rpath-link,$(THIRD_PARTY_INSTALL_DIR)/mbedtls/lib
endif

# If aliyun (or leftover libaliyun_app.a) needs lws_*, link the shared library instead of .a.
APP_MAIN_NEED_LWS_SO :=
ifneq ($(findstring y,$(CONFIG_APP_CLOUD_ALIYUN)),)
APP_MAIN_NEED_LWS_SO := y
endif
ifneq ($(filter %/libaliyun_app.a,$(LIBS)),)
APP_MAIN_NEED_LWS_SO := y
endif
ifeq ($(APP_MAIN_NEED_LWS_SO),y)
APP_MAIN_LWS_SO := $(wildcard $(THIRD_PARTY_INSTALL_DIR)/libwebsockets/lib/libwebsockets.so)
ifeq ($(APP_MAIN_LWS_SO),)
APP_MAIN_LWS_SO := $(firstword $(wildcard $(THIRD_PARTY_INSTALL_DIR)/libwebsockets/lib/libwebsockets.so.*))
endif
ifneq ($(APP_MAIN_LWS_SO),)
LIBS += $(APP_MAIN_LWS_SO)
LDFLAGS += -Wl,-rpath-link,$(THIRD_PARTY_INSTALL_DIR)/libwebsockets/lib
$(info [app_main] link $(APP_MAIN_LWS_SO) (avoid static OpenSSL shim vs libssl))
else
$(warning [app_main] aliyun needs LWS but libwebsockets.so missing under $(THIRD_PARTY_INSTALL_DIR)/libwebsockets/lib)
endif
endif

# KM01W / AV100 uClibc GCC: `$(CC)` may be plain `gcc` from a path-only toolchain; use -dumpmachine too.
APP_MAIN_GCC_MACHINE := $(shell $(CC) -dumpmachine 2>/dev/null)
APP_MAIN_IS_ARM_UCLIBC := $(strip $(if $(findstring uclibc,$(APP_MAIN_GCC_MACHINE)),1,$(if $(findstring uclibcgnueabi,$(CC)$(CROSS_COMPILE)),1,)))
# Same directory holds libgcc.a and libgcc_s.so; use libgcc.a (always present) to locate -L for -lgcc_s.
APP_MAIN_GCC_LIB_FILE := $(shell $(CC) -print-file-name=libgcc.a 2>/dev/null)
APP_MAIN_GCC_LIBDIR := $(dir $(APP_MAIN_GCC_LIB_FILE))

ifneq ($(APP_MAIN_IS_ARM_UCLIBC),)
$(info [app_main] ARM uClibc detected: CC=$(CC) machine=$(APP_MAIN_GCC_MACHINE) GCC_LIBDIR=$(APP_MAIN_GCC_LIBDIR))
else
$(info [app_main] Not ARM uClibc (machine=$(APP_MAIN_GCC_MACHINE)), using default link path)
endif

# Size / map flags (ActionCam/application/main/rules.mk: LDFLAGS often only -Wl,--gc-sections before objects).
APP_MAIN_LD_SIZEOPT :=
# Always emit link map for size analysis (ipc_main.map next to the binary).
APP_MAIN_LD_SIZEOPT += -Wl,-Map,$(BUILD_DIR)/$(TARGET).map
ifeq ($(APP_MAIN_SIZE_OPT),y)
APP_MAIN_LD_SIZEOPT += -Wl,--gc-sections
ifneq ($(APP_MAIN_IS_ARM_UCLIBC),)
# arm-anycloud uClibc + old binutils: --as-needed interacts badly with libgcc atomics vs DSOs.
else
APP_MAIN_LD_SIZEOPT += -Wl,--as-needed
endif
endif

# Shared tail: -L/-rpath-link for core .so and third_party deps; then -lsystem_core …
APP_MAIN_LD_CORE_TAIL :=
ifeq ($(APP_MAIN_DYNAMIC_CORE_SO),y)
ifneq ($(strip $(SYSTEM_CORE_INSTALL_DIR)),)
APP_MAIN_LD_CORE_TAIL += -L$(SYSTEM_CORE_INSTALL_DIR)/lib
endif
ifneq ($(strip $(HARDWARE_INSTALL_DIR)),)
APP_MAIN_LD_CORE_TAIL += -L$(HARDWARE_INSTALL_DIR)/lib
endif
ifneq ($(strip $(FRAMEWORK_INSTALL_DIR)),)
APP_MAIN_LD_CORE_TAIL += -L$(FRAMEWORK_INSTALL_DIR)/lib
endif
ifneq ($(strip $(FRAMEWORK_INSTALL_DIR)),)
APP_MAIN_LD_CORE_TAIL += -Wl,-rpath-link,$(FRAMEWORK_INSTALL_DIR)/lib
endif
ifneq ($(strip $(SYSTEM_CORE_INSTALL_DIR)),)
APP_MAIN_LD_CORE_TAIL += -Wl,-rpath-link,$(SYSTEM_CORE_INSTALL_DIR)/lib
endif
ifneq ($(strip $(HARDWARE_INSTALL_DIR)),)
APP_MAIN_LD_CORE_TAIL += -Wl,-rpath-link,$(HARDWARE_INSTALL_DIR)/lib
endif
ifneq ($(strip $(THIRD_PARTY_INSTALL_DIR)),)
APP_MAIN_LD_CORE_TAIL += -Wl,-rpath-link,$(THIRD_PARTY_INSTALL_DIR)/cjson/lib
APP_MAIN_LD_CORE_TAIL += -Wl,-rpath-link,$(THIRD_PARTY_INSTALL_DIR)/iniparser/lib
APP_MAIN_LD_CORE_TAIL += -Wl,-rpath-link,$(THIRD_PARTY_INSTALL_DIR)/iconv/lib
APP_MAIN_LD_CORE_TAIL += -Wl,-rpath-link,$(THIRD_PARTY_INSTALL_DIR)/zlib/lib
APP_MAIN_LD_CORE_TAIL += -L$(THIRD_PARTY_INSTALL_DIR)/cjson/lib -L$(THIRD_PARTY_INSTALL_DIR)/iniparser/lib -L$(THIRD_PARTY_INSTALL_DIR)/iconv/lib -L$(THIRD_PARTY_INSTALL_DIR)/zlib/lib
endif
ifneq ($(strip $(APP_MAIN_AKAUDIOFILTER_FILE)),)
APP_MAIN_LD_CORE_TAIL += -L$(APP_MAIN_AKAUDIOFILTER_LIB_DIR) \
	-Wl,--no-as-needed -Wl,-l:$(notdir $(APP_MAIN_AKAUDIOFILTER_FILE)) -Wl,--as-needed
endif
APP_MAIN_LD_CORE_TAIL += -Wl,--allow-shlib-undefined
# Scheme B (Nova/tablet): omit -lmultimedia/-lnetwork so Home path does not load those DSOs;
# app_main dlopen()s them after /tmp/ipc_gui_ready (CONFIG_APP_MAIN_DEFER_HEAVY_SO=y).
ifeq ($(CONFIG_APP_MAIN_DEFER_HEAVY_SO),y)
APP_MAIN_LD_CORE_TAIL += -lsystem_core -lhardware -lruntime -lnotification -lstorage
$(info [app_main] DEFER_HEAVY_SO: link without -lmultimedia -lnetwork)
else
APP_MAIN_LD_CORE_TAIL += -lsystem_core -lhardware -lruntime -lnotification -lstorage -lmultimedia -lnetwork
endif
else
ifeq ($(APP_MAIN_IS_ARM_UCLIBC),)
APP_MAIN_LD_CORE_TAIL += -static-libgcc
endif
endif

# uClibc ARM link strategy (same as ActionCam/application/main/rules.mk):
#   Put EVERYTHING (static .a + framework DSOs) inside one -( )- group.
#   Append libgcc.a explicitly at the end — gcc driver flags (-static-libgcc / -shared-libgcc)
#   are unreliable on this GCC 5.5.0 toolchain; config.mk also adds libgcc.a via LDFLAGS
#   for each .so build so every DSO self-contains __aeabi_*/__sync_* symbols.
ifneq ($(APP_MAIN_IS_ARM_UCLIBC),)
APP_MAIN_UCLIBC_LD_PRE := $(APP_MAIN_LD_SIZEOPT) -rdynamic -Wl,-rpath,/lib:/usr/lib
APP_MAIN_UCLIBC_LD_MID :=
APP_MAIN_UCLIBC_LD_POST := -lstdc++ -ldl $(APP_MAIN_LD_CORE_TAIL) $(APP_MAIN_GCC_LIB_FILE)
endif

ifeq ($(APP_MAIN_IS_ARM_UCLIBC),)
LDFLAGS += -rdynamic -Wl,-rpath,/lib:/usr/lib -Xlinker "-(" $(LIBS) -Xlinker "-)" -lc -lstdc++ -ldl
LDFLAGS += $(APP_MAIN_LD_CORE_TAIL)
LDFLAGS += $(APP_MAIN_LD_SIZEOPT)
endif

# project config: hand-written headers from source include, auto-generated config.h from
# PROJECT_GEN_INCLUDE_DIR (out/<product>/build/project, exported by build.sh).
PROJECT_GEN_INCLUDE_DIR ?= $(dir $(PROJECT_CONFIG_FILE))include
ifneq ($(strip $(PROJECT_CONFIG_FILE)),)
INCLUDE += -I$(dir $(PROJECT_CONFIG_FILE))include
endif
INCLUDE += -I$(PROJECT_GEN_INCLUDE_DIR)
INCLUDE += -I$(APPLICATION_INSTALL_DIR)/include
INCLUDE += -I$(SOC_PLATFORM_INSTALL_DIR)/include
INCLUDE += -I$(HARDWARE_INSTALL_DIR)/include
INCLUDE += -I$(FRAMEWORK_INSTALL_DIR)/include/services/multimedia
INCLUDE += -I$(FRAMEWORK_INSTALL_DIR)/include/services/network
# rtsp_server.h is installed to include/rtsp/rtsp_server/ (see framework/services/network/Makefile)
INCLUDE += -I$(FRAMEWORK_INSTALL_DIR)/include/rtsp/rtsp_server
INCLUDE += -I$(FRAMEWORK_INSTALL_DIR)/include/services/notification
INCLUDE += -I$(FRAMEWORK_INSTALL_DIR)/include/runtime
INCLUDE += -I$(FRAMEWORK_INSTALL_DIR)/include/gateway
INCLUDE += -I$(FRAMEWORK_INSTALL_DIR)/include
INCLUDE += -I$(SYSTEM_CORE_INSTALL_DIR)/include
INCLUDE += -I$(THIRD_PARTY_INSTALL_DIR)/cjson/include
INCLUDE += -I$(THIRD_PARTY_INSTALL_DIR)/lvgl/include
INCLUDE += -I$(THIRD_PARTY_INSTALL_DIR)/freetype/include/freetype2
INCLUDE += -I$(THIRD_PARTY_INSTALL_DIR)/libpng/include
INCLUDE += -I$(MODULE_ROOT)

CFLAGS += $(INCLUDE)
CXXFLAGS = $(filter-out -std=c99,$(CFLAGS))
ifeq ($(APP_MAIN_SIZE_OPT),y)
CFLAGS += -ffunction-sections -fdata-sections
endif

$(shell mkdir -p $(BUILD_DIR))

# CONFIG_APP_MAIN_OBJECTS_ONLY comes from project config.mk (static image, no app_main link).
.PHONY: aos_objects
ifeq ($(CONFIG_APP_MAIN_OBJECTS_ONLY),y)
AOS_OBJECT_BASES := main app_config app_main app_main_config app_main_defer_gui_noop
aos_objects: $(addprefix $(BUILD_DIR)/,$(addsuffix .o,$(AOS_OBJECT_BASES)))
endif

.PHONY: all clean tablet_dep tablet_build copy_nfs copy_nfs_libs install_rootfs

all: multimedia_dep tablet_build tablet_dep $(TARGET) copy_nfs install_rootfs

$(TARGET): $(OBJS)
	@echo "Linking app_main $(TARGET)..."
	@if [ -z "$(LIBS)" ]; then \
		echo "ERROR: No libraries found! Check library paths."; \
		exit 1; \
	fi
	@TABLET_OBJS_LINK=""; \
	if [ -n "$(GUI_MODULE)" ]; then \
		[ -z "$(BUILD_APPLICATION_DIR)" ] && { echo "ERROR: BUILD_APPLICATION_DIR not set"; exit 1; }; \
		[ -z "$(GUI_BUILD_DIR)" ] && { echo "ERROR: GUI_BUILD_DIR not set"; exit 1; }; \
		TABLET_OBJS_LINK=$$(find $(GUI_BUILD_DIR)/launcher_obj -name "*.o" ! -name "tablet_main.o" ! -path "*/launcher/fonts/*" 2>/dev/null); \
		[ -z "$$TABLET_OBJS_LINK" ] && { echo "ERROR: packages/$(GUI_MODULE) object files not found"; exit 1; }; \
		TABLET_N=$$(echo "$$TABLET_OBJS_LINK" | wc -l); \
		echo "Linking with $$TABLET_N packages/$(GUI_MODULE) object files and $(words $(LIBS)) libs..."; \
	fi; \
	echo "  CC/LD $(TARGET) (may take 10-30s)"; \
	if [ "$(APP_MAIN_VERIFY_DSO)" != "n" ] && [ -n "$(APP_MAIN_VERIFY_DSO)" ]; then \
		SO_DIRS="$(FRAMEWORK_INSTALL_DIR)/lib $(SYSTEM_CORE_INSTALL_DIR)/lib $(HARDWARE_INSTALL_DIR)/lib"; \
		SO_LIST=""; \
		if [ "$(APP_MAIN_VERIFY_DSO)" = "all" ]; then \
			for d in $$SO_DIRS; do \
				[ -d "$$d" ] && SO_LIST="$$SO_LIST $$(find $$d -maxdepth 1 -name '*.so' -type f 2>/dev/null)"; \
			done; \
		else \
			for d in $$SO_DIRS; do \
				[ -f "$$d/$(APP_MAIN_VERIFY_DSO)" ] && SO_LIST="$$SO_LIST $$d/$(APP_MAIN_VERIFY_DSO)"; \
			done; \
		fi; \
		echo "=== [DSO-VERIFY] Testing $$(echo $$SO_LIST | wc -w) DSO(s) ==="; \
		PROBLEM_SOS=""; \
		for SO_PATH in $$SO_LIST; do \
			SO_NAME=$$(basename $$SO_PATH); \
			A_NAME=$$(echo $$SO_NAME | sed 's/\.so$$/.a/'); \
			A_PATH=$$(dirname $$SO_PATH)/$$A_NAME; \
			echo "--- Testing without $$SO_NAME ---"; \
			mv "$$SO_PATH" "$$SO_PATH.bak_verify"; \
			TEST_LIBS="$(LIBS)"; \
			if [ -f "$$A_PATH" ]; then \
				echo "$$TEST_LIBS" | grep -q "$$A_NAME" || TEST_LIBS="$$TEST_LIBS $$A_PATH"; \
			fi; \
			TEST_OK=0; \
			if [ -n "$(APP_MAIN_IS_ARM_UCLIBC)" ]; then \
				$(CC) $(APP_MAIN_UCLIBC_LD_PRE) $(OBJS) $$TABLET_OBJS_LINK -Xlinker "-(" $$TEST_LIBS $(APP_MAIN_UCLIBC_LD_POST) -Xlinker "-)" -o $(BUILD_DIR)/$(TARGET).verify 2>&1 && TEST_OK=1; \
			else \
				$(CC) -o $(BUILD_DIR)/$(TARGET).verify $(OBJS) $$TABLET_OBJS_LINK $(LDFLAGS) 2>&1 && TEST_OK=1; \
			fi; \
			mv "$$SO_PATH.bak_verify" "$$SO_PATH"; \
			rm -f $(BUILD_DIR)/$(TARGET).verify; \
			if [ $$TEST_OK -eq 1 ]; then \
				echo "  => PASS: link OK without $$SO_NAME => $$SO_NAME causes the problem"; \
				PROBLEM_SOS="$$PROBLEM_SOS $$SO_NAME"; \
			else \
				echo "  => still fails without $$SO_NAME => not (only) this one"; \
			fi; \
		done; \
		echo ""; \
		echo "=== [DSO-VERIFY] Summary ==="; \
		if [ -n "$$PROBLEM_SOS" ]; then \
			echo "  Problem DSO(s):$$PROBLEM_SOS"; \
			echo "  Fix: rebuild these .so without -shared-libgcc (plain gcc -shared -fPIC)"; \
		else \
			echo "  No single DSO removal fixed the link — may be multiple DSOs combined."; \
		fi; \
		echo "=== [DSO-VERIFY] Done (normal link follows) ==="; \
	fi; \
	if [ -n "$(APP_MAIN_IS_ARM_UCLIBC)" ]; then \
		echo "LINK CMD -> $(BUILD_DIR)/$(TARGET).linkcmd"; \
		echo $(CC) $(APP_MAIN_UCLIBC_LD_PRE) $(OBJS) $$TABLET_OBJS_LINK -Xlinker "-(" $(LIBS) $(APP_MAIN_UCLIBC_LD_POST) -Xlinker "-)" -o $(BUILD_DIR)/$(TARGET) > $(BUILD_DIR)/$(TARGET).linkcmd; \
		cat $(BUILD_DIR)/$(TARGET).linkcmd; \
		$(CC) $(APP_MAIN_UCLIBC_LD_PRE) $(OBJS) $$TABLET_OBJS_LINK -Xlinker "-(" $(LIBS) $(APP_MAIN_UCLIBC_LD_POST) -Xlinker "-)" -o $(BUILD_DIR)/$(TARGET); \
	else \
		echo "LINK CMD -> $(BUILD_DIR)/$(TARGET).linkcmd"; \
		echo $(CC) -o $(BUILD_DIR)/$(TARGET) $(OBJS) $$TABLET_OBJS_LINK $(LDFLAGS) > $(BUILD_DIR)/$(TARGET).linkcmd; \
		cat $(BUILD_DIR)/$(TARGET).linkcmd; \
		$(CC) -o $(BUILD_DIR)/$(TARGET) $(OBJS) $$TABLET_OBJS_LINK $(LDFLAGS); \
	fi
	@echo "Extracting debug symbols -> $(BUILD_DIR)/$(TARGET).debug (addr2line/gdb)..."
	$(CROSS_TOOLCHAIN)-objcopy --only-keep-debug $(BUILD_DIR)/$(TARGET) $(BUILD_DIR)/$(TARGET).debug
	$(CROSS_TOOLCHAIN)-strip $(BUILD_DIR)/$(TARGET)
	-$(CROSS_TOOLCHAIN)-objcopy --add-gnu-debuglink=$(BUILD_DIR)/$(TARGET).debug $(BUILD_DIR)/$(TARGET) 2>/dev/null || true
	@if [ -f "$(BUILD_DIR)/$(TARGET).map" ]; then \
		echo "Link map: $(BUILD_DIR)/$(TARGET).map ($$(wc -c < "$(BUILD_DIR)/$(TARGET).map") bytes)"; \
		if [ -n "$(BUILD_APPLICATION_DIR)" ]; then \
			mkdir -p $(BUILD_APPLICATION_DIR)/bin; \
			cp -f $(BUILD_DIR)/$(TARGET).map $(BUILD_APPLICATION_DIR)/bin/; \
			echo "Link map copied: $(BUILD_APPLICATION_DIR)/bin/$(TARGET).map"; \
		fi; \
	else \
		echo "WARNING: link map missing: $(BUILD_DIR)/$(TARGET).map"; \
	fi
	@if [ -n "$(BUILD_APPLICATION_DIR)" ]; then \
		mkdir -p $(BUILD_APPLICATION_DIR)/bin; \
		cp -f $(BUILD_DIR)/$(TARGET) $(BUILD_APPLICATION_DIR)/bin/; \
	fi

# Runs on every 'all': copies main binary, config, run_app.sh to NFS directory for NFS-mount testing
copy_nfs: $(TARGET)
	@NFS_D="$(NFS_DIR)"; [ -z "$$NFS_D" ] || [ "$$NFS_D" = "/nfs/application" ] && NFS_D="$${HOME:-$(HOME)}/nfs/application"; \
	RUNTIME_CFG_DIR="$(dir $(PROJECT_CONFIG_FILE))system/$(CONFIG_SOC_PLATFORM)/$(CONFIG_SOC_SDK_VERSION)/board/$(CONFIG_BOARD)/rootfs/utils/etc/config"; \
	[ -d "$$RUNTIME_CFG_DIR" ] || RUNTIME_CFG_DIR="$(PROJECT_CONFIG_DIR)"; \
	mkdir -p "$$NFS_D" && cp -f $(BUILD_DIR)/$(TARGET) "$$NFS_D/" && echo "Copied $(TARGET) to $$NFS_D/"; \
	if [ -d "$$RUNTIME_CFG_DIR" ]; then cp -f "$$RUNTIME_CFG_DIR"/* "$$NFS_D/" 2>/dev/null || true; echo "Copied config to $$NFS_D/"; fi; \
	RUN_SCRIPT="$(SRC_DIR)/run_app.sh"; [ -z "$(SRC_DIR)" ] && RUN_SCRIPT="$(MODULE_ROOT)/run_app.sh"; \
	if [ -f "$$RUN_SCRIPT" ]; then cp -f "$$RUN_SCRIPT" "$$NFS_D/" && chmod +x "$$NFS_D/run_app.sh"; echo "Copied run_app.sh to $$NFS_D/"; fi

# Sync rootfs/utils/usr/lib to ~/nfs/application/usr/lib (must run after application/Makefile install_app_main)
copy_nfs_libs:
	@NFS_D="$(NFS_DIR)"; [ -z "$$NFS_D" ] || [ "$$NFS_D" = "/nfs/application" ] && NFS_D="$${HOME:-$(HOME)}/nfs/application"; \
	if [ -d "$(ROOTFS_USR_LIB)" ]; then \
		mkdir -p "$$NFS_D/usr/lib" && find "$(ROOTFS_USR_LIB)" -maxdepth 1 -type f -exec cp -af {} "$$NFS_D/usr/lib/" \; 2>/dev/null || true; \
		echo "Copied libs from $(ROOTFS_USR_LIB) to $$NFS_D/usr/lib/"; \
	fi

# Recompile when generated config.h changes. main.c gates app_gui_run on
# CONFIG_APP_PACKAGES_WEARABLE; a stale .o without that macro returns after
# app_lv_open (shell "Done", no "luna launcher started").
APP_MAIN_CONFIG_H := $(PROJECT_GEN_INCLUDE_DIR)/config.h
$(BUILD_DIR)/%.o: %.c $(wildcard $(APP_MAIN_CONFIG_H))
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

multimedia_dep:
	@MM_SO="$(FRAMEWORK_INSTALL_DIR)/lib/libmultimedia.so"; \
	if [ "$(APP_MAIN_DYNAMIC_CORE_SO)" = "y" ]; then \
		if [ ! -f "$$MM_SO" ]; then \
			echo "Error: libmultimedia.so not found at $$MM_SO"; \
			echo "  Build framework first, e.g. ./scripts/build.sh --product-name $(PRODUCT_NAME)"; \
			echo "  Or: make -C application/app_main FRAMEWORK_INSTALL_DIR=\$$PWD/out/$(PRODUCT_NAME)/install/framework ..."; \
			exit 1; \
		fi; \
	else \
		if [ ! -f "$(FRAMEWORK_INSTALL_DIR)/lib/libmultimedia.a" ]; then \
			echo "Error: libmultimedia.a not found at $(FRAMEWORK_INSTALL_DIR)/lib/libmultimedia.a"; exit 1; \
		fi; \
	fi
	@if [ "$(APP_MAIN_DYNAMIC_CORE_SO)" = "y" ]; then \
		if [ ! -f "$(SYSTEM_CORE_INSTALL_DIR)/lib/libsystem_core.so" ]; then \
			echo "Error: libsystem_core.so not found"; exit 1; \
		fi; \
	else \
		if [ ! -f "$(SYSTEM_CORE_INSTALL_DIR)/lib/libsystem_core.a" ]; then \
			echo "Error: libsystem_core.a not found"; exit 1; \
		fi; \
	fi
	@if [ "$(APP_MAIN_DYNAMIC_CORE_SO)" = "y" ]; then \
		if [ ! -f "$(HARDWARE_INSTALL_DIR)/lib/libhardware.so" ]; then \
			echo "Error: libhardware.so not found"; exit 1; \
		fi; \
	else \
		if [ ! -f "$(HARDWARE_INSTALL_DIR)/lib/libhardware.a" ]; then \
			echo "Error: libhardware.a not found"; exit 1; \
		fi; \
	fi

tablet_build:
ifneq ($(GUI_MODULE),)
	@[ -z "$(BUILD_APPLICATION_DIR)" ] && { echo "WARNING: BUILD_APPLICATION_DIR not set"; exit 0; }; true
	@[ -z "$(SRC_APPLICATION_DIR)" ] && { echo "WARNING: SRC_APPLICATION_DIR not set"; exit 0; }; true
	@[ ! -d "$(SRC_APPLICATION_DIR)/packages/$(GUI_MODULE)" ] && { echo "WARNING: packages/$(GUI_MODULE) not found"; exit 0; }; true
	@echo "Building application/packages/$(GUI_MODULE)..."
	@$(MAKE) -C $(SRC_APPLICATION_DIR)/packages/$(GUI_MODULE) \
		SRC_DIR=$(SRC_APPLICATION_DIR)/packages/$(GUI_MODULE) \
		BUILD_DIR=$(BUILD_APPLICATION_DIR)/packages/$(GUI_MODULE) \
		BUILD_FRAMEWORK_DIR=$(BUILD_FRAMEWORK_DIR) \
		SRC_FRAMEWORK_DIR=$(SRC_FRAMEWORK_DIR) \
		FRAMEWORK_DIR=$(FRAMEWORK_DIR) \
		BUILD_SYSTEM_CORE_DIR=$(BUILD_SYSTEM_CORE_DIR) \
		SRC_SYSTEM_CORE_DIR=$(SRC_SYSTEM_CORE_DIR) \
		SYSTEM_CORE_INSTALL_DIR=$(SYSTEM_CORE_INSTALL_DIR) \
		FRAMEWORK_INSTALL_DIR=$(FRAMEWORK_INSTALL_DIR) \
		APPLICATION_INSTALL_DIR=$(APPLICATION_INSTALL_DIR) \
		HARDWARE_INSTALL_DIR=$(HARDWARE_INSTALL_DIR) \
		HARDWARE_DIR=$(HARDWARE_DIR) \
		THIRD_PARTY_INSTALL_DIR=$(THIRD_PARTY_INSTALL_DIR) \
		SOC_PLATFORM_INSTALL_DIR=$(SOC_PLATFORM_INSTALL_DIR) \
		SOC_PLATFORM_DIR=$(SOC_PLATFORM_DIR) \
		PROJECT_DIR=$(PROJECT_DIR) \
		FRAMEWORK_CONFIG_FILE=$(FRAMEWORK_CONFIG_FILE) \
		PROJECT_CONFIG_FILE=$(PROJECT_CONFIG_FILE) \
		TABLET_EXCLUDE_MAIN=1 \
		COMPILATION_RULES=$(COMPILATION_RULES) \
		all || (echo "ERROR: Failed to build packages/$(GUI_MODULE)"; exit 1)
endif

tablet_dep:
ifneq ($(GUI_MODULE),)
	@[ -z "$(BUILD_APPLICATION_DIR)" ] && exit 0; true
	@[ -z "$(GUI_BUILD_DIR)" ] && exit 0; true
	@TABLET_OBJS_CHECK=$$(find $(GUI_BUILD_DIR)/launcher_obj -name "*.o" ! -name "tablet_main.o" ! -path "*/launcher/fonts/*" 2>/dev/null); \
	if [ -z "$$TABLET_OBJS_CHECK" ]; then \
		echo "WARNING: packages/$(GUI_MODULE) object files not found"; exit 0; \
	else \
		echo "Found $$(echo "$$TABLET_OBJS_CHECK" | wc -l) object files from packages/$(GUI_MODULE)"; \
	fi
endif

clean:
	@rm -rf $(BUILD_DIR)
	@find $(MODULE_ROOT) -name "*.o" -type f -delete 2>/dev/null || true
	@find $(MODULE_ROOT) -name "*.d" -type f -delete 2>/dev/null || true

# Install settings.json to rootfs config (luna: program -> /usr/sbin/$(TARGET); binary placed by package.sh).
# Guarded: skips silently on products/stages without a /usr/sbin staging dir (e.g. nova root-fs layout).
install_rootfs: $(TARGET)
	@if [ ! -d "$(ROOTFS_USR_SBIN)" ]; then 		echo "Skip install_rootfs: $(ROOTFS_USR_SBIN) not found"; 		exit 0; 	fi
	@mkdir -p "$(ROOTFS_USR_SBIN)" "$(ROOTFS_ETC_CONFIG)"
	@RUNTIME_CFG_DIR="$(dir $(PROJECT_CONFIG_FILE))system/$(CONFIG_SOC_PLATFORM)/$(CONFIG_SOC_SDK_VERSION)/board/$(CONFIG_BOARD)/rootfs/utils/etc/config"; \
	if [ ! -d "$$RUNTIME_CFG_DIR" ]; then RUNTIME_CFG_DIR="$(PROJECT_CONFIG_DIR)"; fi; \
	if [ -f "$$RUNTIME_CFG_DIR/settings.json" ]; then \
		sed -E 's|"program"[[:space:]]*:[[:space:]]*"[^"]*"|"program": "/usr/sbin/$(TARGET)"|' \
			"$$RUNTIME_CFG_DIR/settings.json" > "$(ROOTFS_ETC_CONFIG)/settings.json"; \
		echo "Installed settings.json -> $(ROOTFS_ETC_CONFIG)/ (program=/usr/sbin/$(TARGET))"; \
	fi
