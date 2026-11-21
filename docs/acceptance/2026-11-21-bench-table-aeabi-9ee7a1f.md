# 跑分总表（纵向）+ M33 goc32 div 的 EABI helper 修复（2026-11-21）

```text
commit:            9ee7a1f（docs/BENCH.md 总表）；1cd70da（user/aeabi.c）
change:            三块板的结果合成一张纵向表（板+引擎为行，四个内核为列），
                   删掉遥测侧留下的两张重复 C3 表、两张只有 rvm/rcc 的 RP2350 表；
                   aeabi.c 在 __ARM_ARCH_EXT_IDIV__ 时直接用 UDIV/SDIV
baseline:          e0c6f65（远程跑分卡片 930fd31 / 0d8d26e 的原始数字）
target/config:     docs/BENCH.md；user/aeabi.c；arm-rir（QEMU mps2）、18 目标构建
board/port/image:  QEMU mps2-an385（Cortex-M3）；无 Pico 2 在机
toolchain:         clang 23.1.4（thumbv8m.main / thumbv7m / armv6m），
                   goc32 bin/goc32.exe（-target arm），go 1.27.1 windows/amd64
host:              Windows 11 / EKSOFT
T0:                PASS — go test ./... -count=1
T1:                PASS — go run ./cmd/tos build all -> 18/18（M0+ 的循环路径、
                   M3/M33 的指令路径都参与构建）
T2:                PASS — go run ./cmd/tostest smoke -target arm / -target rv32
                   -> SMOKE TEST OK；另有 goc32 的 benchgoc 在 mps2 上前后对照（见下）
T3:                NOT RUN — 无 Pico 2；M33 的 goc32 div 需要重编 benchgoc 后重测
T4:                NOT RUN
```

## 为什么改 `aeabi.c`

远程 M33 跑分里 `goc32` 的 `div` 是 **1338 ms**，同板其它三个内核是 24–62 ms，
而同一块硅片的 RV32 侧只要 40 ms。这不是测量噪声：goc32 的 ARM 后端对每个 `%`
都发 `bl __aeabi_idivmod`（`internal/native32/arm.go`），而 `user/aeabi.c` 把
M0+ 的 32 步软件循环无条件用于所有 ARM 目标——M33 有 `SDIV/UDIV`，却仍走循环。

修复：`#if defined(__ARM_ARCH_EXT_IDIV__)` 时 `return n / d`，让 clang 生成
`udiv/sdiv`；M0+（无该宏）保持循环。`__aeabi_*divmod` 的汇编 shim 不变。

## QEMU mps2 前后对照（同一份 goc32 镜像、同一份 `bench/bench.c`）

```sh
C:\Users\SONG\Documents\Projects\goc32\bin\goc32.exe -target arm -o benchgoc-arm.tos bench/bench.c
cp benchgoc-arm.tos files.local/bin/benchgoc
go run ./cmd/tos build -config configs/arm-rir.conf
go run ./cmd/tostest smoke -target arm -commands "benchgoc"
```

| goc32 / mps2 (ms) | fib | loop | array | div |
|---|---:|---:|---:|---:|
| 修复前（e0c6f65 的 aeabi.c） | 2 | 7 | 3 | **69** |
| 修复后 | 2 | 4 | 1 | **4** |

两轮的四个校验和都是 `46368 / 999850000 / 225792000 / 599994`。
`div` 的 17× 差距与 M33 上 1338 vs 40 的方向一致，说明离群值来自 helper，
不是 goc32 整体慢。QEMU 与真机时钟不可直接换算，**表里的 1338 仍保留并标 `*`**，
等 Pico 2 重跑后再替。

## 总表

`docs/BENCH.md` 现在只有一张结果表（板+引擎为行），数字与
`2026-11-21-bench-pico2-full-930fd31.md`、`2026-11-21-bench-pico2-rv32-412038c.md`
一致；本轮只做排版与 `*` 脚注，不改任何测量值。

## 复跑

```sh
go test -count=1 ./...
go run ./cmd/tos build all
go run ./cmd/tostest smoke -target arm
go run ./cmd/tostest smoke -target rv32
```

上板（T3）需要 Pico 2 与重建的 `benchgoc`（`goc32 -target rp2350`），按
`docs/BENCH.md` 的「复现」一节重跑 M33 的 goc32 一行。
