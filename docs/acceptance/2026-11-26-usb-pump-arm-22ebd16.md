# E3 板侧：ARM USB 泵迁到内核线程（2026-11-26）

```text
commit:            22ebd16
change:            RP2350 ARM 板的 usb_cdc_poll() 从 idle/PendSV 钩子迁到
                   prio 0、固定 core 0 的内核线程（特权 Thread mode + PSP 栈），
                   plat_start_tasks() 建线程成功后清 g_idle_poll；新增
                   configs/rpi-pico2-usbpump.conf；RV32 泵配置注释同步。
baseline:          2b7d55f（ktask 退用例卡）
target/config:     rpi-pico2-smp（+usb_pump）、rpi-pico2；18 目标构建
board/port/image:  Pico 2 ARM（本机无板，未上板）
toolchain:         go 1.27.3；clang 23.1.4
host:              Windows / 本机
T0:                PASS（go test -count=1 ./...、go vet ./...）
T1:                PASS 17/18（x86 MKFS，pre-existing，docs/OPEN-ITEMS.md §17）
T2:                NOT RUN（板级改动，QEMU 无 USB 控制器）
T3:                NOT RUN（无板，见下）
T4:                NOT RUN
logs:              build/t-pump-*.log（不入库）
notes:             泵镜像 = text +1,764 B、.bss +36,904 B（池）；默认镜像不变
```

## 执行的命令与结果

```sh
go test -count=1 ./...                                            # PASS
go vet ./...                                                      # PASS
go run ./cmd/tos build -target <t>                                # 17/18（x86 §17）

go run ./cmd/tos build -target rpi-pico2-smp -config configs/rpi-pico2-usbpump.conf
go run ./cmd/tos build -target rpi-pico-smp     # 等其余 17 个目标
```

## 关键输出

```text
# 镜像尺寸（同一 target 的默认 vs 泵）
rpi-pico2-smp 默认   text 185,048  data 3,988  bss 128,704
rpi-pico2-smp 泵     text 186,812  data 3,988  bss 165,608   (+36,904 = 6x6 KiB 池)

# 默认镜像没有池（--gc-sections），RP2040 默认 44,592 B
```

## 结构

- `board/rp2350-generic/platform.c`：`#ifdef TOS_USB_PUMP` 下
  `usb_pump_task()`（`usb_cdc_poll(); process_ktask_sleep(1);`）与
  `plat_start_tasks()`（`process_ktask_create("usbpump", ..., 4096, 0, 0)`，
  成功则 `g_idle_poll = NULL`，失败保留旧钩子并打印一行）。
- 内核线程由 E3-M3 的 ARM 路径运行：特权 Thread mode、自己的 PSP 栈、tick 不抢占、
  SVC 阻塞/让出。`/proc/ktask` 里应出现 `usbpump` 及高水位。
- `configs/rpi-pico2-usbpump.conf`：`rpi-pico2-smp` + `usb_pump`；
  `configs/rpi-pico2-rv32-usbpump.conf` 的“ktask 只支持 RV32”注释已改。

## 板侧要跑的（本机无板）

按 `docs/OPEN-ITEMS.md` §19/§22：

1. 刷 `configs/rpi-pico2-usbpump.conf` 的镜像（单核/双核各一），确认启动打印
   `[usb] pump thread pid N on cpu 0`，`cat /proc/ktask` 有 `usbpump` 行与高水位。
2. 长输出：`cat /proc/ksyms` 或 21 KiB 文件经 CDC，逐字节核对（§19 的清单）。
3. `heartbeat`、`crash`、`threadbench`、`ls /bin`、`top` 类命令不退化。
4. 回退镜像（默认 hooks）对照，确认泵是从 `g_idle_poll` 切过来的，不是两边都在跑。

## 没做 / 没跑的

- **RP2040**：它的泵在 tick 里（`board/rpi-pico/platform.c`），不是 `g_idle_poll`；
  且 36 KiB 池占 264 KiB 的 14%，需要一槽/更小池的构建旋钮和它自己的板测。单独一步。
- **M33 单核镜像**：`configs/rpi-pico2-usbpump.conf` 固定 `rpi-pico2-smp`；单核要单独
  的配置（同 target 名不同即被拒，见 tos 的校验）。
- **T2**：QEMU 的 arm 目标没有 USB 控制器，泵路径只能在板上验。
- **x86**：`MKFS: No more free space`（§17）。
