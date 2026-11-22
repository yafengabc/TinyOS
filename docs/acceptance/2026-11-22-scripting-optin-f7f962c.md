# csa/pvm 改为 opt-in，默认镜像不再带旧 AMX 引擎（2026-11-22）

```text
commit:            f7f962c
change:            从 7 个 board.conf 的 features 去掉 scripting；csa/pvm 需要
                   [features] scripting = true；新增 configs/pvm.conf 和
                   configs/esp32c3-pvm.conf 供 PVM 跑分
target/config:     rpi-pico2 默认 / configs/pvm.conf；esp32c3-pvm.conf
board/port/image:  无板在机
toolchain:         go 1.27.1 windows/amd64, clang 23.1.4
host:              Windows 11 / EKSOFT
T0:                PASS — go test ./... -count=1
T1:                PASS — go run ./cmd/tos build all -> 18/18
T2:                PASS — rv32 默认镜像 smoke
T3:                NOT RUN — 无板
T4:                NOT RUN
```

## 数字（rpi-pico2）

| 镜像 | 文件数 | 块 | 占用 |
|---|---:|---:|---:|
| 默认（改前，含 csa+pvm） | 43 | 71 | 284 KiB |
| 默认（现在） | 41 | 62 | **248 KiB** |
| release（已剪） | 17 | 25 | 100 KiB |
| `configs/pvm.conf` | 43 | 71 | 284 KiB |

`csa` 6 块 + `pvm` 3 块 = 9 块 / 36 KiB，从默认镜像消失；PVM 跑分用
`configs/pvm.conf`（任意 target）或 `configs/esp32c3-pvm.conf`。源码保留在树里，
只是不再默认装进镜像；以后要彻底删源可以另开一刀（会动 bench/compare/docs）。

`docs/BENCH.md` 与 `scripts/bench/compare.py` 的 pvm 命令已指向 pvm 配置。
