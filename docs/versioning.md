# Git tag 与 MCU 版本

MCU 与 Wi-Fi MCU 使用相同的主规则：tag 为 `vMAJOR.MINOR.PATCH`，设备版本去掉 `v`，再追加当前提交的前六位 hash。例如 tag `v0.9.0` 的提交为 `abcdef...`，设备返回 `0.9.0-abcdef`。初始版本为 `0.9.0`，不再手工维护 `oss-*` 常量。

| 构建来源 | 构建记录 version | UART `0x11` 返回 |
| --- | --- | --- |
| 干净的 `v0.9.0` tag | `0.9.0-abcdef` | `0.9.0-abcdef` |
| tag 后的干净提交 | 最近可达 tag 的版本 + 当前 hash | 与构建记录相同 |
| 有未提交修改或未跟踪源码 | `0.9.0-abcdef-dirty` | `0.9.0-abcdef-d` |
| 尚无可达版本 tag | 基准 `0.9.0` + 当前 hash，按需加 dirty | 同上，dirty 缩写为 `-d` |

普通构建选择当前提交最近可达的合法版本 tag，不使用其他分支上不可达的较大 tag。必须在本仓库的 Git checkout 内构建，避免误用父仓库版本。已有 Wi-Fi MCU 的版本解析器只接受固定 20 字节应答中的最多 16 字符；因此仅将 UART 的 `-dirty` 缩写为 `-d`，版本号和 hash 不截断，超长版本会使构建失败。

`make test`、`make ota`、`make hal-check` 自动生成：

- `include/m1/generated/build_version.h`：编译时版本，已加入忽略规则。
- `build/version.json`：同一次 Make 调用的 Git 版本快照。

`m1-app.json`、`m1-boot.json`、OTA manifest 均读取该快照，记录 `version`、`uart_version`、`base_version`、`git_tag`、完整 `git_commit` 和 `dirty`，保留原来的 `source_commit/source_dirty` 字段。打包阶段不重新解析 Git。独立运行原厂对照工具时，也会先生成该头文件；不需要手工填写版本。

校验指定 tag 的本地构建：

```sh
make test ota VERSION_TAG=v0.9.0
```

该模式要求 tag 精确指向 HEAD 且源码干净；错误 tag、脏源码或 UART 版本超长均报错。后续版本在通过验证的提交上新建递增 tag，例如 `v0.9.1`，不要移动已有 tag。

GitHub Actions 拉取完整 Git 历史与 tag，tag 构建自动使用上述严格校验；分支和 PR 构建也携带对应提交 hash。Artifact 名称包含解析出的版本，例如 `m1-mcu-ota-0.9.0-abcdef`。

自 `v0.9.1` 起，推送版本 tag 且测试/构建通过，CI 自动创建 GitHub 开发预览 Release。tag 必须位于 `main` 历史中，说明必须来自该 tag 已提交的唯一 Release Log 章节。已有 `v0.9.0` 保持原样，不补改旧 tag。重跑核对已有附件和说明，禁止覆盖不同内容；实机状态仍单独记录，不自动刷机。维护步骤见 [发布流程](release/README.md)。
