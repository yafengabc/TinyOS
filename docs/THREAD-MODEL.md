# 线程调度实验（分支 `exp/thread-sched`）

> 状态：**实验分支**，不进 main。S0–S2 已完成（测量、资源接缝、资源块）。
> 目标不是"把 ktask 的栈改小"，而是**调度单位换成线程、进程只管资源**。

## 0. 为什么开这个分支

现在的 `proc_t` 是一张表同时装两种东西：

- **资源（进程级）**：`win_base/win_size`、`wx_start`、`heap_base/heap_end`、`brk`、
  `cwd[64]`、`fds[12]`、`entry`；
- **调度（线程级）**：`saved`（`regs_t`）、`user_esp`、`stack_top/stack_size`、`state`、
  `prio`、`cpu`、`cpu_ms/ticks`、`start_ns`、`wpipe/wdir/wbuf/wlen`、`stop/kill_pending`、
  `kframe`。

`TOS_KTHREADS` 已经用 `leader` 指针 + `PROC_GROUP()` 表达"线程属于某个组"，但只做到一半：

| 访问 | 今天的行为 | 问题 |
|---|---|---|
| `fds[]` | `PFD(p) = PROC_RES(p)->fds`（S2） | 正确：一个组一份 |
| `cwd[]` | `PROC_RES(p)->cwd`（S1/S2） | 正确：`chdir` 移动整个组 |
| `win_*` / `heap_*` / `brk` | 直接访问，读时经 `PROC_OF` | 线程槽里仍有拷贝，但没有读者 |
| `proc_t` 大小 | **360 B/槽**（C3，S2；`TOS_KTHREADS` 384 B） | 旧的 712 B 里 352 B 是每槽一份的 `fds`+`cwd` |

## 1. 已量到的数字

C3（`esp32c3-generic`，`max_procs = 8`，rv32，`-Oz`，`TOS_KEEP_SYMS=1` 后读符号）：

- S2 前：`procs` = 5,696 B → **712 B/槽**；其中 `fds[12]` = 288 B、`cwd[64]` = 64 B
  （`file_t` 在 RV32 上 24 B），合计 352 B 是每槽一份、但只有进程能用。
- S2 后：`procs` = 2,880 B → **360 B/槽**，`proc_res` = 2,816 B。两者合计 5,696 B，
  **总量不变**；`TOS_KTHREADS` 的组字段再加 24 B/槽，即 384 B。
- 线程真正的成本是栈：`thread_create(fn, arg, stack_bytes)` 从程序堆取，1 KiB 起步。
- 内核线程池：`ktask_stacks[6][6144]` = **36 KiB**，仅 RV32。它的真实调用者是 WiFi OSI
  （`drivers/wifi/esp32c3`，不以 `TOS_KTHREADS` 为前提），所以普通 RV32 镜像应当把它
  整块丢掉。S0 的 `/proc/ktask` 一度把它拉进所有 RV32 镜像（C3 默认 `.bss`
  122,496 → 159,584 B），S2 用 `TOS_KTHREADS || TOS_WIFI` 收回了这个引用。
- `process_ktask_create(name, fn, arg, stack_bytes, prio)` 的 `stack_bytes` **只被记录**，
  栈顶永远是槽顶 → 请求 2 KiB 仍占 6 KiB 槽。S0 的 paint/高水位就是为 S4 量这个数。

## 2. 阶段

| 阶段 | 内容 | 行为变化 | 验证 |
|---|---|---|---|
| **S0** ✅ | ktask 栈 paint + 高水位；`/proc/ktask`（槽、已用、高水位）；`KTASK_STACK_BYTES` 可覆盖 | 无 | T0/T1；QEMU `cat /proc/ktask` |
| **S1** ✅ | **资源接缝**：`PROC_OF(p)` 定义 + `cwd` 改走它；`ps` 的 `wink`、栈增长消息改读组的字段 | `chdir` 变成组级（修 bug） | T0/T1/T2；两个 kthreads 配置构建 |
| **S2** ✅ | **资源块**：`fds`+`cwd` 迁到 `proc_res[]`（按组长槽索引）；`PROC_RES(p)`；领槽时清块；线程不再拷贝 cwd | 无（cwd 语义 S1 已修） | T0/T1/T2；`kthreadtest` 的 fd 共享项；`free`/`ps` |
| **S3** | **按线程调度**：run queue/优先级/quantum 以线程为单位；切组时才重编程保护（`plat_board_task_protect(PROC_OF(next))`）；`ps` 两级显示 | 语义明确化 | 全 T3 矩阵（含 `*-kthreads`） |
| **S4** | **第一个真实客户：USB 泵做成 ktask**（先在 RV32 上，ktask 机制只在那里） | 去掉 PendSV/短写协议 | `cat 21 KB`、`ls /bin`、`crash`、`heartbeat`、`smpbench`、tick 精度 |
| **S5** | 可选：两个表（EPROC/ETHREAD）+ `fork` 语义 | 新功能 | 需新测试 |

