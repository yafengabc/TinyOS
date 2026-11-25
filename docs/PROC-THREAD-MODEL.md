# 进程-线程模型（替换旧任务模型）

> 讨论：issue #57（https://github.com/yafengabc/TinyOS/issues/57）、
> ARM 内核线程 E3：issue #58。
> 分支：`exp/thread-sched`。状态：**E0 设计 + 基线（含评审修订）**。
> 旧模型（一个 `proc_t` 同时装资源与调度）**废弃**，不保留第二套路径。

## 0. 目标与非目标

目标：

- 所有可构建目标（rv32 / rv32smp / C3 / M3 / M33 / RP2040 / x86）都只使用一套模型：
  **进程（EPROC）拥有资源，线程（ETHREAD）是唯一调度单位**。
- shell、exec 出来的程序、用户线程、WiFi OSI、USB 泵，全部落到模型里。
- 旧模型删除：没有“任务槽兼任进程与线程”的 `#ifdef` 旧路径，没有线程槽里的资源拷贝。
- 内核线程在所有端口可用：RV32 继续 M-mode ktask；**ARM 新增 PendSV 内核线程**（E3，见 #58）。

不做：

- **不实现 POSIX fork**（已定）。进程创建走 `spawn/exec`，线程创建走 clone/thread API。
- 不改用户 ABI 的可见语义（pid、tids、wait/join），除非为了模型必须。

## 1. 类型与字段归属

```c
struct proc {                        /* EPROC：资源 + 进程身份 */
    int      pid;                    /* 进程 id；线程的 tgid 由它派生   */
    char     name[10];               /* 进程名，ps 的进程行             */
    int      state;                  /* FREE/NEW/LIVE/DYING             */
    struct proc *parent;
    int      exit_code;              /* wait() 拿到的退出码             */
    int      live_threads;           /* 还有多少线程没退出              */
    spin_t   glock;                  /* brk + fd 表                     */

    /* 地址空间与堆 */
    uint32_t win_base, win_size, wx_start;
    uint32_t heap_base, heap_end, brk;

    /* 资源块（S2 的 proc_res 并进来） */
    file_t   fds[MAX_FDS];
    char     cwd[TOS_PATH_MAX];

    /* 子进程/退出码记忆；等待目标在每个等待线程上 */
    int      lag_pid[LAG_MAX], lag_code[LAG_MAX];
    uint8_t  lag_n;

    /* 进程级停止：stop/cont 作用于全部线程，避免半停 */
    int      stop_pending;
};

struct thread {                      /* ETHREAD：调度 */
    struct proc *proc;               /* NULL = 内核线程                 */
    int      tid;                    /* 调度 id                          */
    int      state;                  /* 见 §2.2                         */
    int      prio, home_cpu, cpu;
    uint8_t  is_main;                /* 进程的主线程                    */

    regs_t   saved;                  /* trap 帧 / 初始帧                 */
    regs_t  *kframe;
    uint32_t stack_top, stack_size;  /* 用户栈；内核线程是自己的栈        */
    uint8_t  kstack_slot;            /* 内核线程栈池下标                 */

    struct pipe *wpipe;
    int      wdir;
    uint32_t wbuf, wlen;
    int      wait_child;             /* ST_WAIT_CHILD：等的子 pid        */
    int      join_tid;               /* ST_WAIT_THREAD：等的 tid         */
    uint32_t in_buf, in_len;         /* ST_WAIT_INPUT：行缓冲            */
    uint64_t wake_tick;

    volatile int *held_mutex;        /* kmutex 死亡释放                  */
    volatile int *wmutex;

    uint64_t start_ns, cpu_ms;
    uint32_t cpu_ticks;
    int      kill_pending;
    int      exit_code;              /* ST_THREAD_ZOMBIE：join 的返回码  */
};
```

字段规则：

| 归属 | 内容 |
|---|---|
| 进程 | 窗口/堆/brk、fds、cwd、父进程、子进程/退出码记忆、live_threads、glock、name、stop |
| 线程 | 帧、栈、state、prio/cpu/统计、等待对象、mutex、kill、exit_code、is_main |
| 派生 | `current` 是 `thread *`；`current_proc = current->proc`；内核线程为 NULL |
| 不复制 | 线程不再有 heap/brk/cwd/fds 的拷贝；所有访问走 `t->proc` |
| 一次性的 | `entry`、初始 `user_esp` 只在 `process_exec()` 构造初始帧时用，属于局部变量，**不进 proc** |

辅助：`thread_is_kernel(t)` 即 `(t)->proc == NULL`；`tgid` 不存字段，用
`t->proc ? t->proc->pid : t->tid`。`quantum` 属于每 CPU 的 `cpu_t`，不复制到线程。

