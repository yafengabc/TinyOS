# 本机验收：Pico 2 四镜像、#46、§22 线程矩阵、§24 复核（2026-11-26）

```text
commit:            2a9a69a
change:            本机验收三件事：issue #46（Pico 2 单核 ARM 的 CDC 长输出截断，四个镜像
                   全部回归）、OPEN-ITEMS §22（四个镜像的线程矩阵）、OPEN-ITEMS §24（rv32 wheap）
baseline:          b63eca2
target/config:     rpi-pico2、rpi-pico2-smp、rpi-pico2-rv32、rpi-pico2-rv32-smp，
                   全部默认配置（kthreads + usb_pump）
board/port/image:  Pico 2；刷机走 UART0 COM19，所有验收走原生 CDC COM27（DTR=True）
toolchain:         go 1.27.3；clang 23.1.4；python 3.14.9
host:              Windows / 本机
T0:                NOT RUN（本轮无代码改动，只验收）
T1:                四个镜像构建通过
T2:                rv32 `wheap` 正常；`soak.py --cycles 3 --targets rv32` 3/3 PASS（1:27）
T3:                四镜像 CDC 长输出 + 六项心跳 + 线程矩阵（见下）
T4:                四镜像逐项 PASS（每个 dd/rm 100 轮 + soak_board -m 30 墙钟 30 分钟，见下）
logs:              build/testlogs/issue46-<image>.log、build/testlogs/t4-all4.log、
                   build/testlogs/t4-30min-all4.log、build/testlogs/soakboard-<image>.log、
                   build/soak-<image>.log、build/soak-rv32-current.log（本地，不入库）
```

## #46：四个镜像的 CDC 长输出全部完整

#46 的机制是"单核上没有上下文抽 USB TX 环 → `putc()` 在 128+64 B 之后丢字节"，所以
必须在 CDC 上验，UART0 不能代替。四个镜像各自重刷后从 COM27（DTR=True）读：

| 镜像 | `ls /bin` | `wc` | `crash` | 心跳 6 项 | `/proc/ktask` |
|---|---|---|---|---|---|
| `rpi-pico2` | 47 names，回 prompt | `5 47 328` | HardFault 现场 + 回 prompt | 6/6 | `4096 248/248 usbpump` |
| `rpi-pico2-smp` | 47 names，回 prompt | `5 47 328` | 同上 | 6/6 | `4096 208/208 usbpump` |
| `rpi-pico2-rv32` | 47 names，回 prompt | `5 47 328` | `illegal instruction` + 回 prompt | 6/6 | `4096 652/652 usbpump` |
| `rpi-pico2-rv32-smp` | 47 names，回 prompt | `5 47 328` | 同上 | 6/6 | `4096 652/652 usbpump` |

328 B 超过原先的 ~192 B 截断点，且每个镜像的 `ls /bin` 都逐字到达并回到提示符。
ARM 的 fault 报告用 `HardFault/MemManage` 字样，Hazard3 用 `illegal instruction`——
脚本最初的判据只认前者，误报过 RV32 的 fault 项，已修正后重跑四个镜像。

`flash_heartbeat.py` 硬编码 `dtr=False`，且 CDC 在 BOOTSEL 期间消失，不能用于 CDC
镜像（加 `--dtr` 也不够：刷机前就要开端口）。因此用 `build/issue46_full.py` 在已运行
的板子上按同一六项判据验收。

## §22：四个镜像的线程矩阵

| 镜像 | `kthreadtest` | `threadbench` | 线程栈溢出用例 |
|---|---|---|---|
| `rpi-pico2` | `all ok (0 failure(s))` | 104% | ok |
| `rpi-pico2-smp` | `all ok (0 failure(s))` | 61% | ok |
| `rpi-pico2-rv32` | `FAILED (1 failure(s))` | 100% | ok |
| `rpi-pico2-rv32-smp` | `FAILED (1 failure(s))` | 54% | ok |

RV32 的那一条是 `the holder died on its fault`：用例要求线程在访存 fault 里死掉，
但 RP2350 bootrom 把 PMP 8..10 硬编码，地址 0 在 U 态不 fault——已知平台事实
（ATK RV32 同样），不是这次的回归。四个镜像的线程栈溢出用例都通过：
`thread stack overflow ... killing`、`killed only the thread`、`leader survived`。

## T4：逐镜像压力（TESTING.md §6 的新要求）

`772dd29` 把 T4 改成按镜像算——ARM/RV32、UP/SMP 不能互相顶替，`dd` 落的也是各自板上的
flash；`d6b7217` 又把 soak 从 20 轮改成 `-m 30` 墙钟，让六块板受同样的压力。四个镜像逐个
刷机后跑 `dd_stress.py --rounds 100` 与 `soak_board.py -m 30`：

| 镜像 | dd/rm 100 轮 | soak_board 30 分钟 | cycles |
|---|---|---|---|
| `rpi-pico2` | DD OK 100/100 | PASS（30 min, 10 commands） | 237 |
| `rpi-pico2-smp` | DD OK 100/100 | PASS（30 min, 10 commands） | 229 |
| `rpi-pico2-rv32` | DD OK 100/100 | PASS（30 min, 10 commands） | 244 |
| `rpi-pico2-rv32-smp` | DD OK 100/100 | PASS（30 min, 10 commands） | 241 |

每轮 `deep 3` 都有 `stack grown` + `journal intact`，`piperace` 都是 `ok`；`free` 前后
除峰值水位外一致，全程无 `KERNEL PANIC` / `System halted`。

两次修订之间跑过 `-n 20` 的版本（四个镜像也都 PASS），但那不满足 `d6b7217` 的墙钟判据，
所以上表是重跑的 `-m 30` 结果。第一次跑 `-n 20` 时把判定行 `tail -5` 截掉了，也重跑保存了
完整输出。

## §24：本机复核，不复现

远端 `70e8ea1` 说在同一份 `syscall.c` 上不复现，本机确认：

```text
tos> wheap
wheap: 12 KiB declared, 12 KiB taken, pattern intact
tos>
python scripts/soak.py --cycles 3 --targets rv32
rv32     PASS  tasks      20054016     19584  (3 cycles)
1/1 targets passed
```

之前的 A/B 记录在本机也稳定过，但那次 soak 耗时 **8:48**，本轮串行跑只用 **1:27**。
根因是时序而不是代码：被测镜像是 `opt -O2 (kernel) / -Oz (size-opt programs)`，
QEMU TCG 解释执行 -Oz 的用户程序本来就慢，而 `soak.py` 每命令只给 15 s
（`--commandwait-ms` 默认 15000）；当时又并行跑着板测的 dd/heartbeat/reboot，`wheap`
越过窗口后，后面 27 条命令被串进同一段输出，整轮因此判失败。这是**测试超时假象**，
不是内核 bug，也不是 `a004448` 的回归。当前树上不复现，§24 按"未复现"结案。

以后在负载高的机器上跑 `soak.py`，先加大 `--commandwait-ms`（例如 60000）再下结论；
单看 `SMOKE TEST OK` 也不够——那次失败里它也出现了，实际有 27 条命令没回提示符。
