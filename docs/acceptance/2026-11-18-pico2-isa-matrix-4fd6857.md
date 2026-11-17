# Pico 2：ARM/RV32 × 单核/SMP 四个对照镜像（2026-11-18）

```text
commit:            4fd6857
change:            新增 rpi-pico2-rv32 板 + rpi-pico2-rv32 / -smp / rpi-pico2-smp 目标
baseline:          9ee7d27（boot2 XIP 设置，ARM 单核仍不启动）
target/config:     18 个目标（Pico 2 四个镜像：ARM 单/双核、RV32 单/双 hart）
board/port/image:  Pico 2（用户现场）/ 待刷四个 UF2
toolchain:         go 1.27.3, clang 23.1.4
host:              Windows NT 10.0.17763 (MSYS2 UCRT64)
T0:                PASS
T1:                PASS 18/18
T2:                NOT RUN（本机无 RP2350）
T3:                NOT RUN——需要 Pico 2 真机逐个刷
T4:                NOT RUN
```

## 背景

ARM 单核镜像（含 `d16f949` 的 XIP 设置）在 Pico 2 上仍无反应、USB 不枚举。为了把
“ARM 启动路径”和“共同路径（flash/XIP/板级）”分开，补上另外三个镜像：

| 镜像 | 测什么 |
|---|---|
| `build/tinyos-rpi-pico2.uf2` | ARM 单核（现状：Pico 2 不启动；ATK 6/6） |
| `build/tinyos-rpi-pico2-smp.uf2` | ARM 双核：如果单核不启动、双核更不会，但可确认 SMP 构建的启动入口一致 |
| `build/tinyos-rpi-pico2-rv32.uf2` | Hazard3 单 hart：**能启动 → 问题在 ARM 启动链（M33/secure/boot2）**；不启动 → 共同路径 |
| `build/tinyos-rpi-pico2-rv32-smp.uf2` | Hazard3 双 hart |

## 改动

```text
board/rpi-pico2-rv32/board.conf   base = rp2350rv-generic, rpi-pico2；无新增 C 文件
targets/rpi-pico2-rv32.conf       rv32 / rv32imac / heap 32k
targets/rpi-pico2-rv32-smp.conf   同上 + cpus 2
targets/rpi-pico2-smp.conf        ARM + cpus 2
```

UF2 family 不同（ARM Secure `0xe48bff59`，RISC-V Secure `0xe48bff5a`），**不能混刷**。

## 验证

```text
go test ./...           PASS
go run ./cmd/tos build all   PASS 18/18
```

## 没做 / 待验证

- 四个镜像都 **`NOT RUN`**（本机没有 RP2350）。请按上表逐个刷 Pico 2，记录哪一个
  能枚举 USB / 进 shell；结果决定 #7 A 轨往 ARM 启动链还是共同路径查。
- 8 MiB flash 事实（`board/rpi-pico2*/board.conf` 仍是 4m）等启动修好后一起改。