**命名**：进程名在 `proc->name`。**用户线程不存名字**，显示为
`<进程名>#<tid>`（Windows 的任务管理器只给 TID，Linux 的 `/proc` comm 默认也是
进程名；线程名是调试器/`prctl` 的可选功能，不是身份）。**内核线程没有 proc**，
名字是它们唯一的身份，也是验收证据（`/proc/ktask ... usbpump`），所以放在内核
线程栈池的元数据里（`const char *names[KTASK_SLOTS]`，6×4 B），`thread` 只留
`kstack_slot`。这样比每线程 `name[8]` 省 8 B/线程，也不会截断名字；以后要
`thread_setname` 再加可选 API。

## 2. 不变量

1. **调度只认线程**：`sched_pick()` 遍历 `threads[]`；`proc` 不进运行队列。
2. **进程存活 = 至少一个线程**：主线程 + 其它线程；最后一个线程退出时才回收
   窗口、fd 表、cwd 和 `proc` 槽。
3. **主线程退出 = 进程退出**：`t->is_main` 退出，其余线程被终止（现有
   `begin_group_exit` 语义），最后一个线程做清理。
4. **保护按进程**：`apply_protection()` 的 key 是进程的窗口形状（S3a）；内核线程
   不编程。切线程不切组就跳过。
5. **fds/cwd 只在 proc**：线程共享；`close()`/`chdir()` 对整个进程生效（S1/S2）。
6. **wait 是进程语义，join 是线程语义**：`wait(pid)` 等进程退出，`join(tid)` 等线程；
   等待的**目标**（`wait_child`/`join_tid`）在每个等待线程上，子进程表和退出码
   记忆在 proc 上。
7. **内核线程没有 proc**：无窗口、不参与 wait；退出直接回收。
8. **信号归属**：`kill` 对进程（全部线程，`kill_pending` 逐线程置位以便及时退出）；
   `stop/cont` 放 proc（`stop_pending`），调度器跳过被停进程的全部线程。
9. **调度器不选没有可运行线程的进程**：`proc` 只是资源容器；没有线程就等待回收。
10. **最后线程的原子性**：`live_threads--` 与 `== 0` 的判断必须在同一个
    `proc_lock` 临界区；最后一个线程的退休与 proc/窗口/fd 的释放也在其中。
11. **窗口块**：非主线程的槽块永远 `win_size == 0`；所有退休路径只清本槽的块，
    绝不通过 `PROC_RES` 别名去清组长的窗口（E1a 的注释 + 断言）。
12. **proc_create 失败顺序**：先退线程、再退 proc；两步都在 `proc_lock` 下，失败
    路径不留下“有线程没进程”或“有进程没线程”的槽。

### 2.1 主线程退出：进程立即退出（选型）

两种可选语义：

| | A. 主线程退出 = 进程退出（选它） | B. 主线程退出后进程继续（`pthread_exit` 式） |
|---|---|---|
| 窗口/fd/进程槽 | 主线程一退就开始终止其余线程，最后一个线程收尾，**很快释放** | 只要还有一个 worker 在跑，窗口和 fd 表就**一直被占住** |
| 线程槽/栈 | worker 在下一个 trap 退出，槽和栈很快回收 | worker 想跑多久就占多久；无人 join 时是孤儿 |
| 退出码/等待 | 退出码就是主线程的；父进程 `wait` 的语义不变 | 需要额外规则：主线程没了，进程退出码谁定？谁来回收？ |
| 复杂度 | 现有 `begin_group_exit()` 就是它 | 需要“无主进程”状态、孤儿 worker、`wait` 与 `join` 的新组合 |
| 失败面 | 有界：最迟一个 tick 后全部展开 | 一个死循环 worker 可以永久钉住进程资源 |
| POSIX 对照 | `return from main` → `exit()`，杀全部线程 | 只有显式 `pthread_exit()` 才是这个语义 |

结论：**A**。理由不是“简单”，而是资源上严格更省：进程的窗口、fd 表、`proc` 槽
只有等最后一个线程退出才可能释放；B 把这段时间交给任意 worker 决定，A 把它收敛到
“一个 tick 的收尾”。同时 A 与现有 `begin_group_exit()`、`wait(pid)`、退出码语义
一致，不需要给“无主进程”再发明一套规则。

代价与边界：A 会丢掉主线程退出时其他线程还没做完的工作。想让 worker 跑完的程序
应当 `join` 它们，或让主线程等到最后；如果将来确实需要 `pthread_exit` 式语义，
再加一个显式的 `thread_exit_main_keep()`（单独设计、单独测试），**不作为默认**。

实现落点（E2）：`t->is_main` 退出 → `proc->state = DYING`，其余线程置
`kill_pending` 并唤醒；最后一个线程在 `thread_retire` 里释放窗口/fd/`proc`。
内核线程没有 `proc`，退出直接回收，不参与这条。

