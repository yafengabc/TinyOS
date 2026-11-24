# SHPR1 的 SecureFault 字节，以及 `arm_default_handler` 报错向量号（2026-11-24）

```text
TinyOS commit:     本提交（`05a0e18` 的复核补丁）
target/config:     atk-dnr2350am / -smp、rpi-pico2 / -smp、rpi-pico / -smp、
                   arm（Cortex-M3，QEMU）构建；18 目标；arm QEMU smoke + soak
board/port:        ATK / Pico 2 / RP2040 均 NOT RUN（本机无板）
toolchain:         go 1.27.1 windows/amd64；clang 23.1.4
T0:                PASS
T1:                PASS — 18/18
T2:                PASS — arm smoke；soak rv32+arm 2/2
T3:                部分 PASS - Pico 2 ARM 两块镜像均读到
                   shpr1=0x40404040 (`d7a3c8a`); RP2040 accept.py
                   与 ATK hammer 仍 NOT RUN
T4:                NOT RUN
```

## 背景

`05a0e18` 把 SHPR1 的 MemManage 字节补上（`0x00404040`），修掉了 `run prot heap`
→ MemManage → `sched_idle()` 里 tick 永不到达、看门狗复位的问题。复核时顺带核对
ARMv8-M 的位域，发现同一类还有两处没堵上。

## 问题一：`[31:24]` 在 M33 上不是“保留”

`05a0e18` 的提交信息写的是“drop the write into SHPR1's reserved byte”。在
**ARMv7-M**（QEMU 的 Cortex-M3）那一字节确实是保留；在 **ARMv8-M**
（RP2350 的 Cortex-M33）它是 **SecureFault** 的优先级。

SecureFault 的向量是 `arm_default_handler` → `arm_fault` → `arm_handle_fault()`，
于是用户态触发的安全违例走的是**用户帧**那条分支，和 MemManage 完全一样：
它最后落到 `process_exit()` → `sched_switch()` → `sched_idle()`。SecureFault 停在
复位值 `0x00`，高于 SysTick（RP2350 是 `0x20`），于是同样的饥饿——只是触发条件
比 MemManage 窄得多（安全状态下的 `SG`/非法安全切换之类）。

修法：`TOS_ARM_MPU_V8`（= M33 那颗 MPU 的 PMC 模型宏）存在时写
`ARM_PRIO_FAULT << 24`，v7-M 保持写 0：

```c
#ifdef TOS_ARM_MPU_V8
#define SHPR1_SECUREFault (ARM_PRIO_FAULT << 24)
#else
#define SHPR1_SECUREFault 0u
#endif
```

## 问题二：`arm_default_handler` 不看 IPSR，总是报“vec 0”

未使用的 NVIC 槽、NMI、DebugMon、SecureFault 都进 `arm_default_handler`，原来它
写死 `movs r1, #0`，于是 `arm_handle_fault()` 拿到 `vec = 0`：`vec_name()` 打印
“exception”，`fault_addr()` 也不会按 MemManage/BusFault 去取地址。诊断正好把
最需要信息的那条路径变成最没信息的一条。

改成 `mrs r1, ipsr`（入口处读到的就是当前异常编号，v6-M 也有这个寄存器），并给
`vec_name()` 补 `case 7: "SecureFault"`。其它 fault 桩本来就各自写死编号，不受影响。

## 验证

- T0 PASS（`go test -count=1 ./...`）、T1 PASS（18/18）、T2 PASS
  （`tostest smoke -target arm`；`soak.py --cycles 2 --targets rv32,arm` 2/2）。
- 6 个 RP2 目标 + `arm`（Cortex-M3）单独构建 PASS——两个 `#ifdef` 分支都走了一遍。
- **T3 部分**：Pico 2 ARM 已确认 —— 两块镜像的转储都是 `shpr1=0x40404040`
  （2026-11-25，`d7a3c8a`，见
  [`2026-11-24-pico2-matrix-05a0e18.md`](2026-11-24-pico2-matrix-05a0e18.md) 注 5）。
  RP2040 的 `accept.py`（IPSR 读取在 M0+ 上同样生效）与 ATK 的 hammer 仍是
  `NOT RUN`，清单在 [`docs/OPEN-ITEMS.md`](../OPEN-ITEMS.md) 的 5d 节。

## 硬件旁证（Pico 2，父提交）

同一天 Pico 2 的四镜像回归
（[`2026-11-24-pico2-matrix-05a0e18.md`](2026-11-24-pico2-matrix-05a0e18.md)）跑的是
`05a0e18`，真机 M33 上读到的就是：

```text
shpr1=0x00404040 shpr2=0x40000000 shpr3=0x20e00000
```

`[31:24] = 0x00` —— 也就是本提交要改的那一个字节：SecureFault 停在复位值、高于
SysTick（`0x20`），和 MemManage 修复前是同一个形状。SecureFault 本身在板上不容易触发，
所以这条只需核对寄存器值，不需要专门构造安全违例。

**复测（2026-11-25，`d7a3c8a`）**：刷含本提交的镜像后，`rpi-pico2` 与
`rpi-pico2-smp` 的转储都变成 `shpr1=0x40404040`，MemManage 仍在 `bits[7:0]`；
五例 `prot`、heartbeat 6/6、smptest 11/11、dd 50/50、smpbench 2.05x 全部通过。

`arm_default_handler` 改成读 IPSR 之后 `start.S` 也变了，RP2040（M0+）共用这个文件，
并且 `arch/arm/platform.c` 里 `#ifdef` 之外的部分也一起变，所以 RP2040 的
`scripts/rp2040/accept.py` 仍在待跑清单里。

## 附：为什么第 7 号向量要单独命名

`vec_name()` 的默认分支返回 “exception”，而 `fault_addr()` 用 `f->vec` 判断
地址寄存器该取 `MMFAR` 还是 `BFAR`。之前 `arm_default_handler` 报 0，等于把
“哪个向量进来的”这个唯一线索抹掉了——`05a0e18` 那次的现场靠的是临时插桩
（`[SELF]` 打印 VECTACTIVE），本来这一行就该是现成的。
