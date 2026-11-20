#
# Makefile for application/packages/ipcamera/ipc_cam
# Headless SO: libipc_cam_app.so (module_init / module_start)
#
# Link model aligned with packages/wearable/dashcam: only -lrtsp_protocol.
# Nebula is optional via dlopen() in module_start; headless stubs satisfy
# media_player/display undefs that libnebula_protocol references on products
# without CONFIG_FW_MULTIMEDIA_MEDIA_PLAYER / CONFIG_HW_DISPLAY.
#
include $(COMPILATION_RULES)
FRAMEWORK_OR_PROJECT := $(or $(FRAMEWORK_CONFIG_FILE),$(PROJECT_CONFIG_FILE))
include $(FRAMEWORK_OR_PROJECT)

MODULE_ROOT ?= $(shell pwd)
BUILD_DIR ?= $(MODULE_ROOT)/build
OBJ_DIR := $(BUILD_DIR)/obj
LIB_DIR := $(BUILD_DIR)/lib

TARGET_SO := $(LIB_DIR)/libipc_cam_app.so

CSRCS := \
	$(MODULE_ROOT)/ipc_cam_app.c \
	$(MODULE_ROOT)/ipc_cam_rtsp.c \
	$(MODULE_ROOT)/ipc_cam_record.c \
	$(MODULE_ROOT)/ipc_cam_storage.c

ifeq ($(CONFIG_APP_NEBULA),y)
CSRCS += $(MODULE_ROOT)/ipc_cam_nebula_stubs.c
ifeq ($(BUILD_SHARED),n)
CFLAGS += -DIPC_CAM_NEBULA_STATIC=1
endif
endif

OBJS := $(patsubst %.c,$(OBJ_DIR)/%.o,$(notdir $(CSRCS)))

INCLUDE += -I$(MODULE_ROOT)
INCLUDE += -I$(SRC_APPLICATION_DIR)/app_main
INCLUDE += -I$(FRAMEWORK_INSTALL_DIR)/include
INCLUDE += -I$(FRAMEWORK_INSTALL_DIR)/include/services/multimedia
INCLUDE += -I$(FRAMEWORK_INSTALL_DIR)/include/services/storage
INCLUDE += -I$(FRAMEWORK_INSTALL_DIR)/include/services/network
INCLUDE += -I$(FRAMEWORK_INSTALL_DIR)/include/gateway
INCLUDE += -I$(FRAMEWORK_INSTALL_DIR)/include/runtime
INCLUDE += -I$(SYSTEM_CORE_INSTALL_DIR)/include
INCLUDE += -I$(HARDWARE_INSTALL_DIR)/include
INCLUDE += -I$(THIRD_PARTY_INSTALL_DIR)/cjson/include
PROJECT_GEN_INCLUDE_DIR ?= $(dir $(PROJECT_CONFIG_FILE))include
INCLUDE += -I$(dir $(PROJECT_CONFIG_FILE))include
INCLUDE += -I$(PROJECT_GEN_INCLUDE_DIR)

CFLAGS += -Wall
# BUILD_SHARED is set in project config.mk. Unset means the Linux .so.
ifneq ($(BUILD_SHARED),n)
CFLAGS += -fPIC
endif
CFLAGS += -include $(PROJECT_GEN_INCLUDE_DIR)/config.h

# Shared object on Linux. BUILD_SHARED=n archives libipc_cam_app.a.
ifneq ($(BUILD_SHARED),n)
IPC_CAM_SO_LDFLAGS := -L$(FRAMEWORK_INSTALL_DIR)/lib -lrtsp_protocol -Wl,-rpath,/usr/lib
RTSP_PROTOCOL_LIB := $(FRAMEWORK_INSTALL_DIR)/lib/librtsp_protocol.so
else
RTSP_PROTOCOL_LIB := $(FRAMEWORK_INSTALL_DIR)/lib/librtsp_protocol.a
endif
IPC_CAM_LINK_DEPS := $(RTSP_PROTOCOL_LIB)

vpath %.c $(MODULE_ROOT)

.PHONY: all clean install

ifneq ($(BUILD_SHARED),n)
all: $(TARGET_SO)
else
TARGET_A := $(LIB_DIR)/libipc_cam_app.a
all: $(TARGET_A)
endif

$(OBJ_DIR) $(LIB_DIR):
	@mkdir -p $@

$(OBJ_DIR)/%.o: %.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) $(INCLUDE) -c $< -o $@

ifneq ($(BUILD_SHARED),n)
$(TARGET_SO): $(OBJS) $(IPC_CAM_LINK_DEPS) $(MODULE_ROOT)/Makefile | $(LIB_DIR)
	@for f in $(IPC_CAM_LINK_DEPS); do \
		if [ ! -f "$$f" ]; then echo "ERROR: missing link dependency $$f"; exit 1; fi; \
	done
	$(CC) -shared -o $@ $(OBJS) $(LDFLAGS) $(IPC_CAM_SO_LDFLAGS)
	@echo "Built $@ with $(IPC_CAM_SO_LDFLAGS)"

install: $(TARGET_SO)
	@mkdir -p "$(APPLICATION_INSTALL_DIR)/lib"
	@mkdir -p "$(ROOTFS_DIR)/utils/usr/lib"
	@mkdir -p "$(ROOTFS_DIR)/usr/lib"
	cp -f "$(TARGET_SO)" "$(APPLICATION_INSTALL_DIR)/lib/"
	cp -f "$(TARGET_SO)" "$(ROOTFS_DIR)/utils/usr/lib/"
	cp -f "$(TARGET_SO)" "$(ROOTFS_DIR)/usr/lib/"
	@for f in $(IPC_CAM_LINK_DEPS); do \
		if [ -f "$$f" ]; then \
			cp -af "$$f" "$(ROOTFS_DIR)/utils/usr/lib/"; \
			cp -af "$$f" "$(ROOTFS_DIR)/usr/lib/"; \
			echo "Installed $$(basename $$f) (ipc_cam runtime dep)"; \
		else \
			echo "ERROR: missing ipc_cam runtime dep $$f"; exit 1; \
		fi; \
	done
	@echo "Installed libipc_cam_app.so"
else
$(TARGET_A): $(OBJS) $(IPC_CAM_LINK_DEPS) $(MODULE_ROOT)/Makefile | $(LIB_DIR)
	@for f in $(IPC_CAM_LINK_DEPS); do \
		if [ ! -f "$$f" ]; then echo "ERROR: missing link dependency $$f"; exit 1; fi; \
	done
	$(AR) rcs $@ $(OBJS)
	@echo "Built $@"

install: $(TARGET_A)
	@mkdir -p "$(APPLICATION_INSTALL_DIR)/lib"
	cp -f "$(TARGET_A)" "$(APPLICATION_INSTALL_DIR)/lib/"
	@echo "Installed libipc_cam_app.a"
endif

clean:
	rm -rf "$(BUILD_DIR)"
