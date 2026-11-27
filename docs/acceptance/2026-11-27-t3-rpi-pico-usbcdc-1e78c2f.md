# T3 `rpi-pico`（UP）：USB CDC 控制台 + 内核线程泵（2026-11-27）

```text
commit:            1e78c2f（被测提交）
change:            none —— 关闭 #66：在合并后的 main 上重做 RP2040 UP 的 T3
baseline:          68f8801（上一张 RP2040 卡，当时 kthreadtest/wheap 判据未定）
target/config:     rpi-pico / targets/rpi-pico.conf（defaults + board/rpi-pico/board.conf）
board/port/image:  RP2040 Pico（UID 0x000000e6…0x00000033）, USB CDC COM29 (2e8a:000a),
                   UART0 = CH343 COM20; image build/tinyos-rpi-pico.uf2
toolchain:         go 1.27.1, clang 23.1.4, python 3.13.14
host:              Windows / Git Bash, D:\projects\TinyOS
T0:                NOT RUN（本卡只做板级 T3；T0/T1 由 #57 主表跟踪）
T1:                NOT RUN（同上）
T2:                NOT RUN（同上）
T3:                PASS —— accept.py 30/30（console: usb-cdc）+ 泵线程 + kthreadtest + ps/wheap
T4:                NOT RUN（#66 明确不含 T4；RP2040 UP 的 T4 在 #64 单独 PASS）
logs:              build/accept66-cdc.log, build/bootlog66-uart0.txt,
                   build/evidence66-ktask.log, build/evidence66-wheap.log（不入库）
notes:             kthreadtest 的 fault 项在 M0+（无 MPU）上显示 N/A memprot: off，
                   总数仍是 all ok (0 failure(s)); wheap 的窗口看驻留进程 ps 的 wink，
                   RP2040 是 36 KiB（4 KiB grain + 4 KiB 增长位），不是旧判据的 32 KiB。
```

## 执行的命令与结果

```sh
go run ./cmd/tos build -target rpi-pico
python scripts/rp2040/bootsel_poke.py COM29 20 --dtr          # 未进 BOOTSEL，改按 BOOTSEL 键
python scripts/rp2350/bootsel_poke.py COM20 20                 # 同上，UART0 也不收
python scripts/rp2040/accept.py --uf2 build/tinyos-rpi-pico.uf2 -p COM29 \
    --target rpi-pico --bootsel-timeout 300
    # ACCEPT OK 30/30 但 console: uart —— 刷机后 CDC 重枚举，脚本拿了旧端口
python scripts/rp2040/accept.py -p COM29 --target rpi-pico
    # ACCEPT OK 30/30, console: usb-cdc（不刷机复跑，本次计分）
python scripts/rp2350/serial_console.py COM29 \
    "kthreadtest" "cat /proc/ktask" "ps" "wheap"
python scripts/rp2350/serial_console.py COM29 "wheap 5000 &" "ps"
# 启动日志：从 CDC COM29 发 reboot，UART0 COM20 抓 20 s（CDC 重枚举不影响抓日志）
```

## 关键输出

```text
ACCEPT OK 30/30
target: rpi-pico  port: COM29  console: usb-cdc

[rp2040] boot clk=125MHz
[console] uart0 primary
[usb] init done
[usb] pump thread pid 1 on cpu 0
TinyOS shell (pid 2).

ktask: slot   size   used     hw  name
  0    4096    232    232  usbpump

kthreadtest: ok   join the thread that wrote to a dead address (memprot: off)
kthreadtest: N/A  the holder died on its fault (memprot: off)
kthreadtest: all ok (0 failure(s))

 pid  tid ppid prio state  c cpu% cpums wink stk/sz  name
   2    2    0    1 run   0    0.1     0   16    0.9/4  msh
   3    3    2    1 rdy   -   66.6     0   36    0.4/4  wheap
wheap: 12 KiB declared, 12 KiB taken, pattern intact
```

## 没跑的

- T0/T1/T2：本卡只承担板级 T3，见 `#57` 主表。
- T4：`NOT RUN`（#66 通过标准不含 T4；UP 的 30 分钟浸泡在 #64 已 PASS）。
- fault 转储的 CDC 对账：镜像的策略是 dump 只走 UART0，`accept.py` 记为 skipped；
  不是删测试，原始 UART0 dump 在 `#64` 的验收里。
