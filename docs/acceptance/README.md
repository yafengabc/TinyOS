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
- **commit 字段必须是已推送的 sha**：卡片在 `git push` 之后写，或同一次 push 里 amend。
  pre-rebase 的 sha 在仓库里不存在，卡片的可追溯性就没了。
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
| 2026-11-17 | A-1 board capability + target/config validation | `3a46de1` | RP2350 Cortex-M33 SMP（COM14） | T0–T3 PASS；T4 `NOT RUN` | [A-1 RP2350](2026-11-17-a1-rp2350smp-3a46de1.md) |
| 2026-11-17 | A-2 ABI single source + offset asserts | `cdd13b0` | RP2350 Cortex-M33 SMP（COM14） | T0–T3 PASS；T4 `NOT RUN` | [A-2 RP2350](2026-11-17-a2-rp2350smp-cdd13b0.md) |
| 2026-11-17 | A-4 flash exclusive + cache seams | `b49d49f` | RP2350 Cortex-M33 SMP（COM14） | T0–T4 PASS | [A-4 RP2350](2026-11-17-a4-rp2350smp-b49d49f.md) |
| 2026-11-17 | A-6 pre-tick wait / LCD baseline | `b73db94` | RP2350 Cortex-M33 SMP（COM14） | T0–T3 PASS；完整 1e6 转换 `NOT RUN` | [A-6 RP2350](2026-11-17-a6-rp2350smp-b73db94.md) |
| 2026-11-17 | A-6 full 1e6 conversion | `4577fd6` | RP2350 ARM + RV32 SMP（COM14） | T0–T4 PASS | [A-6 full](2026-11-17-a6full-rp2350-4577fd6.md) |
| 2026-11-17 | A-6 long pre-tick bounds | `49dfda6` | RP2350 ARM + RV32 SMP（COM14） | T0–T4 PASS | [A-6 bounds](2026-11-17-a6bounds-rp2350-49dfda6.md) |
| 2026-11-17 | USB CDC long-output flush | `7fc425d` | RP2350 ARM + RV32 USB CDC（COM17） | T0–T3 PASS；T4 `NOT RUN` | [USB CDC](2026-11-17-usbcdc-rp2350smp-7fc425d.md) |
| 2026-11-17 | A-3 exec_format seam | `c25a740` | RP2350 Cortex-M33 SMP（COM14） | T0–T3 PASS；T4 `NOT RUN` | [A-3 RP2350](2026-11-17-a3-rp2350smp-c25a740.md) |
| 2026-11-17 | A-8 KASSERT + kfree halt | `d15fe16` | RP2350 Cortex-M33 SMP（COM14） | T0–T3 PASS；proc_set_state `NOT RUN` | [A-8 RP2350](2026-11-17-a8-rp2350smp-d15fe16.md) |
| 2026-11-17 | A-8 full proc_set_state | `5d19a24` | RP2350 ARM + RV32 SMP（COM14） | T0–T4 PASS | [A-8 full](2026-11-17-a8full-rp2350-5d19a24.md) |
| 2026-11-17 | deep 6 clean kill | `da9979f` | RP2350 ARM + RV32 SMP（COM14） | T0–T3 PASS；T4 `NOT RUN` | [deep 6](2026-11-17-deep6-rp2350-da9979f.md) |
| 2026-11-17 | A-1/A-2/A-4/A-6 组合回归 | `e5c9cac` | ESP32-C3（COM4） | T0–T3 PASS；soak 9 轮无复位；goc32 `FAIL`（另记 issue） | [C3](2026-11-17-a1-a2-c3-e5c9cac.md) |
| 2026-11-17 | W1/W2 combined check | `d15fe16` | RP2350 Hazard3 RV32 SMP（COM14） | T0–T2 PASS；deep 6 FAIL；T4 PASS | [W1/W2 RV32](2026-11-17-w1w2-rp2350rvsmp-d15fe16.md) |
| 2026-11-17 | goc32 shared-header compatibility | `4fc1c74` | host / goc32 + native32 | T0–T2 PASS；T3/T4 `NOT RUN` | [goc32](2026-11-17-goc32-host-4fc1c74.md) |
| 2026-11-17 | A-3/A-8 组合回归 | `0b7b4fe` | ESP32-C3（COM4） | T0–T3 PASS；priotest 5/5（宿主空闲）；soak 5 轮无复位 | [C3 A-3/A-8](2026-11-17-a3-a8-c3-0b7b4fe.md) |
| 2026-11-17 | B-3 LZ4TOS V1 | `d0a3cee` | QEMU rv32/rv32smp/arm；11 目标 | T0–T2 PASS；V0/V1 两分支都过；T3 `NOT RUN` | [B-3](2026-11-17-b3-lz4tos-v1-rv32smp-d0a3cee.md) |
| 2026-11-17 | B-3 + 复核修正在 C3 | `736173c` | ESP32-C3（COM4） | T0–T3 PASS；soak 5 轮 resets 0 / errors 0 | [C3](2026-11-17-b3-c3-736173c.md) |
| 2026-11-17 | B-1a 用户排除进 board.conf | `0b1bbb1` | rp2040 构建 + 11 目标 | T0/T1 PASS；ramdisk 字节相同 | [B-1a](2026-11-17-b1a-rp2040-0b1bbb1.md) |
| 2026-11-17 | B-1b chip 表 + 生成头 | `e783748` | QEMU + 11 + x86 + ESP32-C3 | T0–T3 PASS；不一致时启动 panic 已验证 | [B-1b](2026-11-17-b1b-qemu-c3-e783748.md) |
| 2026-11-17 | B-1c Targets 只剩镜像选择 | `c56cc5c` | 12 目标 + QEMU | T0–T2 PASS；rp2040/C3 镜像字节相同 | [B-1c](2026-11-17-b1c-hash-c56cc5c.md) |
| 2026-11-17 | A-3 表分派补齐 | `30cd699` | QEMU + 11 + ESP32-C3 | T0–T3 PASS；mock 运行验证见 §2d | [A-3](2026-11-17-a3-table-30cd699.md) |
| 2026-11-17 | B-2a RP park 共享状态 | `20004a5` | 6 个 RP 目标 + QEMU | T0–T2 PASS；T3 `NOT RUN`（§2f） | [B-2a](2026-11-17-b2a-park-state-20004a5.md) |
| 2026-11-17 | B-6a C7 接口 + loopback lower | `9889c89` | QEMU 3 + 11 + ESP32-C3 | T0–T3 PASS；rselftest ok | [B-6a](2026-11-17-b6a-remote-ep-9889c89.md) |
| 2026-11-17 | A-5 mem_domain 接缝 | `51f561c` | QEMU 3 + 11 + x86 + ESP32-C3 | T0–T3 PASS；窗口数值未变 | [A-5](2026-11-17-a5-mem-domain-51f561c.md) |
| 2026-11-17 | A-3 第二容器 mock | `2ab6745` | QEMU rv32（exec_mock） | T0–T2 PASS；hello.mock 加载成功 | [A-3 mock](2026-11-17-a3-mock-2ab6745.md) |
| 2026-11-17 | B-2a + B-3 hardware smoke | `a16513b` | RP2350 ARM + RV32（COM14） | T0/T2/T3/T4 PASS；T1 x86 FAIL；RP2040 `NOT RUN` | [B-2a/B-3 RP2350](2026-11-17-b2a-b3-rp2350-a16513b.md) |
| 2026-11-17 | current HEAD smoke | `63f336a` | RP2350 ARM + RV32 (COM14) | T0/T2/T3 PASS; T1 x86 FAIL; T4 `NOT RUN` | [current HEAD](2026-11-17-head63f336a-rp2350-63f336a.md) |
| 2026-11-17 | B-8 heap/ofile 收缩 | `f4bafc0` | rv32smp 对比 + QEMU + ESP32-C3 | T0–T3 PASS；kernel 省 224 B | [B-8](2026-11-17-b8-shrink-f4bafc0.md) |
| 2026-11-17 | B-1c 漏迁修复（#8–#11） | `81411d4` | 12 目标 + QEMU + doctor | T0–T2 PASS；C3 字节相同 | [B-1c fixes](2026-11-17-b1c-fixes-81411d4.md) |
| 2026-11-17 | B-4a LCD 协议上移 | `968b246` | 11 目标 + QEMU arm/rv32/rv32smp | T0–T2 PASS；T3/T4 `NOT RUN`（§2e，issue #15） | [B-4a](2026-11-17-b4a-lcd-proto-968b246.md) |
| 2026-11-17 | fs_ro 构建修复（#14） | `a271253` | fs_ro x86/rp2350smp/esp32c3 + 12 目标 + QEMU 3 | T0–T2 PASS；读写路径 `.text` 逐字节相同 | [fs_ro](2026-11-17-fsro-ramdisk-a271253.md) |
| 2026-11-17 | x86 发行版工具链构建 + 起机 | `b2db294` | QEMU i386（CI `qemu smoke`，run `44015548419`） | T0–T2 PASS；本地换 LD 前后镜像逐字节相同 | [x86](2026-11-17-x86-ci-b2db294.md) |
| 2026-11-17 | CI smoke 并行化 + soak 移出 | `0207150` | CI `44017659699`：rv32/rv32smp/arm/x86 + fs_ro | T0–T2 PASS；295 s → ≤94 s/leg | [CI smoke](2026-11-17-ci-smoke-matrix-0207150.md) |
| 2026-11-17 | B-3 第二节 `exec_format.version` + `/proc/execfmt` | `075a90c` | 12 目标 + fs_ro + QEMU 4（CI `44024533427`） | T0–T2 PASS；soak 3 轮通过 | [B-3 two](2026-11-17-b3-execfmt-version-075a90c.md) |
| 2026-11-17 | A-2 收尾：goc32 syscall 快照（#19） | `f4e1b27`（goc32 `d3b6cd0`） | host + goc32；CI `44030431025` | T0 PASS；T1/T2 `NOT RUN`（未改构建输入） | [snapshot](2026-11-17-a2-goc32-snapshot-f4e1b27.md) |
