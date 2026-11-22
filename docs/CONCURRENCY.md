# 并发与锁顺序

这份文档是散文版，`internal/build/lockorder_test.go` 是可执行版（T0 每次跑）。
两者不一致时以测试为准——测试红就是有人加了违规调用。

## 1. 为什么有这张表

`docs/HISTORY.md:34` 记的 34 s 启动：RV32 SMP 上 `usb_cdc_flush()` 走到
`usb_tx_drain()`，后者取 `g_tx_lock`，而另一条路径是 `g_tx_lock` 再 flush——两
个方向互等。读代码看不出来；一条"持这把锁时不许调用 X"的规则能看出来。

同样的错在 SMP 上会表现为"偶发 30 秒"，比崩溃更难查，所以宁可用一条粗规则。

## 2. 锁清单

| 锁 | 位置 | 保护什么 | 谁在下面 |
|---|---|---|---|
| `proc_lock` | `kernel/process.c:394` | 进程表、状态、窗口归属 | `p->lock`、`glock` |
| `fs_lock` | `kernel/ramdisk.c:103` | 整个 littlefs API 调用 | `heap_lock` |
| `heap_lock` | `kernel/heap.c:172` | 内核堆的空闲链 | 无 |
| `out_lock` | `kernel/console.c:50` | 控制台输出缓冲 | 无（叶子） |
| `mb_lock` | `kernel/mailbox.c:31` | 4×256 B 消息池 | 无（叶子） |
| `p->lock` | `kernel/pipe.c:27` | 管道引用计数（读端/写端） | 无 |
| `glock` | `PROC_GROUP()->glock` | 进程组（会话/信号） | 无 |
| `g_tx_lock` | `drivers/usb/rp2350rv/usb_cdc.c:160` | USB 发送环 | USB 控制器锁 |

## 3. 顺序

```text
proc_lock ──┬─→ p->lock          (pipe.h: proc_lock then p->lock, never reverse)
            └─→ glock

fs_lock ─────→ heap_lock         (ramdisk.c: littlefs allocates through the heap)

g_tx_lock ───→ USB controller    (usb_cdc.c: "always g_tx_lock then the USB lock")

out_lock, mb_lock, heap_lock     叶子：不取任何其它锁
```

规则：

1. 只允许从上往下取；反向就是潜在死锁，哪怕当前调用图里还没形成环。
2. 持锁时不睡眠：`proc_wait`、`wait_for`、`msleep` 一律先放锁。
3. 持 `proc_lock` 不碰文件系统（`fs_*`/`lfs_*`）和 USB flush——它们会把
   `fs_lock`/`g_tx_lock` 拉到你下面，方向就反了。
4. 持 `heap_lock` 不再进堆（`kmalloc`/`kfree` 不可重入），也不进文件系统。
5. 持 `fs_lock` 可以进堆（`heap_lock` 在下面）；反过来不行。

## 4. 中断屏蔽

`plat_spin_lock_irqsave()`（`kernel/cpu.c:279`）返回旧中断状态，
`plat_spin_unlock_irqrestore()` 恢复。计时器 tick 自己会取 `proc_lock`，所以
**默认所有 `proc_lock` 都用 irqsave 版本**。例外只有三处，都在 `process.c`
的注释里写明理由：`sched_switch()`（锁和屏蔽是两段独立生命周期）、
`cpu_take_task()`、`process_ktask_create()`（进入时中断已关）。

`fs_lock`、`heap_lock`、`out_lock`、`mb_lock` 用普通 `spin_lock()` 即可：
它们的调用者都在任务上下文且不会在 tick 里重入。

## 5. 静态检查怎么工作

`TestLockDiscipline` 把每个 `kernel/*.c` 拆成函数体（花括号配对），如果函数体里
出现了 `&proc_lock` 之类的取锁，就检查同一函数体里有没有禁忌调用：

- `proc_lock`：`proc_wait`、`wait_for`、`msleep`、`usb_cdc*`、`fs_*`、`lfs_*`
- `fs_lock`：`proc_wait`、`msleep`、`usb_cdc*`
- `heap_lock`：`kmalloc`、`kfree`、`fs_*`

这是函数级而不是区间级，故意的：按花括号追踪"锁区间"在遇到 `return` 错误路径时
会失准，一旦误报，检查就会被人关掉。函数级只会多报，不会漏报。

确实是被误报时，在函数体里写一行：

```c
/* LOCKCHECK: allow the wait happens after the unlock below */
```

理由必须写清楚；`TestLockDisciplineDetects` 保证检查器本身还能抓到真违规。

## 6. 现在不拆锁

`proc_lock` 一把锁保护整张表是当前规模下的正确选择：临界区是一次十六项扫描加一次
状态写入，拆细后边界检查的成本高于竞争收益。`fs_lock` 同理（一次 littlefs 调用
一个临界区）。真要拆，先有 #53 的宿主测试再说，否则回归面太大。
