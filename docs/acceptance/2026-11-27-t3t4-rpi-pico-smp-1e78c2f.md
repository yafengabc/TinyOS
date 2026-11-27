# T3+T4 `rpi-pico-smp`：USB CDC 控制台 + 内核线程泵 + 30 分钟浸泡（2026-11-27）

```text
commit:            1e78c2f（被测代码；写卡时 HEAD=8034639，只有文档提交）
change:            none —— 关闭 #67：在合并后的 main 上重做 RP2040 SMP 的 T3+T4
baseline:          68f8801（上一张 RP2040 卡，当时 kthreadtest/wheap 判据未定）
target/config:     rpi-pico-smp / targets/rpi-pico-smp.conf（defaults + board/rpi-pico/board.conf）
board/port/image:  RP2040 Pico（UID 0x000000e6…0x00000033）, USB CDC COM29 (2e8a:000a),
                   UART0 = CH343 COM20; image build/tinyos-rpi-pico-smp.uf2
toolchain:         go 1.27.1, clang 23.1.4, python 3.13.14
host:              Windows / Git Bash, D:\projects\TinyOS
T0:                NOT RUN（本卡只做板级 T3+T4）
T1:                NOT RUN（同上）
T2:                NOT RUN（同上）
T3:                PASS —— accept.py 30/30（console: usb-cdc）+ 泵线程 + kthreadtest + ps/wheap
T4:                PASS —— soak_board -m 30：30 min、86 cycles、6 commands，free 零漂移
logs:              build/accept67-cdc.log, build/bootlog67-uart0.txt,
                   build/evidence67-ktask.log, build/evidence67-wheap.log,
                   build/soak67-30min-rerun.log（不入库）
notes:             第一次 30 分钟浸泡被外部拔线/主机休眠打断，判 FAIL（deep 29/30），
                   作废；本次为插回后重跑。kthreadtest 的 fault 项对无 MPU 的 M0+ 记
                   N/A memprot: off，总数 all ok (0 failure(s))。wheap 的窗口看驻留
                   进程的 wink，RP2040 是 36 KiB。
```

## 执行的命令与结果

```sh
go run ./cmd/tos build -target rpi-pico-smp
python scripts/rp2040/accept.py --uf2 build/tinyos-rpi-pico-smp.uf2 -p COM29 \
    --target rpi-pico-smp --bootsel-timeout 300
    # ACCEPT OK 30/30 但 console: uart —— 刷机后 CDC 重枚举，脚本拿了旧端口
python scripts/rp2040/accept.py -p COM29 --target rpi-pico-smp
    # ACCEPT OK 30/30, console: usb-cdc（不刷机复跑，本次计分）
python scripts/rp2350/serial_console.py COM29 \
    "kthreadtest" "cat /proc/ktask" "ps" "wheap"
python scripts/rp2350/serial_console.py COM29 "wheap 5000 &" "ps"
# 启动日志：从 CDC COM29 发 reboot，UART0 COM20 抓 20 s
python scripts/rp2350/soak_board.py -p COM29 -m 1 --image rpi-pico-smp -l build/soak67-smoke.log
    # PASS (1 min, 3 cycles, 6 commands) —— 预检
python scripts/rp2350/soak_board.py -p COM29 -m 30 --image rpi-pico-smp \
    -l build/soak67-30min-rerun.log
    # PASS (30 min, 86 cycles, 6 commands)
```

## 关键输出

```text
ACCEPT OK 30/30
target: rpi-pico-smp  port: COM29  console: usb-cdc

[rp2040] boot clk=125MHz
[console] uart0 primary
[usb] pump thread pid 1 on cpu 0

ktask: slot   size   used     hw  name
  0    4096    232    232  usbpump
kthreadtest: all ok (0 failure(s))

 pid  tid ppid prio state  c cpu% cpums wink stk/sz  name
   2    2    0    1 run   1    0.1     0   16    0.9/4  msh
 310  310    2    1 run   0  133.3     0   36    0.5/4  wheap

rpi-pico-smp             PASS  (30 min, 86 cycles, 6 commands)
  cycle 1..86: deep=86 piperace=86
  free before == free after:
    kernel heap 8192 0 8192 peak 3360 / SRAM 270336 264
    kernel 59520 58 / tasks 204800 200 / unused 6016 5
  uptime at end: 1280.736 s, cpus=2, cpu=1, pid=707
```

## 没跑的

- T0/T1/T2：本卡只承担板级 T3+T4，见 `#57` 主表。
- `prot rodata/heap/stack/null`：这个镜像没有 `prot` 程序，`soak_board.py` 自动
  skip 并在日志里逐条写明；不是删测试。
- CDC 上的 fault 转储对账：镜像策略是 dump 只走 UART0，`accept.py` 记 skipped。

## 第一次浸泡作废的原因（不是固件回归）

第一次 `soak_board -m 30` 报 `FAIL (30 min, 30 cycles, 6 commands)`，唯一 problem 是
`deep 3 grew the stack 29/30 times`。对照系统事件：19:41:45 主机进入睡眠（Button or Lid），
19:41:54 恢复，20:02:51 系统时钟从 11:41:54Z 同步到 12:02:51Z（+21 min）；用户下班拔线，
板子在该窗口复位（日志 pid 258 → 3、`[cpu1] took pid 2`、末尾 uptime 30 s）。`free` 前后仍
完全一致。该次结果记为环境打断，不用于判据，原始日志留在 `build/soak67-30min.log`。

重跑时日志内 pid 从 19 单调递增到 707、无启动行、无 pid 回退，`deep`/`piperace` 86/86。
末尾 uptime 1280.736 s 与"预跑 127 s + 1803 s"的墙钟相差约 650 s，原因未定位；按 #67
的列明判据不构成失败，作为观察记录在此，若后续 T4 再出现同样偏差再单独查。
