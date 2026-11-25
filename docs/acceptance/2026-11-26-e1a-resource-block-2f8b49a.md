# E1a：进程窗口迁进资源块（2026-11-26）

```text
commit:            2f8b49a
change:            win_base/win_size/wx_start/heap_base/heap_end/brk 从调度槽迁到
                   proc_res；entry/user_esp 改成 process_exec 局部并删字段；
                   win_alloc/retire 按槽扫块/清块；线程不再拷 heap/brk；
                   窗口不变量断言。行为不变。
baseline:          c45d365（E0 文档 + 名字决定）
target/config:     rv32、rv32smp、arm、esp32c3-generic、rpi-pico、rpi-pico2-smp；
                   三个 kthreads 配置、两个 ktask selftest 配置、c3-kthreads
board/port/image:  QEMU rv32/rv32smp/arm；ESP32-C3 COM4（默认镜像 + c3-kthreads）
toolchain:         go 1.27.3；clang 23.1.4；QEMU 11.1.3；Python 3.14.9；esptool
host:              Windows / 本机
T0:                PASS
T1:                PASS 17/18（x86 MKFS 溢出，pre-existing，docs/OPEN-ITEMS.md §17）
T2:                PASS（smoke、soak 10 轮、kthreadtest 10/10 ×3、ktest UP/SMP）
T3:                PASS（ESP32-C3：accept 42/42、kthreadtest all ok）
T4:                NOT RUN
logs:              build/t0-e1a.log、build/t1-e1a-all.log、build/t-e1a-*.log、
                   build/t2-e1a-*.log（不入库）
notes:             实验分支 exp/thread-sched；E1a 只动资源归属，不改表
```

## 执行的命令与结果

```sh
go test -count=1 ./...                                            # PASS
go run ./cmd/tos build all                                        # 17/18（x86 见 §17）
go run ./cmd/tos build -target rv32 / arm                         # PASS
go run ./cmd/tostest smoke -target rv32 / arm                     # PASS
python scripts/soak.py --cycles 10 --targets rv32,arm             # 2/2 PASS

# 窗口泄漏：free -> 20 x run hello -> free
go run ./cmd/tostest smoke -target rv32 -port 5635 \
    -commands "free,run hello,...(20),free"                      # tasks 19584 -> 19584

# 三个 kthreads 配置各 10 x kthreadtest
go run ./cmd/tos build -config configs/rv32-kthreads.conf         # PASS
go run ./cmd/tos build -config configs/rv32smp-kthreads.conf      # PASS
go run ./cmd/tos build -config configs/arm-kthreads.conf          # PASS
# 两个 ktask selftest：cpu_bad=0，/proc/ktask 656/656

# C3 真机
python scripts/esp-c3/flash.py --build -p COM4                    # PASS
python scripts/esp-c3/accept.py -p COM4 --no-reset                # 42/42
go run ./cmd/tos build -config configs/c3-kthreads.conf && flash
python build/c3_cmd.py COM4 60 "kthreadtest"                      # all ok
python scripts/esp-c3/flash.py --build -p COM4                    # 恢复默认
```

## 关键输出

```text
# 窗口泄漏（rv32）
tasks      20054016     19584
... 20 次 run hello ...
tasks      20054016     19584
SMOKE TEST OK

# 尺寸（TOS_KEEP_SYMS，16 槽）
rv32: procs 5376（336 B/槽）+ proc_res 6016（376 B） = 712 B/进程
ARM : procs 4352（272 B/槽）+ proc_res 6016（376 B） = 648 B/进程
（E1a 前：720 / 656；entry+user_esp 不再占槽）

# C3
42/42 passed
kthreadtest: all ok (0 failure(s))

# ktask selftest（UP 与 SMP）
[ktest] ktask ran 5 sleeps + yield, cpu_bad=0
0    6144    656    656  ktest

# kthreadtest 10/10 ×3（rv32 / rv32smp / arm）
kthreadtest: all ok (0 failure(s))
```

## 没跑的

- **M33 / RP2040 真机**：本机无板（只有 C3 的 COM4）；两种架构的 kthreads 镜像
  构建通过，真机清单在 `docs/OPEN-ITEMS.md` §22。
- **T4**：没有对应测试项。
- **x86 释放镜像**：`MKFS: No more free space`，HEAD 上同样失败（§17）。
- **WiFi 镜像**：需要外部 lib 目录，本轮没有构建。
