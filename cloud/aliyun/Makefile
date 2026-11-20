#
# Makefile for application/cloud/aliyun module
#
include $(COMPILATION_RULES)
include $(PROJECT_CONFIG_FILE)

PROJ = aliyun_app
MODULE_ROOT ?= $(shell pwd)
SRC_DIR ?= $(MODULE_ROOT)
BUILD_DIR ?= $(MODULE_ROOT)/build

SRC_FILES := aliyun_app.c \
             aliyun_sdk/include/c_utils/hal_util_fs.c \
             aliyun_sdk/include/c_utils/hal_util_mem.c \
             aliyun_sdk/include/c_utils/hal_util_mutex.c \
             aliyun_sdk/include/c_utils/hal_util_random.c \
             aliyun_sdk/include/c_utils/hal_util_storage.c \
             aliyun_sdk/include/c_utils/hal_util_time.c

OBJS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(SRC_FILES))

STATIC_LIB := lib$(PROJ).a
DYNAMIC_LIB := lib$(PROJ).so
TARGET_STATIC_LIB := $(BUILD_DIR)/$(STATIC_LIB)
TARGET_DYNAMIC_LIB := $(BUILD_DIR)/$(DYNAMIC_LIB)

INCLUDE += -I$(THIRD_PARTY_INSTALL_DIR)/cjson/include
INCLUDE += -I$(THIRD_PARTY_INSTALL_DIR)/curl/include
INCLUDE += -I$(THIRD_PARTY_INSTALL_DIR)/mbedtls/include
INCLUDE += -I$(THIRD_PARTY_INSTALL_DIR)/libwebsockets/include
INCLUDE += -I$(SYSTEM_CORE_INSTALL_DIR)/include
INCLUDE += -I$(HARDWARE_INSTALL_DIR)/include
INCLUDE += -I$(FRAMEWORK_INSTALL_DIR)/include/services/multimedia
INCLUDE += -I$(FRAMEWORK_INSTALL_DIR)/include/services/notification
INCLUDE += -I$(SRC_APPLICATION_DIR)/app_main
INCLUDE += -I$(MODULE_ROOT)
INCLUDE += -I$(MODULE_ROOT)/aliyun_sdk/include
INCLUDE += -I$(MODULE_ROOT)/aliyun_sdk/include/c_utils

CFLAGS += -Werror -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast
CFLAGS += $(INCLUDE)

$(shell mkdir -p $(BUILD_DIR))

.PHONY: all install clean

all: $(TARGET_STATIC_LIB) install

$(TARGET_STATIC_LIB): $(OBJS)
	$(AR) -rsv $(TARGET_STATIC_LIB) $(OBJS)

ifeq ($(CONFIG_OS_NAME), linux)
$(TARGET_DYNAMIC_LIB): $(OBJS)
	$(CXX) $(CXXFLAGS) $(OBJS) -shared -fPIC -o $@ $(LDFLAGS)
endif

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

install: $(TARGET_STATIC_LIB)
	@mkdir -p $(APPLICATION_INSTALL_DIR)/lib
	@cp -f $(TARGET_STATIC_LIB) $(APPLICATION_INSTALL_DIR)/lib
	@if [ -d "$(MODULE_ROOT)/aliyun_sdk/lib" ]; then \
		cp -f $(MODULE_ROOT)/aliyun_sdk/lib/*.a $(APPLICATION_INSTALL_DIR)/lib 2>/dev/null || true; \
	fi

clean:
	@rm -rf $(BUILD_DIR)
	@find $(MODULE_ROOT) -name "*.o" -type f -delete 2>/dev/null || true
	@find $(MODULE_ROOT) -name "*.d" -type f -delete 2>/dev/null || true
