# ATK 全 RP2350 目标回归 + embedded UF2 修复（2026-11-18）

```text
commit:            199339d（含 a06be75 generic 目标 / a499be8 build all）；
                   修复为本轮新增（uf2.go rp2350RoundToSector）
change:            tos build all 构建全部 15 个镜像；逐个刷 ATK 测 7 个 RP2350 目标
baseline:          无（功能回归，非对比）；bug 由 a06be75 引入（其提交注明 Hardware: NOT RUN）
target/config:     atk-dnr2350am(+smp) / atk-dnr2350am-rv32(+smp) / rp2350-generic /
                   rp2350rv-generic / rpi-pico2
board/port/image:  ATK-DNRP2350AM; UART0 COM14; build/tinyos-<target>.uf2
toolchain:         go 1.27.1, clang 23.1.4, qemu 11.1.2, python 3.14.0
host:              Windows NT 10.0.17763 (MSYS2 UCRT64)
T0:                PASS（build/vet/test -count=1，含新增 TestRP2350RoundToSector）
T1:                PASS（`tos build all` → all 15 targets built）
T2:                NOT RUN（按用户要求不跑 dd/rm；smoke 不在本卡范围）
T3:                PASS——本卡即 ATK 真机，7 个 RP2350 目标逐个 heartbeat 6/6
T4:                NOT RUN（用户明确不做 dd/rm）
```

## `build all`

```sh
go run ./cmd/tos build all     # all 15 targets built
```

15 目标：arm, atk-dnr2350am(-smp), atk-dnr2350am-rv32(-smp), esp32c3-generic,
rp2350-generic, rp2350rv-generic, rpi-pico(-smp), rpi-pico2, rv32, rv32nc,
rv32smp, x86。

## 真机结果（ATK-DNRP2350AM，COM14，逐个刷入）

| 目标 | 类型 | heartbeat |
|---|---|---|
| atk-dnr2350am | ARM 单核 | **6/6** |
| atk-dnr2350am-smp | ARM SMP | **6/6** |
| atk-dnr2350am-rv32 | RV32 单核 | **6/6** |
| atk-dnr2350am-rv32-smp | RV32 SMP | **6/6** |
| rp2350-generic | ARM 基线 | **6/6**（修复后） |
| rp2350rv-generic | RV32 基线 | **6/6**（修复后） |
| rpi-pico2 | ARM（Pico 2 板定义，同芯片刷 ATK） | **6/6** |

`rpi-pico2` 是 Pico 2 的板定义（LED GP25、无面板），刷到 ATK 上只验证**同硅**
镜像能启动跑 shell；Pico 2 真机仍归 issue #7。

## 发现并修复的 bug：embedded 镜像的 UF2 未按扇区对齐

**现象**：`rp2350-generic` / `rp2350rv-generic` 刷入后 **BOOTSEL 盘不消失、板子
不重启**（"drive did not eject"）。对照：同一 BOOTSEL 盘刷 `atk-dnr2350am-smp`
0.8s 即 eject，所以不是板子/盘的问题。

**根因**：`a06be75` 给 `fs_backing = embedded`（RAM disk）加了可启动 UF2，`flat`
长度取 `endOff + 20`（PICOBIN end block 之后即结束），**不是 4 KiB 扇区整数倍**：

```text
atk-dnr2350am-smp  flat = 0xD8000  (884736)  4K-aligned  -> 正常
rp2350-generic     flat = 0x65BF4  (416756)  非 4K        -> ROM 拒收，停在 BOOTSEL
rp2350rv-generic   flat = 0x632A4            非 4K        -> 同上
```

flash-backed 镜像恰好对齐是因为 littlefs 分区 512 KiB；embedded 镜像没有这个巧合。
RP2350 boot ROM 要求 UF2 的 flat 镜像按 4 KiB 扇区收尾，否则保持 BOOTSEL 不重启。

**修复**：`internal/build/uf2.go` 新增 `rp2350RoundToSector()`，两个 emit 函数的
embedded 分支用它把 `flat` 补到 4 KiB；flash 分支本已对齐，行为不变。加了
`TestRP2350RoundToSector` 单测钉住该不变式。

**验证**：修复后两个 generic 镜像 flat = 417792 / 409600（4K 对齐），刷入后启动并
heartbeat 6/6；`atk-dnr2350am-smp` 仍 6/6（未受影响）。

## 没跑 / 未通过

- **dd/rm 压力**：按用户要求 `NOT RUN`。
- **T2 soak / QEMU smoke**：不在本卡范围（T2 soak 另有 issue #23 的 flake）。
- **Pico 2 真机**：`NOT RUN`（issue #7 A 轨仍 OPEN）。
- **RP2040 / C3 / x86 真机**：本轮只测 RP2350；三者属其它板。
