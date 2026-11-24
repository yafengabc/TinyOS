# Pico 2 四镜像回归（`[user] rvc = auto`）

commit: `6b15b8b`
date: 2026-11-24
board/port: Raspberry Pi Pico 2 (RP2350) — CH343 UART `COM19`，USB CDC `COM27`
toolchain: go 1.27.1 windows/amd64, clang 23.1.4, python 3.14.9
flash: UF2 → RP2350 BOOTSEL（`scripts/rp2350/bootsel_poke.py COM27 --dtr` + `flash.py`）

验证的是 `0ec6c2b…6b15b8b` 这组改动：RVC 从“整镜像去 C”改成板级策略
`[user] rvc = auto`——先按 target 的 ISA 编译链接，只把链接后确实跨 4 KiB 页的
程序换成 no-RVC 重链，`buildFSImage()` 里的 `checkCodePages()` 是最后的门禁。
`6b15b8b` 同时带入 `exec` 的 cwd 语义、`layoutcheck.go`（board.conf 与 layout.c
三个窗口数必须一致）。

## 构建

`go run ./cmd/tos build -target <4 targets>` 4/4 PASS。两个 RV32 镜像都只回退六个
程序（28 KiB 的 code run 必跨页）：

```text
RVC bin/edit: rebuilt without compressed instructions
RVC bin/pio:  rebuilt without compressed instructions
RVC bin/sh:   rebuilt without compressed instructions
RVC bin/csc:  rebuilt without compressed instructions
RVC bin/rcc:  rebuilt without compressed instructions
RVC bin/rvm:  rebuilt without compressed instructions
```

`msh` 保留压缩指令，窗口回到 16 KiB（`__riscv_compressed` 分支）。ARM 两镜像没有
RVC 回退行。

## 结果

| 项 | rpi-pico2 | rpi-pico2-smp | rpi-pico2-rv32 | rpi-pico2-rv32-smp |
|---|---|---|---|---|
| heartbeat | 6/6 PASS | 6/6 PASS | 6/6 PASS | 6/6 PASS |
| smptest | — | 11/11 clean | — | 11/11 clean |
| smptest --func | — | 6/7（注 1） | — | 6/7（注 1） |
| dd_stress 200 轮 | — | 200/200 | 200/200 | 200/200 |
| smpbench 1250000 | — | 2.05x | — | 2.00x |
| reboot_soak 20 次 | — | 20/20（0.40 s） | — | 20/20（0.56 s） |
| CSC cover/state/ready | 3/3（cover 全链；state/ready 到 rvm） | 3/3 | 3/3（cover 全链；state/ready 到 rvm） | 3/3 |
| `free`/`mem`/`smp`/`prot *` | PASS | PASS | PASS | PASS |

CSC 判据同前：`csc` 产 RIR → `rvm` 执行 → `rcc` 产镜像 → 执行镜像，rvm 与 native
输出一致。各条链的 `rcc` 规模：

- ARM：`cover` 4612 text / 199 labels；`state` 7200 / 271；`ready` 3268 / 118。
- RV32：`cover` 4284 / 199；`state` 5244 / 278；`ready` 2896 / 114。
  （单核两镜像只跑了 cover 的完整链，state/ready 跑的是 `csc`+`rvm`。）

窗口与保护（启动日志）：ARM SMP `[mpu] win=20020000..20024000 wx=20021500
regions=3`；RV32 单核 `[pmp]` 路径下 `prot heap`/`prot stack` 仍分别
`store access fault`（`0x…c0de` / `0x…c110`），ARM 对应 `MemManage`——守卫洞在
`rvc = auto` 下依旧未被覆盖。

`exec` 的 cwd 语义（`b1921a2`）在板上核对：`cd /tests` 后裸名 `cover.bin` 直接运行，
`cd /` 复位正常。

T0: 只跑了 `go test ./internal/build -skip TestPinnedToolchainSources` PASS；
全量 `go test ./...` `NOT RUN`。
T2: `NOT RUN`（本轮无 QEMU）。
T4: 两个 SMP 镜像按 TESTING §5.4 门槛（dd ≥100、smpbench ≥1.8x、20 次重启）PASS；
两个单核镜像未做 soak。

## 注

1. 两个 SMP 镜像的 `smptest --func` 都是 6/7，唯一 FAIL 是 `py demo.py`：RP2350
   默认镜像自 `f7f962c` 起不带 `pvm`（opt-in），与上一张卡片一致。
2. `flash_heartbeat.py` 首次在 `COM27` 上打不开端口（`PermissionError`，非本轮进程
   持有）；镜像已刷好，改用 `heartbeat.py -p COM19` 在运行中的镜像上跑 6/6，次日
   `bootsel_poke.py COM27` 恢复正常。
3. 板子最后运行 `rpi-pico2-rv32`（单核）；`/tests` 下留着本轮生成的
   `cover/state/ready` 的 `.rir` 与 `cover.bin`。
4. 上一张卡片（`2026-11-23-pico2-matrix-f91db82.md`）里“所有用户程序
   `-march=rv32ima`”的修复 1 已被本轮的 `[user] rvc = auto` 取代。
