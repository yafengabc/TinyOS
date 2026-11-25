# E1b 阶段 1：线程表与进程表拆开（2026-11-26）

```text
commit:            948d5b6
change:            调度表改为 threads[MAX_THREADS]（max(2*MAX_PROCS,16)，上限 32）；
                   进程资源搬到 procs[MAX_PROCS]，线程用 t->proc 指向它；
                   PROC_RES/PFD 改指针；exec 分配 proc + 主线程；
                   thread_create 共享 leader 的 proc；ktask 的 proc == NULL 并补齐
                   空指针分支；退休只由主线程释放 proc。身份仍在主线程（E2 搬）。
baseline:          2f8b49a（E1a 代码）+ 07262b9（E1a 卡）
target/config:     rv32、rv32smp、arm、esp32c3-generic；configs/rv32-kthreads、
                   rv32smp-kthreads、arm-kthreads、c3-kthreads、
                   rv32-ktasktest、rv32smp-ktasktest
board/port/image:  QEMU rv32/rv32smp/arm；ESP32-C3 COM4（默认镜像 + c3-kthreads）
toolchain:         go 1.27.3；clang 23.1.4；QEMU 11.1.3；Python 3.14.9；esptool
host:              Windows / 本机
T0:                PASS（go test -count=1 ./...、go vet ./...）
T1:                PASS 17/18（x86 MKFS 溢出，pre-existing，docs/OPEN-ITEMS.md §17）
T2:                PASS（soak 3 轮 3/3；kthreadtest 10/10 ×3；ktest UP/SMP）
T3:                PASS（ESP32-C3：accept 42/42 ×2、kthreadtest all ok、threadbench 100%）
T4:                NOT RUN
logs:              build/t1c-<target>.log、build/t-e1b-*.log（不入库）
notes:             实验分支 exp/thread-sched；E1b 分两段，卡面对应第一段
```

## 执行的命令与结果

```sh
go test -count=1 ./...                                            # PASS
go vet ./...                                                      # PASS
# 18 个 target 逐个 build（build all 会停在 x86）
go run ./cmd/tos build -target <t>                                # 17/18 PASS

# 默认镜像的 T2（每轮 soak 前必须重建默认镜像，否则会拿 kthreads 镜像跑）
go run ./cmd/tos build -target rv32 / rv32smp / arm                # PASS
python scripts/soak.py --cycles 3 --targets rv32,rv32smp,arm       # 3/3 PASS

# 线程镜像：三个配置各 10 轮 kthreadtest
go run ./cmd/tos build -target rv32    -config configs/rv32-kthreads.conf
go run ./cmd/tos build -target rv32smp -config configs/rv32smp-kthreads.conf
go run ./cmd/tos build -target arm     -config configs/arm-kthreads.conf
go run ./cmd/tostest smoke -target <t> -commands "kthreadtest 10"

# ktask selftest（UP + SMP）
go run ./cmd/tos build -target rv32    -config configs/rv32-ktasktest.conf
go run ./cmd/tos build -target rv32smp -config configs/rv32smp-ktasktest.conf

# C3 真机：默认镜像，然后 c3-kthreads，最后刷回默认
python scripts/esp-c3/flash.py --build -p COM4                     # PASS
python scripts/esp-c3/accept.py -p COM4 --no-reset                 # 42/42
go run ./cmd/tos build -target esp32c3-generic -config configs/c3-kthreads.conf
python scripts/esp-c3/flash.py -p COM4
python build/c3_cmd.py COM4 150 "kthreadtest 10"                   # all ok
python scripts/esp-c3/accept.py -p COM4 --no-reset                 # 42/42
python build/c3_cmd.py COM4 90 "threadbench"                       # ratio 100%
python scripts/esp-c3/flash.py --build -p COM4                     # 恢复默认
python scripts/esp-c3/accept.py -p COM4 --no-reset                 # 42/42
```

## 关键输出

