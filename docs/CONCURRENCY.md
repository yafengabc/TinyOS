# 内核锁顺序与持锁规则

这份文档是内核里所有全局锁的**唯一**总表。`kernel/process.c`、`kernel/ramdisk.c`、
`kernel/pipe.h` 的局部注释各自都对，但只覆盖自己那个文件；需要判断"这把锁持有时
能不能调那个函数"时，以这里为准。`internal/build/lockcheck_test.go` 在 T0 里静态
检查下面「禁止事项」一节，违规会报 `file:line: <call> inside <lock>`。

## 全局锁清单与顺序

```
proc_lock  →  { fs_lock → heap_lock,  p->lock,  out_lock,  g_tx_lock }
mb_lock     (独立，不与上面任何锁嵌套)
```

- `proc_lock`（`kernel/process.c`）：保护进程表、调度状态、FD 表。SMP 上两个核同时
  从表里挑任务，一把锁覆盖整张表是诚实的选择——临界区只有 16 项扫描加一次状态
  写入，细分锁在边界检查上的开销比省下的争用更大。**不提前拆。**
- `fs_lock`（`kernel/ramdisk.c`）：保护 littlefs 的 ofile 表和块设备访问。
  顺序必须是 `fs_lock → heap_lock`，因为 littlefs 通过 `kmalloc` 分配；反向没有
  任何调用点。
- `heap_lock`（`kernel/heap.c`）：保护内核堆的空闲链表。只允许作为 `fs_lock` 的
  下一级拿，不独立跨函数调用。
- `p->lock`（`kernel/pipe.h`）：pipe ring 的叶子锁，在 `proc_lock` 之下。ring 和
  挂在它上面的任务由同一次调用决定——"pipe 空了"和"我睡了"必须是一步。
- `out_lock`（`kernel/console.c`）：只保护控制台的输出序列化。`console_write()`
  抢不到锁时整块丢，但同核重入必须等待，否则命令行 ≥16 字符会没输出。
- `g_tx_lock`（USB CDC）：只保护 TX ring。与 `out_lock` 的关系见下。
- `mb_lock`（`kernel/mailbox.c`）：独立的 leaf lock，不与上面任何锁嵌套。

`proc_lock` 之下**唯一**允许的下一级是上面花括号里的那几把；花括号之间互不嵌套。
任何新增的锁嵌套关系必须先改这张表，再改代码。

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

持 `fs_lock` 时禁止 `proc_wait`。

**USB 锁序**：`usb_cdc_flush()` 必须先 `usb_tx_drain()`，再**单独**取 USB 锁。
`usb_tx_drain()` 内部是 `g_tx_lock` → USB 锁，倒过来取就是两核死锁。

如果某个点确实必须违反上表，在调用行尾加带理由的逃生口，测试才放行：

```c
kmalloc(4); /* LOCKCHECK: allow 生命周期在锁外已分配，这里只是拷贝 */
```

理由必须非空；没有理由的 `LOCKCHECK: allow` 不算豁免。新增豁免要在 code review
里单独说明。

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
