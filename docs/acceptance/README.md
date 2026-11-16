# 验收记录

一次运行一个文件，只存摘要；原始日志不进 git。

## 规则

- **文件名**：`YYYY-MM-DD-<change>-<target-or-board>-<shortsha>.md`
  （例：`2026-11-17-a0-rv32smp-7453bd6.md`）。
- **头部字段**与 [`docs/TESTING.md`](../TESTING.md) §10 一致；正文只放 PASS/FAIL 表和
  关键输出（≤30 行）。
- `NOT RUN` 必须显式写出，并链接到 [`docs/OPEN-ITEMS.md`](../OPEN-ITEMS.md) 的阻塞项。
- 摘要要能**单独读懂**：跑了什么命令、结果是什么、别人怎么重跑。
- **原始日志不入库**：QEMU / 串口输出留在本地或 CI artifact（`test-logs`）；确需长期
  保存的贴到 issue 或 commit 说明。仓库里出现几百 KB 的 `.log` 说明这一步做错了。
- 只追加，不改旧记录；同一个 commit 重跑就加新文件、新日期。
- **不回填历史**：从 2026-11-17 起。更早的结论与实测在
  [`docs/HISTORY.md`](../HISTORY.md)。

## 与其它文档的分工

| 文档 | 回答什么 |
|---|---|
| `docs/TESTING.md` | 怎么跑（T0–T4、命令、判据） |
| `docs/acceptance/`（本目录） | 跑过什么（一次运行一份摘要） |
| `docs/HISTORY.md` | 为什么这么改（根因、修复、当时的证据结论） |
| `docs/OPEN-ITEMS.md` | 还没做什么、被什么阻塞 |

## 索引

| 日期 | 变更 | commit | 目标 / 板 | 结果 | 文件 |
|---|---|---|---|---|---|
| 2026-11-17 | A-0 SMP exec 并发 | `7453bd6` | QEMU rv32/rv32smp/arm；11 目标构建 | T0/T1/T2 PASS；T3 `NOT RUN` | [A-0 QEMU](2026-11-17-a0-rv32smp-7453bd6.md) |
| 2026-11-17 | deep 6 0 8 guard + PIO 视觉 | `88ef705` | RP2040 单核（COM19/COM17） | T0–T4 PASS；`prot` 构建期不存在 | [deep6/guard](2026-11-17-deep6-guard-rp2040-88ef705.md) |
| 2026-11-17 | RP2040 SMP review 回归 | `88ef705` | RP2040 SMP（COM19/COM17） | T0–T4 PASS（RP2350 SMP `NOT RUN`） | [SMP regression](2026-11-17-smp-regression-rp2040smp-88ef705.md) |
| 2026-11-17 | A-0 实跑补测 | `7453bd6` | RP2040 SMP（COM19） | T3/T4 PASS；20/20 concurrent exec | [A-0 rerun](2026-11-17-a0-rerun-rp2040smp-7453bd6.md) |
| 2026-11-17 | A-0 SMP exec 并发 | `2fdd6e8`（代码 `7453bd6`） | RP2350 Hazard3 RV32 SMP（COM14/COM17） | T3/T4 PASS；RP2350 ARM `NOT RUN` | [A-0 RP2350 RV32](2026-11-17-a0-rp2350rvsmp-2fdd6e8.md) |
| 2026-11-17 | A-0 SMP exec 并发 | `2fdd6e8`（代码 `7453bd6`） | RP2350 Cortex-M33 SMP（COM14） | T3/T4 PASS；USB CDC 长输出截断 5/6 | [A-0 RP2350 ARM](2026-11-17-a0-rp2350smp-2fdd6e8.md) |
| 2026-11-17 | A-1 board capability + target/config validation | `aadd711` | RP2350 Cortex-M33 SMP（COM14） | T0–T3 PASS；T4 `NOT RUN` | [A-1 RP2350](2026-11-17-a1-rp2350smp-aadd711.md) |
| 2026-11-17 | A-2 ABI single source + offset asserts | `cdd13b0` | RP2350 Cortex-M33 SMP（COM14） | T0–T3 PASS；T4 `NOT RUN` | [A-2 RP2350](2026-11-17-a2-rp2350smp-cdd13b0.md) |