```text
# 表尺寸（clang -Xclang -fdump-record-layouts）
rv32  thread_t 352 B（+TOS_KTHREADS 360）  proc_t 384   拆分前 712 B/槽
arm   thread_t 288 B（+TOS_KTHREADS 296）  proc_t 384   拆分前 648 B/槽

# 由 sizeof 计算的表总量（进程/线程 -> E1b / 拆分前）
C3 8/16  : 8704 -> 5696 (+2.9 KiB)      rv32 16/32 : 17408 -> 11392 (+5.9 KiB)
RP2040   : 8704 -> 5696 (+2.9 KiB)      arm  16/32 : 15360 -> 10368 (+4.9 KiB)

# T2
rv32     PASS  tasks      20054016     19584  (3 cycles)
rv32smp  PASS  tasks      19988480     19520  (3 cycles)
arm      PASS  tasks       3342336      3264  (3 cycles)

# kthreadtest（rv32 / rv32smp / arm 各 10 轮）
kthreadtest: 30 thread slot(s) usable beside shell+self
kthreadtest: 4 threads x 400 sbrk(16): 25600 bytes, offset sum 1279200
kthreadtest: all ok (0 failure(s))

# ktask selftest（UP 与 SMP）
[ktest] ktask ran 5 sleeps + yield, cpu_bad=0
  0    6144    656    656  ktest

# C3 真机
42/42 passed                       # 默认镜像
kthreadtest: 14 thread slot(s) usable beside shell+self
kthreadtest: all ok (0 failure(s)) # c3-kthreads
42/42 passed                       # c3-kthreads 镜像
threadbench: ratio 100% (100% = no overhead)
42/42 passed                       # 刷回默认镜像后
```

## 顺带修的测试假设（不是放宽）

新的表长大了，两个旧假设失效，按新表的事实改写：

- `kthreadtest` 的槽位耗尽循环原来是 `for i < 16` 且断言 `n < 16`。表变成 32 槽后
  16 次永远填不满，断言按定义失败。改为 40 次尝试、断言 `n < 40`，并把每次的栈
  从 4 KiB 降到 API 最小值 512 B——用例本意是“先耗尽表、不是先耗尽堆”，原来靠
  4 KiB×14 恰好没先碰到堆；512 B 下 C3 上是表先满（14），rv32 上是 30。
- 并发 sbrk 用例的基线原来在创建 4 个线程之前取。`kthread_create()` 的栈来自
  malloc，而 malloc 在空闲链表不够时会用 sbrk 扩堆，于是这段增长被算进了
  “线程要求的字节数”（C3 上 33920 ≠ 25600）。现在线程先自旋等一个 go 标志，
  基线在四个栈都分配完之后取，测量窗口里只有 sbrk 本身。
- `KT_TRACK 24` 的上限注释写的是“大于进程表（18）”。线程表 32 槽后改为 40，
  否则 join 之后找不到那条记录、栈无法归还。

## 实现过程中修掉的一个真 bug

第一版在 `exec()` 里把 `live_threads` 设成 1（把主线程也算作线程）。`process_exit()`
看到 `live_threads > 0` 就走 `begin_group_exit_locked()`，等“最后一个线程”收尾——
可主线程自己永远不会再减，于是每个退出都挂在那里，`ls` 之后再无提示符。
`live_threads` 只数工作线程（`thread_create()` 加、`thread_exit()` 减），主线程是
决定者、不在计数里；改为 0 后 `ls`/`uptime`/`free` 全部正常。

## 没跑的

- **M33 / RP2040 / Pico 2 真机**：本机只有 C3 的 COM4；ARM(M3) 与 RV32 的线程镜像
  已在 QEMU 上跑过，真机清单在 `docs/OPEN-ITEMS.md` §22。E3（ARM 内核线程）
  需要 M33 板子，仍未开始。
- **T4**：没有对应测试项。
- **x86**：`MKFS: No more free space`，HEAD 上同样失败（§17），本轮未深挖。
- **WiFi 镜像**：需要外部 lib 目录，本轮没有构建。
- **RP2350 泵镜像**：S4 的 USB 泵板测属于 §19，本轮未重跑。
