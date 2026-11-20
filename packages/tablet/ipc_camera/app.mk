# ipc_camera build tweaks. Everything app-specific lives here; the shared
# tablet Makefile only knows this file through the generic app.mk include.
#
# Gateway protocol headers live under setting/, camera headers under camera/.
$(APP_SO_BUILD_DIR)/ipc_camera/obj/%.o: APP_CFLAGS_EXTRA := -I$(MODULE_ROOT_NORM)/setting
# tablet_video_ctrl.c is shared camera code built into ipc_camera's SO.
# The explicit rule overrides the generic %.o pattern, so it must consume
# APP_CFLAGS_EXTRA itself — that is how the -Icamera below reaches the compile.
$(APP_SO_BUILD_DIR)/ipc_camera/obj/tablet_video_ctrl.o: APP_CFLAGS_EXTRA := -I$(MODULE_ROOT_NORM)/camera
$(APP_SO_BUILD_DIR)/ipc_camera/obj/tablet_video_ctrl.o: $(MODULE_ROOT_NORM)/setting/tablet_video_ctrl.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fPIC $(APP_CFLAGS_EXTRA) -I$(LAUNCHER_DIR) -c $< -o $@
IPC_CAMERA_APP_EXTRA_OBJS := $(APP_SO_BUILD_DIR)/ipc_camera/obj/tablet_video_ctrl.o
# Resolve gateway protocol symbols at link time; dlopen(libipc_camera) needs DT_NEEDED
# or relocation fails. libnebula_protocol.so references zxing_scan_qrcode (from libzxing);
# without -lzxing, musl ld fails when dlopen pulls nebula_protocol.
IPC_CAMERA_SHLIB_LIBS := -L$(FRAMEWORK_INSTALL_DIR)/lib -lnebula_protocol -lrtsp_protocol \
	-L$(THIRD_PARTY_INSTALL_DIR)/zxing/lib -Wl,-rpath,/usr/lib \
	-Wl,--no-as-needed -lzxing -Wl,--as-needed
