# 架构 review 阶段 0（护栏）：T0/T1/T2 + C3 验收（2026-11-22）

```text
commits:           104bad1 config 严格化
                   b4b35ce 构建期窗口不变量 + mklfs 栈 hint 检查
                   7468a0a 共享畸形 corpus（ELF/LZ4，Go + 内核解码器）
                   268c971 锁顺序文档 + 持锁阻塞静态检查
                   dbe4828 抽取 uaccess_range_ok / u32math
                   ed9a985 宿主 harness（uaccess / u32math / pipe）
                   5dc4dca heap harness 不再 Skip
target/config:     rpi-pico2、esp32c3-generic；rv32/arm QEMU（默认）
board/port:        ESP32-C3（COM4），esp32c3-generic 默认镜像
toolchain:         go 1.27.1 windows/amd64；clang 23.1.4（宿主 harness）
host:              Windows 11 / EKSOFT
T0:                PASS — go test ./... -count=1
T1:                PASS — go run ./cmd/tos build all -> 18/18
T2:                PASS — tostest smoke rv32 + arm
T3:                PASS（部分）— esp32-c3 accept.py 24/24；见下
T4:                NOT RUN
```

## 各项证据

| 提交 | 负例先红 | 正例 |
|---|---|---|
| 104bad1 | `heep = 8k` → `unknown key "heep"`（含行号）；`pvm-float = true` → `unknown feature`；未知 section 各一 | 59 个仓库配置全通过；18/18 构建 |
| b4b35ce | 暂存 ELF 的 stack note 改 6144 → mklfs 拒绝（不带 `--grain`，证明是新检查）；Go 侧 4 个窗口几何负例 | 18/18 构建；`stack_align` 板仍通过 |
| 7468a0a | 改错一条 `expect` → 红并打印 `got/want` | 12 个 case，LZ4 由 Go 与真实 `kernel/lz4.c` 双向比对 |
| 268c971 | `TestLockDisciplineDetects` 内建违规/豁免/无关三例 | 全仓 0 命中 |
| dbe4828 | —（纯搬移） | T0/T1/T2 + corpus 结果不变 |
| ed9a985 | 把 `last byte` 的期望改成 0 → `FAIL last byte: got 1 want 0` | uaccess/u32math/pipe 三个 harness 通过 |
| 5dc4dca | 无 CC 时 `t.Fatal`（本机有 clang，未人为移除） | T0 通过 |

## C3 板测（COM4）

- 镜像：`esp32c3-generic` 默认（`rir` on，含 `csc`/`rvm`/`rcc`，`/tests/cover.c` 1 块）。
- `scripts/esp-c3/accept.py -p COM4 --no-reset`：**24/24 passed**（启动、`ls`、
  `hello`、`hello_printf`、`mem`、`uptime`、`ps`、四个 `prot` 用例、`crash`、
  shell 存活、`wfile`/`cat`/`source`、`^C`、`reboot`、文件系统重启后仍在）。
- `free`：tasks 240 KiB，未用 9 KiB；`ps` 显示 `msh` 窗口 16 KiB。
- 覆盖脚本 `csc → rvm → rcc → 原生`：**BLOCKED**，见下。`rvm`/`rcc`/`cover`
  因此没有输出，`tests/onboard/cover.expected` 无法比对。

## 未通过 / 未运行

1. **C3 上 `csc` 无法 exec**（原有问题，不是本批改动引入）：

   ```text
   tos> csc /tests/cover.c /tests/cover.rir
   exec: '/bin/csc' needs a 128 KiB window, the largest free block is 224 KiB
   ```

   `free` 显示任务区 240 KiB、`msh` 占 16 KiB，剩 224 KiB；`win_alloc()`
   对 128 KiB 窗口要求 128 KiB 基址对齐（`kernel/process.c` 的 `align` 循环
   `align*2 <= bytes`），240 KiB 区域内最坏浪费可达 112 KiB，于是 224 KiB
   的空隙放不下。已开 issue 记录，修复属于行为改动，不在阶段 0。
2. **#52（`platform.h` 窄头 + include 约束）未做**：本批只完成了 #48/#49/#50/
   #51/#53/#54。#47 保持打开直到 #52 落地。
3. T4 `NOT RUN`；RP2040/Pico 2 未参与本轮。

## 命令

```sh
go test -count=1 ./...
go run ./cmd/tos build all
go run ./cmd/tostest smoke -target rv32
go run ./cmd/tostest smoke -target arm
python scripts/esp-c3/flash.py --build -p COM4
python scripts/esp-c3/accept.py -p COM4 --no-reset
```
