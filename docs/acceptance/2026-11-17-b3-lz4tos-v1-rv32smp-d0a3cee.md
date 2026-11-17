# B-3 LZ4TOS V1：QEMU + 11 目标（2026-11-17）

```text
commit:            d0a3cee
change:            flags 高字节版本：内核接受 V0/V1，mklfs 写 V1（0x0101）
baseline:          04359e2
target/config:     rv32, rv32smp, arm；11 目标构建
board/port/image:  纯 QEMU；无真机
toolchain:         go 1.27.3, clang 23.1.4, qemu 11.1.3, python 3.14.9
host:              Windows 11 / MSYS2 UCRT64
T0:                PASS（build/vet/test -count=1）
T1:                PASS（11/11）
T2:                PASS（rv32/rv32smp/arm smoke）
T3:                NOT RUN（OPEN-ITEMS §2c）
T4:                NOT RUN（格式改动，见 T3）
logs:              not kept
notes:             goc32 仍产 V0，作为旧分支样本
```

## 执行的命令与结果

```sh
go run ./cmd/tos build -target rv32smp                          # build ok
# ramdisk 里所有 LZ4TOS 头：hdr[6]=0x01, hdr[7]=0x01（V1）
go run ./cmd/tostest smoke -target rv32|rv32smp|arm \
  -commands "ps, piperace, deep 3, deep 6, mem"                 # 3x SMOKE TEST OK
# 临时把 mklfs 的版本改 0 重建：头 01 00，smoke 通过（V0 分支），随后恢复
```

## 关键输出

```text
[('0x12284', '0x1', '0x1'), ...]      # V1
[('0x1', '0x0')]                      # 临时 V0 构建
SMOKE TEST OK                         # 两条路径都过
```

## 没跑的

- **真机**：C3 / RP2350 ARM+RV32 / RP2040 都没刷。镜像格式变了，模板命令见
  `docs/OPEN-ITEMS.md` §2c。
- goc32 侧写 V1：本轮刻意不动子模块，goc32 继续写 V0。
