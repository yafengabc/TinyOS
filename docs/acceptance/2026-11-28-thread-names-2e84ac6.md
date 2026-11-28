# 线程 API 改名 + threads 镜像放行主线程栈检查（2026-11-28）

```text
commit:            2e84ac6  user: kernel threads are thread_create,
                            cooperative ones are co_*
change:            TOS_KTHREADS：thread_create/join/exit、
                   thread_mutex_lock/unlock/try；
                   TOS_THREADS：co_create/join/exit/yield/read/self、
                   co_mutex_t/*。纯 C API 改名，syscall 编号/ABI/快照不变。
                   TOS_THREADS 镜像里 process_stack_guard() 对主线程放行；
                   kthreadtest 的 exec 探针从 /bin/echo 改为 /bin/ls
target/board:      x86（threads=1，QEMU）；ESP32-C3（kthreads，实机）；
                   18 个构建目标；rp2040/pico2 的 kthreads 配置只构建
toolchain:         go 1.27.x；clang/ld.lld 23.1.4
T0:                PASS（go test ./...、go vet、goc32 + native32 测试）
T1:                PASS 18/18；另构建 rpi-pico-kthreads、rpi-pico2-smp-kthreads
T2:                PASS smoke rv32 / rv32smp / arm
T3:                x86 threads PASS（QEMU）；C3 kthreads PASS（kthreadtest
                   all ok、prodcons 32768 B）；C3 默认 accept 42/42；
                   rp2040 / rp2350 板测 NOT RUN
T4:                NOT RUN
logs:              build/t-fin-*.log、build/t-rn-*.log（本地，不入库）
```

## 命名

| 语义 | 旧 | 新 |
|---|---|---|
| 内核调度、U-mode、栈内核知道（推荐） | `kthread_create/join/exit`、`kmutex_*` | **`thread_create/join/exit`、`thread_mutex_*`** |
| 纯用户态、堆内栈、只在 yield 切换（不推荐） | `thread_create/...`、`mutex_t` | **`co_create/...`、`co_mutex_t`** |

syscall 本来就叫 `SYS_THREAD_CREATE/EXIT/JOIN`（48–50），所以这是纯源码级改名：
`abi/syscalls.csv` 和 goc32 快照没有变化。SDK 不提供别名——`thread_create`
的语义变了，别名只会让旧代码静默编到另一套 API 上。

## threads 镜像的栈检查取舍

合作线程的 sp 在 `TOS_HEAP` 里，内核只有主线程的栈区间可比。`TOS_THREADS`
现在通过 `featureDefines()` 进入内核编译，`process_stack_guard()` 里：

```c
#ifdef TOS_THREADS
    if (thread_is_main(p))
        return 1;
#endif
```

- 收益：x86/RP2040/rp2350-ARM 上合作线程的 syscall 不再被误杀；
- 代价：该镜像失去主线程的 syscall 栈检查（x86/RP2040 上是唯一的栈边界）；
  worker kthread 不是主线程，检查保留；
- `user/lib.h`、`configs/x86-threads.conf`、`docs/OPEN-ITEMS.md` §30 都注明
  “合作线程不推荐，新代码用 `thread_create()`”。

## 顺手修的两处

- `kthreadtest` 的 “exec works again after the table drains” 还在 exec
  `/bin/echo`，而 echo 早在 multi-call 合并时进了 `/bin/tools`；改用
  `/bin/ls`（每个镜像都有），这是那项一直 FAIL 的原因；
- `user/heap.c` 的对齐断言从“带三元的数组长度”改成 `_Static_assert`，
  否则 toscc 无法解析这个文件（下一个提交要链接它）。

## 验证

```text
x86 threads:   threadtest -> worker 1..6 + done, counter=6；swcost 正常
C3 kthreads:   kthreadtest -> all ok (0 failure(s))
               prodcons   -> 32768 bytes, both threads blocked and woke
C3 default:    accept 42/42
```

- `TOS_THREADS` 下 `deep` 在 x86 镜像里不再有 syscall 栈检查（预期，文档已写）；
- rp2040/pico2 的 kthreads 配置只做了构建，板上 T3 记 `NOT RUN`。
