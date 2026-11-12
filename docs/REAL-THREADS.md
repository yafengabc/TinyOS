# 真多线程评估（实验分支 `threads-preemptive`）

目标：回答一个问题——TinyOS 要不要、以及怎样，把现在的**用户态协作线程**升级成
**内核调度的真线程**（可被 tick 抢占、同一地址空间内多个执行流、阻塞按线程生效）。

本文件是评估与路线，不是已完成的功能。测量工具是 `user/swcost.c`。

## 1. 现在的线程是什么

`user/thread.c`（menuconfig `Kernel → Threads`，`threads = 1`）：

- 纯用户态：一个线程 = 一段栈 + 保存的 `sp`，切换靠 `thread_switch.S` 压/弹
  callee-saved 寄存器；**内核完全不知道线程存在**。
- 上限 4 个（含 main），栈由程序自己的堆（`TOS_HEAP`）提供，默认每次
  `thread_create` 再从堆里切 4 KiB。
- 切换只发生在 `thread_yield()` / `thread_read()`；普通 `read()`、`sleep()` 会把
  **整个进程**挂起。
- `mutex_lock` 是普通标志，不是原子操作——因为同进程线程不会同时跑。
- 一个进程永远只在一个 CPU 上：SMP 目标上两个线程也无法并行。

代价：内核 0 B；每个用到的程序 +约 1 KiB 代码；每线程 4 KiB 栈（+ 堆里的管理结构）。

## 2. "真多线程"要满足的条件

1. **内核可见**：每个线程有独立的寄存器现场和用户栈，能被 1 ms tick 抢占。
2. **共享地址空间**：同进程线程共享 code/data/heap；堆分配互相可见。
3. **阻塞按线程**：一个线程 `read()` 睡着，兄弟线程继续跑。
4. **生命周期按组**：线程退出不结束进程；主线程 / `exit_group` 结束整组。
5. **共享内核对象**：fd 表、cwd 是进程级（线程间 `close`/`dup2` 互相可见）。
6. **可选并行**：SMP 目标上同组两个线程可以同时跑在两个核上（真并行）。

## 3. 两条实现路线

### 路线 A：线程 = 进程表里的一个槽，共享组首（推荐）

复用现成的一切：`proc_t` 已有完整的 `regs_t saved`、`cpu`、`state`、阻塞字段；
`sched_switch()` 已经能把任意任务的现场换进换出；`apply_protection()` 已经按窗口
编程 PMP/MPU。

改动：

| 位置 | 改动 | 规模 |
| --- | --- | --- |
| `proc_t` | `int tgid; proc_t *leader;`（组首为 NULL） | 8 B/槽 |
| `apply_protection()` | 用 `GROUP(p)` 的窗口字段 | 几行 |
| fd 访问 | `p->fds[]` → 组首的 fd 表（约 50 处机械替换） | 中 |
| `sbrk` / brk | 指向组首的 `brk`（堆共享） | 小 |
| `cwd` | 组首的 cwd | 小 |
| `task_release()` | 线程退出只回收槽和栈，不 `win_free`、不重挂子进程 | 小 |
| 新系统调用 | `SYS_THREAD_CREATE(fn, arg, stack_top) / EXIT / JOIN` | 中 |
| `ps` | 显示 `tgid`，同组排在一起 | 小 |
| 信号 | `^C`/SIGKILL 作用于整组 | 中 |

成本：

- **内存**：每线程一个 `proc_t`（实测 856 B）+ 用户栈（用户给，通常 4–8 KiB）。
  比协作线程多 856 B/线程，换来内核调度。
- **切换**：与进程切换同一条路径 = trap 入口 + 保存 `regs_t`（约 60 B）+
  `sched_switch` + trap 出口。同组切换可以跳过保护重编程（窗口没变），比跨进程还便宜。
- **并行**：同组线程可以分到两个核（SMP 构建），这是协作线程做不到的。

风险集中在**语义**，不在机制：`wait`/`exit`/信号/`exec` 的组语义要一条条定清楚。

### 路线 B：内核里独立的轻量 thread 结构

