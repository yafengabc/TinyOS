# 架构 review 阶段 0（护栏）：T0/T1/T2 + C3 验收（2026-11-22）

```text
commits:           2f5629b config 严格化（#48）
                   edf26ae 构建期窗口不变量 + mklfs 栈 hint 检查（#49）
                   a6e35d4 共享畸形 corpus（ELF/LZ4，Go + 内核解码器）（#50）
                   4b28fda 宿主 harness（uaccess / u32math / pipe）（#53）
                   de284dc heap harness 不再 Skip（#53）
                   —— 另：2b137a3 锁顺序 + 检查（#51）、d0cfe8d 抽取（#54）由并行线程先落
注:                以上原为 rebase 前的 da49262/d2318f9/6989829/5f32397/2f1ebf3，
                   在仓库中均不存在；2026-11-22 按 README 规则换为已推送的最终 sha。
target/config:     rpi-pico2、esp32c3-generic；rv32/arm QEMU（默认）
board/port:        ESP32-C3（COM4），esp32c3-generic 默认镜像
toolchain:         go 1.27.1 windows/amd64；clang 23.1.4（宿主 harness）
host:              Windows 11 / EKSOFT
T0:                PASS（本 run 时本地有 TestPinnedToolchainSources 失败，见"未通过"§2；
                   该同步已于 2026-11-22 完成，现 `go test -count=1 ./...` 全绿）。
T1:                PASS — go run ./cmd/tos build all -> 18/18
T2:                PASS — tostest smoke rv32 + arm
T3:                PASS（部分）— esp32-c3 accept.py 24/24
T4:                NOT RUN
```

## 各项证据

| 提交 | 负例先红 | 正例 |
|---|---|---|
| 2f5629b | `heep = 8k` → `unknown key "heep"`（带行号）；`pvm-float = true` → `unknown feature`；未知 section 各一 | 59 个仓库配置全解析；18/18 构建 |
| edf26ae | 暂存 ELF 的 stack note 改 6144、不带 `--grain` → mklfs 拒绝（证明是新加的检查）；Go 侧 4 个窗口几何负例 | 18/18 构建；`stack_align` 板仍通过 |
| a6e35d4 | 改错一条 `expect` → 红并打印 `got/want` | 12 个 case；LZ4 由 Go 端口与真实 `kernel/lz4.c` 双向比对 |
| 4b28fda | 把 `last byte` 的期望改成 0 → `FAIL last byte: got 1 want 0` | uaccess/u32math/pipe 三个 harness 通过 |
| de284dc | 无 CC 时 `t.Fatal`（本机有 clang，未人为移除） | T0 通过 |

## C3 板测（COM4）

- 镜像：`esp32c3-generic` 默认（`rir` on，含 `csc`/`rvm`/`rcc`，`/tests/cover.c`）。
- `scripts/esp-c3/accept.py -p COM4 --no-reset`：**24/24 passed**（启动、`ls`、
  `hello`、`hello_printf`、`mem`、`uptime`、`ps`、四个 `prot`、`crash`、shell 存活、
  `wfile`/`cat`/`source`、`^C`、`reboot`、文件系统重启后仍在）。
- `free`：tasks 240 KiB，未用 9 KiB；`ps` 显示 `msh` 窗口 16 KiB。
- 覆盖脚本 `csc → rvm → rcc → 原生`：**BLOCKED**（见下）。

## 未通过 / 未运行

1. **C3 上 `csc` 无法 exec**（原有问题，不是本批引入，已开 #55）：

   ```text
   tos> csc /tests/cover.c /tests/cover.rir
   exec: '/bin/csc' needs a 128 KiB window, the largest free block is 224 KiB
   ```

   任务区 240 KiB、`msh` 16 KiB；`win_alloc()` 对 128 KiB 窗口要求 128 KiB
   基址对齐，最坏浪费 112 KiB，224 KiB 空隙放不下 128 KiB。`rvm`/`rcc`/`cover`
   因此没有输入，九行比对无法进行。
2. **`TestPinnedToolchainSources` 在共享工作区红**（本 run 时的状态，**已修复**）：
   当时 `../tinyos-script` 比 `user/{csc,rvm,rcc}` 新（并行线程正在同步，9449fdd 等）。
   同一测试在没有 `../tinyos-script` 的干净 worktree 里 Skip，仓库自身不红。
   2026-11-22 双方 pinned 源已同步完毕，现 `go test -count=1 ./...` 全绿；
   `docs/OPEN-ITEMS.md` §15 已相应标记为已解决。
3. **#52（`platform.h` 窄头 + include 约束）未做**；#47 保持打开。
4. T4 `NOT RUN`；RP2040/Pico 2 未参与本轮。

## 并行线程的重复

#51 与 #54 在本分支开发期间已由并行线程落到 `main`（`2b137a3`、`d0cfe8d`），
本分支的对应提交（`268c971`、`dbe4828`）和它们的卡片已丢弃，改为 cherry-pick
其余提交到最新的 `origin/main` 上验证；`docs/CONCURRENCY.md` 以并行线程的版本为准。

## 命令

```sh
go test -count=1 ./...                 # 共享工作区会红在 pin 同步
go test -count=1 -skip TestPinnedToolchainSources ./...
go run ./cmd/tos build all
go run ./cmd/tostest smoke -target rv32
go run ./cmd/tostest smoke -target arm
python scripts/esp-c3/flash.py --build -p COM4
python scripts/esp-c3/accept.py -p COM4 --no-reset
```
