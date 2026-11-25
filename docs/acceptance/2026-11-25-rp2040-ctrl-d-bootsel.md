# RP2040 Ctrl-D 进 BOOTSEL：补齐 ROM 需要的 watchdog tick（2026-11-25）

```text
TinyOS commit:     本提交
change:            rp2040 Ctrl-D 走单一 ROM 路径；clocks_init() 启动 1 MHz
                   watchdog tick；ROM 从 core 0 的 tick 调用（不是 thread mode）；
                   删掉 scratch/SOF 强制触发/复位后二次调 ROM
target/config:     rpi-pico、rpi-pico-smp（默认配置）；18 目标构建（x86 见注 1）
board/port/image:  RP2040 Pico（COM19 UART0/CH343、COM29 CDC）；rpi-pico-smp
toolchain:         go 1.27.1 windows/amd64；arm-none-eabi（msys ucrt64）
T0:                PARTIAL — go vet 干净；go test 仅 TestPinnedToolchainSources
                   失败（用户 WIP 的 NAT_YIELD/NAT_READY 未同步到 tinyos-script，
                   与本改动无关，见注 2）
T1:                PASS — 17/18；x86 在 HEAD 上也失败（pre-existing，注 1）
T2:                NOT RUN（RP2040 无 QEMU 映像；本改动不经过 QEMU 端口）
T3:                PASS（Ctrl-D：UART0 发 0x04 → `[bootsel] ROM reset_usb_boot()`
                   → 0.81 s 出现 RPI-RP2；从该卷刷回同一镜像成功）。CDC 的
                   `crash` fault-isolation 仍失败，属 §5c 的独立开口项
T4:                NOT RUN
```

## 1. 根因（源码依据）

RP2040 的 ROM `reset_usb_boot()`（pico-bootrom-rp2040）：

```c
watchdog_hw->scratch[0] = mask;  watchdog_hw->scratch[1] = disable;
watchdog_reboot(_usb_boot_reboot_wrapper, SRAM_END, 10);
while (true) __wfi();
```

`watchdog_reboot` 走 **10 ms 倒计时**，需要 `WATCHDOG_TICK` 的 1 MHz 时钟；本板此前
**故意把 tick 停掉**（`platform.c` 原注释），所以 ROM 永远停在 `wfi`。RP2350 用的是
REBOOT2（`0x4252`），不经过 watchdog，所以那边一直好用。

## 2. 改动

- `clocks_init()`：`CTRL=0`、`LOAD=0x1fffff`，再 `TICK = 12 | ENABLE`（clk_ref 12 MHz
  → 1 MHz）——即 pico-sdk `start_all_ticks()` 在 RP2040 上做的事。
- `bootsel_try_direct()` → `bootsel_enter_rom()`：唯一入口。由 core 0 的 tick 调用
  （**不是 thread mode**：本内核的 idle 由 `sched_switch()` 进入，而它在 SVCall 里，
  所以没任务时 IPSR 一直是 0x0b）；先使能 ROSC，**先停 SysTick**（否则 tick 里的
  "清残留 watchdog"会取消 ROM 的倒计时），再调 ROM。若 ROM 意外返回则恢复 SysTick。
- 删除：`bootsel_request()`（`scratch[0]="TOSB"` + 立即 TRIGGER）、`bootsel_rom_call()`
  （SOF 强制触发）、`bootsel_early_handover()`、`plat_bootsel_force_check()`、
  `g_bootsel_force`、`g_bootsel_rom_req`、Ctrl-B 分支，以及 `tusb_shim.c` 里对应的
  ISR 钩子。理由：pc 指向 bootrom 里的 wrapper，外部无法复刻，半截记录 + 强制触发
  正是"有时复位但没进 BOOTSEL"的来源。
- `plat_timer_tick()` 保留每 tick 清残留 watchdog 的防御（启动期仍有东西会重新 arm 它），
  只是不再与 BOOTSEL 路径共享状态。

## 3. 验证

板测（RP2040 Pico，`rpi-pico-smp`）：

1. BOOTSEL 刷入 `build/tinyos-rpi-pico-smp.uf2`，串口 COM19 出现 `tos>`。
2. 发单字节 `0x04`：串口打印 `[bootsel] ROM reset_usb_boot()`；**0.81 s** 后主机
   出现 `RPI-RP2`（F:）；串口没有新的启动行。重复一次为 0.80 s。
3. 从该卷把同一 UF2 拖回去：0.66 s 弹出，板子重新启动到 `tos>`（闭环）。

对照：合并远程实现（要求 IPSR==0）时，Ctrl-D 只置位、无串口输出、无盘——即
`docs/RP2040-BOOTSEL.md` §1 的第 3 条不成立，本内核没有可达的 thread mode。

CDC 的 `crash` fault-isolation 仍失败（`handed` 增长但主机只收到 fault 前约 50 B，
端口随后需要复位），与本次 Ctrl-D 改动无关，记在 `docs/OPEN-ITEMS.md` §5c。

## 注

1. `go run ./cmd/tos build all` 在 HEAD（未含本改动）上就已经是 **17/18**：`x86` 的
   300 KiB 文件系统镜像 `MKFS: No more free space`。与本改动无关（本次只动
   `board/rpi-pico/platform.c` 与 `usb/rp2040/tusb_shim.c`），已记入
   `docs/OPEN-ITEMS.md` §17。
2. `TestPinnedToolchainSources` 失败：`user/csc/csc.c`、`user/rvm/rvm.c`、
   `user/rcc/*.inc` 与 `../tinyos-script` 不一致（`0d16e55` 只把 `NAT_YIELD`/
   `NAT_READY` 加在 TinyOS 侧）。这是用户未提交的 WIP，不是本改动引入。
