# 评审三项线程退出缺陷的修复（2026-11-26）

```text
commit:            a957cbd
change:            a) 组退出时描述符在 proc_lock 之外释放（管道对端唤醒会再取锁，
                      SMP 上死锁，UP 因自旋锁是空操作而掩盖）；b) 同一 tid 只允许
                      一个 join 者，第二个返回 -1；c) 进程退出中拒绝创建线程，
                      发布前在锁内复查并把槽还回去。
baseline:          0ef1393（评审提交的三个探针）
target/config:     rv32-kthreads、rv32smp-kthreads、arm-kthreads；C3
board/port/image:  QEMU rv32/rv32smp/arm；ESP32-C3 COM4
toolchain:         go 1.27.3；clang 23.1.4；QEMU 11.1.3；Python 3.14.9；esptool
host:              Windows / 本机
T0:                PASS（go test -count=1 ./...、go vet ./...）
T1:                PASS 17/18（x86 MKFS，pre-existing，docs/OPEN-ITEMS.md §17）
T2:                PASS（三个探针 + 默认 kthreadtest 10/10 ×3；soak 3 轮 3/3）
T3:                PASS（C3 accept 42/42）
T4:                NOT RUN
logs:              build/t-fix-*.log（不入库）
notes:             探针默认不进 kthreadtest；它们是命名子命令
```

## 三个缺陷与修法

1. **pipes（SMP 死锁）**：`finish_group_locked()` 在 `proc_lock` 内调用
   `task_release()`；关描述符会走 `process_pipe_closed()` → 再取 `proc_lock`。
   UP 上 `plat_spin_acquire()` 是空操作所以看不出来，rv32smp 上直接
   `[spin] cpu 1 stuck on lock` panic。
   **修法**：最后一个 worker 先解锁 → `task_release(main)` → 再取锁
   `finish_group_locked()`（只做 reap + 释放）。窗口里进程是 `PROC_DYING`、
   主线程 `ST_DYING`、worker 还占着自己的槽，没人能复用。
2. **double-join**：第二个 join 者在 `ST_WAIT_THREAD` 里排在第一个后面，
   `wake_joiner_locked()` 只投递一次，它就永远睡着。
   **修法**：join 时若已有同 tid 的等待者，直接 -1（POSIX 未定义；拒绝是唯一
   不泄漏槽的答案）。
3. **create-exit**：退出中的进程仍能创建线程，新槽指向即将退休的窗口。
   **修法**：`process_thread_create()` 在进程非 `PROC_LIVE`/主线程 `ST_DYING` 时
   拒绝（先查一次，发布时锁内再查一次并归还已认领的槽）。

## 执行的命令与结果

```sh
go test -count=1 ./...                                            # PASS
go vet ./...                                                      # PASS
go run ./cmd/tos build -target <t>                                # 17/18（x86 §17）

# 三个探针，UP + SMP + ARM
go run ./cmd/tostest smoke -target rv32    -commands "kthreadtest double-join, kthreadtest pipes"
go run ./cmd/tostest smoke -target rv32smp -commands "kthreadtest double-join, kthreadtest pipes"
go run ./cmd/tostest smoke -target arm     -commands "kthreadtest double-join, kthreadtest pipes"
# create-exit 连跑 3 次（SMP）
go run ./cmd/tostest smoke -target rv32smp -commands "kthreadtest create-exit"

python scripts/soak.py --cycles 3 --targets rv32,rv32smp,arm       # 3/3 PASS
python scripts/esp-c3/flash.py --build -p COM4                     # PASS
python scripts/esp-c3/accept.py -p COM4 --no-reset                 # 42/42
```

## 关键输出

```text
# double-join（rv32 / rv32smp / arm 都是这个结果）
kthreadtest: first joiner got 42, second got -1
kthreadtest: ok   first joiner collected the exit code
kthreadtest: ok   second joiner refused instead of sleeping forever

# pipes（rv32smp：修前 panic，修后回到提示符）
kthreadtest pipes: leader exits holding 3/4, reader 260 asleep
kthreadtest pipes: expect status 3 and a prompt (no prompt = deadlock)
tos> ps
pid  tid ppid prio state  c cpu% cpums wink stk/sz  name

# create-exit（rv32smp，3 次）
kthreadtest create-exit: exit after 208 cycle(s), sibling flagged 0
kthreadtest create-exit: exit after 222 cycle(s), sibling flagged 0
kthreadtest create-exit: exit after 202 cycle(s), sibling flagged 0

# 默认测试不变
kthreadtest: all ok (0 failure(s))
soak: rv32/rv32smp/arm 3/3 PASS
```

## 没跑的

- **M33 / RP2040 真机**：本机无板；同一探针清单在 §22。
- **T4**：没有对应测试项。
- **x86**：`MKFS: No more free space`（§17）。
