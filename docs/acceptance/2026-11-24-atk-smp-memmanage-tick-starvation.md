# ATK-DNR2350AM ARM SMP `[STALL]`：MemManage 优先级压在 SysTick 之上

```text
commit: 工作区（基于 b0a4e04，未提交）
change: 定位并修复 atk-dnr2350am-smp 在 prot heap + deep 3 负载下的
        [STALL] / 看门狗复位；arm_exceptions_init() 的 SHPR1 未写 MemManage 字节
baseline: none
target/config: atk-dnr2350am、atk-dnr2350am-smp、atk-dnr2350am-rv32、
               atk-dnr2350am-rv32-smp / defaults
board/port/image: ATK-DNR2350AM (RP2350) / UART COM14 (CH343)、USB CDC COM26
                  / build/tinyos-atk-*.uf2
toolchain: go 1.27.1 windows/amd64, clang 23.1.4, python 3.14.9
host: Windows（Asia/Shanghai）
T0: NOT RUN（本轮未跑 go test/go vet）
T1: PASS（ATK 四 target 全部 build ok）
T2: NOT RUN（无 QEMU）
T3: PASS（SMP：hammer 20/20 无 STALL；单核：hammer 10/10 无 STALL）
T4: NOT RUN（本轮只做 hammer 压测，未跑 dd/smpbench/reboot soak）
logs: $env:TEMP\hammer_clean_raw.txt、hammer_single_raw.txt（未入库）
```

## 复现

`atk-dnr2350am-smp` 上连续跑 `prot heap`（等 4 s）+ `deep 3`（等 8 s）：
1200 ms 没有新 tick 时板级诊断打印 `[STALL]`，随后看门狗复位。修复前 20 轮里
4 轮命中；把 `6fefed7` 的 `usb_rxq_fill` 顺序回退做对照，签名一致，排除本次拉取。

`[STALL]` / `[STALL2]` 的关键读数：死核 CPU0 的 `seq0` +446k、`i0` +22k，
`tc0` 只 +1；空闲快照恒为 `q0=4`（MemManage）、`q1=11`（SVCall）。

临时自采样（受害核自己打印 per-core 寄存器）：

```text
[SELF] c=0 ip=4 pm=0 ic=0440f804 sh1=40404000 sh3=20e00000
```

`ic` 的 PENDSTSET=1、VECTPENDING=15 表示 SysTick 一直 pending；`pm=0` 排除
PRIMASK；`sh1` 的 MemManage 字节是 0x00，高于 SysTick 的 0x20，所以它永远
抢不进来。空闲循环跑在 `run prot heap` 的 MemManage 处理器里，喂狗又只发生在
boot 核的 tick 中，于是看门狗复位。

## 修复

1. `arch/arm/platform.c`：`arm_exceptions_init()` 的 SHPR1 写入补上 MemManage
   字节（`bits[7:0]`），结果 `0x00404040`；同时把原 `<<24`（保留字节）去掉。
2. `arch/arm/scs.h`：更正写反的 SHPR1 位域注释（`[23:16]` UsageFault、
   `[15:8]` BusFault、`[7:0]` MemManage）。

## 结果

| 项 | atk-dnr2350am | atk-dnr2350am-smp |
|---|---|---|
| 构建 | build ok | build ok |
| hammer 20 轮 | — | 20/20 无 STALL、无复位 |
| hammer 10 轮 | 10/10 无 STALL、无复位 | — |

每一轮都是 `heap_fault=True deep_ok=True stall=False`；`banner=False` 表示
中途没有重启（正常状态）。RV32 两镜像本轮未上板，只验证了构建。

## 注

1. 本轮插桩（`[SELF]`、`[STALL2]`、`g_proc_lock_owner`、`LKOWN/LKREL` 等）已
   全部清理，工作区只剩上面两处修复。
2. 修复只改异常优先级配置，不触碰调度器、USB 或 SIO 门。
