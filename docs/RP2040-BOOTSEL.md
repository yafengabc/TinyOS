# RP2040：Ctrl-D 进 BOOTSEL（实现说明与板测清单）

状态：**板级 NOT RUN**（改动的作者没有这块板）。本文件既是实现说明，也是给测试机的操作/采集清单。

---

## 1. 结论先看

RP2040 上 `Ctrl-D` 要进 BOOTSEL，必须同时满足三件事：

1. **watchdog 的 1 MHz tick 在跑**（`WATCHDOG_TICK`）。ROM 的 `reset_usb_boot()` 用
   `watchdog_reboot(wrapper, SRAM_END, 10)` 做 10 ms 倒计时复位，tick 停了就永远停在
   `while (true) __wfi();`。
2. **倒计时期间不能被我们自己 disarm**。本板 tick 里有一条"清除 bootrom 残留 watchdog"
   的防御；调用 ROM 前必须先关 SysTick，否则倒计时被清掉。
3. **从 core 0 的 SysTick 里调 ROM**。这里没有可用的 thread mode：本内核的 idle
   循环由 `sched_switch()` 进入（`sched_idle()`），而 `sched_switch()` 本身在
   SVCall 里，所以"没有任务可跑"时 `plat_console_getchar()` 看到的 IPSR 一直是
   0x0b。ROM 的复位是 watchdog 硬件在倒计时结束时完成的，不依赖中断上下文；
   前提只是 tick 在跑、SysTick 先停掉。ROSC 也要使能（复位明确不复位 ROSC/XOSC，
   bootrom 的时钟就是它）。

另外：**RP2350 不需要这些**。它的 ROM 提供 REBOOT2（`'R','B' = 0x4252`），是芯片级
reboot 请求，不经过 watchdog。这就是同一段 Ctrl-D 代码在 Pico 2/ATK 上一直好用、在
RP2040 上一直不行的原因。

---

## 2. 两边的 ROM 路径（源码依据）

RP2040 bootrom（`pico-bootrom-rp2040/bootrom/bootrom_main.c`）：

```c
static void __attribute__((noreturn)) _usb_boot_reboot_wrapper() {
    _usb_boot(watchdog_hw->scratch[0], watchdog_hw->scratch[1]);
}
void __attribute__((noreturn)) reset_usb_boot(uint32_t mask, uint32_t disable) {
    watchdog_hw->scratch[0] = mask;
    watchdog_hw->scratch[1] = disable;
    watchdog_reboot((uintptr_t)_usb_boot_reboot_wrapper, SRAM_END, 10);  /* 10 ms */
    while (true) __wfi();
}
```

- `watchdog_reboot()` 写 `scratch[4..7]`（`0xb007c0d3`、pc、sp）并 **enable watchdog**，
  由 1 MHz tick 递减；`delay == 0` 才会直接 `TRIGGER`，这里不是。
- `pc` 指向 bootrom 里的 wrapper —— 外部拿不到，所以"自己写 scratch[4..7] + 强制
  TRIGGER"这条路**不可能完整复刻**（旧代码就是这么做的，会写出半截记录）。

RP2350（pico-sdk `reset_usb_boot` 的另一条分支）：

```c
rom_reboot(REBOOT2_FLAG_REBOOT_TYPE_BOOTSEL | REBOOT2_FLAG_NO_RETURN_ON_SUCCESS, 10, ...);
```

pico-sdk/MicroPython 之所以总能成功，是因为 `runtime_init_clocks()` 最后会
`start_all_ticks()`（`tick_start(TICK_WATCHDOG, clk_ref/MHz)`，12 MHz → 12 → 1 MHz），
而 MicroPython 的 `machine.bootloader()` 还会先补一句
`rosc_hw->ctrl = ROSC_CTRL_ENABLE_VALUE_ENABLE << 12`（`0xfab << 12`）。

---

## 3. 这次改了什么

| 文件 | 改动 |
|---|---|
| `board/rpi-pico/platform.c` | `clocks_init()`：先 `CTRL=0`、`LOAD=0x1fffff`，再写 `TICK = 12 | ENABLE`（1 MHz）。原来是**故意停掉** tick |
| 同上 | `bootsel_try_direct()` → `bootsel_enter_rom()`：唯一入口。由 core 0 的 tick 调用；使能 ROSC；先停 SysTick（保护 ROM 的 10 ms 倒计时），再 `reset_usb_boot(0,0)`。**不是** thread mode：本内核的 idle 跑在 SVCall 里 |
| 同上 | 删除 `bootsel_request()`（`scratch[0]="TOSB"` + PSM_WDSEL + 立即 TRIGGER）、`bootsel_rom_call()`（SOF 强制触发）、`bootsel_early_handover()`（复位后二次调 ROM）、`plat_bootsel_force_check()`、`g_bootsel_force`、`g_bootsel_rom_req`、Ctrl-B 分支 |
| 同上 | `plat_timer_tick()`：保留"每 tick 清残留 watchdog"的防御，注释改成"BOOTSEL 路径不依赖它"；不再打印 "Ctrl-D disabled" |
| `drivers/usb/rp2040/tusb_shim.c` | 删除 USB ISR 里那段"看到 scratch[4] 就强制 TRIGGER" |

