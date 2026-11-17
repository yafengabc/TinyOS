# Pico 2 A 轨：SWD 真机现场（issue #7，2026-11-17）

```text
commit:            2be39bd 基础 + d663bde（variant/pico2 板，B1/B2 已落地）
change:            Pico 2 启动排障 A 轨：用 Debugprobe(SWD) 抓 PC=0xeffffffe 现场
baseline:          none（排障现场，非回归）
target/config:     rp2350pico2 / configs defaults（单核 bring-up 镜像）
board/port/image:  Raspberry Pi Pico 2 (RP2350 rev2, 8MiB w25q64fv);
                   Debugprobe = RP2040 刷 build/debugprobe_on_pico.uf2 (CMSIS-DAP,
                   UID E6609CB2D34AB733); SWD 500k-1MHz;
                   flash 写入 build/kernel-rp2350pico2.elf via OpenOCD
toolchain:         openocd 0.12.0+dev-gacff23f (D:\projects\TinyOS\build\tools\openocd-pico,
                   支持 rp2350.cfg); pyocd 0.46.1 (rp2350 builtin, 但对 Secure 态 AHB-AP FAULT)
T0:                PASS（最新 d663bde：build/vet/test -count=1）
T1:                PASS（13 目标 + fs_ro；rp2350pico2 构建成功，UF2 1,769,984 B）
T2:                NOT RUN（Pico 2 未在 QEMU 之外跑通 smoke；不适用）
T3:                ===本卡=== SWD 真机现场已抓到；B3 上板心跳 NOT RUN（启动失败）
T4:                NOT RUN
对照固件:          .tmp/micropython-pico2.uf2（官方, PID_0005 CDC 正常运行）
logs:              build/pico2-catch2.log, build/pico2-run_catch.log,
                   build/pico2-scb*.log, build/pico2-write-upy.log,
                   build/ocd-telnet-*.log（原始日志不入库）
```

## 做了什么

1. **调试探针**：RP2040（原 COM19 的 TinyOS 板）刷 `debugprobe_on_pico.uf2`，
   `pyocd list --probes`/OpenOCD 均识别为 `Raspberry Pi Debugprobe on Pico
   (CMSIS-DAP)` UID `E6609CB2D34AB733`。SWD 接 Pico 2 板背面 SWD 焊盘
   （GP2→SWCLK, GP3→SWDIO, GND→GND；Pico 2 由 USB-C 供电）。
2. **工具选择**：MSYS2 的 openocd 0.12.0 无 rp2350.cfg，弃用；改用仓库自带的
   官方 `build/tools/openocd-pico/openocd.exe`（0.12.0+dev，带 rp2350.cfg /
   rp2350-rescue.cfg）。pyocd 0.46.1 能连但 AHB-AP 在 Secure 态报 FAULT。
3. **恢复芯片**：Pico 2 初次连接 core0 处于 `Sleeping [Secure]`、AHB-AP FAULT、
   XIP 不可读。`target/rp2350-rescue.cfg`（reset + halt bootROM）能稳定恢复。
4. **写入镜像**：OpenOCD `flash write_image erase build/kernel-rp2350pico2.elf`
   + `verify_image` 成功。Flash bank 识别为 RP2350 rev2 / w25q64fv / 8 MiB /
   2048 扇区（注意：board.conf 按 4 MiB 配置，实际焊 8 MiB）。
5. **复现现场**：`reset run` 后 2 s halt，稳定抓到 HardFault 现场（见下）。

## 真机现场（A 轨第一份）

```text
Info : SWD DPIDR 0x4c013477 DPv3          # RP2350 SWD 正确识别
Info : [rp2350.cm0] Cortex-M33 r1p0 processor detected   # 两个核都识别成功
Error: [rp2350.cm0] clearing lockup after double fault
Error: [rp2350.cm0] Polling failed, trying to reexamine
[rp2350.cm0] halted due to debug-request, current mode: Handler HardFault
xPSR: 0x29000003   pc: 0xeffffffe   msp: 0x20019468
[rp2350.cm1] halted due to debug-request, current mode: Thread
xPSR: 0x09000000   pc: 0x000000da   msp: 0xf0000000
```

**第二次复现**（同一镜像、rescue 后重跑）：

```text
xPSR: 0x29000003   pc: 0xeffffffe   msp: 0x20019618
```

与 OPEN-ITEMS §1b / issue #7 历史记录完全一致：`PC=0xeffffffe`，`xPSR` 的
IPSR=3（HardFault）、bit29=1（Thumb），MSP 指向 SRAM（~0x20019xxx）。
OpenOCD 报 `clearing lockup after double fault`——即 core0 进入 HardFault 后
fault handler 再次 fault，最终 PC 落到 bootROM 的 lockup 向量 `0xeffffffe`。
此后 SWD 连核心寄存器/DHCSR(0xE000EDF0) 都读不到（`Failed to read memory`），
与"board wedges / 只能靠 SWD"的现象吻合。core1 停在 ROM `0xda`。

