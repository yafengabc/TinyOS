# 用户态线程库：join 归还栈 + cwd/churn 用例（2026-11-25）

```text
commit:            6be8bd7
change:            kthread_create() 的栈与 start record 在 join 成功/创建失败时归还；
                   kthreadtest 增加 cwd 跨线程、200 次 create/join、每线程 yield 计数；
                   新增 configs/rv32-kthreads.conf，与 arm-kthreads 对称。
baseline:          a85be94（S4 卡）
target/config:     configs/rv32-kthreads.conf（rv32 + kthreads）、
                   configs/arm-kthreads.conf、全 18 目标、rpi-pico2-rv32-usbpump
board/port/image:  纯 QEMU：rv32 virt、arm mps2-an385；本机无板
toolchain:         go 1.27.3 windows/amd64；clang 23.1.4；QEMU 11.1.3；Python 3.14.9
host:              Windows / 本机
T0:                PASS
T1:                PASS 17/18（x86 MKFS 溢出，pre-existing，docs/OPEN-ITEMS.md §17）
T2:                PASS（rv32/arm kthreadtest、threadbench、smoke、soak 2/2）
T3:                NOT RUN（本机无板；与本次用户库改动无关）
T4:                NOT RUN
logs:              build/t0-s5.log、build/t1-s5-all.log、build/t5-rv32k-*.log、
                   build/t5-armk-*.log、build/t2-s5-*.log（不入库）
notes:             实验分支 exp/thread-sched；先在旧库上跑出失败，再修
```

## 执行的命令与结果

```sh
go test -count=1 ./...                                            # PASS
go run ./cmd/tos build all                                        # 17/18（x86 见 §17）
go run ./cmd/tos build -config configs/rv32-kthreads.conf         # PASS
go run ./cmd/tostest smoke -target rv32 -port 5589 \
    -commands "kthreadtest"                                       # PASS
go run ./cmd/tos build -config configs/arm-kthreads.conf          # PASS
go run ./cmd/tostest smoke -target arm -port 5590 \
    -commands "kthreadtest,threadbench"                           # PASS
go run ./cmd/tos build -target rv32 / arm                         # PASS
go run ./cmd/tostest smoke -target rv32 / arm                     # PASS
python scripts/soak.py --cycles 2 --targets rv32,arm              # 2/2 PASS
go run ./cmd/tos build -config configs/rpi-pico2-rv32-usbpump.conf # PASS
```

## 关键输出

```text
# 修之前（同一镜像，旧 user/kthread.c）
kthreadtest: 200 create/join cycle(s), heap ... (stopped early)
kthreadtest: FAIL 200 create/join cycles fit in the heap
kthreadtest: FAIL create/join returns the thread stack
kthreadtest: FAILED (6 failure(s))

# 修之后：rv32 PMP
kthreadtest: ok   leader chdir(/bin)
kthreadtest: ok   thread saw the leader's cwd and moved it
kthreadtest: ok   thread chdir moved the whole process
kthreadtest: 200 create/join cycle(s), heap 2148603176 -> 2148603176 bytes
kthreadtest: ok   200 create/join cycles fit in the heap
kthreadtest: ok   create/join returns the thread stack
kthreadtest: ok   both yielding threads ran all rounds
kthreadtest: 472 sibling switch(es), 0 region set(s)
kthreadtest: all ok (0 failure(s))

# arm MPU：同样的用例，405 次兄弟切换 / 0 次重编程，堆前后不变
```

## 没跑的

- **T3 / T4 `NOT RUN`**：本机无板；这次改的是用户态线程库，与 S4 的泵线程一样，
  真机结论仍等 Pico 2 / RP2040 那一侧。
- **`threadbench` 的 ratio**：ARM 上这次打印 `ratio 0%`，是 `clock_ms()` 的 1 ms
  粒度把 inline 阶段量成 0 ms，不是回归；各阶段的 `sw/self/tick/prot` 都正常。
- **x86 释放镜像**：`MKFS: No more free space`，HEAD 上同样失败（§17）。
- **WiFi 镜像**：需要外部 lib 目录，本轮没有构建。
- **线程库的多线程安全**：表本身不是线程安全的（与用户库其余部分一致），
  这次只把这个限制写在声明处，没有加锁。
