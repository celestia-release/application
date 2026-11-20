#
# Makefile for application/tests module
#
include $(COMPILATION_RULES)
include $(PROJECT_CONFIG_FILE)

MODULE_ROOT ?= $(shell pwd)
BUILD_DIR ?= $(MODULE_ROOT)/build
_CELESTIA_ROOT := $(abspath $(dir $(PROJECT_CONFIG_FILE))/../..)
# Rootfs staging paths (inlined; build.sh exports SOC_PLATFORM_BUILD_DIR/ROOTFS_DIR).
REL_SOC_PLATFORM_DIR ?= system/$(CONFIG_SOC_PLATFORM)/$(CONFIG_SOC_SDK_VERSION)
ifndef SOC_PLATFORM_BUILD_DIR
PRODUCT_NAME ?= $(notdir $(patsubst %/,%,$(dir $(PROJECT_CONFIG_FILE))))
_CELESTIA_ROOT ?= $(abspath $(dir $(PROJECT_CONFIG_FILE))/../..)
SOC_PLATFORM_BUILD_DIR := $(_CELESTIA_ROOT)/out/$(PRODUCT_NAME)/build/$(REL_SOC_PLATFORM_DIR)
endif
ROOTFS_DIR ?= $(SOC_PLATFORM_BUILD_DIR)/rootfs

# main.c 已废弃（�?main 入口）；主程序仅�?application/app_main 产出
SOURCES =
TARGET = ipc_main
OBJS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(SOURCES))

# Add packages/tablet object files if CONFIG_APP_PACKAGES_TABLET is enabled
# Note: TABLET_OBJS will be calculated at runtime in tablet_dep
ifeq ($(CONFIG_APP_PACKAGES_TABLET), y)
TABLET_BUILD_DIR := $(BUILD_APPLICATION_DIR)/packages/tablet
endif

LIBS = $(shell find $(THIRD_PARTY_INSTALL_DIR)/cjson/lib -maxdepth 1 -name "*.a" 2>/dev/null)
LIBS += $(shell find $(SYSTEM_CORE_INSTALL_DIR)/lib -maxdepth 1 -name "libsystem_core.a" 2>/dev/null)
LIBS += $(shell find $(FRAMEWORK_INSTALL_DIR)/lib -maxdepth 1 -name "libruntime.a" 2>/dev/null)
LIBS += $(shell find $(FRAMEWORK_INSTALL_DIR)/lib -maxdepth 1 -name "libnotification.a" 2>/dev/null)
# FFmpeg libraries (dependency order: avutil -> avcodec -> avformat)
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
LIBS += $(shell find $(FRAMEWORK_INSTALL_DIR)/lib -maxdepth 1 -name "libmultimedia.a" 2>/dev/null)
LIBS += $(shell find $(FRAMEWORK_INSTALL_DIR)/lib -maxdepth 3 -name "*.a" 2>/dev/null | grep -v libmultimedia.a | grep -v libruntime.a | grep -v libnotification.a)
LIBS += $(shell find $(HARDWARE_INSTALL_DIR)/lib -maxdepth 3 -name "*.a" 2>/dev/null)
LIBS += $(shell find $(APPLICATION_INSTALL_DIR)/lib -maxdepth 3 -name "*.a" 2>/dev/null)
LIBS += $(shell find $(SOC_PLATFORM_INSTALL_DIR)/lib -maxdepth 3 -name "*.a" 2>/dev/null)
# Other third-party libraries (excluding cjson and ffmpeg which are already added above)
LIBS += $(shell find $(THIRD_PARTY_INSTALL_DIR) -maxdepth 3 -name "*.a" 2>/dev/null | grep -v cjson | grep -v ffmpeg)

LDFLAGS = -Xlinker "-(" $(LIBS) -Xlinker "-)" -static-libgcc -lc -lstdc++
BIN_DIR := $(BUILD_APPLICATION_DIR)/bin

INCLUDE += -I$(APPLICATION_INSTALL_DIR)/include
INCLUDE += -I$(SOC_PLATFORM_INSTALL_DIR)/include
INCLUDE += -I$(HARDWARE_INSTALL_DIR)/include
INCLUDE += -I$(FRAMEWORK_INSTALL_DIR)/include/services/multimedia
INCLUDE += -I$(FRAMEWORK_INSTALL_DIR)/include/services/network
INCLUDE += -I$(FRAMEWORK_INSTALL_DIR)/include/services/notification
INCLUDE += -I$(FRAMEWORK_INSTALL_DIR)/include/runtime
INCLUDE += -I$(FRAMEWORK_INSTALL_DIR)/include/gateway
INCLUDE += -I$(FRAMEWORK_INSTALL_DIR)/include
INCLUDE += -I$(SYSTEM_CORE_INSTALL_DIR)/include
INCLUDE += -I$(THIRD_PARTY_INSTALL_DIR)/cjson/include