S4 的三条硬要求：泵线程优先级高于用户任务（否则 #46 回归）；固定 core 0；栈深用 S0
的高水位实测（`tud_task` + `kprintf` 叠起来可能是 1.5–3 KiB，先试 4 KiB）。

## 2b. S1 实际做了什么（2026-11-25）

- `kernel/process.c` 新增 `PROC_OF(p)`（今天就是 `PROC_GROUP`），并写明：窗口、堆与
  `brk`、`entry`、cwd、fd 表都归组，调度字段（栈、`saved`、state、cpu、prio、等待状态）
  留在槽上。
- `process_cwd_str()` / `process_set_cwd_str()` 改走 `PROC_OF(current)`：**线程里的
  `chdir` 现在移动整个组**（此前只改调用者那份拷贝）。
- `ps` 的 `wink` 列、`process_stack_grow()` 的消息改读组的字段（线程显示它所处组的
  窗口/堆）。
- `thread_create()` 仍然把 `heap_base`/`heap_end`/`brk`/`cwd` 拷贝进线程槽，但**已经
  没有读者经过这些拷贝**；S2 把它们换成指向组资源块的指针并删掉拷贝。

**没在 S1 改的**：`process_stack_guard()`/`process_stack_grow()` 仍以
`p->win_size == 0` 判"这个槽有没有窗口"，所以线程的栈目前**不受软件栈守卫**——线程
栈在组的窗口里，S3 让调度按线程走时要把这条一起改成"用组的窗口 + 线程自己的栈范围"。

验证：T0 `go test -count=1 ./...` PASS；`rv32` / `esp32c3-generic` / `rpi-pico` /
`arm` / `rpi-pico2-smp` 构建 PASS；`configs/arm-kthreads.conf`、
`configs/c3-kthreads.conf` 构建 PASS；QEMU rv32 smoke PASS；soak rv32+arm 2/2。

## 2c. S2 实际做了什么（2026-11-25）

- `kernel/process.c` 新增 `proc_res_t { file_t fds[MAX_FDS]; char cwd[TOS_PATH_MAX]; }`
  和一张与 `procs[]` 并排的 `proc_res[MAX_PROCS]`。`PROC_RES(p)` 用
  `PROC_GROUP(p)` 在 `procs[]` 里的下标取块：没有 kmalloc、没有失败路径，线程也不用
  再存一个指针。
- `proc_t` 删掉 `fds` 和 `cwd`；`PFD(p)` 改为 `PROC_RES(p)->fds`，cwd 的两个访问器
  和 exec 的继承都走同一个块。`proc_alloc()` 领到槽时（`ST_FREE`，没有组长，
  所以没有线程可能指向它）顺带清块，`process_init()` 清全表。
- `thread_create()` 删掉 cwd 拷贝；`heap_base/heap_end/brk` 的拷贝还在，仍无读者，
  留给 S3（软件栈守卫要按"组的窗口 + 线程自己的栈范围"重写时一起处理）。
- 顺手修 S0 的链接问题：`/proc/ktask` 引用 `process_ktask_info()`，把 36 KiB 的
  `ktask_stacks` 拉进了所有 RV32 镜像。现在该节点只在 `TOS_KTHREADS` 或 `TOS_WIFI`
  下编译（WiFi OSI 才是 ktask 的真实用户），其余镜像恢复 `--gc-sections` 的丢弃。

验证：T0 PASS；`tos build all` 17/18（`x86` 见 `docs/OPEN-ITEMS.md` §17，pre-existing）；
rv32/arm QEMU smoke PASS；`pwd`/`cd`/`ls` 会话 PASS；`configs/arm-kthreads.conf` 的
`threadbench` + `kthreadtest` PASS（14 槽耗尽后重新创建、leader 打开的 fd 由线程读取、
leader 关闭后线程看到关闭）；soak rv32+arm 2/2。

## 3. 不进 main 的理由

- S2 保持"**槽死即窗口释放**"不变：资源块和槽同生命周期，按槽索引，领槽即清。
  S3 才真正动这条不变量（线程槽自己死了不能释放组的窗口），SMP 上会引入"最后一个
  线程死才释放"的竞态面。
- 现有镜像里只有 `*-kthreads.conf` 用到线程，收益是**结构性**的；等 S4 的泵线程证明
  值得，再考虑回 main。

## 4. 怎么用这个分支

```sh
git checkout exp/thread-sched
# 量 ktask 栈高水位（需要有人创建 ktask；S4 之后是泵线程）：
# /proc/ktask 只在能创建 ktask 的镜像里存在（TOS_KTHREADS 或 TOS_WIFI）：
go run ./cmd/tos build -config configs/arm-kthreads.conf && \
    go run ./cmd/tostest smoke -target arm -commands "threadbench,cat /proc/ktask"
# 想试更小的池：在 kernel/process.c 里 -DKTASK_STACK_BYTES=2048（或改默认值）
```

`/proc/ktask` 现在会打印：

```
ktask: slot   size   used     hw  name
  no kernel threads
```

（kthreads 镜像里没有创建者时的正常输出；WiFi 或 S4 之后每个槽一行。普通镜像
没有这个节点，因为它的编译会留住 36 KiB 的池。）
