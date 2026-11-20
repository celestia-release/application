<p align="right"><b>中文</b> | <a href="README.md">English</a></p>

# Luna 应用：`camera`（拍照 / 预览）

- 独立编成 `libcamera_app.so`（命名与 `application/packages/tablet/camera` 的 `app.json` 对齐）。
- 入口是 `app_entry_start_camera`，由 launcher 通过 `dlopen` + `dlsym` 或 `installed_apps.json` 拉起。
- 图标沿用 Nova Camera：`res/icon.png`（`nova/camera/res/` 里是占位图，Luna 的 `camera/res/` 与它同步）。
- 预览复用 `tablet_camera_create()`，走现有 framework / LVGL 通路。
