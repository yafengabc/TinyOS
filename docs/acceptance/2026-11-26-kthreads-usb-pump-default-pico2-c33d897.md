# 四镜像默认开启 kthreads + usb_pump（2026-11-26）

```text
commit:            c33d897
change:            六个 RP2350 board.conf 的默认 features 加 kthreads + usb_pump；
                   board/rp2350rv-generic/platform.c 同步 M33 的严格模式（09da1cd）
baseline:          09da1cd
target/config:     rpi-pico2 / rpi-pico2-smp / rpi-pico2-rv32 / rpi-pico2-rv32-smp，
                   全部用默认配置（不带 -config；build/ 下的临时 conf 已删）
board/port/image:  Pico 2（RP2350）COM19 = CH343 UART0，BOOTSEL 期间不消失
toolchain:         go 1.27.3；clang 23.1.4；Python 3.14.9
host:              Windows / 本机
T0:                FAIL（仅 TestPinnedToolchainSources，worktree 与 ../tinyos-script
                   pinned 拷贝不同步，pre-existing）
T1:                4/4 构建通过（18 目标全量未跑）
T2:                NOT RUN（板级配置改动，QEMU 不覆盖）
T3:                PASS ×4（见下表）
T4:                PASS ×4（accept33 自带）
logs:              build/acc4-<image>.log、build/check-ktask.log（本地，不入库）
notes:             exp/thread-sched 分支；Pico 2 的 UART0 这次有输出（线的问题已解决），
                   验收改走 UART0，因此 --uf2 路径能抓到完整 banner
```

## 为什么默认打开

内核线程方案（`docs/PROC-THREAD-MODEL.md` 的 E2/E3）此前只在 `-config` 里逐镜像打开，
板测要手写临时 conf，镜像一旦漏写就退回旧模型而没人发现。用户决定**以后默认就是这套**，
所以把 `kthreads` + `usb_pump` 写进板级 `board.conf`：构建命令回到 `-target <board>`。

## 结果

| 镜像 | accept33 | `/proc/ktask` | `uptime`（刷完立即查） | `kthreadtest` 完整套件 |
|---|---|---|---|---|
| `rpi-pico2`（ARM UP） | 19/19 | `0 4096 248 248 usbpump` | 1.745 s | 单项全 ok（总结行未在窗口内） |
| `rpi-pico2-smp`（ARM SMP） | 21/21 | `0 4096 208 208 usbpump` | 1.733 s | `all ok (0 failure(s))` |
| `rpi-pico2-rv32`（RV32 UP） | 20/20 | `0 4096 652 652 usbpump` | 1.824 s | 单项全 ok（总结行未在窗口内） |
| `rpi-pico2-rv32-smp`（RV32 SMP） | 22/22 | `0 4096 652 652 usbpump` | 1.816 s | `FAILED (1 failure(s))`，见下 |

`uptime` 全部 1.7–1.8 s：刷机前取一次、启动后立即取，确认板子是真的重启了，
而不是把上一轮还在跑的 shell 当成了新固件（§5c 那类假通过）。

## 判据用 `/proc/ktask`，不用 banner 行

SMP 两轮的启动日志里**没有** `[usb] pump thread pid N on cpu 0`：启动早期第二核与
`console_write()` 抢输出锁，整块丢行（`boot/cpu1 ready` 之前的行都可能掉）。UP 两轮有。

严格模式的意义是「泵没起来控制台就哑」，但 `[usb] pump thread NOT started` 这一行
同样可能被启动期丢掉，所以**这条打印不能当判据**。能当判据的是运行期状态：
`/proc/ktask` 里有 `usbpump`、`used/hw` 非零，且 `uptime` 与本次刷机对得上。

## RV32 的 1 个 FAIL 是平台事实

```text
kthreadtest: ok   create a thread that dies holding a mutex
kthreadtest: FAIL the holder died on its fault
kthreadtest: ok   a dead holder released the mutex
kthreadtest: FAILED (1 failure(s))
```

用例要求「线程在访存 fault 里死掉」，但 RP2350 bootrom 把 PMP 8..10 硬编码成可用，
地址 0 在 U 态不 fault，前提在 Hazard3 上不成立（ATK RV32 早先也是同一条）。
ARM 同镜像 `all ok (0 failure(s))`。不是本次改动引入。

## 范围与没做的

- 只给 RP2350 的六个 board.conf 开。**RP2040**（Cortex-M0+）的 USB 泵还在 tick 里、
  kthreads 未在该核验过；**ESP32-C3 / QEMU / x86** 保持默认关。要不要一起开待定。
- 负向验证（让 `process_ktask_create` 必然失败，确认控制台真的哑）仍未做：泵一停
  CDC 与 UART0 都没有字符入口，只能物理按 BOOTSEL。
- T1 只构建了 4 个 Pico 2 目标；ATK 的四个目标没有构建也没有上板（同一份源码，
  但引脚和设备表不同）。
- UP 两轮的 `kthreadtest` 只读到单项 ok，40 s 读窗口不够跑完默认套件，总结行没抓到。
