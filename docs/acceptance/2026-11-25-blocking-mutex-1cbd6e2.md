# 阻塞式 mutex：睡眠、移交、持有者死亡释放（2026-11-25）

```text
commit:            1cbd6e2
change:            新增 SYS_MUTEX_LOCK(56) 与 ST_WAIT_MUTEX：占用时线程睡眠，
                   unlock 把所有权直接交给第一个等待者；线程死亡时交出其持有的字；
                   ps 显示 mutex。user/heap.c 改用阻塞锁；TRY 保留。
                   kthreadtest 增加阻塞等待与持有者 fault 两个用例。
baseline:          0fc2ea4（heap mutex 卡）
target/config:     configs/rv32-kthreads.conf、configs/rv32smp-kthreads.conf、
                   configs/arm-kthreads.conf、c3-kthreads、全 18 目标、pump 配置
board/port/image:  纯 QEMU：rv32/rv32smp virt、arm mps2-an385；本机无板
toolchain:         go 1.27.3 windows/amd64；clang 23.1.4；QEMU 11.1.3；Python 3.14.9
host:              Windows / 本机
T0:                PASS（含 syscall snapshot 56 与 goc32）
T1:                PASS 17/18（x86 MKFS 溢出，pre-existing，docs/OPEN-ITEMS.md §17）
T2:                PASS（rv32/rv32smp/arm 各 10 次 kthreadtest；smoke、soak 2/2）
T3:                NOT RUN（本机无板）
T4:                NOT RUN
logs:              build/t0-block.log、build/t1-block-all.log、build/t-block-*.log、
                   build/t2-block-*.log（不入库）
notes:             实验分支 exp/thread-sched；OPEN-ITEMS §20 的后续项已完成
```

## 执行的命令与结果

```sh
go test -count=1 ./...                                            # PASS
go run ./cmd/tos build all                                        # 17/18（x86 见 §17）
go run ./cmd/tos build -target rpi-pico / esp32c3-generic         # PASS
go run ./cmd/tos build -config configs/rv32-kthreads.conf         # PASS
go run ./cmd/tos build -config configs/rv32smp-kthreads.conf      # PASS
go run ./cmd/tos build -config configs/arm-kthreads.conf          # PASS
go run ./cmd/tos build -config configs/c3-kthreads.conf           # PASS
# 每个 kthreads 配置 10 x kthreadtest
go run ./cmd/tostest smoke -target rv32 / arm                     # PASS
python scripts/soak.py --cycles 2 --targets rv32,arm              # 2/2 PASS
go run ./cmd/tos build -config configs/rpi-pico2-rv32-usbpump.conf # PASS
```

## 关键输出

```text
# 等待中的线程：state=mutex，cpu 0.0（不在运行队列，不是自旋）
kthreadtest: ps with a mutex waiter:
 pid ppid prio state  c cpu% cpums wink stk/sz  name
   1    0    1 wait  -    0.1     0   64    0.5/4  msh
   2    1    1 run   0   16.3     0  128    0.5/4  kthreadte
 232    2    1 mutex -    0.0     0  128    0.0/4  thr2

kthreadtest: ok   leader took the test mutex
kthreadtest: ok   the waiter is blocked, not spinning
kthreadtest: ok   leader released the test mutex
kthreadtest: ok   the blocked thread ran after the handoff
kthreadtest: ok   the waiter saw its critical section
kthreadtest: ok   mutex is free after the waiter exited

# 持有者 fault：join 139，字被交回
[pid 233] store access fault at 0x80101a2a
kthreadtest: ok   create a thread that dies holding a mutex
kthreadtest: ok   the holder died on its fault
kthreadtest: ok   a dead holder released the mutex
kthreadtest: ok   concurrent malloc/free keeps every block intact
kthreadtest: all ok (0 failure(s))
```

三个配置各 10/10，包含并发 malloc/free 压力。

## 没跑的

- **T3 / T4 `NOT RUN`**：本机无板；板侧清单仍是 §19（泵线程）与 §2c/§5c。
- **多锁/嵌套**：内核每个线程只跟踪一个持有字（堆锁够用）；同时持两把锁时的死亡
  释放不在覆盖范围，嵌套也可能死锁，写在 `lib.h` 的 API 注释里。
- **超时/公平性**：没有超时；等待者按进程表顺序选取，近似 FIFO，没有严格排队。
- **ARM SMP 并发**：`mps2-an385` 是单核，M33 双核只做了构建。
- **x86 释放镜像**：`MKFS: No more free space`，HEAD 上同样失败（§17）。
- **WiFi 镜像**：需要外部 lib 目录，本轮没有构建。
