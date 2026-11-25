# 线程调度实验（分支 `exp/thread-sched`）

> 状态：**实验分支**，不进 main。S0–S2、S3a、S3b 已完成；S4 的代码已写
> （USB 泵线程 + CPU 亲和），等 Pico 2 RV32 板测。
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
| **S4** ✅（UP 代码） | **第一个真实客户：USB 泵做成 ktask**：`usb_pump = true` 时 `usb_cdc_poll()` 从 trap/idle 钩子移进一个 prio 0、固定 core 0 的内核线程；新增 `home_cpu` 亲和与 `plat_start_tasks()` 板级钩子 | 控制器的轮询有了自己的线程上下文 | 构建 + T0/T1/T2；**SMP 被 §21 阻塞**；UP 的 T3 见 §19 |
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

## 2f. S4 实际做了什么（2026-11-25，代码完成、板测待跑）

- **CPU 亲和**：`proc_t.home_cpu`（-1 = 任意）。`sched_pick()` 跳过不属于本 CPU 的
  任务，`preempt_if_higher()` 不因别的 CPU 的任务而请求切换。普通任务从不设置；
  `process_ktask_create()` 新增最后一个参数 `cpu`，WiFi 的四个调用点传 -1。
- **板级启动钩子**：`plat_start_tasks()`（`kernel/plat/tasks.h`，弱默认空）在
  `cpu_init()` 之后、`process_start_shell()` 之前调用。`plat_init()` 太早——那时
  进程表还不存在；`plat_board_init()` 在它里面。
- **泵线程**（`board/rp2350rv-generic/platform.c`，`#ifdef TOS_USB_PUMP`）：
  `for (;;) { usb_cdc_poll(); process_ktask_sleep(1); }`，prio 0、`home_cpu = 0`、
  请求 4 KiB 栈（槽仍是 6 KiB，S0 的 `/proc/ktask` 高水位会给出真实数字）。
  创建成功后把 `g_idle_poll` 清空：trap 返回用户态时的 `board_poll_deferred()` 和
  idle 池都不再 poll，控制器只有一个所有者。创建失败则保留旧钩子并打一行。
  启动时打 `[usb] pump thread pid N on cpu 0`。
- **`/proc/ktask`**：节点现在也在 `TOS_USB_PUMP` 下编译，并显示线程的真实名字
  （此前固定打印 `ktask`）；泵线程是 S0 测量脚手架的**第一个真实客户**。
- **构建开关**：feature `usb_pump`（define-only）、`configs/rpi-pico2-rv32-usbpump.conf`。
  默认镜像不含这个线程，现有发布行为不变。ktask 机制只在 RV32，所以没有 ARM 版。

**SMP 阻塞（2026-11-25，ktask selftest 发现）**：`ktask_running`/`ktask_entry_sp`
是全局的，start.S 用它们决定栈，两个核同时跑 ktask + 用户任务会串核 panic
（`kernel stack out of bounds`）。所以泵镜像先只跑单核 `rpi-pico2-rv32`；修法见
`docs/OPEN-ITEMS.md` §21。

**没有在这步做的**：控制台写者仍然走 `plat_console_can_take()` + 短写/重试，
`write()` 还不会阻塞在泵线程上。S4 只是把状态机搬进线程、给它自己的栈和上下文；
下一步才是让写者与泵共享一个队列并真正睡眠等待。短写的回归判据（§5c 的
`cat` 21 KiB 两路逐字节一致）仍然适用。

验证：T0；`tos build all` 17/18（x86 §17）；rv32/arm QEMU smoke；soak 2/2；
`kthreadtest` 在 ARM MPU 与 RV32 PMP 上仍全过（亲和默认 -1 不改变普通任务）。
泵线程本身在 QEMU 上没有 USB 控制器，**T3 由板侧按 §19 跑**。

## 2g. 用户态线程库：join 归还栈 + cwd/churn 用例（2026-11-25）

新用例先失败，再修：

- **泄漏**：`kthread_create()` 从进程堆里 `malloc` 栈（`bytes + 32`）和 start
  record，`kthread_join()` 从来没还过；创建失败（槽耗尽、用户指针校验失败）也直接
  return，两个块一起漏。`kthreadtest` 的 200 次 create/join 用例在 96 KiB 窗口里
  第 2 次左右就 `kthread_create` 失败，随后 yield 用例也建不出线程：
  `heap ... -> ... (stopped early)` + `FAILED (6 failure(s))`。
