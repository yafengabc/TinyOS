# 线程调度 S3b：线程栈边界与守护字：QEMU rv32 / arm（2026-11-25）

```text
commit:            3a17fcc
change:            SYS_THREAD_CREATE 第三参数改为 struct tos_thread_stack *
                   （top + usable size）；线程槽记录自己的栈范围，刷 0xCC、
                   写 USER_STACK_GUARD；线程溢出在所有端口都由 canary 检查并只杀线程。
baseline:          1c9b157（S3a）
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
logs:              build/t0-s3b.log、build/t1-s3b-all.log、build/t2-s3b-*.log、
                   build/t3b-armk2.log、build/t3b-rv32k.log（不入库）
notes:             实验分支 exp/thread-sched；main 未动
```

## 执行的命令与结果

```sh
go test -count=1 ./...                                            # PASS
go run ./cmd/tos build all                                        # 17/18（x86 见 §17）
go run ./cmd/tos build -target rv32 / arm / rv32smp / rpi-pico \
    / rpi-pico2-rv32-smp                                          # PASS
go run ./cmd/tos build -config configs/arm-kthreads.conf          # PASS
go run ./cmd/tos build -config configs/c3-kthreads.conf           # PASS
go run ./cmd/tos build -config <tmp rv32 + kthreads>              # PASS
go run ./cmd/tostest smoke -target arm -port 5569 \
    -commands "kthreadtest"                                       # PASS
go run ./cmd/tostest smoke -target rv32 -port 5576 \
    -commands "kthreadtest"                                       # PASS
go run ./cmd/tostest smoke -target rv32 / arm                     # PASS
python scripts/soak.py --cycles 2 --targets rv32,arm              # 2/2 PASS
```

## 关键输出

```text
# ARM MPU（arm-kthreads）
kthreadtest: ok   create an overflowing thread
[pid 22] thread stack overflow (sp 0x200f6720, stack 0x200f6710..0x200f7720)
[pid 22] a thread stack cannot grow in place; killing
kthreadtest: ok   thread stack overflow killed only the thread
kthreadtest: ok   leader survived the thread stack overflow
kthreadtest: 403 sibling switch(es), 0 region set(s)
kthreadtest: all ok (0 failure(s))

# rv32 PMP（临时 kthreads 配置）
[pid 22] thread stack overflow (sp 0x80116700, stack 0x80116710..0x80117720)
kthreadtest: ok   thread stack overflow killed only the thread
kthreadtest: 444 sibling switch(es), 0 region set(s)
kthreadtest: all ok (0 failure(s))

rv32     PASS  tasks      20054016     19584  (2 cycles)
arm      PASS  tasks       3342336      3264  (2 cycles)
2/2 targets passed
```

`join` 收到 139（与用户态 fault 相同），组长继续运行；这两个镜像都是先跑完
create/join、槽耗尽、fd 共享等全部旧用例，再跑 overflow 和兄弟切换。

## 没跑的

- **T3 / T4 `NOT RUN`**：本机没有板。这条改动影响 `process_stack_guard()` 和线程创建
  路径，RP2040（软件 canary 是唯一机制）与 Pico 2（MPU/PMP + canary）的实际镜像由
  另一侧按 T3 矩阵跑。
- **真实溢出场景**：QEMU 用例是每层递归后 `yield()`，确保第一次越界就在系统调用入口
  被 canary 抓住；`deep 6` 那种"越界后继续跑一段再调用"的形状没有单独构造。
- **x86 释放镜像**：`MKFS: No more free space`，HEAD 上同样失败（§17）。
- **WiFi 镜像**：需要外部 lib 目录，本轮没有构建。
