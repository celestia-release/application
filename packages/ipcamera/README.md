<p align="right"><a href="README.en.md">English</a> | <b>中文</b></p>

# packages/ipcamera

无屏（headless / `launch_mode=ipcamera`）设备类型应用壳。与 `wearable` / `tablet` 同级。

- 主程序二进制：`ipc_main`（`CONFIG_APP_MAIN_TARGET`，名称暂不改）
- 通用业务 App：`ipc_cam/` → `libipc_cam_app.so`（`CONFIG_APP_IPCAMERA_IPC_CAM=y`）
- 契约：导出 `module_init` / `module_start`，`module_start` 内阻塞保活
- 不依赖 `packages/common`（无 LVGL）

系统服务（`application/services/uvc`、`weather` 等）不属于本形态。
