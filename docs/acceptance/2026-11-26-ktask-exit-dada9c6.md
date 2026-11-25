# ktask 退出路径的用例（2026-11-26）

```text
commit:            dada9c6
change:            ktask 自测新增第二个内核线程（kexit，返回即退出）：验证
                   SYS_KTASK_EXIT 归还池槽与名字、线程无 proc 直接回收、槽可复用；
                   自测线程在 5 次 sleep 后检查并打印 "exit recycled its slot"。
baseline:          7a10e68（E3-M3 卡）
target/config:     arm-ktasktest、rv32-ktasktest、rv32smp-ktasktest；18 目标
board/port/image:  QEMU mps2（Cortex-M3）、rv32、rv32smp；ESP32-C3 COM4
toolchain:         go 1.27.3；clang 23.1.4；QEMU 11.1.3；Python 3.14.9；esptool
host:              Windows / 本机
T0:                PASS（go test -count=1 ./...、go vet ./...）
T1:                PASS 17/18（x86 MKFS，pre-existing，docs/OPEN-ITEMS.md §17）
T2:                PASS（三个 ktest 配置；soak 3 轮 3/3）
T3:                PASS（C3 accept 42/42）
T4:                NOT RUN
logs:              build/t-kexit-<target>.log（不入库）
notes:             TOS_KTASK_TEST 下的代码；默认镜像不受影响
```

## 执行的命令与结果

```sh
go test -count=1 ./...                                            # PASS
go vet ./...                                                      # PASS
go run ./cmd/tos build -target <t>                                # 17/18（x86 §17）

go run ./cmd/tos build -target arm     -config configs/arm-ktasktest.conf
go run ./cmd/tos build -target rv32    -config configs/rv32-ktasktest.conf
go run ./cmd/tos build -target rv32smp -config configs/rv32smp-ktasktest.conf
go run ./cmd/tostest smoke -target <t> -commands "cat /proc/ktask"

python scripts/soak.py --cycles 3 --targets rv32,rv32smp,arm       # 3/3 PASS
python scripts/esp-c3/flash.py --build -p COM4                     # PASS
python scripts/esp-c3/accept.py -p COM4 --no-reset                 # 42/42
```

## 关键输出

```text
# mps2（特权 Thread mode）
[ktest] create pid=1 exit=2
[ktest] ktask ran 5 sleeps + yield, cpu_bad=0 priv=1
[ktest] ktask exit recycled its slot
ktask: slot   size   used     hw  name
  0    6144    352    352  ktest

# rv32 / rv32smp
[ktest] create pid=1 exit=2
[ktest] ktask ran 5 sleeps + yield, cpu_bad=0
[ktest] ktask exit recycled its slot
  0    6144    592    592  ktest

# T2 / T3
rv32/rv32smp/arm soak 3 轮 3/3 PASS
42/42 passed
```

## 这个用例补的是什么

`ktask_trampoline()` 在入口函数返回后调 `ktask_exit()` → `SYS_KTASK_EXIT`：
清池位图、清名字、`proc_retire_locked()`（无 proc，不碰进程状态）、
`current = NULL`、切走。此前自测线程为了给 `/proc/ktask` 留高水位一直 sleep，
这条路径只在代码审查里存在。现在：

- 退出后没有活槽还带 `kexit` 名字；
- 新线程能拿到同一个槽（复用）；
- `/proc/ktask` 只剩 ktest 一行（名字和槽都回收了）。

与 RV32 的 ktask 走同一份代码（同一个 `thread` 结构、同一个退出状态机），
所以这条用例在三个 QEMU 配置上一起跑。

## 没跑的

- **M33 / RP2040 真机**：本机无板；清单在 `docs/OPEN-ITEMS.md` §22。
- **T4**：没有对应测试项。
- **x86**：`MKFS: No more free space`（§17）。
- **USB 泵迁移**：仍在 PendSV/idle 钩子上（#58 的板侧部分）。
