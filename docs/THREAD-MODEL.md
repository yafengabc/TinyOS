# 线程调度实验（分支 `exp/thread-sched`）

> 状态：**实验分支**，不进 main。S0 已完成（测量脚手架），S1 起是行为改动。
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
| `fds[]` | `PFD(p) = PROC_GROUP(p)->fds` | 正确（组共享） |
| `cwd[]` | `current->cwd` 直接访问 | **线程里 `chdir` 只改自己那份**，与 POSIX/Windows 不符 |
| `win_*` / `heap_*` / `brk` | 直接访问 | 线程本来就不该有窗口，靠约定 |
| `proc_t` 大小 | **646 B/槽**（`procs` 10.1 KiB / 16） | 线程槽白占 `fds`(320 B) + `cwd`(64 B) |

## 1. 已量到的数字（S0）

- 线程槽今天 = 646 B；把 `fds`+`cwd` 共享后约 **262 B**（估算，字段见上表）。
- 线程真正的成本是栈：`thread_create(fn, arg, stack_bytes)` 从程序堆取，1 KiB 起步。
- 内核线程池：`ktask_stacks[6][6144]` = **36 KiB**，仅 RV32；`--gc-sections` 在不用
  ktask 的镜像里会整块丢掉（已验证 C3 默认镜像 `.bss` = 122 480 B，大符号只有
  `arena` 80 KiB）。
- `process_ktask_create(name, fn, arg, stack_bytes, prio)` 的 `stack_bytes` **只被记录**，
  栈顶永远是槽顶 → 请求 2 KiB 仍占 6 KiB 槽。
- `process_ktask_create` **目前没有调用者**：ktask 机制存在但未使用。

## 2. 阶段

| 阶段 | 内容 | 行为变化 | 验证 |
|---|---|---|---|
| **S0** ✅ | ktask 栈 paint + 高水位；`/proc/ktask`（槽、已用、高水位）；`KTASK_STACK_BYTES` 可覆盖 | 无 | T0/T1；QEMU `cat /proc/ktask` |
| **S1** ✅ | **资源接缝**：`PROC_OF(p)` 定义 + `cwd` 改走它；`ps` 的 `wink`、栈增长消息改读组的字段 | `chdir` 变成组级（修 bug） | T0/T1/T2；两个 kthreads 配置构建 |
| **S2** | **结构拆分**：`fds`+`cwd` 迁到 `struct res`，leader 内联、线程存指针（或独立 `res[]` 表）；窗口/堆字段跟随 | 线程槽 646→~262 B | 同上 + `free`/`ps` 计数 |
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

## 3. 不进 main 的理由

- S2/S3 动的是"**槽死即窗口释放**"这条核心不变量（`win_alloc` 的简单性建立在这上面），
  SMP 上会引入"最后一个线程死才释放"的竞态面。
- 现有镜像里只有 `*-kthreads.conf` 用到线程，收益是**结构性**的；等 S4 的泵线程证明
  值得，再考虑回 main。

## 4. 怎么用这个分支

```sh
git checkout exp/thread-sched
# 量 ktask 栈高水位（需要有人创建 ktask；S4 之后是泵线程）：
go run ./cmd/tos build -target rv32 && go run ./cmd/tostest smoke -target rv32 \
    -commands "cat /proc/ktask"
# 想试更小的池：在 kernel/process.c 里 -DKTASK_STACK_BYTES=2048（或改默认值）
```

`/proc/ktask` 现在会打印：

```
ktask: slot   size   used     hw  name
  no kernel threads
```

（没有创建者时的正常输出；S4 之后每个槽一行。）
