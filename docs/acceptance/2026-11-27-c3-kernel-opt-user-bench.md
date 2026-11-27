# C3 实验：内核 `-Oz` 会不会拖慢用户计算（2026-11-27）

```text
commit:            3e0f66e（+ 本机实验，无代码改动）
question:          RP2350 M33 SMP 上 -Oz 的 smpbench 1.75，是“内核按 tick 偷 CPU”，
                   还是 SMP/板级特有？
target/config:     esp32c3-generic（RV32，单核 160 MHz）
board/port/image:  ESP32-C3，COM4，flash.py --full 每次重刷
toolchain:         go 1.27.3；clang/ld.lld 23.1.4
T0/T1/T2/T3/T4:    NOT RUN（只做对照实验；实验后已刷回默认镜像并 accept 42/42）
logs:              build/t-c3exp-*.log、build/t-c3flash-*.log（不入库）
```

## 方法

用户程序固定 `TOS_USER_OPT=-O2`（含 coremark），只换内核的 `TOS_OPT`：

- A：内核 `-O2`，默认镜像（trace 开）
- B：内核 `-Oz`，默认镜像
- C：内核 `-Oz`，`boot_trace = 0`（`TOS_DIAG` 关）

每个镜像 `flash.py --full` 后跑 `coremark 1000` 三次，取 `[tos] CoreMark 1.0 : N
iterations/sec`。

## 结果

| 变体 | run 1 | run 2 | run 3 |
|---|---|---|---|
| A: 内核 `-O2` | 346 | 346 | 346 |
| B: 内核 `-Oz` | 343 | 344 | 343 |
| C: 内核 `-Oz` + trace 关 | 344 | 344 | 344 |

## 结论

- **内核 `-Oz` 对纯用户计算的影响 <1%**（346 → 343–344），tick/diag 不是问题：
  trace 关掉也不变（344）。
- 所以 RP2350 M33 SMP 上 37 → 57 ms 的差距**不是“内核每 tick 偷一块 CPU”的普遍
  现象**。C3 是单核，复现不了 SMP；剩下的解释只能在 SMP 侧（两个核每毫秒都进
  `process_tick`，-Oz 下 `sched_switch` 多出的调用/`__aeabi_memcpy4`/`proc_set_state`
  拉长临界区）或 RP2350 的窗口布局/cache。
- 与 clang 的静态结论一致：`sched_switch` 在 -Oz 下 1,776 → 624 B，丢失
  `sched_pick`/`proc_set_state`/帧拷贝的内联，但单核上这点开销测不出来。

## 对 RP2350 的下一步（如果要继续）

1. `release.conf`（`TOS_DIAG=0`）+ 内核 `-Oz` + `user_opt=-O2` 的 smpbench：排除
   tick 诊断路径；
2. 若仍是 1.75：把内核 `-Oz` 与 `-O2` 的用户窗口基址对齐（pad `__kernel_end`）再测，
   区分 SMP 临界区与 bank/cache 布局；
3. 两者都不成立才需要看调度器热路径——不值得为此改逻辑。

## 实验后状态

已重新构建并刷入默认 `esp32c3-generic` 镜像，`scripts/esp-c3/accept.py` **42/42**。