- **修法**（`user/kthread.c`）：一张 `KT_TRACK 24` 的表记录 `{tid, base, rec}`；
  `join` 返回 0 后 `free(rec)`、`free(base)`（内核在返回前已经退休槽，栈上不会再有人
  跑）；`SYS_THREAD_CREATE` 失败则立刻把两个块还掉。表满只丢 bookkeeping，线程照跑；
  和用户库其余部分一样，这个表不是线程安全的。
- **新用例**：
  - cwd 跨线程（S1 的行为第一次有测试）：leader `chdir("/bin")`，线程 `getcwd`
    看到 `/bin`，线程 `chdir("/")` 之后 leader 也看到 `/`。
  - 200 次 create/join，`sbrk(0)` 前后差 ≤ 8 KiB（无泄漏）。
  - 兄弟切换的计数器改成**每线程一个**：共用一个 `volatile int` 自增是测试自己的
    丢更新竞态，之前只是时序上恰好通过。
- **可重复**：新增 `configs/rv32-kthreads.conf`（QEMU rv32 + `kthreads`），和
  `configs/arm-kthreads.conf` 对称；不再需要手写临时配置。

验证：T0 PASS；`tos build all` 17/18（x86 §17）；rv32/arm QEMU smoke PASS；
soak 2/2；`configs/rv32-kthreads.conf` 与 `configs/arm-kthreads.conf` 上
`kthreadtest` 全过（200 次循环堆不增长、472/405 次兄弟切换 0 次重编程）、
`threadbench` 正常；`configs/rpi-pico2-rv32-usbpump.conf` 仍可构建。

## 2h. SMP：sbrk() 的读改写必须原子（2026-11-25）

- 新用例（`kthreadtest`）：4 个线程各 400 次 `sbrk(16)`，比较最终 brk 与返回偏移
  之和（每次成功调用应返回 0,16,32,... 的一个唯一值）。修之前 rv32smp 上三次运行
  得到 **25,200 / 25,088 / 25,264 字节**（应为 25,600），偏移和也偏小——两个 CPU
  同时进 `process_sbrk()`，读到同一个 `brk`，一块内存被发了两次。
- 修法：读-判-写放进 `group_lock(current)`。这是系统调用上下文，中断已屏蔽，所以
  用和 fd 表一样的普通自旋锁，不是 irqsave 对。`apply_protection()` 放在锁外：它读
  的是组**当前**的 `brk`，兄弟把 break 推得更远只会让本 CPU 的区域变大。
- 用例本身也修了 SMP 上的两个假设：yielder 从 2 个改成 4 个（2 个时两颗核各跑一个，
  切换数会掉到个位数）；“保护重编程 0 次”放宽到 ≤2 次，因为另一颗核可能带着一份
  sbrk 之前的缓存，重编程一次是正确的。
- 验证：rv32、rv32smp、arm 三个 kthreads 配置各跑 10 次 `kthreadtest`，全部
  `all ok`；rv32smp 每次都是 25,600 字节 / 偏移和 1,279,200；soak rv32+arm
  **10 轮** 2/2。

## 2i. ps 的线程行 + 用户堆的边界（2026-11-25）

- `kthreadtest` 现在在 `slow` 线程睡眠时打印一次 `ps`。rv32 与 arm 上都能看到
  leader `0.5/4`、线程 `0.7/4`：线程显示的是自己 768 B 的帧，不是 leader 的栈；
  state `sleep`、ppid=leader、wink 是组的 128 KiB。S3b 的 `stk/sz` 对线程成立。
  测试里用 `slow_frame` 存了一次数组地址：只读写两个元素时编译器把数组拆成了标量，
  ps 会显示 0.0。
- 同时试了 4 线程 × 200 次 `malloc/free`（块上写校验和）：**用户堆不是线程安全的**。
  rv32（UP）10 次里失败 2 次，一次把线程栈写坏后内核按 `thread stack overflow`
  杀线程；rv32smp 10 次里失败 1 次，`store access fault at 0x80100514`。这条用例
  没有进默认测试——失败率不定，不能当回归——记为 `docs/OPEN-ITEMS.md` §20，
  **合回 main 前必须解决**。
- 试过用 `__atomic_exchange` 给 `heap.c` 加锁：M0+（RP2040）和 ESP32-C3 没有 A
  扩展，链接直接报 `undefined symbol: __atomic_exchange_4` / `__atomic_store_4`，
  所以修法要么是内核辅助的 mutex、要么每线程堆，见 §20。本轮没有改 `heap.c`。

## 2j. 用户堆的线程安全（2026-11-25）

- 内核 mutex：`SYS_MUTEX_TRY`(54) / `SYS_MUTEX_UNLOCK`(55)。test-and-set 在系统
  调用里完成：用户态在 M0+ / C3 上没有原子指令，单核上也是 trap 屏蔽中断才排除
  抢占；SMP 用一把 `user_mutex_guard` 自旋锁串行化，解锁在 guard 的 release 屏障
  里写 0。锁字是调用者窗口里的一个 `int`，每个进程各自一份。