CFLAGS += $(INCLUDE)
CXXFLAGS = $(filter-out -std=c99,$(CFLAGS))

$(shell mkdir -p $(BIN_DIR))

.PHONY: all clean tablet_dep tablet_build skip_main_build

# 主程序唯一入口�?application/app_main/main.c；本目录不再产出主可执行体（Phase 3 收尾）�?all: skip_main_build
skip_main_build:
	@echo "Main executable is from application/app_main only; tests do not build ipc_main."

$(TARGET): $(OBJS)
	@echo "Linking $(TARGET)..."
	@# Recalculate TABLET_OBJS at link time if needed
	@if [ "$(CONFIG_APP_PACKAGES_TABLET)" = "y" ]; then \
		TABLET_OBJS_LINK=$$(find $(TABLET_BUILD_DIR)/launcher_obj -name "*.o" ! -name "tablet_main.o" 2>/dev/null); \
		if [ -n "$$TABLET_OBJS_LINK" ]; then \
			echo "Including $$(echo "$$TABLET_OBJS_LINK" | wc -l) object files from packages/tablet"; \
		fi; \
	fi
	@if [ -z "$(LIBS)" ]; then \
		echo "ERROR: No libraries found! Check library paths."; \
		echo "  FRAMEWORK_INSTALL_DIR=$(FRAMEWORK_INSTALL_DIR)"; \
		echo "  SYSTEM_CORE_INSTALL_DIR=$(SYSTEM_CORE_INSTALL_DIR)"; \
		echo "  THIRD_PARTY_INSTALL_DIR=$(THIRD_PARTY_INSTALL_DIR)"; \
		exit 1; \
	fi
	@echo "Ensuring all library files have proper index..."
	@for lib in $(LIBS); do \
		if [ -f "$$lib" ]; then \
			if [ -n "$(RANLIB)" ]; then \
				$(RANLIB) "$$lib" 2>/dev/null || true; \
			else \
				ranlib "$$lib" 2>/dev/null || $(AR) -s "$$lib" 2>/dev/null || true; \
			fi; \
		fi; \
	done
	@mkdir -p $(BIN_DIR)
	@# Collect packages/tablet object files at link time
	@# main.c calls app_lv_open, so packages/tablet objects are required
	@TABLET_OBJS_LINK=""; \
	if [ "$(CONFIG_APP_PACKAGES_TABLET)" = "y" ]; then \
		if [ -z "$(BUILD_APPLICATION_DIR)" ]; then \
			echo "ERROR: BUILD_APPLICATION_DIR not set, cannot find packages/tablet objects"; \
			echo "  main.c requires app_lv_open from packages/tablet"; \
			exit 1; \
		fi; \
		if [ -z "$(TABLET_BUILD_DIR)" ]; then \
			echo "ERROR: TABLET_BUILD_DIR not set, cannot find packages/tablet objects"; \
			echo "  main.c requires app_lv_open from packages/tablet"; \
			exit 1; \
		fi; \
		TABLET_OBJS_LINK=$$(find $(TABLET_BUILD_DIR)/launcher_obj -name "*.o" ! -name "tablet_main.o" 2>/dev/null); \
		if [ -z "$$TABLET_OBJS_LINK" ]; then \
			echo "ERROR: packages/tablet object files not found at $(TABLET_BUILD_DIR)/launcher_obj"; \
			echo "  main.c requires app_lv_open from packages/tablet"; \
			echo "  Please build application/packages/tablet first"; \
			echo "  BUILD_APPLICATION_DIR=$(BUILD_APPLICATION_DIR)"; \
			echo "  TABLET_BUILD_DIR=$(TABLET_BUILD_DIR)"; \
			exit 1; \
		fi; \
		echo "Linking with $$(echo "$$TABLET_OBJS_LINK" | wc -l) packages/tablet object files"; \
	fi; \
	$(CC) -o $(BIN_DIR)/$(TARGET) $(OBJS) $$TABLET_OBJS_LINK $(LDFLAGS)
	@test -d ~/nfs/av200 || mkdir -p ~/nfs/av200
	$(CROSS_TOOLCHAIN)-strip $(BIN_DIR)/$(TARGET)

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

