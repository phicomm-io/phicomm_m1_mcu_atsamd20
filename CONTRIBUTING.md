# Contributing

Use `feature/<topic>` branches based on `dev`. Open a PR to `dev`; promote tested changes from `dev` to `main`. `main` contains the stable source. Release tags must name commits reachable from `main`. Hotfixes use a PR to `main` and a follow-up PR to `dev`.

Keep changes focused and preserve unrelated work. Explain the trigger, resulting behavior and validation. Record software tests, published artifacts and physical-device results separately; use PASS, FAIL, PARTIAL, BLOCKED or NOT_RUN.

## Interfaces and configuration

Firmware, HTTP API, MQTT protocol and persisted configuration have independent versions. A firmware major version alone does not change a wire protocol. Add optional fields and advertise capabilities. Readers tolerate unknown optional response fields; writers validate types, bounds and unsupported parameters before changing state. Use stable error codes, never translated text, for business logic. Keep request IDs and write retry semantics intact.

Changes to stored layouts require explicit schema versions, migration fixtures, interrupted-write tests and upgrade/downgrade notes. Preserve credentials and product settings where possible. Partial updates preserve omitted values. Apply changes only after validation, and restore the prior in-memory state when persistence fails.

Keep identifiers, MQTT topics and entity unique IDs independent of display language. Translate user-visible strings through i18n keys with matching English/Chinese placeholders. Preserve names entered by users.

## Validation and release

Run the checks in the repository workflow for the affected subsystem. Firmware changes require all affected variants to build within flash/RAM limits. Changes to commands, migration or discovery require behavior tests. Device tests must identify the target and image; build or upload success alone is not runtime acceptance.

PRs and `dev` produce candidate artifacts. Production signing and publication are restricted to release tags from `main`. Frontend dependencies use a complete commit and verified SHA-256, including offline builds. Freeze historical release artifacts and notes; never replace a published image under the same version.

## 跨仓库接口 / Shared contracts

接口、协议与配置兼容性以 [IoTCloud 开发文档](https://phicomm-io.github.io/iotcloud-community-protocols/) 为集中入口。跨仓库变更先更新规范、Schema 和样例，在实现 PR 中引用同一个规范提交，并更新 `protocols.lock.json`；CI 校验完整提交与接口索引 SHA-256。

Shared interfaces are maintained in [IoTCloud Developer Docs](https://phicomm-io.github.io/iotcloud-community-protocols/en/). Update the contract and examples first, reference the same specification commit in implementation PRs, and update the pinned `protocols.lock.json`.
