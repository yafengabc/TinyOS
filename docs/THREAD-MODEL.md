# 线程调度实验（分支 `exp/thread-sched`）

> 状态：**实验分支**，不进 main。S0–S2、S3a、S3b 已完成（测量、资源接缝、资源块、
> 按组重编程保护、线程栈边界）；S4 是 USB 泵线程。
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
| **S3a** ✅ | **保护按组重编程**：`cpu_t.prot` 缓存窗口的 6 个输入，相同就跳过 `plat_task_protect()`；`schedstat.prot` 计量。调度本来就是按槽（每个线程一个槽、自己的 prio/quantum），这次只是把"切谁才要重编程"写清楚 | 无 | T0/T1/T2；kthreads QEMU 的兄弟切换 0 次重编程 |
| **S3b** ✅ | **线程栈边界**：`SYS_THREAD_CREATE` 的第三参数改为 `struct tos_thread_stack *`（top+size）；线程槽记录自己的栈范围，刷 0xCC、写 canary；溢出只杀线程；`ps` 的 `stk/sz` 分母对线程生效 | 线程栈有了边界与统计 | T0/T1/T2；kthreadtest 的 overflow 用例 |
| **S3c** | `ps` 两级显示 | 显示 | 暂缓：列格式被 `cmd/tostest/emutest.go` 与 `scripts/rp2350/accept33.py` 解析，且 `ppid` 已经指向组长 |
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

## 2d. S3a 实际做了什么（2026-11-25）

- 先确认：调度单位**本来就是线程**——每个线程是一个 `proc_t` 槽，有自己的 `saved`、
  `state`、`prio`、`cpu`、`quantum`，`sched_pick()` 按优先级和 `rr` 在槽之间轮转。
  S3 原来写的"run queue/优先级/quantum 以线程为单位"不需要改结构。
- 缺的是"切组才重编程保护"：`sched_switch()` 原来每次切换都调
  `apply_protection(next)`，同一进程的两个线程之间也重写 MPU/PMP。现在 `cpu_t.prot`
  保存**窗口的 6 个输入**（`win_base/win_size/wx_start/brk/stack_top/stack_size`），
  与当前组的这 6 个值相同就跳过。
  - 用输入本身而不是"组指针 + 代数"：槽被回收再分配时，只要窗口形状真的相同，
    跳过就是正确的；不同则一定重写。也顺带覆盖了 `sbrk`/栈增长后另一端 CPU 的
    缓存失效——下一次切换看到的 `brk` 已经不同。
  - 每 CPU 一份：MPU/PMP/页表都是每 CPU 的，两个线程可以同时在两个核上跑。
- `struct schedstat` 加 `prot`（实际编程次数），`threadbench` 每阶段打印 `prot=`。
  `kthreadtest` 新增"两个线程互相 `yield()`"用例：**488 次兄弟切换、0 次重编程**
  （rv32 PMP）和 **403 次切换、0 次重编程**（arm MPU）。
- `ps` 两级显示**暂缓**：列格式被 `cmd/tostest/emutest.go` 和
  `scripts/rp2350/accept33.py` 解析，而 `ppid` 列已经指向组长，线程名 `thrN` 也能认；
  改格式的收益小于改测试面的成本。放进 S3c。

验证：T0 PASS；`tos build all` 17/18（x86 §17）；rv32/arm QEMU smoke PASS；
`configs/arm-kthreads.conf` 与临时 rv32 kthreads 配置上 `kthreadtest` PASS、兄弟切换
0 次重编程；soak rv32+arm 2/2。rv32 QEMU 上 `threadbench` 挂起是 **pre-existing**
（2ec7d57 的 worktree 同样挂），记在 `docs/OPEN-ITEMS.md` §18。

## 2e. S3b 实际做了什么（2026-11-25）

- `SYS_THREAD_CREATE` 的第三个参数从"栈顶"改成 `struct tos_thread_stack *`
  （`include/tos_abi.h`：`top` + `size`）。理由：ARM 的 SVC 帧只带 r0–r3、x86 只带
  eax/ebx/ecx/edx，加第四个参数要改三个 arch 的入口；一个在系统调用中拷贝的指针
  不改变陷阱约定，也能按普通用户指针校验。
- `process_thread_create(entry, arg, top, size)` 校验：`top` 16 字节对齐、`size`
  非 0 且 4 字节对齐、`[top-size-4, top)` 整个落在组窗口内。然后：
  `p->stack_size = size`；把 `[top-size, top)` 刷成 `0xCC`（`ps` 的高水位扫描）；
  在 `top-size-4` 写 `USER_STACK_GUARD`（调用方为此保留 4 字节，`kthread_create()`
  实际留了 32 字节）。
- `process_stack_guard()`：窗口判断改成 `PROC_GROUP(p)->win_size`；**线程的 canary
  在所有端口都检查**——MPU/PMP 无法把线程栈和它周围的堆分开，canary 是唯一能发现
  越界的东西；进程栈仍由板子的 `plat_user_stack_guard()` 决定。
- `process_stack_grow()`：线程直接返回 0（栈下面是堆里的另一块分配），溢出即杀线程，
  消息为 `thread stack overflow ... a thread stack cannot grow in place; killing`。
- 两个边界细节：新线程的 sp 初始值正好等于 `stack_top`，第一次系统调用时原来的
  `sp < top` 会把它误判成溢出，改成 `sp <= top`；`stack_used()` 的窗口检查也必须用组
  （线程自己的 `win_size` 是 0，否则 `ps` 的分子恒为 0）。

验证：T0 PASS；`tos build all` 17/18（x86 §17）；rv32/arm QEMU smoke PASS；
soak rv32+arm 2/2；`kthreadtest` 在 ARM MPU 与 RV32 PMP 上新增：
`create an overflowing thread` → 只杀该线程、`join` 得到 139、组长继续；兄弟切换
仍是 403/444 次、0 次重编程。

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
