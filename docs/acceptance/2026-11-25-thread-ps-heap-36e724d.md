# ps 的线程行 + 用户堆并发发现（2026-11-25）

```text
commit:            36e724d
change:            kthreadtest 在睡眠线程存活时打印 ps（验证线程自己的 stk/sz）；
                   并发 malloc/free 压力暴露用户堆不是线程安全的，记为
                   docs/OPEN-ITEMS.md §20（合回 main 前解决），本轮不改 heap.c。
baseline:          3750823（SMP sbrk 卡）
target/config:     configs/rv32-kthreads.conf、configs/rv32smp-kthreads.conf、
                   configs/arm-kthreads.conf、全 18 目标、pump 配置
board/port/image:  纯 QEMU：rv32/rv32smp virt、arm mps2-an385；本机无板
toolchain:         go 1.27.3 windows/amd64；clang 23.1.4；QEMU 11.1.3；Python 3.14.9
host:              Windows / 本机
T0:                PASS
T1:                PASS 17/18（x86 MKFS 溢出，pre-existing，docs/OPEN-ITEMS.md §17）
T2:                PASS（三个 kthreads 配置各 10 次 kthreadtest；smoke/soak 2 轮）
T3:                NOT RUN（本机无板）
T4:                NOT RUN
logs:              build/t-final-*.log、build/t-rep-*.log、build/t2-final-soak2.log
                   （不入库）
notes:             heap 压力用例只在调查时运行，没有留在默认测试里
```

## 执行的命令与结果

```sh
go test -count=1 ./...                                            # PASS
go run ./cmd/tos build all                                        # 17/18（x86 见 §17）
go run ./cmd/tos build -config configs/rv32-kthreads.conf         # PASS
go run ./cmd/tos build -config configs/rv32smp-kthreads.conf      # PASS
go run ./cmd/tos build -config configs/arm-kthreads.conf          # PASS
# 每个配置 10 x kthreadtest
python scripts/soak.py --cycles 2 --targets rv32,arm              # 2/2 PASS
go run ./cmd/tos build -config configs/rpi-pico2-rv32-usbpump.conf # PASS
```

## 关键输出

```text
# ps 里的睡眠线程（rv32 与 arm 相同）
kthreadtest: ps while the thread sleeps:
 pid ppid prio state  c cpu% cpums wink stk/sz  name
   1    0    1 wait  -    0.6     0   64    0.5/4  msh
   2    1    1 run   0    6.7     0  128    0.5/4  kthreadte
  20    2    1 sleep -    0.0     0  128    0.7/4  thr0

# 并发 malloc/free（调查用例，未进默认套件）
rv32（10 次里 2 次）：
  [pid 1171] thread stack overflow ... killing
  kthreadtest: FAIL concurrent malloc/free keeps every block intact
rv32smp（10 次里 1 次）：
  [pid 1636] store access fault at 0x80100514 (address 0x5352515c)
arm：10/10 通过（概率窗口）

kthreadtest 10/10 on rv32, rv32smp, arm（含 ps 行）
rv32 PASS / arm PASS（soak 2 轮）
```

## 没跑的

- **并发 malloc/free 的回归测试**：失败率不定（rv32 2/10、rv32smp 1/10、arm 0/10），
  不能当回归用例；证据和三种修法写在 `docs/OPEN-ITEMS.md` §20，合回 main 前必须
  解决。`__atomic_exchange` 加锁在 RP2040（M0+）与 C3 上链接失败，所以不是
  `heap.c` 一行能修的。
- **T3 / T4 `NOT RUN`**：本机无板；板侧清单见 §19（泵线程）与 §2c/§5c。
- **ARM SMP**：`mps2-an385` 是单核，Cortex-M33 双核只做了构建。
- **x86 释放镜像**：`MKFS: No more free space`，HEAD 上同样失败（§17）。
- **WiFi 镜像**：需要外部 lib 目录，本轮没有构建。
