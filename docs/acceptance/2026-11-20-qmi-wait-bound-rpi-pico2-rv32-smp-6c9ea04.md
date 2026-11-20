# QMI 等待上限按时钟折算（C6/A-6）：板级验收（2026-11-20）

```text
commit:            6c9ea04
change:            两处 QMI FIFO 等待的上限由字面 1e6 改为按 g_clk_hz 折算（~20 ms）
baseline:          b90df38
target/config:     rpi-pico2-rv32-smp / defaults（diag on）
board/port/image:  Raspberry Pi Pico 2（RP2350 Hazard3 ×2），UART0=COM19，CDC=COM27
toolchain:         go 1.24, clang 23.1.4, python 3.13
host:              Windows 11 / EKSOFT
T0:                go build ./... && go vet ./... && go test ./... -> PASS
T1:                build all -> 18/18
T2:                QEMU smoke -> NOT RUN（改动只在 board/rp2350*-generic，QEMU 目标不编译）
T3:                rpi-pico2-rv32-smp 硬件：cat /proc/flash -> PASS
T4:                20 轮 reboot（reboot_soak.py -n 20）-> PASS
logs:              not kept（CSV 在本地 build/reboot-soak-final.csv）
notes:             上限变小（1e6 → 约 1e6 @150 MHz，但按实际时钟折算），要证明 JEDEC 交换没被截断
```

## 为什么改

`docs/ARCHITECTURE-NEXT.md` A-6 / 契约 C6 对 tick 就绪前的等待有明文：保留迭代上限，
但必须按 `plat_cpu_hz()` 或板级常量折算，**不得直接抄 `1e6`**，并写明是 pre-tick。
当天早先加的两处 QMI 上限正好是那个字面量。

`plat_cpu_hz()` 在这里用不了：这段代码在 XIP 关闭的窗口里跑，调用它是**取指落到
flash**，正是这个上限要防的事情。所以读 `g_clk_hz`（`.data`，在 SRAM），
`(hz / 3000) * 20`，约 20 ms（按每次轮询 3 个周期）。

## 结果

```text
$ cat /proc/flash
flash
  detected     8388608 bytes        <- JEDEC 交换在折算后的上限内完成
  configured   8388608 bytes
  lfs_offset    524288 bytes
  lfs_blocks      1920

$ python scripts/rp2350/reboot_soak.py -n 20
20/20 cycles clean
  prompt: min 0.50 s, max 0.57 s, mean 0.52 s
  uptime: 每轮 0.08 s
```

`detected` 仍是实测值而不是 0，说明没有走到超时分支。

## 顺带

`usb_cdc_wait()` 补了同一条规矩的另一半注释：它也是 pre-tick（中断还关着，
用 `plat_ticks()` 建的 deadline 永远不会到期），所以上限是次数、按 `clk_hz` 折算。

`reboot_soak.py` 的重试从“完全没应答”扩到“应答只到一半”：提示符可能先到，上面
那行还在路上，20 轮里会丢 1 轮。

## 怎么重跑

```sh
go run ./cmd/tos build -target rpi-pico2-rv32-smp
python scripts/rp2350/bootsel_poke.py COM27 20 --dtr && cp build/tinyos-rpi-pico2-rv32-smp.uf2 /f/
python scripts/rp2350/reboot_soak.py -n 20
```
