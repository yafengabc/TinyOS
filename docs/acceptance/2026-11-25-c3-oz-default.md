# ESP32-C3 默认改用 `-Oz`：板级 A/B 与 per-target `opt` 旋钮（2026-11-25）

```text
TinyOS commit:     本提交
change:            给 [target] 加 opt 旋钮；esp32c3-generic 默认 -Oz；
                   同时修 accept.py 的提示符匹配（每次命令都在等超时）
target/config:     esp32c3-generic / targets/esp32c3-generic.conf（-Oz）；
                   18 目标全量构建；arm QEMU smoke + soak
board/port/image:  ESP32-C3 / CH343 COM4 / build/tinyos-esp32c3-generic.bin
toolchain:         go 1.27.1 windows/amd64；clang 23.1.4
T0:                PASS（`go test -count=1 ./...`）
T1:                PASS — 18/18
T2:                PASS — arm smoke；soak rv32+arm 2/2
T3:                PASS — C3：accept 42/42；`-O2` vs `-Oz` 各 5 轮（见下）
T4:                NOT RUN
```

## 1. 为什么动这一块

release 镜像（`configs/release.conf`）已经关掉诊断（Pico 2 SMP 111.6 KiB），
所以 flash 容量不紧张；但 C3 的代码预算只有前 512 KiB（`board.conf` 的
`lfs_offset`），而 `-Oz` 在它上面能省 19 KiB。问题上限一直是"`-Oz` 会不会慢"，
所以先在 C3 上量，再决定是否给这一个 image 开。

## 2. C3 板级 A/B（COM4，5 轮，`uptime` 前后取差）

| 项目（min / 中位） | `-O2` 100.7 KiB | `-Oz` 81.6 KiB | 差（min） |
|---|---|---|---|
| 启动到提示符 | 0.41 / 0.42 s | 0.40 / 0.41 s | −0.01 s |
| `dd of=/b.bin bs=4096 count=100`（写 400 KiB） | 3628 / 3708 ms | 3685 / 3704 ms | +1.6 % |
| `dd if=… `（读回+校验） | 356 / 358 ms | 356 / 369 ms | ±0 |
| `dd of=/b2.bin bs=4096 count=200`（写 800 KiB） | 7255 / 7307 ms | 7240 / 7294 ms | −0.2 % |
| `csc /tests/cover.c` | 262 / 264 ms | 263 / 266 ms | +0.4 % |
| `csc /tests/state.c` | 279 / 367 ms | 294 / 296 ms | +5.4 %（双峰，见注） |

注：`csc state` 在两个镜像里都是双峰（~280 ms / ~370 ms），中位数一个 367 一个 296——
这是堆状态导致的，不是 `-Oz` 的贡献。全部差异都在同镜像 30 % 的抖动里，**没有可测退化**。

## 3. 改了什么

| 位置 | 内容 |
|---|---|
| `internal/build/build.go` | `Target` 新增 `Opt`；`ccFlags()` 的链条变成 `TOS_OPT` > config `[build] opt` > `[target] opt` > `-O2`；`build-config.txt` 的 provenance 写清是哪一层 |
| `internal/build/targets.go` | 解析并校验 `[target] opt`（只接受 `-O0..-O3/-Os/-Oz`，拼错直接报错） |
| `internal/build/inischema.go` | `opt` 加进 target schema |
| `targets/esp32c3-generic.conf` | `opt = -Oz`，注释里带测量与理由 |
| `scripts/esp-c3/accept.py` | `buf.rstrip().endswith(MARK)` 永远匹配不到 `"tos> "`（rstrip 把空格吃了），每条命令都在等满 timeout；改成 `buf.endswith(MARK)`，一轮从 ~10 min 降到 ~1 min |
| `targets/README.md`、`configs/README.md` | 记录该旋钮与优先级 |

其它 image 保持 `-O2`：M33 的 `-Oz` 收益要大得多（−103 KiB），但它的热路径里有
flash-park（RAM 函数）、USB CDC、SMP 自旋，必须先在 Pico 2 / ATK 上按同一套脚本量过，
这就是 issue #56 里 P1 之后的工作。

## 4. 结果

- C3 `accept.py`：**42/42 passed**（含写给自身复位后的文件、`reboot` 后文件系统仍在）。
- 刷的是 target conf 的默认镜像（`-Oz`，83516 B），再跑一轮 3 次 bench 确认与上表一致。
- 板上最后留的就是这个默认 `-Oz` 镜像。

## 5. 未跑

- M33（Pico 2 / ATK）与 RP2040 的 `-Oz` A/B：`NOT RUN`，等 P1（冷路径 `noinline`）
  落地后用同一脚本做，结论写回 #56。
- 大负载（`dd 200` 轮 soak、`tosput` 长传）没有在 `-Oz` 下重复：C3 上 `dd` 800 KiB
  写路径已覆盖 littlefs + flash + CRC。
