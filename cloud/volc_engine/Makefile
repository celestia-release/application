#
# Makefile for application/cloud/volc_engine module
#
include $(COMPILATION_RULES)
include $(PROJECT_CONFIG_FILE)

PROJ = volc_engine_svc
MODULE_ROOT ?= $(shell pwd)
SRC_DIR ?= $(MODULE_ROOT)
BUILD_DIR ?= $(MODULE_ROOT)/build

SRC_FILES := volc_engine_app.c \
             volc_engine_authorization.c \
             volc_engine_rtc_token.c

OBJS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(SRC_FILES))

STATIC_LIB := lib$(PROJ).a
DYNAMIC_LIB := lib$(PROJ).so
TARGET_STATIC_LIB := $(BUILD_DIR)/$(STATIC_LIB)
TARGET_DYNAMIC_LIB := $(BUILD_DIR)/$(DYNAMIC_LIB)

INCLUDE += -I$(THIRD_PARTY_INSTALL_DIR)/cjson/include
INCLUDE += -I$(THIRD_PARTY_INSTALL_DIR)/curl/include
INCLUDE += -I$(THIRD_PARTY_INSTALL_DIR)/mbedtls/include
INCLUDE += -I$(SYSTEM_CORE_INSTALL_DIR)/include
INCLUDE += -I$(HARDWARE_INSTALL_DIR)/include
INCLUDE += -I$(FRAMEWORK_INSTALL_DIR)/include/runtime
INCLUDE += -I$(FRAMEWORK_INSTALL_DIR)/include/services/multimedia
INCLUDE += -I$(FRAMEWORK_INSTALL_DIR)/include/services/network
INCLUDE += -I$(FRAMEWORK_INSTALL_DIR)/include/services/notification
INCLUDE += -I$(SRC_APPLICATION_DIR)/app_main
INCLUDE += -I$(MODULE_ROOT)/include
INCLUDE += -I$(MODULE_ROOT)/src/file_parser/include

CFLAGS += -Werror -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast
CFLAGS += $(INCLUDE)

$(shell mkdir -p $(BUILD_DIR))

.PHONY: all install clean

all: $(TARGET_DYNAMIC_LIB) install

ifeq ($(CONFIG_OS_NAME), linux)
# DT_NEEDED: libruntime, libcjson, libcurl, libmbedcrypto, libVolcEngineRTCLite (~10MB).
# Volc RTC must live here (not ipc_main) so Home boot does not mmap/relocate it.
# Wrap -l in --no-as-needed so project LDFLAGS --as-needed cannot drop them.
VOLC_ENGINE_SVC_SHLIB_LIBS := -Wl,-rpath,/usr/lib:/mnt/install/lib:/mnt/install/apps/volc_engine/lib \
	-Wl,--no-as-needed \
	-L$(FRAMEWORK_INSTALL_DIR)/lib -lruntime \
	-L$(THIRD_PARTY_INSTALL_DIR)/cjson/lib -lcjson \
	-L$(THIRD_PARTY_INSTALL_DIR)/curl/lib -lcurl \
	-L$(THIRD_PARTY_INSTALL_DIR)/mbedtls/lib -lmbedcrypto \
	-L$(MODULE_ROOT)/lib -lVolcEngineRTCLite \
	-Wl,--as-needed
$(TARGET_DYNAMIC_LIB): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -shared -fPIC -o $@ $(LDFLAGS) $(VOLC_ENGINE_SVC_SHLIB_LIBS)
else
$(TARGET_DYNAMIC_LIB): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -shared -fPIC -o $@ $(LDFLAGS)
endif

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

install: $(TARGET_DYNAMIC_LIB)
	@mkdir -p $(APPLICATION_INSTALL_DIR)/lib
	@mkdir -p $(APPLICATION_INSTALL_DIR)/include
	@cp -f $(TARGET_DYNAMIC_LIB) $(APPLICATION_INSTALL_DIR)/lib
	@if [ -d "$(MODULE_ROOT)/lib" ]; then \
		cp -f $(MODULE_ROOT)/lib/*.so $(APPLICATION_INSTALL_DIR)/lib 2>/dev/null || true; \
	fi
	@if [ -d "$(MODULE_ROOT)/include" ]; then \
		cp -f $(MODULE_ROOT)/include/* $(APPLICATION_INSTALL_DIR)/include/ 2>/dev/null || true; \
	fi

clean:
	@rm -rf $(BUILD_DIR)
	@find $(MODULE_ROOT) -name "*.o" -type f -delete 2>/dev/null || true
	@find $(MODULE_ROOT) -name "*.d" -type f -delete 2>/dev/null || true
