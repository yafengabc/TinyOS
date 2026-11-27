# T4：合并后的 RV32 SMP 30 分钟 dd（2026-11-27）

```text
commit:            67cd572（merge exp/thread-sched 之后的 main）
change:            none —— 在合并后的 main 上重跑 RV32 SMP 的 30 分钟 T4
baseline:          3f07c44（同一天合并前的那一轮，见 2026-11-27-t4-rv32smp-30min-3f07c44.md）
target/config:     rpi-pico2-rv32-smp / defaults（board.conf 默认 features = pio, rir, kthreads, usb_pump）
board/port/image:  Raspberry Pi Pico 2（RP2350A，8 MiB W25Q64 克隆板）
                   UART0 → CH343 COM19；USB CDC = COM27
toolchain:         go 1.27.x windows/amd64, clang 23.1.4, python 3.14
host:              Windows, Git Bash
T0:                NOT RUN（本次只做 T4）
T1:                NOT RUN
T2:                NOT RUN
T3:                NOT RUN（烧写后 smoke 7/7；T3 另有四镜像回归卡）
T4:                PASS —— 1800 轮 dd，约 31 分钟，ok=1800
logs:              build/t4-rv32smp-30min-merged.log（原始日志不入库）
```

## 为什么重跑

合并前那一轮跑在 `3f07c44` 上；`exp/thread-sched` 合并进 main 之后，RV32 侧多了三件
会碰到这条路径的东西：

1. tick 的 32 位取模（`b26e93a`），tick 从 `__aeabi_uldivmod` 换成逐点内联；
2. 内核默认 `-Os`，用户程序 `user_opt = -O2`（`targets/rpi-pico2-rv32.conf`）；
3. `kthreads` + `usb_pump`：USB 状态机改为 prio 0、固定在 core 0 的内核线程，
   内核线程在 M 态跑（PMP 不约束）。

所以这一轮才是"最终镜像"的 T4，`3f07c44` 那张只作合并前对照。

## 执行的命令与结果

```sh
# 1) 构建（main @ 67cd572）
go run ./cmd/tos build -target rpi-pico2-rv32-smp        # build ok；UF2 2,097,664 B

# 2) 烧写 + 冒烟（UART0 发 Ctrl-D 进 BOOTSEL）
python scripts/rp2350/flash.py --uf2 build/tinyos-rpi-pico2-rv32-smp.uf2 \
    -p COM19 --uart-port COM19 --boot-uart COM19 --smoke
# 7/7 checks passed；`[cpu1] secondary ready`、`[cpu1] took pid 2`（usb_pump 内核线程占了 pid 1）

# 3) 测速 5 轮
python scripts/rp2350/dd_stress.py -p COM19 --target rpi-pico2-rv32-smp --rounds 5
# DD OK 5/5，1.0 rounds/s

# 4) 正式压测（32 KiB/轮：写 + 读回校验 + rm）
python scripts/rp2350/dd_stress.py -p COM19 --target rpi-pico2-rv32-smp --rounds 1800
# DD OK 1800/1800，exit=0
```

## 关键输出

```text
  round 1800/1800  ok=1800  1.0 rounds/s  eta 0s

DD OK 1800/1800  target=rpi-pico2-rv32-smp port=COM19
```

压测前后同一条 UART0（不重启、不重开串口）：

```text
开始前  uptime: 1459.942 s (1459942 ms)  tick=1000 Hz  cpus=2  cpu=0
结束后  uptime: 3326.064 s (3326064 ms)  tick=1000 Hz  cpus=2  cpu=1  pid=4385
        → 差值 1866 s（31m06s），期间没有复位

结束后  df
        littlefs             7864320    389120   7475200        4%

结束后  free
                total      used      free      peak
  kernel heap    32768         0     32768      1792

                bytes       KiB
  SRAM         524288       512
  kernel       107184       104
  tasks        409600       400
  unused         7504         7

结束后  cat /proc/cpuinfo
        processor 0/1 : Hazard3 (RP2350), rv32imac, 150.000 MHz
        board : Raspberry Pi Pico 2, cpus : 2, tick Hz : 1000
```

判定：

- **1800/1800 每轮读回校验通过**，没有坏 pattern、写错误或 `DD FAIL`；
- **uptime 连续**（1460 → 3326 s，差 1866 s ≈ 31 分钟），30 分钟里没有复位、没有看门狗
  重启；
- **kernel heap 只动峰值水位**（used 0 / peak 1792），tasks 400 KiB 不变 → 没有窗口或
  段泄漏；
- **`df` 4%**（used 389,120 B ≈ 380 KiB）→ 1800 个文件全部回收，没有堆积；比合并前
  那轮的 372,736 B 多 16 KiB，是 littlefs 目录块在这一轮里扩了一次，不是每轮残留
  （否则会是 1800 × 32 KiB 量级）；
- `unused` 从合并前的 16 KiB 降到 7 KiB：`usb_pump` 内核线程占了一个窗口，这是合并后
  镜像的预期形状；
- 结束后 shell 仍应答，两个 hart 都在 `rv32imac` 150 MHz。

## 没跑的

- T0/T1/T2/T3 本次没跑：这是纯 T4 补跑，被测镜像与 `67cd572` 的构建产物一致；
  T3 的判据在 `2026-11-24-pico2-matrix-05a0e18.md` 与四镜像卡里。
- ATK 未参与：按 `AGENTS.md` 的 soak 预算，一个 RP2350 RV32 SMP 镜像即可（任一板）。
- `soak_board.py`（窗口/段抖动）没跑：本轮 T4 的判据是 dd 长跑。
