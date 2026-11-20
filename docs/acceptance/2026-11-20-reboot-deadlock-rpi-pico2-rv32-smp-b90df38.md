# `reboot` 死锁（RV32 SMP，锁序反转）：20 轮重启验收（2026-11-20）

```text
commit:            b90df38（被测提交；死锁修复本体在 695db5b，本轮为最终镜像）
change:            usb_cdc_flush() 与 usb_tx_drain() 的加锁顺序相反 → 两核互等；另修启动环 dump 判据
baseline:          5c269c9
target/config:     rpi-pico2-rv32-smp / defaults（diag on，TOS_DIAG=1）
board/port/image:  Raspberry Pi Pico 2（RP2350 Hazard3 ×2），UART0=COM19（CH343），CDC=COM27
toolchain:         go 1.24, clang 23.1.4, python 3.13
host:              Windows 11 / EKSOFT
T0:                go build ./... && go vet ./... && go test ./... -> PASS
T1:                build all -> 18/18
T2:                QEMU smoke -> NOT RUN（改动只落在 board/rp2350rv-generic 与 drivers/usb/rp2350rv，
                   QEMU 目标不编译它们；覆盖面由 T1 的 18 目标构建保证）
T3:                rpi-pico2-rv32-smp 硬件闸门（gate_boot.py -n 10）-> PASS
T4:                20 轮 reboot（reboot_soak.py -n 20）-> PASS；ARM 对照 12 轮 -> PASS
logs:              not kept（CSV 在本地 build/reboot-soak-final.csv、build/reboot-soak-arm.csv）
notes:             T2 的 NOT RUN 是范围判断，不是未跑；T4 的两条判据见下，缺一不可
```

## 判据

`reboot` 之后要同时满足两件事才算 PASS：

1. UART0 的提示符在 **2 s** 内出现；
2. 之后新 shell 自己报的 `uptime` **≤ 2 s**。

第 2 条是必须的：只等“提示符来了”分不出“真的复位了”和“根本没复位、看到的是上一个
shell 的提示符”。2026-11-20 的故障正是后者——芯片压根没复位，而两个通道都在 33 s 后
“有反应”。

## 结果

```text
# rpi-pico2-rv32-smp（被测镜像，b90df38）
20/20 cycles clean
  prompt: min 0.50 s, max 0.56 s, mean 0.52 s
  uptime: 每轮 0.08 s

# rpi-pico2-smp（ARM 对照，走的是另一份 flush 实现，从来无此问题）
12/12 cycles clean
  prompt: min 0.38 s, max 0.38 s, mean 0.38 s
  uptime: 每轮 0.08–0.09 s

# 双通道闸门（rpi-pico2-rv32-smp，-n 10 --cpus 2）
10/10 PASS   uart0 boot 10/10, cdc boot 10/10, cpus=2, uptime 0.31 s
# 双通道闸门（rpi-pico2-smp，-n 5 --cpus 2）
5/5 PASS     uart0 5/5, cdc 5/5, uptime 0.60 s
```

修复前同一判据：10 次里 4 次 34.01 s，其余 0.50–0.57 s。

## 诊断手段本身的验证（issue #33 P1）

`boot_log_dump()` 原来在“上一轮进过 shell”时静默返回，于是 `plat_reboot()` 的
`0x60..0x64` 标记永远不会被打印。改判据后，用**故意挂死**验证它真的会说话：在
`plat_reboot()` 写完 `0x60` 之后插 `for (;;) ;`（看门狗已 armed、无人 `wdt_feed()`），
两次都是 33.8 s 后被复位，下一次启动打印完整的 41 行轨迹，最后一行是：

```text
[L] previous boot b00000002 stopped after ph=00000060
  ph=00000001 fr=00000000 clk=00000000 div=00000000/00000000 sys=00000000 peri=00000000 t=00000000
  ph=00000002 fr=00000000 clk=00b71b00 div=00000000/00000000 sys=00000001 peri=00000001 t=00000000
  ...
  ph=0000002b fr=0000013f clk=08f0d180 div=00000051/00000018 sys=00000002 peri=00000001 t=000006df
  ph=00000060 fr=0000013f clk=08f0d180 div=00000051/00000018 sys=00000002 peri=00000001 t=003b74fe
```

同一镜像的正常 `reboot` 则不打印任何 `[L]` 行（最后一相是 `0x63`，即“已向 ROM 请求
复位”）。实验代码已还原，最终镜像里没有那段 `for (;;) ;`。

顺带量到一件事：**一次挂死的清场时间是 ~33.5 s，不是 16.78 s**。所以
“33.56 ≈ 2 × 看门狗周期”只是算术巧合，不是“两次启动都被清掉”。

## 怎么重跑

```sh
go run ./cmd/tos build -target rpi-pico2-rv32-smp
python scripts/rp2350/bootsel_poke.py COM27 20 --dtr && cp build/tinyos-rpi-pico2-rv32-smp.uf2 /f/
python scripts/rp2350/reboot_soak.py -n 20 --csv build/reboot-soak-final.csv
python scripts/rp2350/gate_boot.py --uart COM19 --cdc COM27 -n 10 --cpus 2
```
