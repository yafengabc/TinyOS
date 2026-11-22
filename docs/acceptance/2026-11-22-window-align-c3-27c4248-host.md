# 窗口对齐上限改到板级 grain（#55）

- commit: `12d41c6`（已推送 main；rebase 到阶段 0 合并后的最终 sha）
- 日期: 2026-11-22
- 变更: `kernel/win_layout.h`（新）、`kernel/process.c`（win_alloc / win_largest_free）、`internal/build/winlayout_test.go`（新）、`internal/build/testdata/win_layout_harness.c`（新）
- 主机: Windows / go1.27.1 windows/amd64 / clang；QEMU rv32、rv32smp、arm
- 级别: 内核窗口分配 → T0 + T1 + T2（按 `docs/TESTING.md`）

## 根因

`win_alloc()`（`kernel/process.c`）把窗口基址对齐到"不超过窗口大小的最大 2 的幂"，这是
`4ef146c` 为整窗 NAPOT 引入的老规则。现代后端已改：C3 用 TOR PMP 且
`plat_window_needs_stack_alignment() == 0`；RP2350 RV32 用 NAPOT ladder（#44）。
对 128 KiB 窗口，旧规则要求 128 KiB 对齐；C3 任务区 240 KiB、`msh` 占前 16 KiB
后剩 224 KiB 空隙，从 +16 KiB 只能进到 +128 KiB，余 112 KiB —— 于是报
`needs a 128 KiB window, the largest free block is 224 KiB`，看似矛盾。

## 修复

- `kernel/win_layout.h:win_align()`：对齐上限取 `dom->grain`，不再随窗口大小翻倍。
  `layout_check()` 已保证：需要 stack 对齐的板（RP2350 RV32 NAPOT，grain 16 KiB =
  stack 16 KiB）的 grain 是 stack 的倍数；其余板 `plat_window_needs_stack_alignment()`
  返回 0。所以各后端的保护约束不降级。
- `win_largest_free()` 返回值按 grain 向下取整，错误消息不再报一个放不下的数字。
- `win_gap_fits()` 供宿主 harness 直接测，不改变内核里的锁与 proc 表结构。

## 结果

| 项目 | 命令 | 结果 |
|---|---|---|
| 负例先红 | 把 `win_align()` 恢复成旧的翻倍算法后 `go test -count=1 -run TestWindowPlacementLayout ./internal/build/` | **红**：139 failures，含 `FAIL C3 align is the 4 KiB grain (131072, 4096)` 与 `FAIL 128 KiB window fits the 224 KiB gap (0, 131072)`；恢复后绿 |
| 正例 | `go test -count=1 -run TestWindowPlacementLayout ./internal/build/ -v` | PASS：`win layout: ok (C3 128 KiB regression + grain sweep)`（grain 4–64 KiB 扫描） |
| T0 | `go test -count=1 ./...` | PASS（rebase 到阶段 0 合并后，无需 `-skip`） |
| T1 | 隔离 worktree `go run ./cmd/tos build all`（rebase 到 `049a0c9` 后重跑） | **18/18**：`all 18 targets built` |
| T2 | `tostest smoke -target {rv32,rv32smp,arm}` | **3/3 `SMOKE TEST OK`**（rebase 后重跑） |

## 未运行 / 边界

- **C3 真机 `NOT RUN`**：会话里没有板子。对方阶段 0 验收卡
  `2026-11-22-stage0-guards-2f1ebf3.md` 已记录同一现象为 BLOCKED，并给出板上命令；
  修后需在 C3 上重跑 `csc → rvm → rcc → cover` 与九行 expected 比对。
- QEMU 不能直接构造 C3 的 240 KiB 任务区 + 4 KiB grain（QEMU 板是 64 KiB grain、
  任务区更大），所以回归用宿主 harness 精确复现该元组；T1/T2 证明改动没破坏
  其他目标。
- T3/T4（Pico 2 真机）：`NOT RUN`，本次没有改 Pico 2 专属路径。
