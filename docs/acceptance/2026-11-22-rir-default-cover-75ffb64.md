# csc/rvm/rcc 默认进板级镜像；一条脚本覆盖三段流程（2026-11-22）

```text
commit:            75ffb64
change:            board.conf 默认启用 [features] rir（qemu-virt/mps2 也启用）；
                   PVM/8k 配置显式 rir = false；新增 files/tests/cover.c、
                   tests/onboard/cover.expected、scripts/rvm-cover.py；
                   accept33.py 增加 csc -> rvm -> rcc -> 原生 的覆盖项
target/config:     rpi-pico2 默认 / rv32-rir（QEMU）/ configs/pvm.conf
board/port/image:  无板在机
toolchain:         go 1.27.1 windows/amd64, clang 23.1.4
host:              Windows 11 / EKSOFT
T0:                PASS — go test ./... -count=1
T1:                PASS — go run ./cmd/tos build all -> 18/18
T2:                PASS — python scripts/rvm-cover.py（QEMU rv32）
T3:                NOT RUN — 无板；accept33.py 的同一项等上板
T4:                NOT RUN
```

## 覆盖脚本

`files/tests/cover.c` 进镜像为 `/tests/cover.c`，覆盖 int、数组、struct、
`while`、字符串、Q16.16 浮点和递归；每个检查一个函数（板上 `csc` 每函数
256 AST/IR）。一次运行：

```text
csc /tests/cover.c /tests/cover.rir
rvm /tests/cover.rir
rcc /tests/cover.rir /bin/cover
cover
```

QEMU 结果：

```text
PASS: csc -> rvm -> rcc -> native, 9 lines, both backends match
```

九行输出（`tests/onboard/cover.expected`）：

```text
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

## 镜像

- `rpi-pico2` 默认 `/bin`：41 个基础程序 + `csc`/`rvm`/`rcc` = 44 个；块数
  62 + 18 = 80（320 KiB），另有 `/tests/cover.c` 1 块。
- `configs/pvm.conf` / `esp32c3-pvm.conf` / `esp32c3-8k*`：`rir = false`，
  保持 csa/pvm 基线镜像（43 文件），不带三段工具。
- 板级镜像不再需要 `*-rir.conf`；那几个配置仍可显式使用。

复跑：

```sh
python scripts/rvm-cover.py
python scripts/rvm-cover.py --target arm
```
