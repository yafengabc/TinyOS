# RP2350（Hazard3 RV32）：PMP 窗口 NAPOT 预算逐级降级（#44）

```text
commit:            41a5b3d (TinyOS)；tinyos-script 9302f6c（未改）
change:            board/rp2350rv-generic/pmp_map.h（新）：窗口映射从"页粒度
                   code/data 切分，放不下就报错"改为按保护强度降级；
                   platform.c 只保留 CSR 写入和一次性提示；
                   internal/build/pmpmap_test.go + testdata/pmp_map_harness.c
                   在宿主机上编译真实头文件验证算法
target/config:     rpi-pico2-rv32-smp + configs/rpi-pico2-rv32-rir.conf
board/port/image:  board/rp2350rv-generic（board.conf: grain 16k,
                   stack_align = 1, guard 4k）；未刷板
                   build/tinyos-rpi-pico2-rv32-smp.uf2, 2 097 664 B,
                   sha256 c96a0ff42df59913bb205ea604b1cf8d2d9c8921f1b8485bd5dff7479b3f0bfa
toolchain:         go 1.27.1 windows/amd64, clang 18（内核）, gcc（宿主 rcc/csc/rvm）,
                   python 3.13.14
host:              Windows 11 / EKSOFT
T0:                PASS — go build ./... && go vet ./... && go test ./... -count=1
                   （含新用例 TestRP2350PmpWindowMap / TestRP2350PmpMapGrain）
                   gofmt：本轮改动的 Go 文件干净；仓库里 10 个既有文件仍未格式化
                   （internal/build/ramdisk_test.go、rv32emu/*），不在本轮 diff 内
T1:                PASS — go run ./cmd/tos build all -> 18/18
T2:                PASS — go run ./cmd/tostest smoke -target rv32 -> SMOKE TEST OK
T3:                NOT RUN — 无 Pico 2 在机（本机唯一串口 COM4 是 ESP32-C3）；
                   镜像已构建，未刷写。阻塞项：#44
T4:                NOT RUN
```

## 症状与根因

16 KiB 粒度镜像上板后，运行板上工具时串口每调度一次打印一行：

```
[pmp] window needs more NAPOT blocks than there are regions
```

Hazard3 只有 8 个软件可写 PMP region，entry 5 固定给 trap vector，所以一个任务
窗口只有 7 个 NAPOT 块。用 `scripts/elfwin.py` 的同一套窗口口径（`max(span,
raw_size) + stack + heap + guard`，再按 grain 取整）把 `build/fs-stage/bin` 里
46 个程序全部算一遍，真正越界的只有 `rcc`：

```
rcc  span 68 KiB  wx +30 KiB（页对齐 +28 KiB）  stack 16 KiB  -> 窗口 96 KiB
     code 3 块 + data 4 块 + stack 1 块 = 8 块
```

而且**取决于 `win_alloc` 给的基址**：0x20020000、0x20028000… 这些 32 KiB 对齐的
基址只要 7 块，0x20024000、0x2002c000… 要 8 块。这解释了为什么现象看起来随机。
`csc`（span 108 KiB、wx 页对齐 +48 KiB、窗口 128 KiB）在全部 8 种基址下都是 7 块，
不需要降级。

旧代码在 `pmp_layout()` 返回 -1 后仍打印并继续 `pmp_program()`，把半套映射装上
去；这条路径在每个任务切换都会走一次，于是刷屏，而且任务本身带着不完整权限运行。

## 修法

`pmp_map()` 按保护强度依次尝试，第一组放得下的胜出：

| 级别 | 映射 | 代价 |
|---|---|---|
| `SPLIT` | 页粒度 RX 前缀 + RWX data + stack | 无（正常情况） |
| `SNAP` | RX/RWX 边界向下对齐到 grain | 最多一个 grain 的 .text 变可写 |
| `IMAGE` | 整个 image 一条 RWX + stack | `.text`/`.rodata` 失去 RX |
| `WINDOW` | 整个窗口一条 RWX | 盖住 guard hole，`prot heap`/`prot stack` 不再 fault |

只有 `WINDOW` 会在开机时打印一次；四级全放不下时把用户内存全关（条目 OFF）并
只打印一次。失败级别不会留下半套条目。按同一套口径重算 46 个程序：45 个保持
`SPLIT`，`rcc` 落到 `SNAP`，没有程序需要后两级。

## 宿主机验证（新用例的实际输出）

```
rcc: SPLIT does not fit, SNAP chosen
csc: SPLIT kept
tiny: SPLIT kept
oversized: refused
sweep: split=3068 snap=1032 image=80 window=0 denied=0 (image seen 1, window seen 0)
ok
```

`pmpmap_test.go` 编译 `board/rp2350rv-generic/pmp_map.h`（纯算术，无 MMIO/汇编），
固定 rcc 与 csc 两组实测几何，并枚举 32–192 KiB 窗口的所有页粒度 code/data 切分
（4180 组映射）。每组检查：块不越出窗口、`SPLIT`/`SNAP`/`IMAGE` 不覆盖 guard
hole、每个段页都被覆盖、块数不超过 7、阶梯不跳级、失败不留残条目。

## 复跑

```sh
go test -count=1 -run TestRP2350Pmp -v ./internal/build/
go run ./cmd/tos build all
go run ./cmd/tostest smoke -target rv32
```

上板复测（T3）需要 Pico 2 的 RV32 CDC/UART 与 `configs/rpi-pico2-rv32-rir.conf`
镜像，按 #44 的三条命令跑 `csc -> rvm -> rcc -> exec`；本轮未执行，因此不改写为
PASS。
