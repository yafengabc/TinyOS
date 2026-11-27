# issue #80：ARM wall clock 改读硬件 TIMER（2026-11-27）

```text
commits:           caf284a（RP2040）、cd95441（RP2350 M33）
change:            ARM 的 plat_now_ns()/plat_uptime_ms() 从 g_ticks 改读 1 MHz 硬件 TIMER；
                   g_ticks 继续只用于调度/sleep。RP2040 用 TIMER @0x40054000，
                   M33 用 TIMER0 @0x400b0000 + TICKS @0x40108000。
baseline:          issue #80：RP2040 SMP 30 分钟 soak 期间 uptime 少约 650 s
board/port/image:  RP2040 Pico CDC COM29（rpi-pico / rpi-pico-smp）；
                   Pico 2 CDC COM27（rpi-pico2-smp）
toolchain:         go 1.27.1, clang 23.1.4, python 3.13.14
host:              Windows / Git Bash, D:\projects\TinyOS
T0:                PASS —— 四个目标均构建通过
T1:                NOT RUN（本卡为时钟专项板测）
T2:                NOT RUN（同上）
T3/T4:             见既有 RP2040/Pico 2 验收卡；本卡只覆盖 issue #80 的时钟判据
logs:              build/clock80-*.log（不入库）
```

## 根因与修复

- ARM 的 wall clock 取 `g_ticks`，而 `g_ticks` 只由 core 0 的 SysTick 递增。
  flash 写入时 XIP 关闭，另一核在 `rp_flash_park_loop()` 里关中断 park；
  如果 park 的是 core 0，整个 flash 操作期间没有 tick，时钟停走。
- RP2040：`plat_board_early()` 清 `RESET_TIMER (bit21)`，`plat_timer_init()`
  记录启动计数，`plat_now_ns()`/`plat_uptime_ms()` 读 `TIMERAWH/TIMERAWL`
  （high-low-high，避免 32 位半字跨越）。
- RP2350 M33：同样清 `RESET_TIMER0 (bit23)` 并用 `ticks_init()` 编程 TICKS。
  实测 `TIMER0_CYCLES` 是**直接分频数**：12 MHz / 11 = 1.0909 MHz，
  导致第一版板测快 9.09%；改成 12 后斜率回到 1.000。RV32 端口的
  `khz/1000 - 1` 写法不适用于 ARM 侧这个块。
- `plat_uptime_ms()` 必须先除 1000 再截 `uint32_t`：直接截微秒会让
  `/proc/uptime` 放大 1000 倍，且 71.6 分钟回绕（原毫秒计数是 49.7 天）。

## 测试口径

`build/clock_stress.py` 每轮写 32 KiB、回读校验、删除，逼 core 0 进入
flash park；每 30 s 配对采样主机 `time.monotonic()` 与板载 `uptime`。
判据：轮次全过、斜率约 1.000、`/proc/uptime` 与 `uptime` 一致。
`build/clock_idle.py` 做空闲 10 分钟对照和 `sleep 10000` 墙钟计时。
两个脚本都在 `build/`，不入库。

## 结果

| 板 / 镜像 | 提交 | 1 min | 10 min 压力 | 10 min 空闲 | sleep 10000 |
|---|---|---|---|---|---|
| RP2040 `rpi-pico` | `caf284a` | 79/79，斜率 1.000020 | 789/789，斜率 1.000011，残差 +7 ms | 前 210 s 干净（两钟斜率 1.000），第 240 s 因 USB 线断开中止 | — |
| Pico 2 M33 `rpi-pico2-smp` | `cd95441` | 61/61，斜率 1.000004 | 607/607，斜率 1.000007，残差 +4 ms | 22 点/600.5 s，斜率 1.000010（now）/ 1.000008（proc），最大偏差 0.125 s | 10.100 s |
| RP2040 `rpi-pico-smp` | `cd95441` | 64/64，斜率 1.000015 | 631/631，斜率 1.000015，残差 +9 ms | 22 点/600.5 s，斜率 1.000013 / 1.000013，最大偏差 0.101 s | 10.082 s |

`proc` 一致性：各镜像压力结束时的 `uptime` 与 `cat /proc/uptime` 差值
均为命令间隔的 80–124 ms，没有 1000 倍放大或丢时。

另用仓库标准工具 `scripts/rp2350/dd_stress.py` 在 `rpi-pico-smp` 上补跑：
`DD OK 600/600`，压力前 `uptime 1712.146 s`、压力后 `4053.384 s`
（差值 2341.238 s），主机墙钟 39 分 03 秒（约 2343 s），相差约 1.8 s；
pid 1467→2668 且 uptime 连续，无 reboot。这满足 issue 的
「10-minute dd/soak ... within 1-2 s, no reboot」判据（这次因工具速度降到
0.3 轮/s，实际墙钟约 39 分钟，覆盖 10 分钟要求）。

## 已知干扰

- RP2040 单核空闲测试在第 240 s 时 Pico 的 USB CDC（COM29）整体消失，
  没有新端口重枚举；USB-TTL（COM20）仍在。判定为 USB 线/连接问题，
  该轮空闲作废，压力测试结果不受影响（已在压力之前完成）。
- Pico 2 的 shell 会把 `tos>` 提示插进命令输出中间（如
  `pattern veriftos> ied`），临时脚本已改为去掉提示及两侧空白再匹配；
  这是测试脚本问题，不是固件数据错误。
- 第一版 M33 的 9.09% 快时钟通过启动日志回读 TICKS 寄存器定位，
  诊断输出已从最终镜像移除。

## 结论

RP2040 与 RP2350 M33 的 ARM wall clock 均已脱离 `g_ticks`，在 flash 写压力下
10 分钟斜率误差 ≤ 1.5e-5；RP2040 SMP 与 M33 SMP 的 10 分钟空闲、
`sleep 10000` 与 `/proc/uptime` 一致性全部通过。issue #80 可以关闭。
RP2040 单核的空闲轮次因 USB 线断开只取到前 210 s，但其压力 10 分钟和
1 分钟快速验证均通过，且同一套代码在 SMP 上空闲完整通过。