`proc_t` 只表示进程，线程单独一张表（每线程约 150 B：saved regs + sp + state +
组指针）。调度器要维护"进程可运行 / 线程可运行"两级结构，`sched_switch` 要同时
换进程和线程。省下约 700 B/线程，但改动面比 A 大，且现有 `proc_t` 里的阻塞字段
（`wbuf/wpipe/wake_tick/in_buf`）都得搬家。

**结论：先做 A。** 等 A 跑出数据，再决定要不要为省 700 B/线程付 B 的复杂度。

## 4. 成本模型与测量

`user/swcost.c` 测三个基线（`clock_ns()` 前后取差，除以迭代数）：

```
tos> swcost
swcost: getpid  <a> ns/op      # 系统调用往返，不调度
swcost: yield   <b> ns/op      # 完整调度路径（单任务时切到自己）
swcost: thread  <c> ns/op      # 协作线程切换（需 threads = 1）
tos> swcost & swcost           # 两个任务互相 yield：跨任务切换
```

预期关系：`a < c < b`；真线程切换 ≈ `b`（同组时略低），因为走的就是同一条
trap + `sched_switch` 路径。把这四个数填到下表，再和"每线程内存 +856 B"一起看：

| 目标 | getpid | yield（自） | yield（跨任务） | 协作线程 | 备注 |
| --- | --- | --- | --- | --- | --- |
| x86 (QEMU TCG) | 7.5 µs | 23 µs | ~20 µs | 32.5 µs | TCG 放大约一个数量级，只看相对值 |
| rv32 (QEMU TCG) | 512 µs | 796 µs |  | — | TCG 下绝对值无意义；比例 1.55 |
| **esp32c3（真机）** | **3.34 µs** | **12.62 µs** | ~12.6 µs | — | 160 MHz / 80 MHz APB，XIP 内核 |
| rp2040 |  |  |  |  | 未接（本实验只做 RV32） |

QEMU/TCG 的绝对值没有参考意义（`getpid` 7.5 µs 里绝大部分是 TCG 的解释开销），
但比例有意义：**一次 `yield` 约是系统调用往返的 1.5–3 倍**，量级与现有进程切换
相同。真线程不需要新的机制预算；真正的代价在每线程一个 `proc_t`（856 B）和组语义
的复杂度上。

## 4.5 P1 実装（RV32/C3，已跑通）

`TOS_KTHREADS`（`[features] kthreads = true`，RV32-only）：

- **线程 = `proc_t` 槽 + `leader` 指针**。槽本身的 `win_size = 0`（不占窗口），
  `apply_protection()` 沿 `PROC_GROUP()` 用组首的窗口编程 PMP，所以同组线程的
  保护视图完全一致，切换时不需要重编程。
- **栈**：用户库 `user/kthread.c` 用 `sbrk()` 从组堆里切一块，栈顶传给内核；
  因为堆边界是共享的（`process_sbrk()` 改的是组首的 `brk`），所有线程的
  PMP data 区域自动覆盖这些栈。
- **入口**：用户侧 `kthread_trampoline(fn, arg)` 调完用户函数后 `kthread_exit(0)`；
  内核只需要 `plat_prepare_user_frame(entry, sp)` + `a0 = arg`。
- **退出/join**：线程退出时把自己的 code 记在槽里（`ST_THREAD_ZOMBIE`）。有 joiner
  在 `ST_WAIT_THREAD` 等就当场把 code 写进它的帧和用户指针，然后回收槽；没人等就
  保留僵尸槽，等 `join` 来取。重复 join 返回 -1。没人 join 的线程会一直占一个槽
  （POSIX 里未被 wait 的子进程同样如此），组长退出时整组回收。
- **组退出**：组长退出时把同组线程槽一起回收（`exec` 也拒绝在线程里调用）。
- **fd 表共享**：`PFD(p)` 沿 `PROC_GROUP()` 取表，线程没有自己的副本；一个线程
  `open/close/dup2` 的结果对全组可见（POSIX 语义），`kthreadtest` 有专项检查。
- **信号按组**：`process_signal()` 把线程 tid 重定向到组长（调用者自己组除外，
  否则会把正在运行的槽回收掉），所以 `^C`/`kill <pid>` 作用于整个进程。
