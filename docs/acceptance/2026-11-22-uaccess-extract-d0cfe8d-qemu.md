# 最小抽取 user_range_ok / checked add+align（#54）

- commit: `d0cfe8d`（已推送 main）
- 日期: 2026-11-22
- 变更: `kernel/uaccess.{c,h}`（新）、`kernel/u32math.h`（新）、`kernel/elf.c`、`kernel/process.c`、`internal/build/uaccess_equiv_test.go`（新）、`internal/build/testdata/uaccess_equiv_harness.c`（新）
- 主机: Windows / go1.27.1 windows/amd64 / clang；QEMU rv32、rv32smp、arm
- 级别: 内核 + 构建源码 → T0 + T1 + T2（按 `docs/TESTING.md`）

## 结果

| 项目 | 命令 | 结果 |
|---|---|---|
| 负例先红 | 删 harness 参照实现的 overflow 分支后 `go test -count=1 -run TestUaccessExtractionEquivalent ./internal/build/` | **红**：4495 mismatches，逐条打印输入（如 `lo=308e16e9 hi=b83ab7c0 va=369ad481 len=ed529204: old=1 new=0`）；恢复后绿 |
| 等价差分 | `go test -count=1 -run TestUaccessExtractionEquivalent ./internal/build/ -v` | PASS：`uaccess equivalence: ok (4096 boundary + 20000 random tuples)` |
| T0 | `go test -count=1 -skip TestPinnedToolchainSources ./...` | PASS（`cmd/tostest`、`internal/build`、`internal/menu` 全绿） |
| T0 全量 | `go test -count=1 ./...` | 仅 `TestPinnedToolchainSources` FAIL：`user/csc/csc.c differs from tinyos-script/src/csc.c` —— 基线问题 `76408d1`，对方线程当时仍在逐文件同步，见 OPEN-ITEMS §15 |
| T1 | 隔离 worktree `HEAD=7d903b1` + 本次改动，`go run ./cmd/tos build all` | **18/18**：`all 18 targets built`；12 UF2、17 kernel ELF、20 bin、`build/build-config.txt` 齐全 |
| T2 smoke | 同一隔离 worktree，`tostest smoke -target {rv32,rv32smp,arm} -commands "ps, ls /bin \| wc, cat /proc/execfmt, piperace, prot stack, deep 3, rselftest"` | **3/3 `SMOKE TEST OK`**；有已知的 prompt-echo flake 警告（`piperace`/`deep 3` 偶发未回显但输出正确），不是本次引入 |
| ELF note 正向 | T1 产物 `build/fs-stage/bin/stk` 的 PT_NOTE（typ=1） | `stack=16384`（0x4000），PT_LOAD span 上取整 4096；`git diff internal/build/build.go` 为空 → Go 侧 `windowNote()` 逐字节未改，解析值必然不变 |

## 纯搬移证据

- `uaccess_range_ok` 四个分支（`len==0` 放行、`va<lo`、`va+len` 回绕、`va+len<=hi`）与抽取前 `elf.c:127` 的 `user_range_ok` 逐字一致，只把 `v->lo/v->hi` 换成入参 `lo/hi`。
- `align_up` / `add_u32_ok` / `align_up_ok` 从 `process.c:445-474` 原样搬入 `u32math.h` 的 `static inline`，语义与数值边界不变（差分 harness 逐条比对）。
- 5 处 `user_range_ok(v, ...)` 调用点全部改为 `uaccess_range_ok(v->lo, v->hi, ...)`。

## 撞车说明（T1 重跑原因）

第一次 T1/T2 在同一工作区后台并发跑，两者共用 `build/user/*.o`，互相覆盖成别的架构 → `ld.lld: ... is incompatible with armelf / elf32lriscv / elf_i386`，多个目标 LD 假失败。停掉后改用独立 worktree（独立 `build/`）重跑 T1 与 T2，18/18 与 3/3 全过。这不是代码问题；以后**不要并发跑 `build all` 与 `tostest smoke`**，也不要两个 agent 共用同一工作区构建。

## 未运行

- T3（Pico 2 真机）、T4：本次是纯搬移，行为等价由差分 harness + T1/T2 覆盖，**NOT RUN**。
- `TestPinnedToolchainSources`：基线失败，修复属于对方线程在同步的 pinned 源，不在本次范围。