- `user/heap.c` 的 `heap_alloc`/`heap_free` 在 `TOS_KTHREADS` 下取锁，等待时
  `yield()`；没有线程的程序不编译这段，代码和以前完全一样。
- 回归：`kthreadtest` 的 4 线程 × 200 次 × 2 块（反向校验和）在 rv32 / rv32smp /
  arm 各 10 次全过；修之前 rv32 10 次失败 2 次、rv32smp 1 次。
- 代价与边界：线程程序每次 malloc/free 两次系统调用。阻塞式 mutex 见 §2k（已完成）。
  `abi/syscalls.csv` 与 goc32 的 snapshot 已同步，goc32 `go test` 与 `hello.c`
  样例编译都通过。

## 2k. 阻塞式 mutex（2026-11-25）

- 新系统调用 `SYS_MUTEX_LOCK`(56)：内核在 `proc_lock` 下检查一个用户字；空闲就置 1
  返回 0，被占就置 `ST_WAIT_MUTEX`、`wmutex = word`，然后走 `block_locked()` 睡下。
  `SYS_MUTEX_UNLOCK` 把所有权**直接交给**第一个等待者（字保持 1，唤醒时把它的返回
  值写成 0），没有“唤醒后再抢”的窗口。没有等待者才清零。
- `SYS_MUTEX_TRY`/`UNLOCK` 保留：TRY 是非阻塞形式（`kmutex_try`），堆锁现在用
  `kmutex_lock`。
- 持有者死亡：每个线程记录一个 `held_mutex`；线程在 `process_thread_exit()` 里
  把字交给等待者（组长正在解散时不交，整组都要走）。这样一次 fault 在 malloc 里
  不会把同进程其他线程永久卡住。
- `ps` 新状态：`mutex`（cpu 0.0，不在运行队列），`proc_waiting()` 把它算作“有人在等”。
- 一个线程同时只跟踪一个持有字，够堆锁用；嵌套/多锁的死亡释放不在覆盖范围，
  写在 `lib.h` 的 API 注释里。

验证：`kthreadtest` 新增两个用例——leader 持锁、线程阻塞（`ps` 显示 `mutex`）、
unlock 后移交；以及线程持锁后故意 fault（store 到 0），内核杀掉它、`join` 得到
139、`mutex_try` 重新拿到字。rv32 / rv32smp / arm 各 10 次全过；并发 malloc/free
用例仍 10/10。

## 2l. ktask selftest（2026-11-25）

- `TOS_KTASK_TEST`（feature `ktask_test`，`configs/rv32-ktasktest.conf`）：在 shell
  之前创建一个 prio 0、CPU 0 的 ktask，睡 5 个 tick、yield 一次，每次醒来记录 CPU，
  然后长睡，把槽留给 `/proc/ktask`。启动打印
  `[ktest] ktask ran 5 sleeps + yield, cpu_bad=0`，`cat /proc/ktask` 显示
  `0 6144 656 656 ktest`（槽、已用、高水位、名字）。
- 这是 S0 的池/paint/高水位、S4 的亲和和 `/proc/ktask` 在 **QEMU 上的第一次真实
  运行**；WiFi 需要 C3，泵需要 USB，都只能上板。
- 同一天在 `rv32smp` 上跑同样的 selftest，暴露了 `ktask_running` 的跨核串扰
  （§21）：两个核一个跑 ktask、一个跑用户任务时，后者进 trap 会走错栈并 panic。
  这就是 selftest 存在的价值：S4 的 SMP 泵镜像在板测前就被拦下了。

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
# /proc/ktask 只在能创建 ktask 的镜像里存在（TOS_KTHREADS / TOS_WIFI /
# TOS_USB_PUMP）：
go run ./cmd/tos build -config configs/arm-kthreads.conf && \
    go run ./cmd/tostest smoke -target arm -commands "threadbench,cat /proc/ktask"
# S4 的泵线程（需要 Pico 2 RV32 硬件）：
go run ./cmd/tos build -config configs/rpi-pico2-rv32-usbpump.conf
# 想试更小的池：在 kernel/process.c 里 -DKTASK_STACK_BYTES=2048（或改默认值）
```

`/proc/ktask` 现在会打印：

```
ktask: slot   size   used     hw  name
  no kernel threads
```

（kthreads 镜像里没有创建者时的正常输出；WiFi 或 S4 之后每个槽一行。普通镜像
没有这个节点，因为它的编译会留住 36 KiB 的池。）
