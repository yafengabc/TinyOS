# USB 启动不再等主机 + RP2040 RESETS 地址修正（2026-11-19）

```text
commit:            4f032cf
change:            删除 usb_cdc_init() 末尾阻塞的 usb_cdc_wait()（三个 USB 端口）；
                   TinyUSB shim 的 RESETS_BASE / USBCTRL 位改回按芯片取；
                   usb_cdc_putc() 恢复"未枚举前不写 CDC FIFO"
baseline:          e9b9b4b
target/config:     atk-dnr2350am-smp（真机 20/20）；rpi-pico-smp（时间戳实测）；
                   全 18 目标构建；QEMU 4 目标 smoke
board/port/image:  ATK-DNRP2350AM Cortex-M33 SMP（COM14/COM24）；
                   Pico 2 克隆板 RP2040（UART0 → CH343 COM19）
toolchain:         go 1.27.3, clang 23.1.4, python 3.14.0
host:              Windows NT 10.0.17763 (MSYS2 UCRT64)
T0:                PASS（gofmt / go vet ./... / go test ./... -count=1）
T1:                PASS（go run ./cmd/tos build all：18/18）
T2:                PASS（tostest smoke rv32 / rv32smp / arm / x86）
T3:                PASS（ATK M33 SMP：拔插电源 20/20 进 shell）
T4:                NOT RUN
logs:              用户终端 + 本地串口采集（原始日志未入库）
```

## 1. 启动不再等 USB 枚举（三个端口）

`usb_cdc_init()` 末尾的 `usb_cdc_wait()` 会一边转圈一边跑 `tud_task()` 直到
`tud_mounted()`。注释写"约 1.5 s（一次 spin ≈ 130 周期）"，实测在 RP2040 上是
**8–10 s**；而且它自己就是唯一推进 USB 状态机的地方，所以它等的东西被它自己拖住了。
接电池/充电头这种没有主机的场景同样白等。

三个端口（rp2040 / rp2350 / rp2350rv）都删掉这个等待：启动路径不再依赖主机。

## 2. RP2040 的 RESETS 地址是 RP2350 的（6.09 s 的真凶）

`third_party/tinyusb/port_rp2/include/hardware/resets.h` 把两个芯片的地址写成了
RP2350 的值：

| 常量 | 头文件原值（RP2350） | RP2040 实际 |
|---|---|---|
| `RESETS_BASE` | `0x40020000` | `0x4000c000` |
| USBCTRL 复位位 | bit 28 | bit 24 |

于是 RP2040 上 `dcd_init()` 复位的是一个**不存在的外设**，然后
`unreset_block_wait()` 轮询一个永远为 0 的 `RESET_DONE`：100 万次未映射 APB 读，
实测 **6.09 s / 每次启动**。现在 `RESETS_BASE` 取自各板 memmap，USBCTRL 位放在
memmap 的 `RESETS_USBCTRL`（RP2040 bit 24、RP2350 bit 28）。

### 实测（rpi-pico-smp，临时 TIMERAWL 计时点，单位 µs）

```text
                            修正前        修正后
usb init enter              66922          605
tud init done             6171921       143946
usb init done             6811898       158202
```

剩下的 143 ms 是 shim 里那个有界 `RESET_DONE` 轮询跑满（RP2040 现在真的会复位
USBCTRL，而它的 done 位在时限内没置起）；不影响功能，记在 `docs/OPEN-ITEMS.md`。

## 3. `usb_cdc_putc()` 恢复"未枚举前不写 FIFO"

中途试过"未枚举时先缓冲进 CDC FIFO，主机连上再发"，ATK 上没问题；但 RP2040 上
UART 终端出现"卡在 `[usb] init done`、等 Windows 枚举完才到 `tos>`"的现象，怀疑是
每个控制台字节都要拿跨核 USB 锁并往不能排空的 FIFO 里塞。因此恢复为
`if (!g_hw_ready || !tud_mounted()) return;`（即本轮改动之前的行为）。

**这一条尚未在硬件上复验**（`NOT RUN`）：RP2040 的 UART 卡顿是否随之消失，需要在
明天的构建上再测一次。

## 结果

| 板 / 镜像 | 测试 | 结果 |
|---|---|---|
| ATK-DNRP2350AM M33 SMP（`atk-dnr2350am-smp`） | 拔插电源 20 次 | **20/20 进 shell** |
| RP2040 `rpi-pico-smp` | `[t]` 时间戳 | 固件 `usb init done` = **158 ms**（修正前 6.81 s） |

## 没做（NOT RUN）

- **RP2040 20 次冷启动计数**：只做了单次时间戳测量。
- **RP2040 UART 卡顿复验**：见上（putc 恢复后的构建未上板）。
- **ATK RV32 SMP**：本轮只刷了 M33 SMP；RV32 镜像构建通过但未上板。
- **ATK `accept33.py`**：未跑（本轮只做启动计数）。
- **Pico 2 两个 SMP 镜像**：未重刷（上一轮 `0193e00` 的 20/20/20+ 仍有效，但那是
  旧构建）。
- **T4 长跑**。
- 两块板 USB 身份相同（`2e8a:000a` + 序列号 `0001`）导致同时插上时 Windows 互相
  干扰、COM 号漂移；改芯片唯一序列号还没做，记在 `docs/OPEN-ITEMS.md`。


## 补记（同夜，`eb8798b`）：RV32 端口必须保留这个等待

`atk-dnr2350am-rv32-smp` 刷入本卡片所述的镜像后，到 `tos>` 立刻 trap：

```text
*** RV32 TRAP ***
breakpoint: mcause=0x3 mepc=0x10013d0a mtval=0x0
ra=0x100131c8 sp=0x20016830 ...
```

`0x10013d0a` 反汇编为 `li a0,0; ebreak`（panic 路径）。Hazard3 端口没有 USB 中断，
`usb_cdc_poll()` 是控制器唯一的事件泵，删掉 `usb_cdc_wait()` 等于抽掉了启动早期的
轮询点。`eb8798b` 只回退 RV32 端口，M33 保持本卡片的改动；回退后用户实测
**启动很快、CDC 瞬间枚举**。

固件侧那个断言的**具体位置仍未知**：发布 ELF 已 strip，`addr2line` 解析不了，
需要一次带符号构建（`docs/OPEN-ITEMS.md` §5b）。


## 补记二（同夜）：RP2040 冷启动 20/20

用户在本卡片所述构建的 RP2040（`rpi-pico-smp`，Pico 2 克隆板之外的 RP2040 板，
UART0 → CH343 COM19）上拔插电源 20 次：**20/20 都起来了**，但主观上“启动慢”。

结合 `[t]` 时间戳（固件 `usb init done` = 158 ms），慢的部分不在固件启动，而在
主机把 CDC 配置起来之前：LED 只在 `usb_cdc_configured()` 为真时熄灭
（`board/rpi-pico/platform.c`），而 CDC 就绪要等 Windows 枚举。下一步两个实验：
把熄灯从 `usb_cdc_configured()` 上解耦，以及冷启动不做 200 ms 的 D+ 掉电重连
（见 `docs/OPEN-ITEMS.md` §5b 第 3 条）。
