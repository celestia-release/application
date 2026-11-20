<p align="right"><b>English</b> | <a href="README.md">中文</a></p>

# Celestia · application

Application layer. It is built on [`framework`](https://github.com/celestia-release/framework) and runs on Anyka SoCs.

## Modules

| Directory | Role |
|-----------|------|
| `app_main/` | Process entry and runtime config (`settings.json`: program, launch mode, module load) |
| `packages/common/` | GUI pieces shared by tablet and wearable |
| `packages/tablet/` | Nova launcher and built-in apps (camera, clock, setting, file manager, …) |
| `packages/wearable/` | Luna launcher and built-in apps (camera, dashcam, power_keyd, …) |
| `packages/ipcamera/` | Headless business shared libraries (`module_init` / `module_start`) |
| `services/` | Application services (`uvc`, `weather`, …) |
| `cloud/` | Cloud adapters (`aliyun`, `volc_engine`, `mike`, …) |
| `tests/` | Unit and integration tests |

## Build

Build from the repo workspace root:

```bash
./build.sh
```

## License

Apache License 2.0. See [LICENSE](LICENSE) and [NOTICE](NOTICE).

Closed-source `.a` / `.so` files under `cloud/aliyun` and `cloud/volc_engine` are **not in this tree** and are not covered by Apache-2.0. If you need those cloud features, obtain the binaries from the vendor and put them back in `lib/` without committing them. Cloud credentials stay as placeholders or environment variables. See [open-source risks](https://github.com/celestia-release/docs/blob/main/open-source.md).

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). Commits use the DCO (`git commit -s`).
