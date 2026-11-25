# 线程调度 S3a：保护按组重编程：QEMU rv32 / arm（2026-11-25）

```text
commit:            f6521ec
change:            cpu_t.prot 缓存窗口的 6 个输入；apply_protection() 在形状未变时
                   跳过 plat_task_protect()。schedstat 增加 prot 计量，
                   threadbench 打印；kthreadtest 增加两个线程互相 yield 的用例。
baseline:          78ad395（S2）；threadbench/rv32 的基线另测 2ec7d57
target/config:     rv32、rv32smp、arm、esp32c3-generic、rpi-pico(-smp)、
                   rpi-pico2-smp、rpi-pico2-rv32-smp；configs/arm-kthreads.conf、
                   configs/c3-kthreads.conf、临时 rv32 + kthreads 配置
board/port/image:  纯 QEMU：rv32 virt、arm mps2-an385；本机无板
toolchain:         go 1.27.3 windows/amd64；clang 23.1.4；QEMU 11.1.3；Python 3.14.9
host:              Windows / 本机
T0:                PASS
T1:                PASS 17/18（x86 MKFS 溢出，pre-existing，docs/OPEN-ITEMS.md §17）
T2:                PASS（rv32/arm smoke、soak 2/2、kthreadtest on rv32+arm kthreads）
T3:                NOT RUN（本机无 RP2040 / Pico 2 / ATK）
T4:                NOT RUN
logs:              build/t0-s3a.log、build/t1-s3a-all.log、build/t2-s3a-*.log、
                   build/t3a-armk.log、build/t3a-rv32k3.log（不入库）
notes:             实验分支 exp/thread-sched；main 未动
```

## 执行的命令与结果

```sh
go test -count=1 ./...                                            # PASS
go run ./cmd/tos build all                                        # 17/18（x86 见 §17）
go run ./cmd/tos build -target rv32 / -target arm                 # PASS
go run ./cmd/tos build -config configs/arm-kthreads.conf          # PASS
go run ./cmd/tostest smoke -target arm -port 5569 \
    -commands "threadbench,kthreadtest"                           # PASS
go run ./cmd/tos build -config <tmp rv32 + kthreads>              # PASS
go run ./cmd/tostest smoke -target rv32 -port 5572 \
    -commands "hello alpha,kthreadtest"                           # PASS
go run ./cmd/tostest smoke -target rv32 / -target arm             # PASS
python scripts/soak.py --cycles 2 --targets rv32,arm              # 2/2 PASS
```

## 关键输出

```text
# ARM MPU, configs/arm-kthreads.conf
threadbench: 3 threads x 300000: 1 ms  sw=4 self=0 tick=1 prot=6
kthreadtest: 403 sibling switch(es), 0 region set(s)
kthreadtest: all ok (0 failure(s))

# rv32 PMP, 临时 kthreads 配置
kthreadtest: 488 sibling switch(es), 0 region set(s)
kthreadtest: all ok (0 failure(s))

rv32     PASS  tasks      20054016     19584  (2 cycles)
arm      PASS  tasks       3342336      3264  (2 cycles)
2/2 targets passed
```

`prot=6` 是 `threadbench` 阶段内 3 个线程 + 3 个 start record 的 `malloc` 推动 `brk`
造成的（每次 `sbrk` 都要重编程），不是切换造成的；兄弟切换的计数见 `kthreadtest` 的
403/488 → 0。

## 没跑的

- **T3 / T4 `NOT RUN`**：本机没有板；RP2040 / Pico 2 / ATK 的实际镜像由另一侧按
  T3 矩阵跑。这条改动只影响切换路径，QEMU 覆盖了 MPU（arm）和 PMP（rv32）两种实现。
- **rv32 QEMU 的 `threadbench`**：挂起（22 s 无输出、无提示符）。用 `2ec7d57` 的
  worktree 复现，同样是挂起，所以是 **pre-existing**，记在
  `docs/OPEN-ITEMS.md` §18；`kthreadtest` 在同一镜像上正常。
- **x86 释放镜像**：`MKFS: No more free space`，HEAD 上同样失败（§17）。
- **WiFi 镜像**：需要外部 lib 目录，本轮没有构建。
