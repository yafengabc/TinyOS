# ktask trap 状态改为每核：SMP 泵解锁（2026-11-25）

```text
commit:            0cdb250
change:            ktask_running/ktask_entry_sp 改成每核：ktask_cpu[MAX_CPUS] 经
                   mscratch 到达（0=flag，4/8=t1/t2 暂存），entry_sp 按 mhartid
                   索引；process_ktask_trap_init() 在 plat_init 和每个次核设置。
                   TRAP_ENTRY 用 mscratch 交换寄存器，不再需要 mhartid 或第二个
                   scratch。泵镜像切回 rpi-pico2-rv32-smp；§21 标记已解决。
baseline:          ff22596（selftest 发现卡）
target/config:     configs/rv32smp-ktasktest.conf（新）、configs/rv32-ktasktest.conf、
                   configs/rv32-kthreads.conf、configs/rv32smp-kthreads.conf、
                   configs/arm-kthreads.conf、configs/rpi-pico2-rv32-usbpump.conf、
                   全 18 目标
board/port/image:  QEMU rv32/rv32smp；ARM 只构建；本机无板
toolchain:         go 1.27.3 windows/amd64；clang 23.1.4；QEMU 11.1.3；Python 3.14.9
host:              Windows / 本机
T0:                PASS
T1:                PASS 17/18（x86 MKFS 溢出，pre-existing，docs/OPEN-ITEMS.md §17）
T2:                PASS（UP/SMP ktask selftest、kthreadtest 10/10 ×3、smoke、soak 10 轮）
T3:                NOT RUN（本机无板；SMP 泵镜像已可交板侧按 §19 跑）
T4:                NOT RUN
logs:              build/t-perhart-*.log、build/t0-perhart.log、
                   build/t1-perhart-all.log、build/t2-perhart-*.log（不入库）
notes:             实验分支 exp/thread-sched；OPEN-ITEMS §21 已解决
```

## 执行的命令与结果

```sh
go test -count=1 ./...                                            # PASS
go run ./cmd/tos build all                                        # 17/18（x86 见 §17）
go run ./cmd/tos build -config configs/rv32-ktasktest.conf         # PASS
go run ./cmd/tos build -config configs/rv32smp-ktasktest.conf      # PASS
go run ./cmd/tostest smoke -target rv32 -port 5628 \
    -commands "cat /proc/ktask"                                   # PASS
go run ./cmd/tostest smoke -target rv32smp -port 5629 \
    -commands "cat /proc/ktask"                                   # PASS（不再 panic）
go run ./cmd/tos build -config configs/rv32-kthreads.conf         # PASS
go run ./cmd/tos build -config configs/rv32smp-kthreads.conf      # PASS
go run ./cmd/tos build -config configs/arm-kthreads.conf          # PASS
# 每个 kthreads 配置 10 x kthreadtest
go run ./cmd/tostest smoke -target rv32 / arm                     # PASS
python scripts/soak.py --cycles 10 --targets rv32,arm             # 2/2 PASS
go run ./cmd/tos build -config configs/rpi-pico2-rv32-usbpump.conf # PASS（SMP）
```

## 关键输出

```text
# 修之前（同一镜像）
kernel stack out of bounds: sp=800ffd40

# 修之后：rv32smp，ktask 在 CPU 0，shell 在 CPU 1
[smp] 2 of 2 CPU(s) online
[ktest] create pid=1
[ktest] ktask ran 5 sleeps + yield, cpu_bad=0
tos> cat /proc/ktask
ktask: slot   size   used     hw  name
  0    6144    640    640  ktest

# UP 仍然正常：656/656 ktest
# kthreadtest：rv32 / rv32smp / arm 各 10/10
# soak：rv32 + arm 10 轮 2/2
```

trap 入口改动是 RV32 专属：ARM 通过 `ktask_dummy_run` 编译，yield/trap 路径没有变化。

## 没跑的

- **T3 / T4 `NOT RUN`**：本机无板；SMP 泵镜像现在可以交板侧按 §19 跑（枚举、
  `/proc/ktask`、21 KiB `cat`、`crash`、`smpbench`、tick）。
- **C3 / RP2350 真机**：只做了构建；C3 是单核，原有的 ktask 路径不受这次改动影响。
- **x86 释放镜像**：`MKFS: No more free space`，HEAD 上同样失败（§17）。
- **WiFi 镜像**：需要外部 lib 目录，本轮没有构建。
