# 进程-线程模型（替换旧任务模型）

> 分支：`exp/thread-sched`。状态：**E0 设计 + 基线**。
> 旧模型（一个 `proc_t` 同时装资源与调度）**废弃**，不再保留第二套路径。

## 0. 目标与非目标

目标：

- 所有可构建目标（rv32 / rv32smp / C3 / M3 / M33 / RP2040 / x86）都只使用一套模型：
  **进程（EPROC）拥有资源，线程（ETHREAD）是唯一调度单位**。
- shell、exec 出来的程序、用户线程、WiFi OSI、USB 泵，全部落到模型里。
- 旧模型删除：没有“任务槽兼任进程与线程”的 `#ifdef` 旧路径，没有线程槽里的资源拷贝。
- 内核线程在所有端口可用：RV32 继续 M-mode ktask；**ARM 新增 PendSV 内核线程**。

不做：

- **不实现 POSIX fork**（已定）。进程创建走 `spawn/exec`，线程创建走 clone/thread API。
  无 MMU 下 fork 的地址空间/指针问题不值得。
- 不改用户 ABI 的可见语义（pid、tids、wait/join），除非为了模型必须。

## 1. 类型与字段归属

```c
struct proc {                        /* EPROC：资源 */
    int      pid;                    /* 进程 id，也是线程的 tgid        */
    int      state;                  /* NEW/LIVE/DYING/ZOMBIE/FREE      */
    struct proc *parent;
    int      exit_code;
    int      live_threads;           /* 还有多少线程没退出              */
    spin_t   glock;                  /* brk + fd 表                     */

    /* 地址空间与堆 */
    uint32_t win_base, win_size, wx_start;
    uint32_t heap_base, heap_end, brk;

    /* 资源块（S2 的 proc_res 并进来） */
    file_t   fds[MAX_FDS];
    char     cwd[TOS_PATH_MAX];

    /* 子进程/等待 */
    int      wait_pid;
    int      lag_pid[LAG_MAX], lag_code[LAG_MAX];
    uint8_t  lag_n;
};

struct thread {                      /* ETHREAD：调度 */
    struct proc *proc;               /* NULL = 内核线程                 */
    int      tid;                    /* 调度 id                          */
    int      tgid;                   /* proc->pid；内核线程 = tid        */
    int      state;
    int      prio, home_cpu, cpu;
    int      quantum;                /* 每 CPU 的 slice 在 cpu_t 里，这里只做统计? */

    regs_t   saved;                  /* trap 帧 / 初始帧                 */
    regs_t  *kframe;
    uint32_t stack_top, stack_size;  /* 用户栈（内核线程是自己的栈）      */
    uint8_t  is_ktask;               /* 内核线程：无窗口、M/privileged   */
    uint8_t  kstack_slot;

    uint32_t user_esp;               /* 兼容用户栈指针的显示?            */
    struct pipe *wpipe;
    int      wdir;
    uint32_t wbuf, wlen;
    int      wait_pid;               /* join 的 tid                      */
    uint64_t wake_tick;

    volatile int *held_mutex;        /* kmutex 死亡释放                  */
    volatile int *wmutex;

    uint64_t start_ns, cpu_ms;
    uint32_t cpu_ticks;
    int      stop_pending, kill_pending;
    char     name[10];               /* 线程名；进程名在 proc 里          */
};
```

字段规则：

| 归属 | 内容 |
|---|---|
| 进程 | 窗口/堆/brk、fds、cwd、父进程、子进程/退出码、live_threads、glock |
| 线程 | 帧、栈、state、prio/cpu/统计、等待对象、mutex、stop/kill、名字 |
| 派生 | `current` 是 `thread *`；`current_proc = current->proc`；内核线程为 NULL |
| 不复制 | 线程不再有 heap/brk/cwd/fds 的拷贝；所有访问走 `t->proc` |

## 2. 不变量

1. **调度只认线程**：`sched_pick()` 遍历 `threads[]`；`proc` 不进运行队列。
2. **进程存活 = 至少一个线程**：主线程 + 其它线程；最后一个线程退出时才回收
   窗口、fd 表、cwd 和 `proc` 槽。
3. **主线程退出 = 进程退出**：其余线程被终止（现有 `begin_group_exit` 语义），
   最后一个线程做清理。
4. **保护按进程**：`apply_protection()` 的 key 是进程的窗口形状（S3a）；内核线程
   不编程。切线程不切组就跳过。
5. **fds/cwd 只在 proc**：线程共享；`close()`/`chdir()` 对整个进程生效（S1/S2）。
6. **wait 是进程语义，join 是线程语义**：`wait(pid)` 等进程退出，`join(tid)` 等线程。
7. **内核线程没有 proc**：无窗口、不参与 wait；退出直接回收。RV32 跑在 M-mode
   自己的栈上；ARM 跑在 privileged Thread mode + 自己的 PSP 栈（见 §4）。
8. **信号归属**：`kill` 对进程（全部线程），`stop/cont` 也按进程处理，避免半停。
9. **调度器不选没有可运行线程的进程**：`proc` 只是资源容器；没有线程就等待回收。

## 3. 表与内存

