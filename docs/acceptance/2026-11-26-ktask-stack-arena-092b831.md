# 内核线程按请求分配栈（2026-11-26）

```text
commit:            092b831
change:            process_ktask_create() 的 stack_bytes 真正生效：内核线程栈改为
                   512 B 粒度的 arena，按请求大小（16 B 对齐，最小 1 KiB）分配，
                   退出归还；arena 按镜像给（WiFi 40 KiB、自测+泵 12 KiB、自测 8 KiB、
                   泵 4 KiB）；/proc/ktask 打印线程自己的 size。
baseline:          3891592（按镜像槽数卡）
target/config:     rv32/rv32smp/arm-ktasktest、rpi-pico2-smp(+usb_pump)、18 目标
board/port/image:  QEMU rv32/rv32smp/mps2；C3 COM4；Pico 2 镜像构建（无板）
toolchain:         go 1.27.3；clang 23.1.4；QEMU 11.1.3；Python 3.14.9；esptool
host:              Windows / 本机
T0:                PASS（go test -count=1 ./...、go vet ./...）
T1:                PASS 17/18（x86 MKFS，pre-existing，docs/OPEN-ITEMS.md §17）
T2:                PASS（三个 ktest 配置；默认测试不变）
T3:                PASS（C3 accept 42/42）；泵镜像上板 `NOT RUN`
T4:                NOT RUN
logs:              build/t-arena-<target>.log（不入库）
notes:             第一次实现有分配器 bug，已修；见下
```

## 机制

- 栈从 `ktask_arena[KTASK_ARENA_BYTES]` 里按 512 B 粒度分配，位图一 bit/粒度，
  分配时**置位**、退出时清位（第一版漏了置位，每个线程都拿到 0 号块，
  第二个线程的栈踩进第一个的 paint —— 这正是“用量随块大小增长”的原因）。
- `stack_bytes` 取整到 16 B，下限 1 KiB（trap 帧 + syscall 路径实测 ~600 B），
  上限 arena 大小；`/proc/ktask` 的 size 列现在是该线程的请求大小。
- arena 按 feature 给：WiFi 40,960（6 KiB blob + 4 KiB net/timer + OSI 线程）、
  自测+泵 12,288、自测 8,192（6144 + 1024）、泵 4,096、其余 1,024；
  `-DKTASK_ARENA_BYTES` / `-DKTASK_SLOTS` 仍可覆盖。

## 实测

```text
# 自测（rv32 / rv32smp / mps2），size 是请求值，used 是 paint 扫描
[ktest] create pid=1 exit=2
[ktest] ktask ran 5 sleeps + yield, cpu_bad=0 priv=1   (arm)
[ktest] ktask exit recycled its slot
ktask: slot   size   used     hw  name
0    6144    576    576  ktest      # rv32
0    6144    672    672  ktest      # rv32smp
0    6144    368    368  ktest      # mps2

# 尺寸（同 target 上一版 -> 本版）
rpi-pico2-smp 泵   .bss 134,840 -> 132,792   (4 KiB arena)
arm ktasktest      .bss 190,632 -> 186,544   (8 KiB arena)
```

## 执行的命令与结果

```sh
go test -count=1 ./...                                            # PASS
go vet ./...                                                      # PASS
go run ./cmd/tos build -target <t>                                # 17/18（x86 §17）

go run ./cmd/tos build -target rv32    -config configs/rv32-ktasktest.conf
go run ./cmd/tos build -target rv32smp -config configs/rv32smp-ktasktest.conf
go run ./cmd/tos build -target arm     -config configs/arm-ktasktest.conf
go run ./cmd/tostest smoke -target <t> -commands "cat /proc/ktask"

go run ./cmd/tos build -target rpi-pico2-smp -config configs/rpi-pico2-usbpump.conf
python scripts/esp-c3/flash.py --build -p COM4                     # PASS
python scripts/esp-c3/accept.py -p COM4 --no-reset                 # 42/42
```

## 为什么下限是 1 KiB

溢出检查是 paint 高水位（没有硬件 guard）：trap 帧 144 B（rv32）/88 B（arm）加
`syscall_dispatch → process_ktask_syscall → sched_switch` 的 C 链实测 ~600 B；
USB 泵在 RV32 硬件上是 652 B。512 B 的旧自测请求在“真的只给 512”之后立刻
踩进相邻块（第一版 arena 里就是这样暴露出来的），所以下限取 1024。

## 没跑的

- **无线镜像**：需要外部 lib，未构建；`TOS_WIFI` 的 arena 40 KiB 只是上界。
- **泵镜像上板**：本机无板；`/proc/ktask` 应显示 `usbpump` 与其真实 size/used。
- **x86**：`MKFS: No more free space`（§17）。
