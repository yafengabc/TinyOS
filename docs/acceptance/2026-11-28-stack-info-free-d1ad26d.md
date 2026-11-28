# 栈区可观测：SYS_STACKINFO、free 的栈区行、1000 次混合回收（2026-11-28）

```text
commit:            d1ad26d  kernel: SYS_STACKINFO exposes the stack area to free
                    bf22d8b  kthreadtest: 1000 mixed 4K/16K create/join cycles
                    0069571  docs: stack area accounting is observable from
                            free and the reclaim test
change:            新增 SYS_STACKINFO（59）+ struct stackarea（reserved/live/
                   peak/blocks）；free 多一行 stack area；kthreadtest 增加
                   1000 次 4K/16K 混合 create/join，检查 live 和 reserved 回到
                   基线；abi/syscalls.csv 与 goc32 快照已更新
target/board:      ESP32-C3（COM4，默认 + kthreads）；ARM/rv32/rv32smp QEMU
                   （无栈区 N/A 分支）；x86 QEMU；18 个构建目标
toolchain:         go 1.27.3 windows/amd64；clang 23.1.4；LLD 23.1.4
T0:                PASS（go test ./...、go vet；含 syscall 表/快照测试）
T1:                PASS 18/18
T2:                PASS smoke rv32 / rv32smp / arm / x86；compat rv32,arm PASS
T3:                C3 PASS（默认 accept 42/42；kthreads：kthreadtest 14 slots
                   all ok、1000 mixed live 8192->8192 / reserved 16384->16384 /
                   peak 32768、prodcons 32768 B、threadbench x2 后再 kthreadtest
                   仍 all ok）；ARM/rv32/rv32smp QEMU kthreadtest PASS（打印
                   N/A）；RP2040 / Pico 2 NOT RUN（未接板）
T4:                NOT RUN（未接板）
logs:              build/t-p3-*.log（本地，不入库）
```

## 做了什么

- **`SYS_STACKINFO`（编号 59）**：`struct stackarea { reserved, live, peak,
  blocks }`。`reserved` 是当前栈区提交的地址空间（没有栈区为 0），`live` 是
  活动栈块之和，`peak` 是启动以来的最大 `reserved`，`blocks` 是活动 4 KiB 块数。
  `live` 直接由 buddy bitmap 的 popcount 乘 4 KiB 得到，不会和分配器账本漂移。
- **ABI**：只追加 syscall 和结构，`TOS_ABI_VERSION` 不变；`abi/syscalls.csv`
  和 `goc32/abi/syscalls.snapshot.csv` 用 `scripts/syscalls-snapshot.py` 更新；
  `tos_abi.h`/`tos_abi_asserts.h` 有 sizeof/offset 断言。
- **`free`**：有栈区的板子多一行：
  `stack area  16384  16  (live 8192, peak 16384)`。没有栈区的板子不打印这一行
  （打印 0 会被读成“满了”，而不是“没有配置”）。
- **`kthreadtest`**：200 次 4K 循环之后增加 1000 次 4K/16K 交替循环。16K 块
  迫使栈区从 16 KiB 长到 32 KiB，释放后又缩回；`live`/`reserved` 与循环前
  比较（`live` 包含 shell 和 leader 自己的栈）。无栈区板子打印
  `N/A 1000 mixed create/join (no stack area)`，仍然跑完前面的检查。

## 实测

```text
C3 默认镜像:
  free（刚启动）  -> stack area 16384  16  (live 8192, peak 16384)
  stk 4 3000 &    -> stack area 32768  32  (live 24576, peak 32768)
  stk 退出后      -> stack area 16384  16  (live 8192, peak 32768)   # 缩回
  accept.py       -> 42/42

C3 kthreads:
  kthreadtest -> 14 thread slot(s) usable beside shell+self
                 1000 mixed 4K/16K create/join, live 8192 -> 8192,
                 reserved 16384 -> 16384, peak 32768
                 all ok (0 failure(s))
  prodcons    -> 32768 bytes, both threads blocked and woke
  threadbench -> ratio 105% / 105%，两轮后再 kthreadtest 仍 all ok

QEMU:
  arm / rv32 / rv32smp kthreadtest -> N/A 1000 mixed ...; all ok
  rv32/rv32smp/arm/x86 smoke      -> SMOKE TEST OK
  compat rv32,arm                 -> COMPAT OK
```

1000 次混合循环在 C3 上约 1.3 s（含 500 次 16 KiB 增长/缩回）。

## 没验证什么

- RP2040 / Pico 2 的 T3/T4 未跑（未接板）；这两个端口没有启用栈区，
  只验证了无栈区回退和 N/A 分支。
- `free` 的 `tasks` 行仍是整个用户区；栈区数字是额外的行，不是从 `tasks`
  里减出来的。这是有意的：栈区是地址空间预留，不是已用 RAM。
- ARM 的 MPU 栈区形状、x86/RP2040 的栈区布局仍未启用。