multimedia_dep:
	@if [ ! -f "$(FRAMEWORK_INSTALL_DIR)/lib/libmultimedia.a" ]; then \
		echo "Error: libmultimedia.a not found at $(FRAMEWORK_INSTALL_DIR)/lib/libmultimedia.a"; \
		echo "Please build framework/services/multimedia first"; \
		exit 1; \
	fi
	@if [ ! -f "$(SYSTEM_CORE_INSTALL_DIR)/lib/libsystem_core.a" ]; then \
		echo "Error: libsystem_core.a not found at $(SYSTEM_CORE_INSTALL_DIR)/lib/libsystem_core.a"; \
		echo "Please build system/core first"; \
		exit 1; \
	fi
	@if [ ! -f "$(HARDWARE_INSTALL_DIR)/lib/libhardware.a" ]; then \
		echo "Error: libhardware.a not found at $(HARDWARE_INSTALL_DIR)/lib/libhardware.a"; \
		echo "Please build hardware first"; \
		exit 1; \
	fi

tablet_build:
ifeq ($(CONFIG_APP_PACKAGES_TABLET), y)
	@if [ -z "$(BUILD_APPLICATION_DIR)" ]; then \
		echo "WARNING: BUILD_APPLICATION_DIR not set, cannot build packages/tablet"; \
		exit 0; \
	fi
	@if [ -z "$(SRC_APPLICATION_DIR)" ]; then \
		echo "WARNING: SRC_APPLICATION_DIR not set, cannot build packages/tablet"; \
		exit 0; \
	fi
	@if [ ! -d "$(SRC_APPLICATION_DIR)/packages/tablet" ]; then \
		echo "WARNING: packages/tablet source directory not found at $(SRC_APPLICATION_DIR)/packages/tablet"; \
		exit 0; \
	fi
	@echo "Building application/packages/tablet..."
	@$(MAKE) -C $(SRC_APPLICATION_DIR)/packages/tablet \
		SRC_DIR=$(SRC_APPLICATION_DIR)/packages/tablet \
		BUILD_DIR=$(BUILD_APPLICATION_DIR)/packages/tablet \
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
		target || (echo "ERROR: Failed to build packages/tablet"; exit 1)
endif

tablet_dep:
ifeq ($(CONFIG_APP_PACKAGES_TABLET), y)
	@if [ -z "$(BUILD_APPLICATION_DIR)" ]; then \
		echo "WARNING: BUILD_APPLICATION_DIR not set, skipping packages/tablet dependency check"; \
		exit 0; \
	fi
	@if [ -z "$(TABLET_BUILD_DIR)" ]; then \
		echo "WARNING: TABLET_BUILD_DIR not set, skipping packages/tablet dependency check"; \
		exit 0; \
	fi
	@echo "Checking packages/tablet object files at $(TABLET_BUILD_DIR)/launcher_obj..."
	@if [ ! -d "$(TABLET_BUILD_DIR)/launcher_obj" ]; then \
		echo "WARNING: packages/tablet build directory not found at $(TABLET_BUILD_DIR)/launcher_obj"; \
		echo "  BUILD_APPLICATION_DIR=$(BUILD_APPLICATION_DIR)"; \
		echo "  TABLET_BUILD_DIR=$(TABLET_BUILD_DIR)"; \
		echo "  This is OK if packages/tablet is not built yet or not needed"; \
		exit 0; \
	fi
	@TABLET_OBJS_CHECK=$$(find $(TABLET_BUILD_DIR)/launcher_obj -name "*.o" ! -name "tablet_main.o" 2>/dev/null); \
	if [ -z "$$TABLET_OBJS_CHECK" ]; then \
		echo "WARNING: packages/tablet object files not found at $(TABLET_BUILD_DIR)/launcher_obj"; \
		echo "  BUILD_APPLICATION_DIR=$(BUILD_APPLICATION_DIR)"; \
		echo "  TABLET_BUILD_DIR=$(TABLET_BUILD_DIR)"; \
		echo "  This is OK if packages/tablet is not built yet or not needed"; \
		exit 0; \
	else \
		OBJ_COUNT=$$(echo "$$TABLET_OBJS_CHECK" | wc -l); \
		echo "Found $$OBJ_COUNT object files from packages/tablet"; \
	fi
endif

clean:
	@rm -rf $(BUILD_DIR)
	@find $(MODULE_ROOT) -name "*.o" -type f -delete 2>/dev/null || true
	@find $(MODULE_ROOT) -name "*.d" -type f -delete 2>/dev/null || true
