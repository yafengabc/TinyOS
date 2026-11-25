# 线程调度 S4：USB 泵作为固定内核线程（构建 + QEMU 回归）：2026-11-25

```text
commit:            a4cd7e2
change:            feature usb_pump：usb_cdc_poll() 从 trap/idle 钩子移进一个
                   prio 0、固定 core 0 的 ktask（sleep 1 tick）；新增 proc_t.home_cpu
                   亲和、process_ktask_create(cpu)、plat_start_tasks() 板级钩子；
                   /proc/ktask 在 TOS_USB_PUMP 下编译并显示线程名。
baseline:          15afbd2（S3b）
target/config:     configs/rpi-pico2-rv32-usbpump.conf（rpi-pico2-rv32-smp + usb_pump）；
                   全 18 目标；rv32/arm QEMU；arm-kthreads、临时 rv32 kthreads
board/port/image:  纯构建 + QEMU；泵线程需要 USB 控制器 → T3 NOT RUN（本机无板）
toolchain:         go 1.27.3 windows/amd64；clang 23.1.4；QEMU 11.1.3；Python 3.14.9
host:              Windows / 本机
T0:                PASS
T1:                PASS 17/18（x86 MKFS 溢出，pre-existing，docs/OPEN-ITEMS.md §17）
T2:                PASS（rv32/arm smoke、soak 2/2、kthreadtest on rv32+arm kthreads）
T3:                NOT RUN（Pico 2 RV32 板测清单见 docs/OPEN-ITEMS.md §19）
T4:                NOT RUN
logs:              build/t0-s4c.log、build/t1-s4b-all.log、build/t2-s4-*.log、
                   build/t-s4-pump*.log（不入库）
notes:             实验分支 exp/thread-sched；默认镜像不含 usb_pump，发布行为不变
```

## 执行的命令与结果

```sh
go test -count=1 ./...                                            # PASS
go run ./cmd/tos build all                                        # 17/18（x86 见 §17）
go run ./cmd/tos build -config configs/rpi-pico2-rv32-usbpump.conf # PASS
go run ./cmd/tos build -target rpi-pico2-rv32-smp                 # PASS（feature 关）
go run ./cmd/tos build -target rv32 / arm                         # PASS
go run ./cmd/tostest smoke -target rv32 / arm                     # PASS
go run ./cmd/tos build -config configs/arm-kthreads.conf          # PASS
go run ./cmd/tostest smoke -target arm -port 5581 \
    -commands "threadbench,kthreadtest"                           # PASS
go run ./cmd/tos build -config <tmp rv32 + kthreads>              # PASS
go run ./cmd/tostest smoke -target rv32 -port 5582 \
    -commands "kthreadtest"                                       # PASS
python scripts/soak.py --cycles 2 --targets rv32,arm              # 2/2 PASS
```

## 关键输出

```text
# 启用 usb_pump（TOS_KEEP_SYMS=1）
kernel-rpi-pico2-rv32-smp.elf  .bss 133968
  ktask_stacks  36864        # 6 x 6 KiB 池被链接进来
  "usbpump" / "[usb] pump thread pid" 都在镜像里

# 同一目标、feature 关
kernel-rpi-pico2-rv32-smp.elf  .bss 97104
  ktask string False  usbpump False   # --gc-sections 照旧丢掉

kthreadtest: 405 sibling switch(es), 0 region set(s)
kthreadtest: all ok (0 failure(s))
rv32     PASS  tasks      20054016     19584  (2 cycles)
arm      PASS  tasks       3342336      3264  (2 cycles)
2/2 targets passed
```

板上的预期启动行是 `[usb] pump thread pid N on cpu 0`；创建失败则打印
`[usb] pump thread not started; keeping the polled hooks`，两条都在
`docs/OPEN-ITEMS.md` §19 的判据里。

## 没跑的

- **泵线程的运行时（T3）`NOT RUN`**：QEMU 的 rv32/arm 没有 USB 控制器。板侧按
  §19 在 Pico 2 RV32 上跑：`pump_repro.py` 看启动/枚举，工程上 `cat /proc/ktask`
  读 `usbpump` 的 used/hw、21 KiB `cat` 双路一致、`crash`、`smpbench`、tick 精度。
- **阻塞写者**：`write()` 仍走 `plat_console_can_take()` + 短写/重试；S4 只把状态机
  搬进线程，写者阻塞在泵线程上的队列是下一步。
- **x86 释放镜像**：`MKFS: No more free space`，HEAD 上同样失败（§17）。
- **WiFi 镜像**：需要外部 lib 目录，本轮没有构建；四个 `process_ktask_create` 调用点
  只加了 `-1`，由编译期签名检查覆盖。
