# 内核线程池按镜像实际使用量分配（2026-11-26）

```text
commit:            edebfb3
change:            KTASK_SLOTS 从固定 6 改为按 feature 推导（WiFi 6、自测+泵 3、
                   自测 2、其余 1），仍可用 -DKTASK_SLOTS 覆盖。单线程的 USB 泵
                   镜像不再付 36 KiB，只付 6 KiB。
baseline:          845b600（ARM 泵迁移卡）
target/config:     rpi-pico2-smp(+usb_pump)、rpi-pico2-rv32-smp(+usb_pump)、
                   arm/rv32/rv32smp-ktasktest、18 目标
board/port/image:  QEMU mps2/rv32/rv32smp；C3 COM4；Pico 2 镜像构建（无板）
toolchain:         go 1.27.3；clang 23.1.4；QEMU 11.1.3；Python 3.14.9；esptool
host:              Windows / 本机
T0:                PASS（go test -count=1 ./...、go vet ./...）
T1:                PASS 17/18（x86 MKFS，pre-existing，docs/OPEN-ITEMS.md §17）
T2:                PASS（三个 ktest 配置各多次；默认测试不受影响）
T3:                PASS（C3 accept 42/42）；泵镜像上板 `NOT RUN`（无板）
T4:                NOT RUN
logs:              build/t-slots-*.log（不入库）
notes:             上一条泵迁移卡的 +36,904 B 是改动前的数字，本条取代它
```

## 为什么原来是 6

`KTASK_SLOTS` 是“一个镜像最多同时有几个内核线程”。只有 ESP32-C3 的 WiFi OSI
需要多个（blob 在不同队列上由不同线程泵），其余都只用 1 个；池是静态均匀数组，
不能发半个槽，所以按峰值算——代价是只跑一个泵的镜像也付 6 个槽。

## 现在的规则

```c
#if defined(TOS_WIFI)                         6    /* OSI 的线程 */
#elif defined(TOS_KTASK_TEST) && defined(TOS_USB_PUMP)  3
#elif defined(TOS_KTASK_TEST)                 2    /* 一个睡、一个退出 */
#else                                         1    /* 泵 */
#endif
```

`#ifndef KTASK_SLOTS` 保留，单个镜像仍可用 `-DKTASK_SLOTS` 覆盖；槽大小
`KTASK_STACK_BYTES` 仍是 6144（实测高水位：M3 352 B、RV32 泵 652 B）。

## 实测（同 target 前后对比）

```text
rpi-pico2-smp 泵镜像   .bss 165,608 -> 134,840   (-30,768 = 5 个槽)
arm ktasktest 镜像     .bss 215,240 -> 190,632   (-24,576 = 4 个槽)
默认镜像               不变（--gc-sections 丢掉池）
```

## 执行的命令与结果

```sh
go test -count=1 ./...                                            # PASS
go vet ./...                                                      # PASS
go run ./cmd/tos build -target <t>                                # 17/18（x86 §17）

# 自测（2 个槽）：ktest + kexit，随后 kexit2 复用空出的槽
go run ./cmd/tos build -target arm     -config configs/arm-ktasktest.conf
go run ./cmd/tos build -target rv32    -config configs/rv32-ktasktest.conf
go run ./cmd/tos build -target rv32smp -config configs/rv32smp-ktasktest.conf
go run ./cmd/tostest smoke -target <t> -commands "cat /proc/ktask"   # 各 3 次

# 泵镜像
go run ./cmd/tos build -target rpi-pico2-smp -config configs/rpi-pico2-usbpump.conf
go run ./cmd/tos build -target rpi-pico2-rv32-smp -config configs/rpi-pico2-rv32-usbpump.conf

python scripts/esp-c3/flash.py --build -p COM4                     # PASS
python scripts/esp-c3/accept.py -p COM4 --no-reset                 # 42/42
```

## 关键输出

```text
# mps2 / rv32 / rv32smp（2 个槽够用，且退出后能复用）
[ktest] create pid=1 exit=2
[ktest] ktask ran 5 sleeps + yield, cpu_bad=0 priv=1
[ktest] ktask exit recycled its slot

# 尺寸
rpi-pico2-smp 泵   text 186,608  bss 134,840
arm ktasktest      text  94,784  bss 190,632
```

## 没跑的

- **泵镜像上板**：本机无板；`/proc/ktask` 应显示 `usbpump` + 高水位，§19/§22 清单。
- **WiFi 镜像**：需要外部 lib 目录，本轮未构建；`TOS_WIFI` 仍是 6 槽（行为不变）。
- **x86**：`MKFS: No more free space`（§17）。