### 2.2 状态与迁移

线程状态（`thread->state`）：

```
FREE -> NEW -> READY -> RUNNING
RUNNING -> READY           抢占/让出
RUNNING -> WAIT_CHILD / WAIT_INPUT / WAIT_SLEEP / WAIT_PIPE /
           WAIT_THREAD / WAIT_MUTEX / KT_WAIT
WAIT_*  -> READY           唤醒
RUNNING -> THREAD_ZOMBIE   退出，等 join
THREAD_ZOMBIE -> FREE      join 取走
RUNNING/READY -> KILLING -> FREE   kill 的延迟路径
```

进程状态（`proc->state`）：

```
FREE -> NEW       exec 领槽（线程还没发布）
NEW  -> LIVE      加载成功、主线程 READY
LIVE -> DYING     主线程退出，其余线程在收尾
DYING -> FREE     最后一个线程退出，退出码交给父进程/记忆
```

`proc` 没有 ZOMBIE 槽：退出码在 `reap` 时交给父进程的 `lag` 或直接唤醒等待者，
槽立即回收（现有语义）。`proc_state_transition_ok()` 与线程的转移表分开写。

## 3. 表与内存

- `struct proc procs[MAX_PROCS]`、`struct thread threads[MAX_THREADS]`。
- `MAX_THREADS = max(2 * MAX_PROCS, 16)`，上限 32：16 个进程各一个主线程时，
  工作线程是**所有进程共享**的余量；`kthreadtest` 会一直建到表满
  （rv32 30 个，C3 14 个）。
- 尺寸（E1b 实测，`clang -Xclang -fdump-record-layouts`）：

| 架构 | `thread_t` | `+TOS_KTHREADS` | `proc_t` |
|---|---|---|---|
| rv32（C3 / RP2040 / Pico 2 RV32） | 256 B | 264 B | 484 B |
| ARM（M3 / M33 / M0+） | 192 B | 200 B | 484 B |

  拆分前一个槽位就是整块：rv32 712 B / ARM 648 B（`fds[12]`、`cwd[64]` 和身份
  字段都在槽里），即每进程 712 / 648 B。E1b 之后每进程 740 / 676 B，但**表按线程
  数分配**，所以默认镜像的 .bss 变大：

| 目标（进程 / 线程） | E1b 表总量 | 拆分前 | 净增 |
|---|---|---|---|
| C3、RP2040（8 / 16） | 7,968 B | 5,696 B | +2.2 KiB |
| RV32 virt（16 / 32） | 15,936 B | 11,392 B | +4.4 KiB |
| arm / M33（16 / 32） | 13,888 B | 10,368 B | +3.4 KiB |
| C3 8K 档（18 / 32） | 16,904 B | 12,816 B | +4.0 KiB |

  （`TOS_KTHREADS` 每线程再 +8 B。数字按 `sizeof` × 表项算，不是从 ELF 量的。）
  E0 的估计是 +0.7…+2.4 KiB；实测更大，原因有二：`thread_t` 比 #59 的 union
  估计大（`regs_t` 144/80 B 加上等待、栈与记账字段），以及 `MAX_THREADS` 取
  2×进程数（上限 32）。这是“线程是调度单位”的必要成本，写在这里是为了
  让 RP2040 一类 264 KiB 的板子有据可查。
- 内核线程栈：RV32 继续 6 KiB 池（`/proc/ktask` 量高水位）；ARM 新增每线程栈池，
  大小用同样的 paint/高水位方式量（E3）。内核线程名和栈池放在一起
  （`const char *names[KTASK_SLOTS]` = 24 B，替代每线程 `name[8]`）。
- `TOS_KTHREADS` 改为只控制**用户态线程 API 与测试程序**，不再是内核模型开关。

## 4. 各端口

| 端口 | 用户线程 | 内核线程 |
|---|---|---|
| rv32 / rv32smp / C3 | 现有 | 现有 ktask（M-mode、每核 trap 状态、栈池） |
| Cortex-M3/M33/M0+ | 现有 | **新增**（E3，#58）：privileged Thread mode、每线程 PSP 栈、PendSV 切换；协作式 |
| x86 | 现有（若有） | 按同一模型，具体实现随 x86 端口；本轮以 ARM/RV32 为主 |

ARM 内核线程的要求见 #58：只跑已知不阻塞路径、PendSV 优先级关系、M0+ 无 PSPLIM
时的栈检测（canary + 池边界/MPU）；第一个客户是把 ARM 的 USB 泵从 PendSV/idle
钩子迁到内核线程。

## 5. 生命周期

- `proc_create(path)`: 分配 proc + 主线程；加载 ELF；主线程从 entry 开始。
  失败：先退线程、再退 proc（§2 第 12 条）。
