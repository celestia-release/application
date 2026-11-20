<p align="right"><b>English</b> | <a href="README.zh-CN.md">中文</a></p>

# Luna Application: `camera` (Photo / Preview)

- Built as standalone `libcamera_app.so` (naming aligned with `application/packages/tablet/camera` `app.json`).
- Entry point: `app_entry_start_camera`, launched by launcher via `dlopen` + `dlsym` or `installed_apps.json`.
- Icon follows Nova Camera convention: `res/icon.png` (placeholder in `nova/camera/res/`, Luna side `camera/res/` syncs with it).
- Preview reuses `tablet_camera_create()`, same pipeline as existing framework / LVGL.