- `struct proc procs[MAX_PROCS]`、`struct thread threads[MAX_THREADS]`。
- `MAX_PROCS` 沿用各目标配置（4/8/16/18）；`MAX_THREADS` 默认 `MAX_PROCS + 8`
  （至少能装下“每个进程一个主线程”再加几个工作线程），在 E1 用实际
  `sizeof` 量，目标 ≤ 250 B/线程；小板上限优先。
- 内核线程栈：RV32 继续 6 KiB 池（`/proc/ktask` 量高水位）；ARM 新增每线程栈池，
  大小用同样的 paint/高水位方式量（E3）。
- `TOS_KTHREADS` 改为只控制**用户态线程 API 与测试程序**，不再是内核模型开关。

## 4. 各端口

| 端口 | 用户线程 | 内核线程 |
|---|---|---|
| rv32 / rv32smp / C3 | 现有 | 现有 ktask（M-mode、每核 trap 状态、栈池） |
| Cortex-M3/M33/M0+ | 现有 | **新增**：privileged Thread mode、每线程 PSP 栈、PendSV 切换；协作式（tick 不抢占内核线程，只在 sleep/yield 切换） |
| x86 | 现有（若有） | 按同一模型，具体实现随 x86 端口；本轮以 ARM/RV32 为主 |

ARM 内核线程的要求：

- 线程跑在内核代码里时不能被 tick 抢占到同一个栈上；和 ktask 一样是协作式，
  只有 `sleep/yield/阻塞` 主动让出。
- 每线程一个内核栈；栈深用 paint/高水位量，先按 2–4 KiB 试。
- 第一个真实客户：把 ARM 的 USB 泵从 PendSV/idle 钩子迁到内核线程（S4 的 ARM 版），
  用它证明模型在 M-profile 上可用。

## 5. 生命周期

- `proc_create(path)`: 分配 proc + 主线程；加载 ELF；主线程从 entry 开始。
- `thread_create(fn, arg, stack)`: 在当前 proc 下分配线程。
- `proc_spawn(path, argv)`: 现有 exec 语义（新进程，父进程 wait）。
- `thread_exit`: 线程 zombie/retire；主线程退出 → 进程退出；最后一个线程回收 proc。
- `ktask_create`: 分配无 proc 的线程（内核栈）。
- `wait/join` 按 §2.6；父进程退出时子进程交给 init（现有语义）。

## 6. ps / /proc

- `ps` 保持现有列（pid ppid prio state c cpu% cpums wink stk/sz name），其中
  pid 显示进程 id，线程行另加 `tid`/tgid 标识（E2 定格式；先保证
  `cmd/tostest/emutest.go` 与 `scripts/rp2350/accept33.py` 的解析不破）。
- 新增/保留 `/proc/threads`（或 `/proc/ps` 两级输出）：每行 `tgid tid state prio
  cpu stk/sz name`，供脚本和 `/proc/ktask` 对照。

## 7. 迁移

| 阶段 | 内容 | 门槛 |
|---|---|---|
| **E0** | 本文档 + 基线（commit、T0/T1/T2、C3、Pico 2 RV32 UP/SMP） | 无行为变化 |
| **E1** | 拆表：`struct proc`/`struct thread`、`threads[]`、`current` 改线程；exec/线程/ktask 创建路径改“进程+主线程”；行为不变 | T0/T1/T2、kthreadtest 10×、ktest、soak |
| **E2** | 生命周期收口：最后线程回收、wait/join、信号归属、ps//proc 两级 | 同上 + 现有 ps 解析脚本 |
| **E3** | ARM 内核线程（PendSV + 每线程栈池）；USB 泵迁移到内核线程 | M3 QEMU + M33/RP2040 板测 |
| **E4** | 全端口矩阵：rv32/rv32smp、C3、M3/M33、RP2040、x86；删旧模型残留 | 全 T3 + 板卡 |
| **E5** | 收尾：文档、`docs/HISTORY`、合并 main 的决定 | — |

每个阶段先固定基线，再改；测试失败先与基线比对。

## 8. 删除清单

- `proc_t` 中的资源/调度混合字段与线程槽里的资源拷贝。
- `PROC_GROUP`/`PROC_OF` 的半模型特例（保留 `PROC_OF` 作为 `t->proc` 的读取）。
- ktask 的“特殊任务”分支，改为 `proc == NULL` 的线程。
- 旧的“任务”命名与注释（process.c 的 task/process 混用）。
- `TOS_KTHREADS` 作为内核模型开关；只保留用户 API/测试的编译开关。

## 9. 基线（E0）

- commit：`0c1e2dd`（本文件之前）。
- T0：`go test -count=1 ./...` PASS。
- T1：`tos build all` 17/18（x86 `MKFS` 溢出，`docs/OPEN-ITEMS.md` §17）。
- T2：QEMU rv32/rv32smp/arm；`kthreadtest` 10/10 ×3、soak 10 轮 2/2。
- T3：Pico 2 RV32 UP/SMP（S4 泵）、ESP32-C3（accept 42/42 ×2、kthreadtest all ok）。
- 已知未覆盖：M33 / RP2040 硬件（本机无板，§22 的清单待跑）。