- **SMP**：已支持（RV32 单核/双核都能构建）。`brk` 和 fd 表用每个组一把
  `spin_t glock` 串行化；单核上 `spin_lock` 编译成空操作，所以单核零成本。
  组长退出是**延迟**的：组长进 `ST_DYING`、记 `live_threads`，兄弟线程被
  `kill_pending`（阻塞中的会被唤醒返回 -1），最后一个线程负责关 fd、回收组长。
  还剩两个已知边界：`sbrk` 只重编程**当前核**的 PMP（所以在 spawn 线程前分配堆，
  `kthread_create` 本身就是这样做的）；`close()` 撞上正在阻塞的 `read/write`
  还没有引用计数保护（单核时代就存在的语义边界）。
- **已知简化**：线程栈不参与爆栈自动翻倍（栈在堆里，没有窗口顶部那段护栏）；
  `ps` 里线程是独立 pid，但 `ppid` 就是组长，能看出分组；线程槽仍然带着一份自己
  用不到的 `fds[12]`（432 B）——进程表是静态数组，拆表只能省表项、省不了 .bss。

QEMU rv32 早期版本（3×300k）：19103 ms vs 21751 ms。C3 真机上先看到的是 171%，
但那是**基准程序的编译器差异**，不是线程开销。把两个相位改成调用同一个 `noinline`
的 `worker()`、循环上界都来自运行时值之后，C3 实测：

```
threadbench: 3 threads x 300000: 35 ms  sw=38 self=1  tick=35
threadbench: 1 thread  x 900000: 34 ms  sw=2  self=34 tick=34
threadbench: inline    x 900000: 35 ms  sw=0  self=35 tick=35
threadbench: ratio 100% (100% = no overhead)
threadbench: sleep() in main, sibling kept running
```

- **线程开销 ≈ 0**：同样的 90 万次迭代，3 个线程 35 ms，单线程 34 ms，caller 里
  顺序执行 35 ms。
- 38 次真切换 × 12.6 µs ≈ 0.48 ms，占 35 ms 的 **1.4%**，与"QUANTUM=1、1 ms
  tick、3 个可运行线程"的预期完全一致。
- 之前的 171% 来自对比对象：旧 inline 循环用编译期常量上界，GCC 展开/优化到
  25 ms；线程里的循环上界是 volatile 全局变量，保持简单循环，约 40 ms。测的是
  编译器，不是内核。

单核上没有加速是预期的；三个线程都完成、`join` 正常回收、主线程 `sleep` 时兄弟
线程继续跑，这三点正是"真线程"与协作线程的分界。

判据（拍板用）：

- 若跨任务 `yield` 在 5 µs 以内，真线程的切换成本可以接受（每秒 20 万次切换）。
- 若每线程 856 B 让 C3/RP2040 的窗口数掉一个，就考虑路线 B 或"线程槽按需分配"。

## 4.6 资源开销（C3，同一 `configs/esp32c3.conf`，heap 80 KiB / 8 槽）

| 构建 | flash（.text+.rodata） | RAM（.data+.bss） |
| --- | ---: | ---: |
| 主线 | 77,448 B | 124,188 B |
| 本分支，kthreads **关** | 77,420 B | 124,188 B |
| 本分支，kthreads **开** | 82,160 B | 124,396 B |

- 关掉时**零成本**：`proc_t` 的 `leader/tgid/exit_code/live_threads/glock`、调度
  计数器、线程状态和全部线程代码都在 `#ifdef TOS_KTHREADS` 里（相对主线 flash
  −28 B，属于对齐噪声；RAM 完全一致）。
- 开启时：flash **+4,712 B**，RAM **+208 B**（8 槽 × 8 B 的组状态 + 线程状态/对齐）。
- 运行时占用的是**进程表槽位**：一个线程 = 一个 `proc_t`（864 B，主线 856 B）。
  C3 只有 8 槽，4 个线程后剩 4 个。
- 用户侧：`kthread.c` 目标文件 1,496 B，只有调用它的程序才链接；`threadbench`
  2,822 B，只出现在开 kthreads 的构建里。

