# 平台接口契约（`plat_*`）

这份文档回答一个问题：**这个 `plat_*` 能不能在这里调？**
每个函数自己的头注释仍然是最权威的细节；这里记录的是跨接口的规则和分类，
新增接口时按这里的类别选，不要发明第四种上下文。

接口分五个文件：`kernel/platform.h`（聚合 + 内存布局/boot/外设）与
`kernel/plat/{console,irq,block,memprot}.h`。`platform.h` 仍然 include 全部，
所以旧代码不受影响；**新外设接口只进窄头**（`internal/build/include_test.go`
会拒绝 platform.h 里的新 `plat_*` 声明）。

## 1. 上下文类别

### A. 临界区 / 中断里可调用

会被在**持锁、关中断、甚至 tick 内部**调用。硬规则：

- 不取任何内核锁（所有锁都在这一层之上）；
- 不睡眠、不调调度器、不跑需要线程上下文的状态机；
- 每次等待都有上界（宁可丢一个字符，也不能让板子静默挂住）。

成员：`plat_console_{init,putc,tx_room,flush,getchar}`、`plat_irq_{save,restore,
disable,enable}`、`plat_ticks`、`plat_tick_hz`、`plat_cpu_id`、`plat_cpu_hz`、
`plat_fault_is_user`、`plat_frame_{get_ret,get_syscall,get_user_sp,set_arg,set_ret}`、
`plat_pre_tick_spins`（纯内联算术）。

细节以 `kernel/plat/console.h` 开头那段为准，尤其是：`putc` 允许丢、
`getchar` 只能做一次寄存器读（它在 `tty_poll()` 里、中断已关）、
`flush` 只在关机路径调用且必须有界。

### B. 任务上下文，同步但不睡眠

会做耗时的设备操作（SPI flash、SD、I2C），但都是**轮询完成**，不进入睡眠，
所以可以在持 `fs_lock` 的情况下调用；也因此**不能**从 ISR 调用。

成员：`plat_block_{read,write,erase,size,count}`、`plat_lfs_blocks`、
`plat_flash_{detected_bytes,exclusive_begin,exclusive_end,probe_debug,unique_id}`、
`plat_{gpio,adc,pwm,spi,i2c,pio,dma}_*`、`plat_dcache_{clean,invalidate}`、
`plat_icache_sync`、`plat_uart_{count,debug,read,tx_probe,write}`、
`plat_usb_poll_ns`。

`plat_flash_exclusive_begin/end` 必须成对：begin 让其它核离开 XIP/缓存路径，
end 恢复；中间只能做 flash 写，不能调用会打印或取锁的东西（打印本身可能走
XIP 常量）。

### C. Boot / 执行流控制

只在特定阶段调用，或**不返回**：

- boot：`plat_early_init`、`plat_init`、`plat_boot_stage`、`plat_start_cpu`、
  `plat_cpu_secondary_init`、`plat_timer_init`、`plat_timer_start_cpu`；
- 上下文切换/系统调用返回：`plat_task_protect`、`plat_prepare_user_frame`、
  `plat_enter_user`（不返回）、`plat_set_kernel_stack`；
- 终止：`plat_reboot`、`plat_shutdown`、`plat_panic_halt`（都不返回）。

## 2. 跨接口的不变量

1. **中断状态**：能用 `plat_irq_save/restore` 就不要用 `disable/enable` 配对。
   临界区可能既从系统调用（中断开）进入，又从 tick（中断已关）进入；
   `enable()` 会让后者提前开中断，而且 ARM 上 `enable()` 第一次运行还会解屏蔽
   tick 源。save/restore 必须用**同一个** state，且不能跨函数保存。
2. **锁**：平台层不取锁。需要串行化的状态（console 输出、block 设备、
   设备寄存器）由调用者用内核锁保护；锁顺序见 `docs/CONCURRENCY.md`。
3. **窗口保护**：`plat_task_protect` 的段按地址升序、互不重叠；
   **绝不能覆盖段之间的空洞**——那个洞就是 `prot heap` / `prot stack` 的检查对象，
   覆盖它等于把两个测试静默关掉。
4. **栈**：`TOS_STACK()` 只接受 2 的幂、至少一页、且整除 `PLAT_APP_GRAIN`
   （需要栈对齐的端口），否则 exec 拒绝；默认栈还要满足 `layout_check()` 的四条。
5. **块设备**：`FS_BLOCK_SIZE=4096`，`plat_block_read/write` 按扇区（512 B）寻址，
   写入前必须擦除（flash 语义），littlefs 负责这一点；驱动不要自己缓存状态。
6. **时间**：`plat_ticks` 是调度 tick 计数，`plat_now_ns` 是自由运行计数器，
   两者都可能被重载/校准，但都保证单调；不要在 ISR 里做 64 位除法。
7. **console 与调试输出分离**：`plat_uart_*` 是设备接口（可能阻塞、可能在任务里
   长时间占用），`plat_console_*` 是内核控制台（A 类）。不要把诊断打印塞进
   console 的 putc。

## 3. 新增接口怎么归类

- 需要被 ISR/持锁路径调用 → A 类，必须证明"不取锁、不睡眠、等待有界"；
- 设备读写 → B 类，进对应的窄头，并说明最坏耗时量级；
- 只在启动或关机用 → C 类，在注释里写明"仅 boot"/"不返回"。

`platform.h` 只保留内存布局、boot 和现有外设声明；新东西进
`kernel/plat/<class>.h`。`include_test.go` 会把 platform.h 里任何新增的
`plat_*` 声明当成错误。
