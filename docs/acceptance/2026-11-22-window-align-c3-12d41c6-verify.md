# #55 窗口对齐修复：C3 真机复测（2026-11-22）

```text
commit under test: 12d41c6 (kernel: cap window placement alignment at the board grain)
docs state:        7e1ac04 (origin/main)
board/port:        ESP32-C3（COM4），esp32c3-generic 默认镜像（rir on）
toolchain:         go 1.27.1 windows/amd64；clang 23.1.4（宿主 harness）
host:              Windows 11 / EKSOFT
T0:                PASS — go test -count=1 ./...
T1:                PASS — go run ./cmd/tos build all -> 18/18
T2:                PASS — tostest smoke rv32 + arm
T3:                PASS — 本卡；C3 上 csc → rvm → rcc → 原生九行一致
T4:                NOT RUN
```

## 复测内容

刷入 `esp32c3-generic` 默认镜像后：

```text
tos> csc /tests/cover.c /tests/cover.rir
csc: /tests/cover.c -> /tests/cover.rir
tos> rvm /tests/cover.rir
cover: start
fib=55
array=61
struct=42
while=55
strlen=5
strcmp=0
float=3.00
cover: ok
result=0 ir=90 reps=1 exec_ms=2
tos> rcc /tests/cover.rir /bin/cover
rcc: 4284 text, 4556 elf, 4603 tos, 199 labels
tos> cover
cover: start
fib=55
array=61
struct=42
while=55
strlen=5
strcmp=0
float=3.00
cover: ok
```

`rvm` 与原生输出都等于 `tests/onboard/cover.expected` 的 9 行（脚本判定 PASS）。
修复前同一镜像报 `exec: '/bin/csc' needs a 128 KiB window, the largest free block is 224 KiB`。

`scripts/esp-c3/accept.py -p COM4 --no-reset`：**24/24 passed**（含四个 `prot` 用例、
`crash`、`wfile`/`cat`/`source`、`^C`、`reboot`、文件系统重启后仍在）。

## 命令

```sh
go test -count=1 ./...
go run ./cmd/tos build all
go run ./cmd/tostest smoke -target rv32
go run ./cmd/tostest smoke -target arm
python scripts/esp-c3/flash.py --build -p COM4
python scripts/esp-c3/accept.py -p COM4 --no-reset
# 板上：csc /tests/cover.c /tests/cover.rir; rvm /tests/cover.rir
#       rcc /tests/cover.rir /bin/cover; cover
```

## 备注

- 本卡是 #55 修复的 T3 补充；`docs/acceptance/2026-11-22-window-align-c3-27c4248-host.md`
  当时标的是 "C3 hardware NOT RUN"。
- 真机上 `rvm` 的 `exec_ms=2`（QEMU 上是 ~100），与窗口变大无关，只是模拟器计时口径。
