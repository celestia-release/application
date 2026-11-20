# setting build tweaks. Everything app-specific lives here; the shared tablet
# Makefile only knows this file through the generic app.mk include.
#
# tablet_setting_storage.c needs USB MSC / demo storage support.
ifneq ($(CONFIG_APP_USB_MSC_SUPPORT),y)
SETTING_SRCS_FILTER_OUT += $(MODULE_ROOT_NORM)/setting/tablet_setting_storage.c
endif
ifneq ($(CONFIG_APP_TABLET_LV_DEMO_STORAGE),y)
SETTING_SRCS_FILTER_OUT += $(MODULE_ROOT_NORM)/setting/tablet_setting_storage.c
endif
# tablet_setting_camera.c includes tablet_camera.h (under camera/).
$(APP_SO_BUILD_DIR)/setting/obj/%.o: APP_CFLAGS_EXTRA := -I$(MODULE_ROOT_NORM)/camera
# The settings app links the launcher registry directly.
SETTING_APP_EXTRA_OBJS := $(APP_SO_BUILD_DIR)/setting/obj/tablet_app_registry.o
$(APP_SO_BUILD_DIR)/setting/obj/tablet_app_registry.o: $(LAUNCHER_DIR)/tablet_app_registry.c $(LAUNCHER_DIR)/tablet_app_registry.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fPIC -I$(LAUNCHER_DIR) -c $< -o $@
# ZXing is built as a third-party shared library: /usr/lib/libzxing.so
# Setting QR pairing must link with it so dlopen(libsetting_app.so) pulls libzxing.so automatically.
# libnetwork: tablet_setting_wifi.c calls wifi_psk_encrypt/wifi_psk_decrypt (settings.json
# wifi.psk) directly (not via the s_wifi_network ops-struct function pointers, which are
# resolved lazily at call time from app_main_get_network() regardless of DT_NEEDED). Those
# two are real symbol refs into libnetwork.so, so this DSO needs its own DT_NEEDED on
# libnetwork.so — app_main's CONFIG_APP_MAIN_DEFER_HEAVY_SO scheme dlopen()s libnetwork.so
# only after boot, and there is no guarantee it is already resident when the user opens the
# Settings app, so relying on incidental prior loading would be fragile.
SETTING_SHLIB_LIBS := -L$(THIRD_PARTY_INSTALL_DIR)/zxing/lib -Wl,-rpath,/usr/lib \
	-Wl,--no-as-needed -lzxing -Wl,--as-needed \
	-L$(FRAMEWORK_INSTALL_DIR)/lib -Wl,-rpath,/usr/lib \
	-Wl,--no-as-needed -lnetwork -Wl,--as-needed \
	-L$(THIRD_PARTY_INSTALL_DIR)/cjson/lib -Wl,--no-as-needed -lcjson -Wl,--as-needed
