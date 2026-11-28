# 合作线程：默认 2 KiB 栈 + 软件守卫（2026-11-28）

```text
commit:            3dde040  user: cooperative thread stacks default to 2 KiB
                            and are guarded
change:            thread_create(fn,arg,0) 默认 4096 -> 2048；每个线程栈底部
                   加 32 B 守卫（canary + 每次切换点的 sp 检查），越界打印
                   "thread: stack overflow, killing the process" 并 exit(139)；
                   OPEN-ITEMS 30 记录既有的 x86/RP2040 syscall 误杀
target/board:      rv32 QEMU（threads = 1）；C3 默认镜像（threads 关闭）；
                   18 个构建目标
toolchain:         go 1.27.x；clang/ld.lld 23.1.4
T0:                PASS（go test ./...、go vet）
T1:                PASS 18/18（默认配置 threads 关闭，thread.c 不参与这些镜像）
T2:                PASS smoke rv32 / rv32smp / arm；另有 rv32 + threads=1 的
                   threadtest / 越界用例
T3:                C3 未重刷（线程关闭，镜像不变）；rp2040 / rp2350 NOT RUN
T4:                NOT RUN
logs:              build/t-thg-*.log、build/t-tg-*.log（本地，不入库）
```

## 改动

- 默认栈：`thread_create(fn, arg, 0)` → **2048 B**（显式传 4096 的调用不受影响），
  最小值仍是 512；
- 守卫：每个线程的 malloc 块多要 32 B，块首写 `"THDR"` canary，可用栈从
  `base+32` 开始；`struct thread` 记住 base；
- 检查点：`thread_yield` 开头、`thread_switch` 返回后、`thread_exit` 开头；
  读当前 sp（rv32 `mv`、Thumb `mov`、i386 `movl %%esp`；其他宿主返回 0 时跳过），
  只要 `sp < base+32` 或 canary 被改就 `exit(139)`。

**限制**：内核不知道合作线程的栈（它们在 `TOS_HEAP` 里），所以这是“下一次 yield
才发现”的检查，和 RP2040 的 canary 同类：发现之前的写入已经发生，进程被杀而不是
带着坏堆继续跑。真正的事前边界需要 MPU/PMP 区域，而线程栈在堆内部，做不到。

## 验证（rv32 QEMU，`threads = 1`）

```text
tos> threadtest
worker 1: 1 ... worker 2: 6
done, counter=6

tos> tguard                     # 临时程序：默认 2K 栈上递归 3 x 1K
thread: stack overflow, killing the process
tos>                             # shell 继续
```

`tguard` 是临时程序，验证后删除，不入库。

## 既有问题：x86/RP2040 的 syscall 误杀（OPEN-ITEMS 30）

x86 和 RP2040 的 `plat_user_stack_guard()` 返回 1，内核每次 syscall 拿当前 sp 去比
**主线程**的栈区。合作线程在堆里调用 `printf`/`poll` 时 sp 远低于主栈，于是被当成
主线程爆栈杀掉：

```text
[pid 2] stack overflow (sp 0x00112fd0, stack 0x0011f000..0x00120000)
[pid 2] no room to grow it; killing
```

用**未修改**的 `thread.c` 复现同样结果（x86-threads 配置 + `threadtest`），所以是
既有问题。rv32/ARM 的 `plat_user_stack_guard()` 返回 0，主线程检查被跳过，那边正常。

修法需要内核知道合作线程的栈区间（注册 syscall），是 ABI 变更；已记入
`docs/OPEN-ITEMS.md` §30，未做。