Ctrl-D 的数据流现在是：UART 轮询/控制台读到 `0x04` → `g_bootsel_req = 1` →
core 0 的下一个 tick 里 `bootsel_enter_rom()`（IPSR=0x0f，这是可以的）→ ROM。

---

## 4. 测试机操作步骤

前置：板子接在 **UART0**（CH343）上，能进 shell；`build/tinyos-rpi-pico.uf2` 或
`-smp` 是本提交构建出来的镜像。

1. 刷机（BOOTSEL 一次），进 shell，记下启动行。
2. 打开串口（115200），确认提示符出现。
3. **发送单个字节 `0x04`**（不是字符串 "Ctrl-D"）：`printf '\004'` / pyserial
   `ser.write(b'\x04')` 都行。
4. 期望现象（**按顺序**）：
   - 串口最后一行：`[bootsel] ROM reset_usb_boot()`；
   - 约 10–15 ms 后板子复位，主机出现 **`RPI-RP2`** 盘；
   - 串口上不再出现新的 TinyOS 启动行（说明没有回到 flash 启动）。
5. 若出现盘：拖一个 UF2 进去，确认能刷写 = 闭环。
6. 若 5 秒内没盘、也没看到 `[bootsel]` 行 → 按 §5 采集。

**注意**：这颗板的 USB CDC 口在复位时会消失，测量请看 UART0；不要只开 CDC。

---

## 5. 失败时采什么

依次做，能一次把范围砍一半：

1. **看有没有走到 ROM**：串口应出现 `[bootsel] ROM reset_usb_boot()`。
   - 没有 → Ctrl-D 没被收到，或 core 0 的 tick 没在跑。先查 `0x4005802c` 的
     `bit9`（tick 是否真的开了）与 `g_bootsel_req` 是否置位（tick 里
     `bootsel_poll_uart()` 之后立即 `bootsel_enter_rom()`；UART0 的轮询在 tick 里，
     shell 卡住时也应该能收到）。
   - 有但没盘 → 进 §5.2。
2. **读三个寄存器**（在调用前后各读一次，或从另一条路读）：
   - `0x40058000` `WATCHDOG_CTRL`：调用后应短暂出现 `ENABLE(bit30)`，然后复位；
   - `0x40058004` `WATCHDOG_LOAD`：应为 `20000`（10 ms × 1000 × 2，RP2040-E1 勘误）；
   - `0x4005802c` `WATCHDOG_TICK`：`bit9=1`、`CYCLES=12`。
   若 `ENABLE` 压根没出现 → ROM 没走到 `watchdog_reboot`（ROSC 或 VTOR 问题）。
   若 `ENABLE` 出现又被清掉 → 有东西在 disarm（检查是否还有别的 tick/NMI 路径）。
3. **只保留 UART0**（拔掉 CDC 线/关掉 CDC 终端）再试一次：排除主机把旧 CDC 节点锁住。
4. **复位后 5 秒内读串口**：如果又打出了 TinyOS 启动行，说明 bootrom 走了 flash 启动
   （通常意味着 `scratch[4..7]` 的那次复位不是 ROM 发起的）。
5. 采集：串口原始日志（`0x04` 前后各 2 秒）、上面三个寄存器的值、主机端是否出现
   `RPI-RP2`、以及 `uuptime`/`[bootsel]` 行。

---

## 6. 已知边界

- **panic 之后**：`g_panic_poll = bootsel_poll_uart` 只负责收 Ctrl-D 并置
  `g_bootsel_req`；真正调 ROM 在 tick 里，而 panic 已经停了调度/tick，所以 panic
  之后 Ctrl-D 不保证能进 BOOTSEL。该路径保持原样。
- **SMP**：tick 只在 core 0 轮询 UART；core 1 收到 Ctrl-D 不会处理。要 Ctrl-D 就用
  core 0（默认 shell 在 core 0）。
- **ROSC**：`xosc_start()` 失败时走 `keep_ref`（ROSC），tick 仍按 12 计数，倒计时会偏快；
  bootrom 能不能起 USB 取决于 ROSC 频率，属于"晶振坏了"的降级场景，不保证。
- **`plat_reboot()`** 仍然用自己的 `watchdog trigger`（立即触发，与 tick 无关），不受本次
  改动影响。

---

## 6a. 2026-11-25 板测结果（rpi-pico-smp）

这台机器上的板：RP2040 Pico，UART0 走 CH343（COM19，115200），CDC 为 2e8a:000a。

- 镜像：`build/tinyos-rpi-pico-smp.uf2`（本提交构建），BOOTSEL 刷入。
- UART0 发单字节 `0x04`：立即打印 `[bootsel] ROM reset_usb_boot()`，
  **0.81 s** 后主机出现 `RPI-RP2`（F:）；串口没有新的 TinyOS 启动行。
- 从该 `RPI-RP2` 把同一 UF2 拖回去：0.66 s 弹出，板子重新启动到 `tos>`。
- 重复一次，结果相同（0.80 s / 0.81 s）。
- 对照（本次合并前的远程实现，要求 IPSR==0）：Ctrl-D 只置位、无任何串口输出、无盘。
  本内核的 idle 在 SVCall 里（IPSR=0x0b），所以那条路径永远走不到。

---

## 7. 回滚

只改了两个文件，`git revert <commit>` 即可。回滚后行为回到"tick 停、Ctrl-D 无效、
只能按 BOOTSEL"。
