# volc_engine build tweaks. Everything app-specific lives here; the shared
# tablet Makefile only knows this file through the generic app.mk include.
#
# tablet_volc.c calls volc_engine_init()/volc_engine_set_*() implemented in
# libvolc_engine_svc.so. Record DT_NEEDED + rpath so dlopen(libvolc_engine_app.so)
# relocates without requiring svc preloaded: musl ld rejects undefined symbols
# at dlopen (RTLD_NOW), and the svc preload (volc_engine_preload_svc_so) runs
# only from inside this app's entry, i.e. after the dlopen of this SO itself.
VOLC_ENGINE_SHLIB_LIBS := -L$(APPLICATION_INSTALL_DIR)/lib -Wl,-rpath,/usr/lib \
	-Wl,--no-as-needed -lvolc_engine_svc -Wl,--as-needed
