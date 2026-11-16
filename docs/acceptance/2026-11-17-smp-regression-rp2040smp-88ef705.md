# RP2040 SMP regression of the RP2350 review fixes（2026-11-17）

```text
commit:            88ef705
change:            RP2040 SMP regression of the SysTick/USB, signal/sleeper, console, path/filesystem, PIO and deep-guard fixes
baseline:          none
target/config:     rp2040smp / configs/rp2040.conf (defaults)
board/port/image:  RP2040 (Pico-class) SMP; UART0 COM19, USB CDC COM17; build/rp2040smp.uf2
toolchain:         go 1.27.1, clang 23.1.4, qemu 11.1.2, python 3.14.0
host:              Windows NT 10.0.17763 (MSYS2 UCRT64)
T0:                PASS (go test -count=1 ./..., go vet ./...)
T1:                PASS (11/11 targets)
T2:                PASS (rv32/rv32smp/arm smoke with deep 3)
T3:                PASS (RP2040 SMP, UART and USB CDC)
T4:                PASS (100 logged rounds of dd/rm; 200 rounds observed earlier)
logs:              build/rp2040smp-review-1117.log, build/rp2040smp-dd100-1117.log, build/heartbeat-rp2040smp-uart.log, build/heartbeat-rp2040smp-usb.log, build/pio-rp2040smp-review.log
notes:             prot is not built on RP2040 (no MPU/PMP); this image predates A-0 7453bd6
```

## 执行的命令与结果

```sh
go run ./cmd/tos build -target rp2040smp                     # build ok
python scripts/rp2350/pio_accept.py --image rp2040smp --uf2 ... -p COM19   # 17/17
python scripts/rp2040/heartbeat.py --port COM19              # 6/6
python scripts/rp2040/heartbeat.py --port COM17 --dtr        # 6/6
smpbench                                                     # 2.04x
smp -s 2 400                                                 # balanced
smp -t 4 2 300                                               # balanced
piperace 2 64                                                # ok
deep 3; deep 6; deep 6 0 8                                   # grow/kill/kill
# 100 x (dd of=/x bs=512 count=1; rm /x)                    # bad=0
dd of=/x bs=4096 count=8; reboot; dd if=/x ...               # pattern verified
# 20 x (sleep 3000; ^C)                                      # prompt returns
```

## 关键输出

```text
cpus=2
17/17 checks passed
HEARTBEAT OK 6/6   (COM19)
HEARTBEAT OK 6/6   (COM17)
smpbench: single 179 ms, dual 172 + 175 ms, speedup 2.04x
smp -s: w0=1181 w1=1182
smp -t: 2378 / 2362 blocks
piperace: ok
deep: 3 levels, journal intact
[pid N] no room to grow it; killing
dd/rm rounds=100 bad=0
dd if=/x after reboot: pattern verified
sleep+^C rounds=20 bad=0
```

## 没跑的

- **RP2350 SMP**：`NOT RUN`，本轮没有该板在线；A-0 的条目见
  `docs/OPEN-ITEMS.md` §2b。
- **`prot`**：RP2040 没有 MPU/PMP，`/bin/prot` 构建期不存在，不是失败。
