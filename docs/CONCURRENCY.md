# 内核锁顺序与持锁规则

这份文档是内核里所有全局锁的**唯一**总表。`kernel/process.c`、`kernel/ramdisk.c`、
`kernel/pipe.h` 的局部注释各自都对，但只覆盖自己那个文件；需要判断"这把锁持有时
能不能调那个函数"时，以这里为准。

`internal/build/lockcheck_test.go` 在 T0 里检查下面「禁止事项」一节的**直接调用**形式
（限 `kernel/*.c` 函数体内，见文末「静态检查覆盖到哪里」）；违规会报
`file:line: <call> inside <lock> (<理由>)`。

## 全局锁清单与顺序

```text
允许的嵌套（只按这个方向）：

  proc_lock ──→ p->lock, glock
  proc_lock ──→ out_lock          （只允许 trylock：抢不到就丢整块输出）
  fs_lock   ──→ heap_lock
  g_tx_lock ──→ USB 控制器锁        （驱动内部，见「USB 锁序」）

禁止的嵌套：

  proc_lock ──✗→ fs_lock, g_tx_lock, heap_lock
  heap_lock ──✗→ fs_lock, proc_lock

mb_lock     (独立，不与上面任何锁嵌套)
```

- `proc_lock`（`kernel/process.c`）：保护进程表、调度状态、FD 表。SMP 上两个核同时
  从表里挑任务，一把锁覆盖整张表是诚实的选择——临界区只有 16 项扫描加一次状态
  写入，细分锁在边界检查上的开销比省下的争用更大。**不提前拆。**
- `fs_lock`（`kernel/ramdisk.c`）：保护 littlefs 的 ofile 表和块设备访问。
  顺序必须是 `fs_lock → heap_lock`，因为 littlefs 通过 `kmalloc` 分配；反向没有
  任何调用点。
- `heap_lock`（`kernel/heap.c`）：保护内核堆的空闲链表，是最底层。`kmalloc()` 可以
  在没有锁的上下文直接调用；允许在 `fs_lock` 之下拿（littlefs 分配内存），**禁止**
  在 `proc_lock` 之下拿，也禁止在它下面再进文件系统。
- `p->lock`（`kernel/pipe.h`）：pipe ring 的叶子锁，在 `proc_lock` 之下。ring 和
  挂在它上面的任务由同一次调用决定——"pipe 空了"和"我睡了"必须是一步。
- `out_lock`（`kernel/console.c`）：只保护控制台的输出序列化。`console_write()`
  抢不到锁时整块丢，但同核重入必须等待，否则命令行 ≥16 字符会没输出。
- `g_tx_lock`（`drivers/usb/rp2350rv/usb_cdc.c`）：只保护 USB TX ring。它与 USB
  控制器锁的顺序在下面「USB 锁序」，**不在**静态检查范围内。
- `mb_lock`（`kernel/mailbox.c`）：独立的 leaf lock，不与上面任何锁嵌套。

`proc_lock` 之上/旁边的锁里，`fs_lock`、`g_tx_lock`、`heap_lock` 在持 `proc_lock`
时一律不得再取；反过来取就是 ABBA 的一半，哪怕现在还没有形成环。`out_lock` 是
唯一的例外，而且只能用 `console_write()` 的 trylock 路径：console 从不取
`proc_lock`，抢不到时整块丢弃，所以不构成环。任何新增的嵌套关系必须先改上面两张
表，再改代码。

## `plat_spin_lock_irqsave()` 的语义

定义在 `kernel/cpu.c:279`：

```c
uint32_t state = plat_irq_save();  /* 返回旧中断状态 */
spin_lock(s);
...
plat_spin_unlock_irqrestore(s, state);  /* 必须成对，且用同一个 state */
```

- 持锁期间**中断关闭**。定时器 tick 自己会拿 `proc_lock`（回收、唤醒控制台读者），
  开着中断拿这把锁等于在 tick 落到临界区时自锁死。
- 返回的旧状态必须原样传给 unlock；不要存到跨函数的数据里，也不要用常量代替。
- 少数几个点（`sched_switch()`、`cpu_take_task()`、`process_ktask_create()`）用
  裸 `spin_lock`，因为进函数时中断已经关了；它们各自在源码里写了理由。
