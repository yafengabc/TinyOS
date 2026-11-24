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
| 2026-11-17 | T2 soak 1/3 根因：harness 等待窗口 | `3dae711` | QEMU rv32/rv32smp/arm/x86 | T0/T2 PASS；soak 3/3；T1/T3/T4 `NOT RUN` | [soak window](2026-11-17-soak-window-3dae711.md) |
| 2026-11-17 | B-4b 一份 ST7789 驱动供两 ISA | `fe1dc85` | rp2350smp/rp2350rvsmp + 12 目标 + QEMU 4（CI `44039230725`） | T0–T2 PASS；两 ELF 与合并前逐字节相同 | [B-4b](2026-11-17-b4b-one-driver-fe1dc85.md) |
| 2026-11-17 | smoke 超时诊断 + soak 窗口 15 s | `a8d55b7` | QEMU rv32/rv32smp/arm | T0/T2 PASS；诊断行按预期出现 | [smoke timeout](2026-11-17-smoke-timeout-a8d55b7.md) |
| 2026-11-17 | B-1d 镜像清单变成数据 | `45fc869` | 13 目标 + fs_ro + QEMU 4 | T0–T2 PASS；13/13 build-config 逐字节相同 | [B-1d](2026-11-17-b1d-targets-conf-45fc869.md) |
| 2026-11-17 | 板组合 `base =` + `board/pico2`（#7 B1/B2） | `b6be508` | 13 目标 + fs_ro + QEMU 4 | T0–T2 PASS；Pico 2 未上板（T3 `NOT RUN`） | [pico2 variant](2026-11-17-pico2-variant-b6be508.md) |
| 2026-11-17 | flash 容量/偏移变成板级事实（#7 前置） | `cf60065` | 12 目标 + fs_ro + QEMU 4 | T0–T2 PASS；值未变；2m/1m 旋钮可用 | [flash facts](2026-11-17-flash-facts-cf60065.md) |
| 2026-11-17 | B-2b 前半：park 握手顺序共享 + host 模型 | `1120aac` | 6 个 RP 目标 + 12 目标 + QEMU 4 | T0–T2 PASS；T3 `NOT RUN`（#17） | [park ordering](2026-11-17-b2b-park-ordering-1120aac.md) |
| 2026-11-17 | current HEAD regression | `167db55` | RP2350 ARM + RV32 (COM14) | T0/T1/T3 PASS; T2 soak 1/3 (RV32 output capture flake); LCD visual `NOT RUN` | [current 167db55](2026-11-17-current167db55-rp2350.md) |
| 2026-11-17 | current HEAD 0751f9d regression | `0751f9d` | RP2350 ARM + RV32 (COM14) | T0/T1/T3 PASS; T2 soak 1/3 (RV32 flake); LCD visual `NOT RUN` | [current 0751f9d](2026-11-17-current0751f9d-rp2350.md) |
| 2026-11-17 | B-4a LCD ???????issue #15? | `0751f9d` | RP2350 ARM + RV32 (COM14) | T3 LCD ?? PASS???????/LCD OK + ?? ls /bin ????heartbeat 6/6 | [LCD visual](2026-11-17-lcd-visual-0751f9d-rp2350.md) |
| 2026-11-17 | B-2b park 1000 ????? | `313270b` | RP2350 ARM + RV32 (COM14) | T0/T1/T3/T4 PASS??? 1000x dd/rm bad=0?smpbench ~2x?T2 soak 1/3 harness flake | [park 1000](2026-11-17-b2b-park-1000round-313270b.md) |
| 2026-11-17 | T2 soak ?????15s ??+??? | `fd3f079` | QEMU rv32/rv32smp/arm | T2 soak 3/3 PASS??????????????? flake | [soak verify](2026-11-17-soak-verify-fd3f079.md) |
| 2026-11-17 | B-2b 后半 park 序列共享（#17） | `73b7914` | RP2350 ARM + RV32 (COM14) | T0–T4 PASS：两板 1000x dd/rm bad=0，smpbench ~2x；.data 逐字节相同 | [park merge](2026-11-17-b2b-park-merge-rp2350.md) |
| 2026-11-17 | RP2040 专项验收（#12） | `2be39bd` | rp2040smp / COM19 / F:(RPI-RP2) | 四判据全过：心跳6/6、1000x dd/rm bad=0、reboot 持久化、goc32 V0 .tos 执行 | [RP2040 accept](2026-11-17-rp2040-accept-2be39bd.md) |
| 2026-11-17 | Pico 2 A 轨 SWD 现场 | `2be39bd` | Pico 2 + Debugprobe (SWD) | 复现 PC=0xeffffffe double fault；boot2/vector 二进制排除项；根因未定 | [A track scene](2026-11-17-pico2-a-track-swd-scene.md) |
| 2026-11-18 | 目标/板名改名 + `tinyos-` 前缀 | `e52ad7d` | 13 目标 + fs_ro + QEMU 4 | T0–T2 PASS；chip flag 回归已修；硬件 `NOT RUN` | [rename](2026-11-18-rename-boards-e52ad7d.md) |
| 2026-11-18 | ATK 回归复核：board 重命名 + generic 基线 | `2d87ded` | ATK-DNRP2350AM (COM14) ARM+RV32；generic 基线 | T0/T1 PASS；两板 heartbeat 6/6；LED/键冒烟；generic-flash 真机 6/6 | [ATK review](2026-11-18-atk-review-rename-2d87ded.md) |
| 2026-11-18 | generic 镜像 + `tos build all` | `a06be75`、`a499be8` | 15 目标 + fs_ro + QEMU 4 | T0–T2 PASS；ATK UF2 逐字节相同；generic 上板 `NOT RUN` | [generic + all](2026-11-18-generic-targets-build-all-a499be8.md) |
| 2026-11-18 | ESP32-C3 改名 `esp32c3-generic` | `c77319d` | ESP32-C3 / COM4 + 15 目标 + QEMU 4 | T0–T3 PASS：真机 boot + 24/24；T4 `NOT RUN` | [C3 generic](2026-11-18-c3-generic-c77319d.md) |
| 2026-11-18 | ATK 全 RP2350 目标 + embedded UF2 修复 | `199339d`+fix | ATK-DNRP2350AM (COM14) | `build all` 15/15；7 个 RP2350 目标真机 6/6；修 rp2350-generic/rv-generic UF2 扇区不对齐不启动 | [all RP2350](2026-11-18-atk-rp2350-all-targets.md) |
| 2026-11-18 | soak 假 FAIL 修复（#23） | `5f0eb55` | QEMU rv32/rv32smp/arm ×3 轮 + smoke 4 | T0–T2 PASS：soak 连续 3 次 3/3；T3/T4 `NOT RUN` | [soak flake](2026-11-18-soak-flake-5f0eb55.md) |
| 2026-11-18 | Pico 2：标准 XIP 设置（boot2） | `d16f949` | 15 目标构建；ATK 镜像逐字节不变 | T0–T1 PASS；Pico 2 真机 `NOT RUN`（待刷） | [XIP setup](2026-11-18-pico2-xip-setup-d16f949.md) |
| 2026-11-18 | Pico 2：ARM/RV32 × 单核/SMP 四个镜像 | `4fd6857` | 18 目标构建 | T0–T1 PASS；四镜像真机 `NOT RUN`（待刷） | [ISA matrix](2026-11-18-pico2-isa-matrix-4fd6857.md) |
| 2026-11-18 | Pico 2 启动到 shell（SAU + 实测波特率 + 不改写 QMI） | `cff442f` | Pico 2 ARM（COM19）+ 18 目标 + QEMU 3 | T0/T1/T2 PASS；T3 Pico 2 ARM PASS（boot + heartbeat）；ATK / SMP / RV32 真机 `NOT RUN` | [Pico 2 boot](2026-11-18-pico2-boot-rp2350-cff442f.md) |
| 2026-11-18 | 启动轨迹开关 `boot_trace` + 提示符不再被打断 | `6ca4f18` | Pico 2（COM19）+ 18 目标 + QEMU 3 | T0/T1/T2 PASS；T3 Pico 2 PASS；ATK 真机 `NOT RUN` | [boot trace](2026-11-18-boot-trace-switch-6ca4f18.md) |
| 2026-11-18 | Pico 2 标准流程：flash smoke + 心跳 | `fdb515a` | Pico 2 ARM（COM19）+ 18 目标 + QEMU 3 | T0–T3 PASS（flash 7/7、heartbeat 6/6）；T4/ATK 真机 `NOT RUN` | [Pico 2 heartbeat](2026-11-18-pico2-heartbeat-fdb515a.md) |
| 2026-11-18 | Pico 2 四镜像心跳 + dd/rm + smpbench | `2349bbe` | Pico 2（COM19）×4 镜像 + 18 目标 + QEMU 3 | T0–T3 PASS（4/4 心跳 6/6、flash 7/7）；dd ARM 200/200 + RV32 340/340；smpbench 1.96–2.04x 无回归；ATK 真机 `NOT RUN` | [matrix](2026-11-18-pico2-matrix-ddrw-smpbench-2349bbe.md) |
| 2026-11-18 | ATK T3：两个 SMP 镜像（issue #7 硬件门槛） | `987d59c` | ATK-DNRP2350AM（COM14）×2 镜像 + 18 目标 + QEMU 3 | T0–T3 PASS（M33 SMP 15/15、RV32 SMP 17/17，含 dd 100 轮）；T4 `NOT RUN` | [ATK T3](2026-11-18-atk-smp-t3-987d59c.md) |
| 2026-11-18 | 复测远端 HEAD，修 boot_trace 默认值回归 | `00daf21` | ATK-DNRP2350AM（COM14）×2 镜像 + 18 目标 + QEMU 3 | T0–T3 PASS（M33 SMP 16/16、RV32 SMP 17/17）；T4/ATK 单核/Pico 2 `NOT RUN` | [retest](2026-11-18-atk-retest-00daf21.md) |
| 2026-11-18 | Pico 2 冷启动：按 pico-sdk 写 XOSC STARTUP | `a34c783` | Pico 2 rv32-smp（COM19）+ 18 目标 + QEMU 3 | T0–T3 PASS（拔插电源 10/10，修复前 ~1/2）；T4/ATK 真机 `NOT RUN` | [cold boot](2026-11-18-pico2-coldboot-crystal-1628fc5.md) |
| 2026-11-18 | 复测上游 HEAD：共享 XOSC 启动实现 | `e9588ee` | Pico 2 rv32-smp（COM19）+ 18 目标 + QEMU 3 | T0–T3 PASS（拔插电源 10/10）；ATK / RP2040 真机 `NOT RUN` | [shared xosc](2026-11-18-rp-xosc-shared-e9588ee.md) |
| 2026-11-18 | Pico 2 冷启动清零：SDK 顺序 + 20 ms 起振 | `0193e00` | Pico 2 两个 SMP 镜像（COM19）+ 18 目标 + QEMU 3 | T0–T3 PASS（rv32-smp 20/20、arm-smp 20+/20）；ATK/RP2040 真机 `NOT RUN` | [cold boot zero](2026-11-18-pico2-coldboot-zero-0193e00.md) |
| 2026-11-19 | 发布镜像精简启动日志 + `configs/release.conf` | `5a030cd` | Pico 2（COM19/COM17）+ 18 目标 + 5 个 release 构建 + QEMU 4 | T0–T3 PASS（release 镜像 heartbeat 6/6）；ATK/RP2040 真机 `NOT RUN` | [release log](2026-11-19-release-log-pico2-5a030cd.md) |
| 2026-11-19 | 启动不再等 USB 枚举 + RP2040 RESETS 地址修正 | `4f032cf` | ATK-DNRP2350AM M33 SMP（COM24）+ RP2040 时间戳 + 18 目标 + QEMU 4 | T0–T3 PASS（ATK 冷启动 20/20）；RP2040 计数/RV32/accept33 `NOT RUN` | [usb no-wait](2026-11-19-usb-no-host-wait-atk-4f032cf.md) |
| 2026-11-19 | Flash capacity detection + filesystem clamp + `/proc/flash` | `36b1f41` | ESP32-C3 COM4 4 MiB + 18 targets + QEMU 3 | T0–T3 PASS; 8 MiB config capped to 4 MiB; oversized superblock refused; T4 / RP2350 `NOT RUN` | [flash capacity](2026-11-19-flash-capacity-esp32c3-36b1f41.md) |
| 2026-11-19 | C3 XIP code inside the MMU page (opt-in xip2k) | `875bfbe` | ESP32-C3 COM4 + 18 targets + QEMU 3 | T0–T3 PASS; default layout unchanged; WiFi xip2k frees 56 KiB; no-WiFi soak 20/20 | [c3 xip2k](2026-11-19-c3-xip2k-875bfbe.md) |
| 2026-11-19 | C3 XIP default moves to 0x4000 (48 KiB back) | `1146e55` | ESP32-C3 COM4 + 18 targets + QEMU 3 | T0–T3 PASS; default 99,152 B / soak 20/20; WiFi -Oz 361,032 B / 159 KiB free; emutest reboot failure pre-existing | [c3 xip4k](2026-11-19-c3-xip4k-default-1146e55.md) |
| 2026-11-20 | 唯一 USB 序列号 + RP2040 `usb_delay_ms` 标定 | `689c07f` `f1be069` | ATK-DNRP2350AM（COM14/COM26）+ 18 目标 | T0/T1/T3 PASS（M33 heartbeat 6/6；Hazard3 两路控制台可用）；RP2040 与 T2/T4 `NOT RUN` | [usb serial](2026-11-20-atk-usb-serial-delay-f1be069.md) |
| 2026-11-19 | RP2350 boot 阶段读 JEDEC RDID 实测容量 | `a9345c9` | Pico 2 + ATK-DNRP2350AM（ARM/RV32 四镜像） | T1–T6 PASS（8388608 / 4194304 实测）；RP2040 `NOT RUN` | [rp2350 rdid](2026-11-19-rp2350-jedec-rdid-a9345c9.md) |
| 2026-11-19 | RP2040 flash 容量探测（PL022 SSI） | `ac0796e` | Pico 1（COM19/COM17）+ 18 目标 | T1–T3 PASS、T4/T5 先 FAIL 后 PASS（`58a45e8`）；原因记在卡里 | [rp2040 rdid](2026-11-19-rp2040-jedec-rdid-ac0796e.md) |
| 2026-11-20 | Pico 2 SMP 冷启动闸门 + `reboot` 停从核 | `5948bd8` `109a61e` | rpi-pico2-smp / rpi-pico2-rv32-smp | ARM 20/20、RV32 35/38（修前 1/3、1/15）；`#31` RP2040 四项 `NOT RUN` | [boot gate](2026-11-20-pico2-smp-boot-gate.md) |
| 2026-11-20 | UART0 只丢 TX 的自检口（`/proc/uart0{,tx}`、`/proc/gp0drive`） | `bc84675` | Pico 2 RV32 SMP（COM27/COM19） | 板侧编程正确、GP0 拉不高 → 故障在线/适配器；RP2040 与 ARM `NOT RUN` | [uart0 tx](2026-11-20-uart0-tx-diagnosis.md) |
| 2026-11-20 | csc + pvm 脚本栈（改名；py/avm/rvm 排除） | `9b0ae91` | host + 18 目标 + QEMU 3 | T0/T1/T2 PASS；Pico2/Pico/ATK 真机 `NOT RUN` | [host/QEMU](2026-11-20-scripting-host-qemu-9b0ae91.md) |
| 2026-11-20 | csc + pvm 脚本栈（C3 8K） | `9b0ae91` | ESP32-C3 COM4 + 8K profile | T3 PASS；csc 100 KiB 窗口；`/bin` 无 py | [C3](2026-11-20-scripting-c3-9b0ae91.md) |
| 2026-11-20 | RV32 SMP `reboot` 34 s 静默：锁序反转死锁（排查过程） | `695db5b` | Pico 2 RV32 SMP（COM27/COM19） | 过程记录，**非验收证据**（见卡首更正，issue #33） | [reboot stall](2026-11-20-rv32-reboot-stall.md) |
| 2026-11-20 | `reboot` 死锁修复 + 启动环 dump 判据（最终验收） | `b90df38` | Pico 2 RV32 SMP（COM27/COM19）+ ARM 对照 | T0/T1/T3/T4 PASS，T2 `NOT RUN`（范围外）；RV32 20/20、ARM 12/12、闸门 10/10 | [reboot deadlock](2026-11-20-reboot-deadlock-rpi-pico2-rv32-smp-b90df38.md) |
| 2026-11-20 | QMI 等待上限按 `clk_hz` 折算（C6/A-6） | `6c9ea04` | Pico 2 RV32 SMP（COM27/COM19） | T0/T1/T3/T4 PASS，T2 `NOT RUN`（范围外）；detected 8388608、20/20 重启 | [qmi bound](2026-11-20-qmi-wait-bound-rpi-pico2-rv32-smp-6c9ea04.md) |
| 2026-11-20 | csc3 + rirvm 寄存器 IR 实验对（`[features] csc3`） | `ade9db4` | ESP32-C3 COM4 + 18 目标 + QEMU 3 | T0–T3 PASS；csc3 100 KiB / rirvm 28 KiB；默认 8K 仍只有 csc+pvm；T4 `NOT RUN` | [csc3](2026-11-20-csc3-rirvm-integration-esp32c3-ade9db4.md) |
| 2026-11-20 | csc3 struct 数组、struct 赋值、`%.Nf` | `b8b009c` | ESP32-C3 COM4 + 18 目标 + QEMU 3 | T0–T3 PASS；板载 `q=6 p3=16 gs=15 f=3.14`；T4 `NOT RUN` | [csc3 struct](2026-11-20-csc3-struct-arrays-esp32c3-b8b009c.md) |
| 2026-11-20 | csc3 流式编译 + RIR2 函数表（无扩展名，magic 识别） | `461ad4e` | ESP32-C3 COM4 + 18 目标 + QEMU 3 | T0–T3 PASS；板载 12.8 KB 源码/21 函数/185 RIR；csc3 92 KiB / rirvm 32 KiB；T4 `NOT RUN` | [csc3 streaming](2026-11-20-csc3-streaming-esp32c3-461ad4e.md) |
| 2026-11-20 | csc3 `break`/`continue`、`do-while`、`for` 声明、`+=`/`++` | `1528681` | ESP32-C3 COM4 + 18 目标 + QEMU 3 | T0–T3 PASS；板载 flow 26/3/4；csc3 100 KiB / rirvm 32 KiB；T4 `NOT RUN` | [csc3 flow](2026-11-20-csc3-flow-esp32c3-1528681.md) |
| 2026-11-20 | csc3 成员/数组元素的 `+=` 与 `++/--` | `52969da` | ESP32-C3 COM4 + 18 目标 + QEMU 3 | T0–T3 PASS；板载 flow2 5/30/9，ret 44；csc3 100 KiB / rirvm 32 KiB；T4 `NOT RUN` | [csc3 lvalue](2026-11-20-csc3-lvalue-compound-esp32c3-52969da.md) |
| 2026-11-20 | csc3 `enum` + `switch/case/default` | `d8913f9` | ESP32-C3 COM4 + 18 目标 + QEMU 3 | T0–T3 PASS；板载 switch 105/2/3；csc3 108 KiB / rirvm 32 KiB；T4 `NOT RUN` | [csc3 switch](2026-11-20-csc3-switch-enum-esp32c3-d8913f9.md) |
| 2026-11-20 | csc3 错误信息带源码行号 | `2f2bac6` | ESP32-C3 COM4 + 18 目标 + QEMU 3 | T0–T3 PASS；板载 parse/codegen 都报 line 3；csc3 108 KiB / rirvm 32 KiB；T4 `NOT RUN` | [csc3 lines](2026-11-20-csc3-line-numbers-esp32c3-2f2bac6.md) |
| 2026-11-20 | csc3 整数常量折叠 + 保守 DCE | `66712b3` | ESP32-C3 COM4 + 18 目标 + QEMU 3 | T0–T3 PASS；板载 fold 14/3/ret17，ir=14；csc3 108 KiB / rirvm 32 KiB；T4 `NOT RUN` | [csc3 fold](2026-11-20-csc3-const-dce-esp32c3-66712b3.md) |
| 2026-11-20 | csc3 整数 `%` / `%=` | `04d72ae` | ESP32-C3 COM4 + 18 目标 + QEMU 3 | T0–T3 PASS；板载 mod 2/2/2/2，ret 6；csc3 108 KiB / rirvm 32 KiB；T4 `NOT RUN` | [csc3 mod](2026-11-20-csc3-modulo-esp32c3-04d72ae.md) |
| 2026-11-20 | csc3 `char[]` / `string` 变量与字符串内建 | `6b13bc1` | ESP32-C3 COM4 + 18 目标 + QEMU 3 | T0–T3 PASS；板载 str 覆盖 `char[]`/`string`/`printf %s`/`strlen`/`strcmp`/`strlcpy`/`strlcat`/`atoi`，`result=5 ir=121`；csc3 116 KiB window / rirvm 32 KiB；T4 `NOT RUN` | [csc3 strings](2026-11-20-csc3-strings-esp32c3-6b13bc1.md) |
| 2026-11-20 | csc3 比较/逻辑/一元运算符 | `12c6314` | ESP32-C3 COM4 + 18 目标 + QEMU 3 | T0–T3 PASS；板载 ops 覆盖 `!=`/`<=`/`>=`/`&&`/`||`/`!`/一元 `-` 与短路，`result=3 ir=183`；T4 `NOT RUN` | [csc3 ops](2026-11-20-csc3-ops-esp32c3-12c6314.md) |
| 2026-11-20 | csc3 Q16.16 折叠 + 复制传播 + NOP 压缩 | `d54fa8d` | ESP32-C3 COM4 + 18 目标 + QEMU 3 | T0–T3 PASS；板载 floatfold 3.0/1.5/1.75、copy 10/10/7；IR 34→22、14→9、185→165；T4 `NOT RUN` | [csc3 opt](2026-11-20-csc3-opt-esp32c3-d54fa8d.md) |
| 2026-11-20 | csc3 嵌套 struct + struct 返回 | `5e5edd8` | ESP32-C3 COM4 + 18 目标 + QEMU 3 | T0–T3 PASS；板载 nest result=21 ir=147，sret result=17 ir=118；T4 `NOT RUN` | [csc3 structs](2026-11-20-csc3-structs-esp32c3-5e5edd8.md) |
| 2026-11-20 | goc32 `irc` RIR2→RV32 原生 ELF | `04e67e2` | ESP32-C3 COM4 + 18 目标 + QEMU 3 | T0–T3 PASS；17 个测试全部原生编译，板载 fold/str/sa3/nest/sret/fmt/hw 等与 rirvm 一致；T4 `NOT RUN` | [irc native](2026-11-20-irc-native-rv32-04e67e2.md) |
| 2026-11-20 | 板上 C `irc`（RIR2→RV32 ELF） | `4859ec6` | ESP32-C3 COM4 + 18 目标 + QEMU 3 | T0–T3 PASS；板上 csc3→irc→exec 覆盖 fold/str/sa3/nest/sret/native/fmt 等；bench 83 ms / 578 ms；T4 `NOT RUN` | [irc C](2026-11-20-irc-c-esp32c3-4859ec6.md) |
| 2026-11-20 | irc 流式输出（72 KiB window） | `6d54b5a` | ESP32-C3 COM4 + 18 目标 + QEMU 3 | T0–T3 PASS；去掉 out/fixup 缓冲，window 156→72 KiB，/bin irc 40.8→18.1 KiB；T4 `NOT RUN` | [irc stream](2026-11-20-irc-stream-esp32c3-6d54b5a.md) |
| 2026-11-20 | 工具改名 csc/rvm/rcc；csa/pvm 暂留对照 | `66dc83d` | ESP32-C3 COM4 + 18 目标 + QEMU 3 | T0–T3 PASS；板上 csa/pvm 与 csc/rvm/rcc 两条链路均通过；avm 移至 tinyos-script/legacy；T4 `NOT RUN` | [rename](2026-11-20-tool-rename-66dc83d.md) |
| 2026-11-20 | rcc 拆分共享层 + RV32 后端 | `3e2bac2` | ESP32-C3 COM4 + 18 目标 + QEMU 3 | T0–T3 PASS；RV32 输出不变，window 72 KiB；为 thumb1/thumb2 后端留出 `rcc_arch_*` 接口；T4 `NOT RUN` | [rcc split](2026-11-20-rcc-split-3e2bac2.md) |
| 2026-11-20 | RV32 原生用例扩到 37 个 + `files.local` 暂存 | `0ae04d7` | QEMU rv32 + ESP32-C3 COM4 + 18 目标 | T0–T3 PASS；37/37 原生输出与 rvm 一致；bigcode 535 IR / 10 KiB text；RP2350 `NOT RUN`；T4 `NOT RUN` | [rv32 native suite](2026-11-20-rv32-native-suite-rv32-0ae04d7.md) |
| 2026-11-21 | 函数表 32→256；rcc 2k IR / 4096 标签 | `b461b91` | ESP32-C3 COM4 + QEMU rv32 + 18 目标 | T0–T3 PASS；many100 111 函数 / 24232 text；bigir 1436 IR / 2062 标签；csc 窗口 127 KiB 不变，rcc 72→120 KiB；RP2350 `NOT RUN`；T4 `NOT RUN` | [func table](2026-11-21-func-table-rv32-b461b91.md) |
| 2026-11-21 | 板上 `csc` AST 128→256 节点（`-Oz`） | `945fbe0` | ESP32-C3 COM4 + QEMU rv32 + 18 目标 | T0–T3 PASS；many32/bigcode/bigir 板上全链路；csc 窗口 127→140 KiB；`bigbody` 仍需宿主；RP2350 `NOT RUN`；T4 `NOT RUN` | [node 256](2026-11-21-node-256-rv32-945fbe0.md) |
| 2026-11-21 | Node 124→56 B（标识符哈希 + 函数文本池） | `8d15cbc` | ESP32-C3 COM4 + QEMU rv32 + 18 目标 | T0–T3 PASS；44 用例全过且 text 不变；csc 窗口 140→128 KiB；many32/bigcode/bigir/strings2/struct3/nest 板上全链路；RP2350 `NOT RUN`；T4 `NOT RUN` | [node hash](2026-11-21-node-hash-rv32-8d15cbc.md) |
| 2026-11-21 | `rcc` Thumb-2 stage 1（整数核心） | `6ed224d` | QEMU mps2 Cortex-M3 + 18 目标 + QEMU 3 | T0/T1/T2 PASS；26 个整数用例与 rvm 一致；板上 `csc→rcc→exec` fold/flow2；字符串/float/native 未实现；RP2350 `NOT RUN` | [thumb2 int](2026-11-21-thumb2-int-arm-6ed224d.md) |
| 2026-11-21 | `rcc` Thumb-2 stage 2（字符串/float/native） | `ee6fcc4` | QEMU mps2 Cortex-M3 + 18 目标 + QEMU 3 | T0/T1/T2 PASS；28 个用例与 rvm 逐行一致（含 bigcode/bigir/many100）；RP2350 `NOT RUN`；Thumb-1 未开始 | [thumb2 stage2](2026-11-21-thumb2-stage2-arm-ee6fcc4.md) |
| 2026-11-21 | `run(path)`：spawn + wait，不替换自己 | `149fab2` | QEMU mps2 + QEMU rv32 + 18 目标 | T0/T1/T2 PASS；mps2 上 `csc /exec1.c` → `rcc` → 子程序输出 + `0`；rv32 同样；文件 IO 未做（OPEN-ITEMS §14）；T3/T4 `NOT RUN` | [run spawn](2026-11-21-run-spawn-qemu-149fab2.md) |
| 2026-11-21 | 脚本文件 IO：open/create/read/write/close/unlink | `c899586` | QEMU mps2 + QEMU rv32 + 18 目标 | T0/T1/T2 PASS；新增 `NATIVEC`（参数走 ARG）；两目标输出 `1 5 0 5 hello 0 0`，板上 `csc→rcc→exec` 同样；T3/T4 `NOT RUN` | [file io](2026-11-21-file-io-qemu-c899586.md) |
| 2026-11-21 | capture(path,buf,cap)：读子程序 stdout | `c2b27ae` | QEMU mps2 + QEMU rv32 + 18 目标 | 宿主镜像 PASS（mps2 46 B，rv32 按 cap 截断）；板上 csc+rcc 链路 **FAIL**（返回 0，OPEN-ITEMS §14）；T3/T4 `NOT RUN` | [capture](2026-11-21-capture-qemu-c2b27ae.md) |
| 2026-11-21 | 修 capture()：shell 管道后 fd 1 被复用 | `af87918` | QEMU mps2 + QEMU rv32 | T0 PASS；T2 PASS（管道前后各两次 capture 都成功）；根因：msh 关掉 fd 1 后 pipe() 拿到 1，dup2(w,1) 把读端关了；T1/T3/T4 `NOT RUN` | [capture fix](2026-11-21-capture-fix-arm-af87918.md) |
| 2026-11-21 | readdir() + isdir() | `6924174` | QEMU mps2 + 18 目标 | T0 PASS；T1 18/18；T2 板上链路列出 / 并给出 1/0/-1；rv32/T3/T4 `NOT RUN` | [readdir](2026-11-21-readdir-arm-6924174.md) |
| 2026-11-21 | rcc 按 4 KiB 块读取 RIR2 | `bb90935` | QEMU rv32 + QEMU mps2 + 18 目标 | T0 PASS（输出逐字节一致）；T1 18/18；T2 两个后端在板上编译 31.6 KB 的 RIR2 并运行（1351）；窗口 124 → 88 KiB，宿主编译慢约 2%；T3/T4 `NOT RUN` | [block read](2026-11-21-rcc-block-read-rv32-bb90935.md) |
| 2026-11-21 | rvm 指令区改为按页读取（去掉 256 IR 上限） | `390b1e0` | QEMU rv32 + QEMU mps2 + 18 目标 | T0 PASS（40 用例，四个原 native-only 转正，旧版可跑用例输出一致）；T1 18/18；T2 板上跑 1574 IR 和 2948 IR 的镜像；窗口 36 → 40 KiB，最紧的 benchloop 慢 15–30%；T3/T4 `NOT RUN` | [rvm page](2026-11-21-rvm-page-390b1e0.md) |
| 2026-11-21 | Thumb-1 真机验收（RP2040） | `7a25c54` | RP2040 CDC COM30 + 18 目标 | T0/T1 PASS；T3 PASS（心跳 6/6、strict 37/37、7 观察项无 fault，与 mps2 基线一致）；T2/T4 `NOT RUN`。同日曾撤回为"HardFault"，16:14–16:18 用**同 sha256**（`39c9ae20…`）重刷两轮均 37/37，撤回解除；当时板上是 #39 的 rir 镜像（内含 Thumb-2 版 `rcc`） | [thumb1 rp2040](2026-11-21-thumb1-rp2040-7a25c54.md) |
| 2026-11-21 | RP2040 板内 builder 链路（rir 镜像） | `f466efa` | RP2040 CDC COM30（rir 配置） | T0/T1 PASS；T3 PASS（整数/字符串/浮点三例板上编译并运行，与 rvm 逐行一致；窗口 128/100/44 KiB vs 216 KiB 用户区）；T2/T4 `NOT RUN` | [rir rp2040](2026-11-21-rir-rp2040-f466efa.md) |
| 2026-11-21 | Thumb-2 真机验收（Pico 2, Cortex-M33） | `ae8c7c7` | RP2350 CDC COM27（rpi-pico2-smp + 44 语料） | T0/T1 PASS；T3 PASS（心跳 6/6、strict 37/37 两轮、7 观察项无 fault）；T2/T4 `NOT RUN`；单核目标起不来另案 #42 | [thumb2 rp2350](2026-11-21-thumb2-rp2350-ae8c7c7.md) |
| 2026-11-21 | 同一套 44 程序在 RV32 端（Pico 2, Hazard3） | `67a70ea` | RP2350 CDC COM27（rpi-pico2-rv32-smp + 44 语料） | T0/T1 PASS；T3 PASS（心跳 6/6、strict 37/37 两轮、7 观察项无 fault）；三个后端至此都验过；T2/T4 `NOT RUN` | [corpus rv32](2026-11-21-corpus-rv32-rp2350-67a70ea.md) |
| 2026-11-21 | 修 Thumb-2 `printf("%d")` 不打印数字 | `0206184` | QEMU mps2 + 18 目标 | T0 PASS；T1 18/18；T2 `t-dv`/`t-pf`/`t2-str`/`t2-sa3`/`t2-fmt` 与 `rvm` 一致；根因：capture helper 插在 `L_pin` 与 `L_picommon` 之间，`R_PRINTV` 落进了 capture；T3/T4 `NOT RUN` | [printf int](2026-11-21-thumb2-printfint-0206184.md) |
| 2026-11-21 | 修 Thumb-1 write() 的长度参数被别名路径破坏 | 7f4e557 | QEMU mps2 + 18 目标 | T0 PASS；T1 18/18；T2 同一份 .rir 走 rvm/Thumb-2/Thumb-1 都输出 1 5 0 5 hello 0 0；43 条 Thumb-1 用例无差异；T3/T4 NOT RUN | [write len](2026-11-21-thumb1-write-len-7f4e557.md) |
| 2026-11-21 | Pico 2 M33 SMP 脚本跑分（bench.c 四内核） | `7236641` | Pico 2（COM27）+ rir 镜像 | T0/T1/T3 PASS（rvm 289/407/799/340 ms，rcc 70/44/120/47 ms）；RV32 工具栈提示 #44，余列 `NOT RUN` | [bench pico2 m33](2026-11-21-bench-pico2-m33-7236641.md) |
| 2026-11-21 | PMP 窗口 NAPOT 预算逐级降级（#44） | `41a5b3d` | rpi-pico2-rv32-smp（无板在机）；18 目标 | T0 PASS（新宿主机用例复现 rcc 的 8 块几何，4180 组映射全过）；T1 18/18；T2 rv32 smoke PASS；T3/T4 `NOT RUN`（无 Pico 2） | [pmp ladder](2026-11-21-pmp-window-ladder-rp2350rv-41a5b3d.md) |
| 2026-11-21 | Pico 2 RV32 SMP 脚本跑分（#44 修复后） | `412038c` | Pico 2（COM27）+ rv32 rir 镜像 | T0/T1/T3 PASS（rvm 281/397/803/352 ms，rcc 61/27/60/42 ms）；余列 `NOT RUN` | [bench pico2 rv32](2026-11-21-bench-pico2-rv32-412038c.md) |
| 2026-11-21 | Pico 2 两侧 SMP 完整脚本跑分（clang/goc32/MicroPython） | `930fd31` | Pico 2（COM27/COM22）+ 多镜像 | T0/T1/T3 PASS（七列数据，校验和全一致）；pvm 与 RV32 MicroPython `NOT RUN` | [bench pico2 full](2026-11-21-bench-pico2-full-930fd31.md) |
| 2026-11-21 | 跑分总表（纵向）+ M33 goc32 div 的 EABI helper 修复 | `9ee7a1f` | QEMU mps2-an385 + 18 目标（M33 无板） | T0/T1/T2 PASS（同一份 goc32 镜像在 mps2 上 div 69→4 ms，校验和不变）；T3/T4 `NOT RUN`（M33 需重编 benchgoc 后重测） | [bench table](2026-11-21-bench-table-aeabi-9ee7a1f.md) |
| 2026-11-21 | Pico 2 两侧 pvm 跑分；goc32 M33 div 复测 | `24c8284` | Pico 2（COM27）+ scripting 镜像 | T0/T1/T3 PASS（pvm M33 398/1337/2358/841，RV32 526/1724/3078/1106）；goc32 div 仍 1343 ms，#45 | [pvm + goc32](2026-11-21-bench-pvm-goc32-24c8284.md) |
| 2026-11-21 | 用户态堆改为 first-fit 空闲链表 | `d24cbf5` | QEMU rv32 + mps2 + 18 目标（无板） | T0/T1/T2 PASS（宿主机 mock sbrk 用例、rv32 的 wheap/mem/bench/deep、mps2 的 kthreadtest/prodcons、goc32 benchgoc 校验和不变）；T3/T4 `NOT RUN` | [userland heap](2026-11-21-userland-heap-d24cbf5.md) |
| 2026-11-21 | goc32 Thumb-2 硬件除法（#45） | `b697eb6` | Pico 2 M33/RV32 + goc32 单测 | T0/T1/T3 PASS（M33 div 1343 ms → 34 ms；RV32 无回归）；RP2040 真机 `NOT RUN` | [goc32 hwdiv](2026-11-21-goc32-hwdiv-b697eb6.md) |
| 2026-11-21 | 回退 EABI 硬件除法分支（#45 由 goc32 修复） | `022b451` | QEMU arm/rv32 + 18 目标（无板） | T0/T1/T2 PASS（tostest arm/rv32 smoke、goc32 与 native32 单测）；T3/T4 `NOT RUN`（板上 34 ms 见 #45 卡片） | [aeabi revert](2026-11-21-aeabi-revert-022b451.md) |
| 2026-11-21 | Pico 2 单核 ARM 起不来（#42）：QMI 探测 CLKDIV 被清零 | `5557f33` | Pico 2 CDC COM27（rpi-pico2 + 44 语料） | T0/T1 PASS；T3 PASS（枚举、uptime 32.8 s、心跳 4/6；SMP 对照 6/6）；T2/T4 `NOT RUN`；遗留：控制台输出约 192 B 截断，另案 | [uni arm boot](2026-11-21-pico2-uni-arm-boot-5557f33.md) |
| 2026-11-22 | Release 镜像瘦身（44 个 littlefs 块） | `79b9aad` | rpi-pico2 + rv32 (QEMU) | T0/T1/T2 PASS（43 文件/71 块 → 19/27 块，省 176 KiB）；T3/T4 `NOT RUN` | [release prune](2026-11-22-release-prune-79b9aad.md) |
| 2026-11-22 | Release 镜像再去掉 hello / hello_printf | `2801153` | rpi-pico2 + rv32 (QEMU) | T0/T1（release 目标）/T2 PASS（17 文件/25 块，100 KiB；比默认省 184 KiB）；T3/T4 `NOT RUN` | [release prune 2](2026-11-22-release-prune2-2801153.md) |
| 2026-11-22 | csa/pvm 改为 opt-in，默认镜像去掉旧 AMX 引擎 | `f7f962c` | rpi-pico2 + rv32 (QEMU) | T0/T1/T2 PASS（默认 43→41 文件、71→62 块，省 36 KiB；pvm 用 configs/pvm.conf 仍可跑）；T3/T4 `NOT RUN` | [scripting opt-in](2026-11-22-scripting-optin-f7f962c.md) |
| 2026-11-22 | csc/rvm/rcc 默认进镜像；一条脚本覆盖三段 | `75ffb64` | rpi-pico2 + rv32 (QEMU) | T0/T1/T2 PASS（cover 9 行，rvm/native 一致）；T3/T4 `NOT RUN` | [rir default + cover](2026-11-22-rir-default-cover-75ffb64.md) |
| 2026-11-22 | 锁顺序文档 + 持锁阻塞静态检查（#51） | `2b137a3` | host / go1.27.1 | 负例先红 PASS（`process.c:501`）；T0 排除基线 pinned 失败后全绿；T1/T2/T3 `NOT RUN`（docs-only） | [lock order](2026-11-22-lock-order-2b137a3-host.md) |
| 2026-11-22 | 最小抽取 uaccess + checked math（#54） | `d0cfe8d` | host + 隔离 worktree；rv32/rv32smp/arm QEMU | 差分等价 PASS（4096+20000 组，负例红 4495 处）；T0 排除基线 pinned 失败后全绿；T1 18/18；T2 3/3 smoke；T3/T4 `NOT RUN` | [uaccess extract](2026-11-22-uaccess-extract-d0cfe8d-qemu.md) |
| 2026-11-22 | 架构 review 阶段 0 护栏（config/corpus/host 测试；#51/#54 由并行线程先落） | `049a0c9` | rpi-pico2 + esp32c3-generic + rv32/arm QEMU + C3（COM4） | T0 PASS（本 run 时本地 pin 同步除外，现已修复）；T1/T2 PASS；T3 PASS（C3 accept 24/24）；C3 上 csc 窗口问题 BLOCKED（#55，已修）；#52 未做 | [stage 0 guards](2026-11-22-stage0-guards-2f1ebf3.md) |
| 2026-11-22 | 窗口对齐上限改到板级 grain（#55） | `12d41c6` | host + 隔离 worktree；rv32/rv32smp/arm QEMU | 负例红 139 处（含 C3 两条核心断言）；T0 全绿；T1 18/18；T2 3/3 smoke；C3 真机 `NOT RUN`（无板） | [window align](2026-11-22-window-align-c3-27c4248-host.md) |
| 2026-11-22 | #55 窗口对齐修复的 C3 真机复测 | `12d41c6` | ESP32-C3（COM4） + 18 目标 + rv32/arm QEMU | T0/T1/T2 PASS；T3 PASS（cover 九行 rvm/native 一致，accept 24/24）；T4 `NOT RUN` | [window align C3 verify](2026-11-22-window-align-c3-12d41c6-verify.md) |
| 2026-11-22 | platform.h 拆四窄头 + include 约束（#52） | `963af77` | host + rv32/arm QEMU | 符号 107/107 等价；三个负例先红；T0 PASS；T1 18/18；T2 2/2 smoke；rv32 `.text` 逐字节相同、ARM 差 4 B（重定位，根因未定位）；T3/T4 `NOT RUN` | [narrow headers](2026-11-22-platform-narrow-headers-qemu.md) |
| 2026-11-22 | 窗口放置改为 buddy 优先/grain 兜底（#55 复核项） | `80ee7d2` | ESP32-C3（COM4） + 18 目标 + rv32/arm QEMU | T0/T1/T2 PASS；T3 PASS（cover 九行一致，accept 24/24）；T4 `NOT RUN` | [window two-pass](2026-11-22-window-place-twopass-80ee7d2.md) |
| 2026-11-22 | pinned rvm 同步后的目标端修复（tos_stat）；脚本 fb80bdc 全测 | `b516695` | ESP32-C3（COM4） + 18 目标 + rv32/arm QEMU | T0/T1/T2 PASS；T3 PASS（cover 九行一致）；T4 `NOT RUN` | [pinned rvm stat](2026-11-22-pinned-rvm-stat-b516695.md) |
| 2026-11-22 | tos_stat_t 统一（目标/host 同名类型）+ 目标端编译检查 | `2c03dcf` | ESP32-C3（COM4） + 18 目标 + QEMU | T0/T1/T2 PASS；T3 PASS（cover 九行，accept 24/24）；检查器负例可复现；T4 `NOT RUN` | [stat typedef](2026-11-22-stat-typedef-2c03dcf.md) |
| 2026-11-22 | platform 接缝检查修正（#52 review）：名字池规则、扫头文件、plat_* 全量钉住 | `22c0ffe` | host / T0 | T0 PASS（新增负例 3 个）；T1/T2/T3/T4 `NOT RUN`（仅测试与基线） | [platform seam check](2026-11-22-platform-seam-check-22c0ffe.md) |
| 2026-11-23 | 阶段 1 收尾：C3 验收加入 cover、平台接口契约、mklfs 抽取 | `adc9c60` | ESP32-C3（COM4） + x86/rv32/arm/rv32smp + 18 目标 | T0/T1 PASS；T2 soak 4/4 PASS；T3 C3 29/29 PASS；T4 `NOT RUN` | [stage 1 tail](2026-11-23-stage1-tail-c3-cover-adc9c60.md) |
| 2026-11-23 | FS 静默限制修复（长名字/点文件/MAX_OPEN）+ 限制清单 | `c471965` | ESP32-C3（COM4） + 18 目标 + rv32/arm QEMU | T0/T1/T2 PASS；T3 PASS（C3 accept 29/29 + 长名字/点文件实测）；T4 `NOT RUN` | [fs limits](2026-11-23-fs-limits-c471965.md) |
| 2026-11-23 | 路径上限 64 → 256（TOS_PATH_MAX），长名字可列 | `279509f` | ESP32-C3（COM4） + 18 目标 + rv32/arm QEMU | T0/T1/T2 PASS；T3 PASS（C3 accept 29/29 + 100 字符名字实测）；T4 `NOT RUN` | [path max](2026-11-23-path-max-279509f.md) |
| 2026-11-23 | 路径上限保留 64 字节（体积取舍；放弃 128/256） | `14b2b6a` | ESP32-C3（COM4） + 18 目标 + rv32/arm QEMU | T0/T1/T2 PASS；T3 PASS（C3 accept 29/29；50 字符名字通过，120 字符被拒）；T4 `NOT RUN` | [path limit 64](2026-11-23-path-limit-64-14b2b6a.md) |
| 2026-11-23 | ls 默认空格分隔、-l 才一行一个 | `0fbe0a8` | ESP32-C3（COM4） + 18 目标 + rv32/arm QEMU | T0/T1/T2 PASS；T3 PASS（C3 accept 29/29 + 四种 ls 形式核对）；T4 `NOT RUN` | [ls plain](2026-11-23-ls-plain-0fbe0a8.md) |
| 2026-11-23 | csc 状态机模式 demo + 每函数上限实测 | `43ada9d` | ESP32-C3（COM4） + rv32 QEMU + 18 目标 | T0/T1 PASS；T2 PASS；T3 PASS（C3 accept 34/34，rvm/native 一致）；T4 `NOT RUN` | [state machine](2026-11-23-state-machine-43ada9d.md) |
| 2026-11-23 | csc 增加 yield()/ready(fd)，rvm/rcc 三后端实现 | `0d16e55` | ESP32-C3（COM4） + rv32/arm QEMU + 18 目标 | T0/T1/T2 PASS；T3 PASS（C3 accept 39/39，ready=1/y=0）；T4 `NOT RUN` | [yield/ready](2026-11-23-yield-ready-0d16e55.md) |
| 2026-11-23 | ARM USB CDC 长输出截断改为 PendSV 延迟泵（#46） | `ea2d89d` | rpi-pico2 / -smp / atk / rpi-pico 构建 + rv32/rv32smp/arm/x86 QEMU + C3（COM4） | T0/T1/T2 PASS（soak 3/3）；T3 C3 accept 39/39，RP2350 ARM/SMP `NOT RUN`（PendSV 路径由 QEMU arm 临时探针验证）；T4 `NOT RUN` | [deferred pump](2026-11-23-usb-deferred-pump-ea2d89d.md) |
| 2026-11-23 | RP2040 重新进入 T3 矩阵 + 一键验收脚本 | `1cc53c4` | 文档 + 宿主机脚本；RP2040 板不在机 | T0 PASS；T1/T2 不需要（无固件改动）；T3 `NOT RUN`（OPEN-ITEMS §2c）；解析层与 C3 插桩跑通过 | [rp2040 matrix](2026-11-23-rp2040-back-in-matrix-1cc53c4.md) |
| 2026-11-23 | RP2040 两个镜像重刷 + 板级验收 + 同提交 T0/T2/T4 | `a97f74c` | RP2040 Pico（COM30，USB CDC）× rpi-pico / rpi-pico-smp | T0 PASS*（pinned 基线红）；T1 PASS（5 镜像）；T2 PASS（smoke×3、soak、compat）；T3 PASS（accept 14/14 ×2、heartbeat 6/6、pio 17/17 + GP25 目视、smpbench 2.00x）；T4 PASS（dd 1000/1000、reboot 20/20）；CSC cover/state/ready 15/15（修 rcc Thumb-1 的 SP 立即数偏移后）；脚本 4 处 bug 已修（未提交） | [rp2040 accept](2026-11-23-rp2040-accept-both-a97f74c.md) |
| 2026-11-23 | Pico 2 四镜像板级矩阵（ARM/RV32 × 单核/双核） | `f91db82` | Pico 2（UART COM19 / CDC COM27）× rpi-pico2 / -smp / -rv32 / -rv32-smp | T0 `NOT RUN`（无源码改动）；T1 4/4 构建 PASS；T2 rv32 smoke PASS；T3 ARM 两镜像 heartbeat 6/6、dd 200/200、CSC 3/3，RV32 两镜像 heartbeat 6/6、dd 200/200、csc+rvm PASS 但 `rcc` 必崩（0x…ffe 取指，根因 RVC 指令跨 PMP 区域；修复后 rv32 单核/双核三条 CSC 链 PASS，用户侧当时改 `-march=rv32ima`，已被 11-24 的 `rvc = auto` 取代）；首刷后根目录输出混入垃圾一次，重刷后 10 次 ls + 3 次热重启未复现；T4 `NOT RUN` | [pico2 matrix](2026-11-23-pico2-matrix-f91db82.md) |
| 2026-11-24 | exec 认 cwd（裸名字先当前目录再 /bin）+ Hazard3 去 RVC 的 msh 窗口收尾 | `b1921a2`、`99c6a07` | ESP32-C3（COM4） + rv32/rv32smp/arm/x86 QEMU + 18 目标 | T0 PASS；T1 18/18（修前 16/18）；T2 smoke 4/4、soak 3/3；T3 C3 accept 42/42，RP2350/RP2040 `NOT RUN`；T4 `NOT RUN` | [exec cwd + hazard3](2026-11-24-exec-cwd-hazard3-window-99c6a07.md) |
| 2026-11-24 | Hazard3 跨页取指的构建期门禁 + 脚本链上板前覆盖（rvm-cover 三链进 CI） | `dc01958` | 4 个 Hazard3 构建 + rv32/arm QEMU + 18 目标 + C3（COM4） | T0 PASS；T1 18/18；T2 smoke 4/4、soak 3/3、rvm-cover 3×2 链 PASS；T3 C3 accept 42/42，RP2350/RP2040 `NOT RUN`；负例（放开到 QEMU）拦下 `bin/edit@0xffe`；T4 `NOT RUN` | [page gate](2026-11-24-hazard3-page-gate-dc01958.md) |
| 2026-11-24 | 用户 ISA 归 target conf（Hazard3 五目标 `march = rv32ima`，去掉构建器特判） | `252af6b` | 5 个 Hazard3 构建门禁 + rv32/rv32smp/arm/x86 QEMU + 18 目标 + C3（COM4） | T0 PASS；T1 18/18；T2 smoke 4/4、soak 3/3、rvm-cover 3/3×2；T3 C3 accept 42/42；负例（opt 里塞 `-march=rv32imac`）被拦；RP2350/RP2040 `NOT RUN`；T4 `NOT RUN` | [ISA in conf](2026-11-24-isa-in-target-conf-252af6b.md) |
| 2026-11-24 | `[user] rvc = auto`：按程序回退 no-RVC（msh 窗口回到 16 KiB） | `0ec6c2b` | 5 个 Hazard3 构建 + rv32/rv32smp/arm/x86 QEMU + 18 目标 + C3（COM4） | T0 PASS；T1 18/18（27 次按程序重建）；T2 smoke 4/4、soak 3/3、rvm-cover 3/3；T3 C3 accept 42/42，RP2350/RP2040 `NOT RUN`；T4 `NOT RUN` | [rvc auto](2026-11-24-user-rvc-auto-0ec6c2b.md) |
| 2026-11-24 | RVC 回退边界改为 window grain（16 KiB 粒度只换 `csc`；noc 树懒编译） | `ba02fbb` | 5 个 Hazard3 构建 + rv32/rv32smp/arm/x86 QEMU + C3（COM4） | T0 PASS；T1 18/18；T2 smoke 4/4、soak 3/3；T3 C3 accept 42/42；RP2350/RP2040 `NOT RUN`；T4 `NOT RUN` | [rvc grain](2026-11-24-rvc-grain-boundary-ba02fbb.md) |
| 2026-11-24 | ATK RISC-V grain 4 KiB → 16 KiB（自 11-21 起启动 panic）+ 构建期 conf/layout 比较 | `3dbcaa7` | atk-dnr2350am-rv32 / -smp + 18 目标 + rv32/rv32smp/arm/x86 QEMU + C3（COM4） | T0 PASS；T1 18/18；T2 smoke 4/4、soak 3/3；T3 C3 accept 42/42；ATK 真机 `NOT RUN`（正是坏掉的那两块板）；T4 `NOT RUN` | [atk grain](2026-11-24-atk-rv32-grain-panic-3dbcaa7.md) |
| 2026-11-24 | 跨页检查回到按页（grain 捷径放过了 rcc） | `c997375` | 5 个 Hazard3 构建 + rv32/rv32smp/arm/x86 QEMU + C3（COM4） | T0 PASS；T1 18/18；T2 smoke 4/4、soak 3/3；T3 C3 accept 42/42；RP2350/RP2040 `NOT RUN`；T4 `NOT RUN` | [page check](2026-11-24-rvc-page-check-again-c997375.md) |
| 2026-11-24 | Pico 2 四镜像板级回归（`rvc = auto` 首次上板） | `6b15b8b` | Pico 2（COM19）× rpi-pico2 / -smp / -rv32 / -rv32-smp | T0 部分（`internal/build`）；T1 4/4 PASS（rv32 各回退 6 个程序，`msh` 保留 C）；T2 `NOT RUN`；T3 PASS（四镜像 heartbeat 6/6、CSC 3/3；两个 SMP：smptest 11/11、dd 200/200、smpbench 2.05x / 2.00x、reboot 20/20；`--func` 6/7，`py` 不在镜像）；T4 PASS（两 SMP 镜像门槛） | [pico2 rvc auto](2026-11-24-pico2-matrix-rvc-auto-6b15b8b.md) |
| 2026-11-24 | ATK 四镜像板级回归 + ARM SMP 控制台死锁（掩码 `usb_cdc_getchar`） | `8e92fa4` | ATK（COM14 UART / COM26 CDC）× atk-dnr2350am / -smp | T0 `NOT RUN`；T1 4/4 PASS；T2 `NOT RUN`；T3 PASS（ARM SMP accept33 21/21、单核 19/19、heartbeat 6/6、hammer 96 条无 STALL；RV32 本轮未重测）；T4 PASS（SMP：dd 200/200、2.06x、reboot 20/20） | [atk deadlock](2026-11-24-atk-arm-usb-console-deadlock-6b15b8b.md) |
| 2026-11-24 | USB RX 环满时丢一个字节（ARM SMP 死锁修复的复盘补丁）+ `SIO_SPINLOCK_ST` 命名 | `8e92fa4` 之后的补丁 | 6 个 ARM 目标构建 + 18 目标 + arm QEMU + soak | T0 PASS；T1 18/18；T2 PASS；T3 `NOT RUN`（ATK 复测由板主做）；T4 `NOT RUN` | [rxq full](2026-11-24-usb-rxq-full-byte-8e92fa4.md) |
| 2026-11-24 | RP2040 控制台改自有 RX 环（关中断的读不再进 TinyUSB）+ tosput 减半的根因分析 | 8e92fa4 之后的补丁 | rpi-pico / -smp + 18 目标 + arm QEMU + soak | T0 PASS；T1 18/18；T2 PASS；T3 NOT RUN（RP2040 无板）；T4 NOT RUN | [rp2040 rx ring](2026-11-24-rp2040-console-rx-ring.md) |
| 2026-11-24 | ATK ARM SMP `[STALL]` 根因：SHPR1 漏写 MemManage 字节，故障处理器里 tick 被饿死 | `05a0e18` | ATK（COM14 UART / COM26 CDC）× atk-dnr2350am / -smp；RP2040（COM19 UART）× rpi-pico / -smp | T0 `NOT RUN`；T1 4/4 PASS；T2 `NOT RUN`；T3 PASS（SMP hammer 20/20、单核 10/10，无 STALL/无复位；rpi-pico / -smp accept 各 30/30）；T4 `NOT RUN` | [memmanage tick](2026-11-24-atk-smp-memmanage-tick-starvation.md) |
| 2026-11-24 | SHPR1 的 SecureFault 字节（M33 不是保留字节）与 `arm_default_handler` 用 IPSR 报真实向量号，复核 `05a0e18` 的同类补丁 | `87ede23` | 6 个 RP2 目标 + `arm`(M3) 构建 + 18 目标 + arm QEMU + soak；Pico 2 ARM 两镜像（`d7a3c8a`） | T0 PASS；T1 18/18；T2 PASS；T3 部分（Pico 2：`shpr1=0x40404040`、prot 五例、heartbeat/smptest/dd/smpbench 全过；RP2040 `accept.py` 与 ATK hammer `NOT RUN`）；T4 `NOT RUN` | [securefault prio](2026-11-24-arm-securefault-priority.md) |
| 2026-11-24 | Pico 2 四镜像回归（SHPR1 修复的板级验证，`shpr1=0x00404040`） | `05a0e18` | Pico 2（COM19 UART / COM27 CDC）× rpi-pico2 / -smp / -rv32 / -rv32-smp | T0 `NOT RUN`；T1 4/4 PASS（rv32 各回退 6 个程序，`msh` 保留）；T2 `NOT RUN`；T3 PASS（四镜像 heartbeat 6/6、CSC 3/3；ARM 五例 prot 均 MemManage；RV32 heap/stack/dev fault、rodata/null 存活）；T4 PASS（两 SMP：smptest 11/11、`--func` 6/7、dd 200/200、smpbench 2.05x / 2.00x、reboot 20/20） | [pico2 shpr1](2026-11-24-pico2-matrix-05a0e18.md) |
| 2026-11-25 | USB 控制台写方向改每核自有 TX 环（`putc` 不再取锁/进 TinyUSB）；锁超时不再永久降级为无锁，改为计数 + 跳过本次操作；`/proc/usbboot` 露计数 | 本提交 | 6 个 RP2 目标 + `arm`(M3) 构建 + 18 目标 + arm QEMU + soak | T0 PASS；T1 18/18；T2 PASS；T3 `NOT RUN`；T4 `NOT RUN` | [tx ring](2026-11-25-usb-tx-ring-bounded-lock.md) |
| 2026-11-25 | TX 环在 Pico 2 上的板级验证：长输出仍截断（`cat` 21 KB 经 CDC，ARM 单核 0/3、SMP 3/6、RV32 单核 0/3、RV32 SMP 3/3）；按方案 1（写路径反压 + 短写重试）修复后 ARM 单核 9/9、SMP 3/3；再补 `arch/rv32` 的 PendSV 等价边界后 RV32 两镜像各 9/9 完整 | `d7a3c8a` → 本提交 | Pico 2（COM19 UART / COM27 CDC）× rpi-pico2 / -smp / -rv32 / -rv32-smp | T0 `NOT RUN`；T1 4/4 PASS；T2 `NOT RUN`；T3 `d7a3c8a` **FAIL** → 修复后四镜像全部 **PASS**（各 9/9 或 3/3 完整）；ATK 四镜像见本表 ATK 补跑行、RP2040 未测；T4 `NOT RUN` | [tx ring pico2](2026-11-25-usb-tx-ring-pico2-d7a3c8a.md) |
| 2026-11-25 | USB 长输出截断的根因（单次 SVC 的缓冲上限）与方案取舍：撤回“写者等主机”，改为 `write` 短计数 + 用户重试 | 本提交（撤回 `ba657e4`） | 6 个 RP2 目标 + `arm`(M3) 构建 + 18 目标 + arm QEMU + soak | T0 PASS；T1 18/18；T2 PASS；T3 **PASS**（短计数已由另一侧实施并实测，四镜像全过，见 tx ring pico2 卡）；T4 `NOT RUN` | [tx flow control](2026-11-25-usb-tx-flow-control.md) |
| 2026-11-25 | ESP32-C3 默认镜像改 `-Oz`（per-target `opt` 旋钮）+ `accept.py` 提示符匹配修复 | 本提交 | C3（COM4）：accept 42/42、`-O2`/`-Oz` 各 5 轮 dd+csc+启动；18 目标 + arm QEMU + soak | T0 PASS；T1 18/18；T2 PASS；T3 PASS（C3 accept 42/42，性能无退化）；T4 `NOT RUN` | [c3 -Oz](2026-11-25-c3-oz-default.md) |
| 2026-11-25 | ATK 四镜像按 TX 环卡片补跑；顺带修掉面板镜像开启时 TAB 展开的无限重画（`stop == cols`，`kernel/screen.c` `putc_locked`） | 本提交 | ATK（COM14 UART / COM26 CDC）× atk-dnr2350am / -smp / -rv32 / -rv32-smp | T0 `NOT RUN`；T1 4/4 PASS；T2 `NOT RUN`；T3 PASS（四镜像各 3 轮，`cat` 21 200 B → 21 314 B 双路逐字节一致、提示符返回、tick 1000 Hz）；T4 `NOT RUN` | [tx ring pico2](2026-11-25-usb-tx-ring-pico2-d7a3c8a.md) |
