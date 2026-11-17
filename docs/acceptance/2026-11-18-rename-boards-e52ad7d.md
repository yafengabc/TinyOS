# 板名/目标改名 + `tinyos-` 前缀（2026-11-18）

```text
commit:            e52ad7d
change:            目标名改成板名（rpi-pico / atk-dnr2350am / rpi-pico2），产物加 tinyos- 前缀；恢复被批量替换误伤的 chip/ISA 选择
baseline:          d8211aa
target/config:     13 个目标 + configs/rv32-fsro.conf
board/port/image:  无真机
toolchain:         go 1.27.3, clang 23.1.4, qemu 11.1.3, python 3.14.9
host:              Windows NT 10.0.17763 (MSYS2 UCRT64)
T0:                PASS
T1:                PASS 13/13 + fs_ro
T2:                PASS（smoke 4/4）
T3:                NOT RUN（无板）
T4:                NOT RUN
```

## 判据

| 判据 | 结果 |
|---|---|
| 板名带厂商、不带芯片 | `rpi-pico`、`rpi-pico2`、`atk-dnr2350am*`；`rp2040`/`rp2350` 只作 chip 名 |
| 产物带项目前缀 | `build/tinyos-<target>.uf2`、`build/tinyos-<target>.bin`、`build/tinyos-ramdisk-<target>.bin` |
| 旧名不再静默可用 | 无 alias：`-target rp2350smp` → `unknown target` |
| chip 选择未被板名污染 | `chip == "rp2040"`/`"rp2350"`、`drivers/*/rp2350rv`、`rp2350rv_memmap.h` 全部恢复原值 |
| RV32 仍可编译 | `atk-dnr2350am-rv32`、`atk-dnr2350am-rv32-smp` 构建通过（曾被改名后的 memmap include 打断） |

## 做了什么

```text
board/rp2040                         -> board/rpi-pico
board/pico2                          -> board/rpi-pico2
target rp2040 / rp2040smp            -> rpi-pico / rpi-pico-smp
target rp2350 / rp2350smp            -> atk-dnr2350am / atk-dnr2350am-smp
target rp2350rv / rp2350rvsmp        -> atk-dnr2350am-rv32 / atk-dnr2350am-rv32-smp
target rp2350pico2                   -> rpi-pico2
configs/rp2040.conf                  -> configs/rpi-pico.conf
configs/rp2350{smp,rv,smp}-kthreads  -> configs/atk-dnr2350am-{smp,rv32,rv32-smp}-kthreads
```

`kernel-<target>.elf` 等中间件保留原名；只有人会拿去烧写的镜像加前缀。

## 回归与修复

批量替换第一遍把 chip 名当板名改了：`chip == "rp2040"` 变成 `"rpi-pico"`，
`#include "rp2350rv_memmap.h"` 变成不存在的 `atk-dnr2350am-rv32_memmap.h`。
后者会直接打断 RV32 编译；前者会让 ATK 丢掉 `TOS_ARM_MPU_V8` 等 flag，重新出现
`[mpu] window needs 5+5 blocks (budget 7)` → pid1 被杀。两者都已按事实恢复，
`TestNoBoardNameSelectsBuildFlags` 继续守住“按 (chip, ISA) 选 flag”。

## 证据

```text
go vet ./...                      PASS
go test ./... -count=1            PASS
go run ./cmd/tos build -target <13 targets>   PASS 13/13
go run ./cmd/tos build -config configs/rv32-fsro.conf  PASS
go run ./cmd/tostest smoke -target {arm,rv32,rv32smp,x86} \
    -commands "ps, deep 3, rselftest"          4/4 SMOKE TEST OK
```

## 没做 / 未验证

- **硬件 `NOT RUN`**：ATK 可直接刷 `build/tinyos-atk-dnr2350am-smp.uf2`（镜像与改名前的
  `rp2350smp` 同源，flag 修复后符号表一致），但本轮没有上板；Pico 2 仍停在 #7 A 轨的
  `PC=0xeffffffe`，本次未动。
- RP2040/Pico 2 的 flash 容量等板级事实未动（Pico 2 的 8 MiB 修正仍是待办）。