## MicroPython 对照（决定性：硬件正常）

把官方 MicroPython Pico 2 UF2（`.tmp/micropython-pico2.uf2`）解出 payload
（`build/pico2-micropython.bin`，UF2 块 payload 从偏移 0x20 起，app 从 0x10000000
共 330496 B），用同一 OpenOCD / 同一 Debugprobe / 同一 SWD 接法写入同一块 Pico 2：

```text
flash write_image erase build/pico2-micropython.bin 0x10000000
reset run
# USB: COM22 VID_2E8A&PID_0005 (MicroPython CDC) -> OK
COM22 REPL:  >>> print(1+1)  ->  2  ->  >>>
```

**MicroPython 完全正常运行**（REPL 可交互，`1+1=2`），halt 时不出现 double fault。结论：

- **硬件、探针、SWD、供电、flash 写入路径全部正常** —— 失败是 TinyOS 软件层，
  不是线材/探针/刷写方法/硬件缺陷。
- 对照公平：两者都用 OpenOCD `flash write_image erase` 写 0x10000000；boot2 区都是
  全 0xef 占位（RP2350 ROM 走 PICOBIN 头直接启动，boot2 非差异点）。

## ROM 单步（缩小 fault 阶段）

`reset halt` 后从 bootROM 入口 `PC=0x88` 单步 40 次，ROM 走到 `0xc2→0xc4→0xc6`
后**回到 0xc2 无限循环**（等待某硬件条件，单步干扰 flash 时序所致）。
在用户 reset vector `0x100000d5` 设 2 字节硬件断点后 `resume`，未命中断点而是直接
double fault lockup——用户代码在到达可调试状态前就 fault，故障在启动极早期。

## 排除项（二进制级）

- **boot2**：两个 UF2 的 boot2 区（0x10ffff00）都是全 `0xef` 占位（0 non-ef），
  非差异点；RP2350 ROM 走 PICOBIN IMAGE_DEF 头启动。
- **vector 头**：ATK 与 pico2 的 `0x10000000` 起始块差异只是各 handler 地址整体
  +4（代码布局不同）与初始 SP 不同（RAM 布局不同），reset vector 分别是
  `0x100000d5` / `0x100000d9`，都合法。
- **入口地址**：两个 ELF entry 都是 `0x100000d5`。
- **源码**：variant 机制保证 `board/rp2350/platform.c` 两板共用，内核零改动
  （`git grep BOARD_PICO2 kernel/` 为空），board/pico2 只有 board.conf +
  board_data.c。软件启动代码无分叉。
- **flash 容量**：ATK 4 MiB、Pico 2 配置 4 MiB / 实际焊 8 MiB；issue 评论已论证
  容量不同不足以解释 `PC=0xeffffffe`。
- **硬件/探针/刷写**：MicroPython 对照已排除。

**根因收敛**：硬件正常、启动链与源码无差异，故障在 TinyOS 用户代码最早的平台
初始化（pico2 初始 SP/栈布局与 ATK 不同，MSP=0x20019xxx；也可能是早期时钟/XIP）。
下一步需在 `0x100000d5` 命中断点后抓首个 fault 的 CFSR/BFAR（当前 double-fault
lockup 下读不到），或对比 pico2 与 ATK 的 `plat` 早期初始化差异。

## 未定 / 下一步（A 轨尚未关闭）

- **根因未定**：现场符合同一 RP2350、同镜像在 Pico 2 上启动早期 double fault，
  但 flash 型号（w25q64fv）、QMI/XIP 时序、CLK_DIV、启动镜像谁是直接原因尚未
  二分。SWD 在 lockup 下读不到 CFSR/HFSR/BFAR/栈帧，需要在 fault 前设断点或
  换更稳的探针/时钟才能拿到 fault 类型。
- **B3 心跳**：`NOT RUN`——启动失败，无 `tos>` prompt。
- **WONTFIX 判定**：尚未做（需要完整排除链）。
- 后续可选：刷官方 MicroPython 验证探针/SWD 路径本身（openocd 识别 RP2350 已
  证明探针可用）；在 `0x100000d5` 设断点单步定位首个 fault 指令；或对比
  Pico 2 官方 SDK 例程的 boot2 配置。

## 证据与判据状态

- issue #7 A 轨二选一（修好 / WONTFIX 证据链）：**进行中**——真机现场已抓到，
  根因与结论未定，本卡不宣称关闭。
- issue #7 B 轨 B1/B2：实现侧已在 `b6be508`/`d663bde` 落地并记卡；本轮复核了
  T0/T1 与 B1 架构判据（board/pico2 两文件、kernel 无 PICO2 ifdef、chip 表注册
  pico2→rp2350），全部成立。
- `scripts/rp2350/pico2_heartbeat.py`（B2 checklist 交付物）：**尚未实现**（测试侧待补）。
