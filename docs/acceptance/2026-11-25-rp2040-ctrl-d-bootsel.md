# RP2040 Ctrl-D 进 BOOTSEL：补齐 ROM 需要的 watchdog tick（2026-11-25）

```text
TinyOS commit:     本提交
change:            rp2040 Ctrl-D 走单一 ROM 路径；clocks_init() 启动 1 MHz
                   watchdog tick；删掉 scratch/SOF 强制触发/复位后二次调 ROM
target/config:     rpi-pico、rpi-pico-smp（默认配置）；18 目标构建（x86 见注 1）
board/port/image:  RP2040 NOT RUN（板不在本机）；操作与采集清单见
                   docs/RP2040-BOOTSEL.md
toolchain:         go 1.27.1 windows/amd64；clang 23.1.4
T0:                PASS（go test -count=1 ./...）
T1:                PASS — 17/18；x86 在 HEAD 上也失败（pre-existing，注 1）
T2:                NOT RUN（RP2040 无 QEMU 映像；本改动不经过 QEMU 端口）
T3:                NOT RUN（交板主，按 docs/RP2040-BOOTSEL.md 执行）
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
- `bootsel_try_direct()`：唯一入口；thread mode；先使能 ROSC（MicroPython
  `machine.bootloader()` 同款），**先停 SysTick**（否则 tick 里的"清残留 watchdog"会
  取消 ROM 的倒计时），再调 ROM。
- 删除：`bootsel_request()`（`scratch[0]="TOSB"` + 立即 TRIGGER）、`bootsel_rom_call()`
  （SOF 强制触发）、`bootsel_early_handover()`、`plat_bootsel_force_check()`、
  `g_bootsel_force`、`g_bootsel_rom_req`、Ctrl-B 分支，以及 `tusb_shim.c` 里对应的
  ISR 钩子。理由：pc 指向 bootrom 里的 wrapper，外部无法复刻，半截记录 + 强制触发
  正是"有时复位但没进 BOOTSEL"的来源。
- `plat_timer_tick()` 保留每 tick 清残留 watchdog 的防御（启动期仍有东西会重新 arm 它），
  只是不再与 BOOTSEL 路径共享状态。

## 3. 验证

本机只有 T0/T1（构建）；**T3 NOT RUN**。板测按 `docs/RP2040-BOOTSEL.md` §4：
UART0 上发单字节 `0x04` → 期望 `[bootsel] ROM reset_usb_boot()`、约 10–15 ms 后出现
`RPI-RP2`、串口没有新的启动行；失败按 §5 采三个 watchdog 寄存器。

## 注

1. `go run ./cmd/tos build all` 在 HEAD（未含本改动）上就已经是 **17/18**：`x86` 的
   300 KiB 文件系统镜像 `MKFS: No more free space`。与本改动无关（本次只动
   `board/rpi-pico/platform.c` 与 `usb/rp2040/tusb_shim.c`），已记入
   `docs/OPEN-ITEMS.md` §17。