- `thread_create(fn, arg, stack)`: 在当前 proc 下分配线程。
- `proc_spawn(path, argv)`: 现有 exec 语义（新进程，父进程 wait）。
- `thread_exit`: 线程 zombie/retire；主线程退出 → 进程退出；最后一个线程回收 proc。
- `ktask_create`: 分配无 proc 的线程（内核栈）。
- `wait/join` 按 §2.6；父进程退出时子进程交给 init（现有语义）。

## 6. ps / /proc

- `ps` 的列（E2 定稿，2026-11-26）：`pid tid ppid prio state c cpu% cpums wink
  stk/sz name`。`pid` 是进程 id，`tid` 是线程 id（主线程两者相同，工作线程靠
  tid 区分），`ppid` 是父进程。列插在 pid 之后：`cmd/tostest/emutest.go` 只查
  子串，`scripts/rp2350/accept33.py` 从行尾读 `wink`，两者都不受影响。
  `name` 列显示 `proc->name`（主线程和工作线程一样）；内核线程不在 ps 里，
  它们的名字在栈池，`/proc/ktask` 从那里读。
- `/proc/threads` 是同一批行的机器视图，每行
  `tgid tid state cpu prio stk/sz name`（`state` 沿用 ps 的短名，`cpu` 为 -1
  表示不在 CPU 上），供脚本用；内核线程仍只看 `/proc/ktask`。

## 7. 迁移

| 阶段 | 内容 | 门槛 |
|---|---|---|
| **E0** | 本文档 + 基线（commit、T0/T1/T2、C3、Pico 2 RV32 UP/SMP） | 无行为变化 |
| **E1a** ✅ `2f8b49a` | 资源（win/heap/brk）搬进 proc 块，`entry`/`user_esp` 改局部，**删掉旧字段**；按槽扫块、退休只清本槽；窗口不变量断言 | T0/T1/T2、kthreadtest 10×、ktest、soak 10 轮、窗口泄漏用例、C3 真机 |
| **E1b-1** ✅ `948d5b6` | 拆 `procs[]`/`threads[]`；`t->proc`、`PROC_RES`/`PFD` 改指针；`current`/`cpu_take_task` 改线程；exec 分配 proc+主线程、线程共享 proc、ktask `proc == NULL` 并补齐空指针分支；退休只由主线程释放 proc | T0/T1/T2、kthreadtest ×3、ktest UP/SMP、C3 真机 |
| **E1b-2** ✅ `fd16196` | 身份（pid/name/parent/exit/live/stop/lag）搬到 `proc`；wait 交给真正等待的线程；SIGSTOP/SIGCONT 作用于整个进程；内核线程名进栈池；ps 打进程 id + 进程名（tid 列留给 E2） | T0/T1/T2、kthreadtest ×3、ktest UP/SMP、C3 真机 |
| **E2** | 生命周期收口：`proc->state` 状态机、信号归属（kill 落到进程）、ps//proc 两级输出、删掉线程槽里的身份残留 | 同上 + 现有 ps 解析脚本 |
| **E3** | ARM 内核线程（#58）+ USB 泵迁移 | M3 QEMU + M33/RP2040 板测 |
| **E4** | 全端口矩阵：rv32/rv32smp、C3、M3/M33、RP2040、x86；删旧模型残留 | 全 T3 + 板卡 |
| **E5** | 收尾：文档、`docs/HISTORY`、合并 main 的决定 | — |

窗口泄漏用例（E1a 新增）：shell 里连续 exec 20 个程序，再反复 create/join 线程，
`free` 的任务区/窗口数字回到初值；`win_largest_free()` 的报告不下降。

## 8. 删除清单

- `proc_t` 中的资源/调度混合字段与线程槽里的资源拷贝。
- `PROC_GROUP`/`PROC_OF` 两套读取（E2 合并成一个 `t->proc` 读取；E1b 阶段 1 先保留
  `PROC_GROUP` 给“谁是组长”用）。
- ktask 的“特殊任务”分支，改为 `proc == NULL` 的线程。
- 旧的“任务”命名与注释（process.c 的 task/process 混用）。
- `TOS_KTHREADS` 作为内核模型开关；只保留用户 API/测试的编译开关。

## 9. 基线（E0）

- commit：`0c1e2dd`（本文件之前；评审修订在 `196bb4f` 之后）。
- T0：`go test -count=1 ./...` PASS。
- T1：`tos build all` 17/18（x86 `MKFS` 溢出，`docs/OPEN-ITEMS.md` §17）。
- T2：QEMU rv32/rv32smp/arm；`kthreadtest` 10/10 ×3、soak 10 轮 2/2。
- T3：Pico 2 RV32 UP/SMP（S4 泵）、ESP32-C3（accept 42/42 ×2、kthreadtest all ok）。
- 已知未覆盖：M33 / RP2040 硬件（本机无板，§22 的清单待跑）。
