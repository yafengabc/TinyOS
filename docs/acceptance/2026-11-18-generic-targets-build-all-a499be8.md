# generic RP2350 镜像 + `tos build all`（2026-11-18）

```text
commit:            a06be75 (generic targets + embedded UF2/RAM disk)
                   a499be8 (tos build all)
change:            targets/rp2350-generic + rp2350rv-generic；UF2 按硅片而不是按 fs 位置；
                   plat_block_* 按 TOS_HAS_FLASH 分叉；tos build all
baseline:          e52ad7d
target/config:     15 个目标 + configs/rv32-fsro.conf
board/port/image:  无真机（ATK-DNRP2350AM 未刷）
toolchain:         go 1.27.3, clang 23.1.4, qemu 11.1.3, python 3.14.9
host:              Windows NT 10.0.17763 (MSYS2 UCRT64)
T0:                PASS
T1:                PASS 15/15 + fs_ro
T2:                PASS（smoke 4/4）
T3:                NOT RUN（无板）
T4:                NOT RUN
```

## 判据

| 判据 | 结果 |
|---|---|
| 芯片基线有镜像 | `rp2350-generic`（M33，60k heap）、`rp2350rv-generic`（Hazard3，rv32imac，32k） |
| 嵌入式文件系统也有可刷的镜像 | `build/tinyos-rp2350-generic.uf2` 834,560 B、`tinyos-rp2350rv-generic.uf2` 813,056 B（RAM disk 已链接） |
| flash 镜像不受影响 | `atk-dnr2350am-smp.uf2` 与改动前逐字节相同（sha256 `b30be458…`） |
| 一条命令构建全部 | `tos build all` → 15/15，失败汇总后非零退出 |
| RV32 不再断链 | 两台 generic 都链接通过（此前 `TOS_HAS_FLASH` 未定义导致 6 个 flash 目标引用不存在的 `__ramdisk_start`） |

## 做了什么

```text
targets/rp2350-generic.conf       board = rp2350-generic, arch = arm,  heap = 60k
targets/rp2350rv-generic.conf     board = rp2350rv-generic, arch = rv32, march = rv32imac, heap = 32k

internal/build/build.go           UF2 分派改到 (chip, arch)：rp2040 / rp2350+arm / rp2350+rv32
internal/build/uf2.go             fs_backing = flash 时拼 littlefs 分区；embedded 时只打内核
board/rp2350-generic/…            include board_config.h；plat_block_* 按 TOS_HAS_FLASH 分叉
board/rp2350rv-generic/…          同上（RV32）
cmd/tos/main.go                   build all（也接受 -target all）
```

`build all` 遍历 `targets/*.conf` 的规范目标，按名字排序，失败继续，最后给出
`N of M targets failed` 汇总。它**不读** `-config`/`tos.conf`：配置属于具体镜像。

## 失败与修复（记录在案）

第一版把 `#if !TOS_HAS_FLASH` 写进 `platform.c`，但该文件没有包含生成的
`board_config.h`，宏在编译单元里是未定义（=0）。结果 6 个 flash 目标（ATK ARM ×3、
ATK RV32 ×2、Pico 2）全部走 RAM 分支，链接器报 `undefined symbol: __ramdisk_start`。
补上 include 后，flash 后端回到原代码路径，ATK 镜像逐字节不变。

## 证据

```text
go vet ./...                                   PASS
go test ./... -count=1                         PASS
go run ./cmd/tos build all                     PASS 15/15
go run ./cmd/tos build -config configs/rv32-fsro.conf   PASS
go run ./cmd/tostest smoke -target {arm,rv32,rv32smp,x86} \
    -commands "ps, deep 3, rselftest"          4/4 SMOKE TEST OK
sha256 build/tinyos-atk-dnr2350am-smp.uf2      b30be4589b2c86b3…（前后一致）
```

## 没做 / 未验证

- **generic 两台都 `NOT RUN`**：没有上板，启动、`mount`、`ps` 均未验证；已记入
  `docs/OPEN-ITEMS.md` §10。generic 无板级外设，只有 UART0/USB 控制台；文件系统在
  RAM，重启回到镜像初始状态是预期行为。**注意区分**：ATK 复核卡
  [`2026-11-18-atk-review-rename-2d87ded.md`](2026-11-18-atk-review-rename-2d87ded.md)
  用临时 `generic-flash` 目标验证的是**芯片基线代码**（`board/rp2350-generic` + 4 MiB
  flash 分区），不是本卡这两个 embedded 目标；RAM-disk 路径仍未上板。
- **Pico 2 仍停在 #7 A 轨**（`PC=0xeffffffe`）；`rpi-pico2` 的 8 MiB flash 事实未改。
- `build all` 串行执行；并行构建未做（多个目标会写同一个 `build/gen/` 和
  `build/build-config.txt`，并行需要先按目标分目录）。