- 单核上 `spin_lock` 编译成空操作，但**不要因此省略 irqsave**：SMP 镜像用同一份
  源码，单核的"能跑"不是多核正确的证据。

## 禁止事项

持 `proc_lock` 时禁止：

- `proc_wait` / `wait_for` / 任何形式的 `sleep` —— 持自旋锁睡眠是死锁。
- `kmalloc` / `kfree` —— 可能阻塞；需要内存时先出锁再分配。
- `usb_cdc_*` / `usb_tx_drain` —— 历史 bug：RV32 SMP 上 `usb_cdc_flush()` 里
  drain 与 `g_tx_lock` 的获取方向反了，reboot 卡 34 s。USB 操作一律放到锁外。
- `fs_*`（包括 `fs_lock` 自身）—— 锁顺序是 `proc_lock` 不进 `fs_lock`。
- `echo_drain` —— 会等控制台。

持 `fs_lock` 时禁止：

- `proc_wait` / `wait_for` / `msleep` —— 同样是不许在锁里睡。
- `usb_cdc_*` / `usb_tx_drain` —— 会把 `g_tx_lock` 拉到 `fs_lock` 下面。

`kmalloc` / `kfree` 在 `fs_lock` 下是**允许**的：`fs_lock → heap_lock` 就是
littlefs 分配内存的路径（见上面的顺序表）。

持 `heap_lock` 时禁止 `kmalloc`/`kfree`（不可重入）、`fs_*`（反向）和任何等待。

**USB 锁序**：`usb_cdc_flush()` 必须先 `usb_tx_drain()`，再**单独**取 USB 锁。
`usb_tx_drain()` 内部是 `g_tx_lock` → USB 锁，倒过来取就是两核死锁。这条在
`drivers/` 里，且是**两个函数之间**的顺序，静态检查看不到（见下），只能靠人看 +
T3 复测。

如果某个点确实必须违反上表，在调用行尾加带理由的逃生口，测试才放行：

```c
kmalloc(4); /* LOCKCHECK: allow 生命周期在锁外已分配，这里只是拷贝 */
```

理由必须非空；没有理由的 `LOCKCHECK: allow` 不算豁免。新增豁免要在 code review
里单独说明。

## 静态检查覆盖到哪里

`internal/build/lockcheck_test.go` 只做一件事：在 **`kernel/*.c` 的函数体内**，找
"持锁区间里出现直接调用"的形式。

- **覆盖**：同一个函数里先取锁、几行后直接 `kmalloc`/`kfree`/`proc_wait`/`fs_*`/
  `usb_cdc*`；同一行"取锁后立刻调用"也算。
- **不覆盖**：
  - 跨函数：`fs_lock` 下调用 `lfs_*`，真正分配内存的是它回调里的 `kmalloc`；
  - 跨文件：`drivers/usb/rp2350rv/usb_cdc.c` 里的 `g_tx_lock` 与 USB 锁；
  - 通过函数指针/注册表调用的路径。
- 所以 `HISTORY.md:34` 那次 34 s（`usb_cdc_flush` → `usb_tx_drain` → `g_tx_lock`
  的跨函数倒序）**不会**被这个检查抓到。它是"便宜的本地护栏"，不是锁序证明。

新增锁、改嵌套、动 USB 路径时，仍然以人读上面两张表和 T2/T3 为准。

## 为什么全局锁是当前正确选择

内核现在用全局锁不是偷懒，是刻意不拆：

- 临界区都很短（进程表 16 项扫描、一次状态写、ring 的几条指针运算）。
- 拆分会带来新的锁顺序，而锁顺序错误比争用难查得多——上面 USB 那次 34 s
  卡死就是反例。
- `docs/ARCHITECTURE-REVIEW-2026-11-22.md` 里关于细粒度锁的建议，等真有测量
  数据表明争用是瓶颈再讨论；在那之前不要"顺手拆一把"。

## 已知例外

- `console_write()` 抢不到 `out_lock` 会**整块丢输出**（`kprintf` 用 trylock）。
  这是刻意的：不能为了不丢日志把控制台变成死锁点。
- 跨翻译单元的持锁（`sched_switch()` 解锁、调用者加锁）目前不在静态检查范围：
  检查器按文件跟踪，函数边界处会清掉未配对的锁。这类路径靠 T2/T3 覆盖。
