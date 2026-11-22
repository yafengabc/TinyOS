# 锁顺序文档 + 持锁阻塞静态检查（#51）

- commit: `2b137a3`（已推送 main）
- 日期: 2026-11-22
- 变更: `docs/CONCURRENCY.md`（新）、`internal/build/lockcheck_test.go`（新）、`kernel/process.c` / `kernel/ramdisk.c` / `kernel/pipe.h`（各一行注释指文档）
- 主机: Windows / go1.27.1 windows/amd64；`GOCACHE=/d/projects/TinyOS/tmp/gocache`
- 级别: docs + T0（按 `docs/TESTING.md` 矩阵，docs-only 不要求 T1/T2/T3）

## 结果

| 项目 | 命令 | 结果 |
|---|---|---|
| 负例先红 | `go test -count=1 -run TestNoBlockingCallInsideLock ./internal/build/`（临时在 `process_pid_exists()` 的 proc_lock 区间插 `kmalloc(4)`） | **红**：`kernel/process.c:501: kmalloc inside proc_lock`，随后删除插入行 |
| 检查器自证 | `go test -count=1 -run 'TestLockCheck\|TestNoBlockingCallInsideLock' ./internal/build/ -v` | PASS：合成违规报第 3 行；同逻辑 `LOCKCHECK: allow` 放行 |
| T0 全量 | `go test -count=1 ./...` | `internal/build` FAIL —— 仅 `TestPinnedToolchainSources`：`user/csc/csc.c differs from tinyos-script/src/csc.c` |
| 基线对照 | 干净 worktree `HEAD=76408d1`（未含本次改动）跑同一命令 | **同一条 FAIL，逐字相同** → 基线问题，不是本次引入 |
| T0（排除基线已知失败） | `go test -count=1 -skip TestPinnedToolchainSources ./...` | **PASS**：`cmd/tostest`、`internal/build`、`internal/menu` 全绿 |

## 负例红证据

```
--- FAIL: TestNoBlockingCallInsideLock (0.03s)
    lockcheck_test.go:252: blocking calls inside locks (see docs/CONCURRENCY.md):
        kernel/process.c:501: kmalloc inside proc_lock
FAIL
```

## 检查器范围

扫 `kernel/*.c`（22 个文件），识别 `plat_spin_lock_irqsave` / `spin_lock` /
`spin_trylock` 与对应 unlock 的 `&name` 对，检查区间内
`proc_wait|wait_for|sleep|kmalloc|kfree|usb_cdc*|usb_tx_drain|fs_*|echo_drain` 调用。
注释与字符串字面量先抹掉但保留行号；函数边界处清掉未配对锁（跨翻译单元的持锁
不在本期范围）。逃生口为行尾 `/* LOCKCHECK: allow <理由> */`，理由必须非空。

## 未运行

- T1（18/18 构建）、T2（QEMU smoke/soak）、T3（Pico 2 真机）：**NOT RUN**。
  本次只改文档、C 注释和一个 Go 测试，没有改任何构建输入或运行期代码；按
  `docs/TESTING.md` 的 docs-only 行只需 T0。
- 基线失败 `TestPinnedToolchainSources` 的修复：**NOT RUN**，属于 `76408d1`
  （TinyOS 侧同步了 pinned 副本，`tinyos-script/src` 侧未同步），已记入
  `docs/OPEN-ITEMS.md`，不属本次范围。
