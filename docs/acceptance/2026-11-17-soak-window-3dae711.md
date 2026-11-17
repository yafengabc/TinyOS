# T2 soak 1/3 的根因：harness 等待窗口（2026-11-17）

```text
commit:            3dae711（代码）；本卡即其证据
change:            tostest 的 waitFor 在有新输出时滑动截止时间（硬上限 3 倍窗口）；
                   scripts/soak.py 的 --commandwait-ms 默认 1500 → 4000
baseline:          df55904
target/config:     rv32 / rv32smp / arm / x86（QEMU）；soak 3 轮
board/port/image:  无真机；对比对象是复核线程的 167db55、0751f9d 两张卡
toolchain:         go 1.27.3, qemu 11.1.3, python 3.14.9
host:              Windows NT 10.0.17763 (MSYS2 UCRT64)；对方那台慢一些
T0:                PASS（含新增 serial_test.go 三个用例）
T1:                NOT RUN（未改镜像）
T2:                PASS（smoke 4 目标；soak 3/3）
T3:                NOT RUN（无板；对方 T3 已 PASS）
T4:                NOT RUN
```

## 现象与排除

复核卡：`soak FAIL 1/3`，失败项是"命令输出丢失"，`rv32`/`rv32smp` 都出现，`arm` 过。
排除代码的根据在卡里已经给了，本轮复核成立：

- 基线 `87f5bf6` 在**同一台机器**上 3/3 过（一次）；
- `a271253` 与 `167db55` 的 `rv32smp` ELF 逐字节相同（`51DA5C33…`），前者也失败；
- 本机在 `075a90c`、`df55904` 上 3/3 过。

同一份镜像在不同机器上成败不同 → 时间假设，不是语义。

## 根因

1. `waitFor()` 是"固定截止时间"：字节还在来也照断。soak 的命令表里 `piperace` 之后
   紧接着 `ls /bin | wc`，断掉的那一刻，下一条命令的回显就插进了上一条的输出里，
   解析 `free` 自然失败。
2. `piperace` 推 64 KiB 期间**一个字节都不打印**，所以"还在工作"在串口上看不见；
   1500 ms 的窗口对慢主机就是不够。

## 修法

```go
// serial.go: 有字节 → 截止时间滑到 now+window，最多 hard = start+3*window
if len(b) > 0 {
    next := time.Now().Add(timeout)
    if next.After(hard) { next = hard }
    if next.After(deadline) { deadline = next }
}
```

```python
# soak.py: 默认窗口 1500 → 4000；等待在提示符出现时就结束，所以快主机不受影响
```

## 证据

```text
go test ./cmd/tostest -run TestWaitFor -count=1 -v
  TestWaitForSlidesWhileOutputArrives   PASS  (0.30s)
  TestWaitForStillGivesUpOnSilence      PASS  (0.15s)
  TestWaitForCapsEndlessOutput          PASS  (0.30s)

# 把滑动那段删掉再跑第一条：
  FAIL: waitFor gave up after 200.4ms while the guest was still printing

python scripts/soak.py --cycles 3 --targets rv32,rv32smp,arm
  rv32 PASS / rv32smp PASS / arm PASS   → 3/3

smoke -target arm|rv32|rv32smp|x86（标准命令表，含 cat /proc/execfmt）
  SMOKE TEST OK ×4
```

## 没做

- 没有在对方那台机器上复测——这是本卡唯一缺口。若他们的 `soak` 仍失败，说明还有第二个
  时间假设（例如 `drain(300ms)`），届时按同样方法定位。
- 没改内核：`free` 的数字在慢主机上是稳定的，失败只在解析阶段。
