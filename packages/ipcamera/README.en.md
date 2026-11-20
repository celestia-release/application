<p align="right"><b>English</b> | <a href="README.md">中文</a></p>

# packages/ipcamera

Headless application shell (`launch_mode=ipcamera`). Sibling of `wearable` and `tablet`.

- Main binary: `ipc_main` (`CONFIG_APP_MAIN_TARGET`; the name stays)
- Shared business app: `ipc_cam/` → `libipc_cam_app.so` (`CONFIG_APP_IPCAMERA_IPC_CAM=y`)
- Contract: export `module_init` / `module_start`. `module_start` blocks and keeps the process alive
- Does not depend on `packages/common` (no LVGL)

System services (`application/services/uvc`, `weather`, and the rest) are not part of this form factor.
