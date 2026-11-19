# ATK 收尾：唯一 USB 序列号 + RP2040 延时标定（2026-11-20）

```text
commit:            689c07f（序列号）+ f1be069（usb_delay_ms）+ 本卡片
change:            RP2350 两个端口的 iSerialNumber 改成芯片唯一 ID；
                   RP2040 的 usb_delay_ms 按实测 9.2 周期/次重新标定
baseline:          a01302a
target/config:     atk-dnr2350am-smp / atk-dnr2350am-rv32-smp（真机）；
                   全 18 目标构建
board/port/image:  ATK-DNRP2350AM（COM14 = 板载 CH343 UART，CDC = COM26）
toolchain:         go 1.27.3, clang 23.1.4, python 3.14.0
host:              Windows NT 10.0.17763 (MSYS2 UCRT64)
T0:                PASS（gofmt / go vet ./... / go test ./... -count=1）
T1:                PASS（go run ./cmd/tos build all：18/18）
T2:                NOT RUN（本次改动只在 USB 初始化路径；QEMU 无 USB CDC 宿主路径）
T3:                PASS（ATK M33：heartbeat 6/6；ATK Hazard3：两路控制台均可用）
T4:                NOT RUN
logs:              本地串口采集 + Windows PnP 设备实例 ID
```

## 1. USB 序列号（`689c07f`）

序列号从写死的 `"0001"` 改成芯片自己的 64 位 ID，来源是 boot ROM 的
`SYS_INFO_CHIP_INFO`（`'G','S'`，与 pico-sdk `pico_unique_id` 同源）。ARM 用
`RT_FLAG_FUNC_ARM_SEC` + Thumb 位，RV32 用 `RT_FLAG_FUNC_RISCV`（无 Thumb 位），
所以同一颗芯片两个镜像报同一个号。

Windows PnP 设备实例 ID：

```text
改动前                    USB\VID_2E8A&PID_000A\0001
M33（atk-dnr2350am-smp）  USB\VID_2E8A&PID_000A\DB8EA31104F9F060
Hazard3（-rv32-smp）      USB\VID_2E8A&PID_000A\DB8EA31104F9F060
```

两块板（ATK + Pico 2）不再被 Windows 当成同一个设备，COM 号不会互相顶掉；
刷机也不改变这个号（ID 在芯片里）。

**RP2040 未做**：它没有 SYS_INFO，唯一 ID 要发 0x4B 读 flash，需要在 USB init
时走 SSI/ROM flash 序列。两块 RP2040 同时插仍会撞。

## 2. `usb_delay_ms()` 标定（`f1be069`，仅 RP2040）

这个忙等只在 RP2040 端口存在（RP2350 没有）。注释假设每次迭代 3 周期，实际按
UART 计时测得 **9.2 周期**：200 ms 的 D+ 掉电重连花了 **614 ms**。除数从 `/3000`
改成 `/9000`，下限从 4000/ms 改成 1200/ms（保证 E15 的 forced-J 仍 > 800 µs）。

**RP2040 真机未验**（板子不在台上），算术来自 2026-11-19 卡片里那组 614 ms 实测。

## 3. 真机结果

| 镜像 | 检查 | 结果 |
|---|---|---|
| `atk-dnr2350am-smp`（01:08:42） | `heartbeat.py -p COM14` | **6/6**（时间推进 1.155 s/1.178 s） |
| `atk-dnr2350am-rv32-smp`（01:08:32） | `uptime` 走 UART 与 CDC 两路 | 两路都回 `tos>`，`cpus=2` |

## 没做（NOT RUN）

- **RP2040 真机**：`usb_delay_ms` 新标定的启动时间、20 次冷启动。
- **T2 QEMU**：本次改动不经过 QEMU 可覆盖的路径。
- **Pico 2 闸门复测**：`fbd3591` 之后还没跑（板子被换下）。
- **#31 RP2040 pump / #29 Hazard3 有界 pump**：仍缺单次 `usb_cdc_poll()` 耗时。
