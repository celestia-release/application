<p align="right"><a href="README.en.md">English</a> | <b>中文</b></p>

# Celestia · application

Celestia 应用层，构建于 [`framework`](https://github.com/celestia-release/framework) 之上，运行于 Anyka SoC 平台。

## 模块

| 目录 | 说明 |
|------|------|
| `app_main/` | 应用入口与运行时配置解析（`settings.json`：程序、启动模式、模块加载） |
| `packages/common/` | tablet / wearable 共用 GUI 组件 |
| `packages/tablet/` | Nova 大屏 launcher 与内置应用（camera、clock、setting、file_manager 等） |
| `packages/wearable/` | Luna 小屏 launcher 与内置应用（camera、dashcam、power_keyd 等） |
| `packages/ipcamera/` | 无屏业务 SO（`module_init` / `module_start`） |
| `services/` | 应用级服务（`uvc`、`weather` 等） |
| `cloud/` | 云服务接入适配层（`aliyun`、`volc_engine`、`mike` 等） |
| `tests/` | 单元与集成测试 |

## 构建

本仓库由顶层 `repo` 工程统一构建，请使用工程根目录的 `build.sh`：

```bash
./build.sh          # 参见 build.sh --help 获取目标板与选项
```

## 许可

以 **Apache License 2.0** 授权，详见 [LICENSE](LICENSE) 与 [NOTICE](NOTICE)。

> `cloud/aliyun`、`cloud/volc_engine` 的闭源 `.a` / `.so` **已从本树删除**，不受 Apache-2.0 覆盖。需要云功能时向厂商取得二进制后再放到原来的 `lib/` 目录，且不要提交。云凭据只用占位符或环境变量，见 [docs/open-source.md](https://github.com/celestia-release/docs/blob/main/open-source.md)。

## 贡献

见 [CONTRIBUTING.md](CONTRIBUTING.md)（采用 DCO，提交需 `-s` 签名）。