## 4.7 边界测试（`kthreadtest`）

`user/kthreadtest.c` 覆盖 threadbench 不碰的情况，C3 真机全部通过：

```
kthreadtest: ok   create
kthreadtest: ok   join returns the exit code
kthreadtest: ok   second join fails
kthreadtest: ok   join after exit (zombie slot)
kthreadtest: ok   join unknown tid fails
kthreadtest: 6 thread slot(s) usable beside shell+self
kthreadtest: ok   slot exhaustion returns -1
kthreadtest: ok   create works again after joining everything
kthreadtest: ok   churn join code
kthreadtest: ok   leader ran while the thread was still asleep
kthreadtest: ok   join sleeping thread
kthreadtest: ok   thread finished before join returned
kthreadtest: overlapped sleep 200 ms (wall clock)
kthreadtest: ok   open readme.txt in the leader
kthreadtest: ok   thread read through the shared fd table
kthreadtest: ok   join the fd thread
kthreadtest: ok   leader close() closed it for the thread
kthreadtest: all ok (0 failure(s))
```

`kthreadtest leave` 在组长退出时留下一个活线程，之后 `ps` 只剩 `msh`，证明整组
被回收、槽和窗口都没有泄漏。C3 的 6 槽 = 8 个进程槽 − shell − 被测程序。

## 4.8 阻塞并发 demo（`prodcons`）

`prodcons` 让一个线程从管道读、主线程写：管道满时 `write()` 睡、空时 `read()` 睡。
用 512 B 的 `CHUNK` 过 32 KiB，C3 真机：

```
prodcons: 32768 bytes through a 512-byte pipe in 25 ms
prodcons: sent sum 4177920, received sum 4177920, 32768 bytes
prodcons: all bytes delivered, both threads blocked and woke
```

单核上这个数字不是吞吐纪录，而是**语义证明**：生产者和消费者互相唤醒、一个阻塞
不影响另一个，校验和一致说明没有丢/重字节。`rv32smp`（QEMU，2 hart）上同一程序
也能通过；`threadbench` 在 2 hart 上是 3 线程 4.9 s vs 单线程 36 s，但 QEMU 的
TCG 单核基线本身受双 vCPU 影响，这个比例不可当真——真实并行数字等 `rp2350rvsmp`。

## 5. 阶段计划

- **P0（已完成）**：评估文档 + `swcost` 基线。
- **P1（已完成，RV32/C3 分支）**：`SYS_THREAD_CREATE/EXIT/JOIN`、线程槽共享窗口、
  `threadbench`；QEMU rv32 上 3 线程全部完成、join 回收、阻塞按线程生效。
- **P2（进行中）**：fd 表已共享、信号已按组、SMP 锁已加；剩下 `exec` 按 POSIX
  杀掉兄弟线程、可选的 `ps` tgid 列，以及阻塞 I/O 与 `close()` 的引用计数。
- **P3（已完成）**：C3 上板实测 `threadbench` + `swcost`；ARM 暂不扩。
- **P4**：决策——保留协作线程库（零内核成本）还是用真线程替换 `thread_*` API。

## 6. 必须提前想清楚的坑

- **内核栈仍是每 CPU 一份**：trap 现场在 `proc_t->saved` 里，切走时随槽保存，
  这一点和现在的进程切换相同，不需要每线程内核栈。
- **用户栈从哪来**：窗口是内核分配的，但线程栈应该由用户给（类似
  `pthread_attr_setstack`），否则内核要改堆边界，`brk` 语义会和用户 `malloc` 打架。
- **栈溢出检测**：现有的 `PLAT_HEAP_GUARD` / 自动翻倍只对"窗口顶部的主栈"成立；
  线程栈在堆中间，只能靠用户自己留够。
- **`exec`**：`exec` 会替换整个地址空间，必须结束同组所有其他线程（类似
  `execve` 的 nproc 语义）。
- **`wait` vs `join`**：`wait` 只等子进程；`join` 只等同组线程。两者不能混。
- **SMP + 共享堆**：两个线程同时 `malloc` 需要用户侧锁，或者把内核的 brk 操作
  做成原子；`printf` 走 console 锁，已经安全。
