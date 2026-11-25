# E2-B1：线程槽只留线程自己的东西（2026-11-26）

```text
commit:            f6949ab
change:            wait_pid 拆成 wait_child/join_tid；thread->leader 换成
                   proc->main（O(1)，切换路径要主线程的栈边界）；is_ktask 删除，
                   proc == NULL 就是内核线程；group exit/kill 的兄弟循环改成
                   “同 proc、不是自己”。结构清理，不打算改行为。
baseline:          e48986d（E2-A 卡）；代码 9f41f18
target/config:     rv32、rv32smp、arm、esp32c3-generic；三个 kthreads 配置、
                   两个 ktask selftest 配置、c3-kthreads
board/port/image:  QEMU rv32/rv32smp/arm；ESP32-C3 COM4（默认 + c3-kthreads）
toolchain:         go 1.27.3；clang 23.1.4；QEMU 11.1.3；Python 3.14.9；esptool
host:              Windows / 本机
T0:                PASS（go test -count=1 ./...、go vet ./...）
T1:                PASS 17/18（x86 MKFS，pre-existing，docs/OPEN-ITEMS.md §17）
T2:                PASS（soak 3 轮 3/3；kthreadtest 10/10 ×3；ktest UP/SMP；leave 用例）
T3:                PASS（C3：accept 42/42 ×2、kthreadtest all ok）
T4:                NOT RUN
logs:              build/t-e2b-*.log（不入库）
notes:             实验分支 exp/thread-sched；最后的注释整理在矩阵之后，只动注释
```

## 执行的命令与结果

```sh
go test -count=1 ./...                                            # PASS
go vet ./...                                                      # PASS
go run ./cmd/tos build -target <t>                                # 17/18（x86 §17）

go run ./cmd/tos build -target rv32 / rv32smp / arm                # PASS
python scripts/soak.py --cycles 3 --targets rv32,rv32smp,arm       # 3/3 PASS

# 三个 kthreads 配置，各 10 轮
go run ./cmd/tos build -target <t> -config configs/<t>-kthreads.conf
go run ./cmd/tostest smoke -target <t> -commands "kthreadtest 10"

# 主线程先退、工作线程还活着的路径（group exit）
go run ./cmd/tostest smoke -target rv32 \
    -commands "kthreadtest leave, ps, uptime"                      # [pid 2] killed，shell 回来

# ktask selftest（UP + SMP）
go run ./cmd/tos build -target rv32    -config configs/rv32-ktasktest.conf
go run ./cmd/tos build -target rv32smp -config configs/rv32smp-ktasktest.conf

# C3 真机
python scripts/esp-c3/flash.py --build -p COM4                     # PASS
python scripts/esp-c3/accept.py -p COM4 --no-reset                 # 42/42
go run ./cmd/tos build -target esp32c3-generic -config configs/c3-kthreads.conf
python scripts/esp-c3/flash.py -p COM4
python build/c3_cmd.py COM4 150 "kthreadtest 10"                   # all ok
python scripts/esp-c3/flash.py --build -p COM4                     # 恢复默认
python scripts/esp-c3/accept.py -p COM4 --no-reset                 # 42/42
```

## 关键输出

```text
# 尺寸：proc 多一个 main 指针，thread 掉了 leader 和 is_ktask
rv32  proc_t 484 -> 488 B   thread_t 256 B（+TOS_KTHREADS 264）  不变
arm   proc_t 484 -> 488 B   thread_t 192 B（+TOS_KTHREADS 200）  不变
默认 16 进程：表 +64 B

# group exit（rv32-kthreads）
kthreadtest: leader exits, thread 3 still alive
[pid 2] killed
pid  tid ppid prio state  c cpu% cpums wink stk/sz  name
1    1    0    1 run   0    1.1     0   64    0.5/4  msh
uptime: 1.445 s ... pid=4

# kthreadtest 10 ×3 / ktask
kthreadtest: all ok (0 failure(s))        # rv32 / rv32smp / arm
[ktest] ktask ran 5 sleeps + yield, cpu_bad=0
0    6144    581    581  ktest            # UP（帧比原来小）
0    6144    640    640  ktest            # SMP

# T2 / C3
rv32     PASS  tasks      20054016     19584  (3 cycles)
rv32smp  PASS  tasks      19988480     19520  (3 cycles)
arm      PASS  tasks       3342336      3264  (3 cycles)
42/42 passed                              # 默认镜像与 c3-kthreads 各一次
```

## 三处清理与两处“别顺手改坏”

- `wait_pid` 一个字段承担两种 id（等子进程的 pid / 等的线程 tid）。拆成
  `wait_child` 与 `join_tid` 后，`ST_WAIT_CHILD` 与 `ST_WAIT_THREAD` 的唤醒路径
  各读各的，不再靠状态隐含含义。
- `thread->leader` → `proc->main`：切换路径（`apply_protection`）每次都要拿主线程
  的 `stack_top/stack_size` 和窗口，用 `is_main` 标志 + 扫表会在 trap 路径里多一次
  32 项扫描；指针是 O(1)。代价是 `proc` 多 4 B、`thread` 少 4 B。
- `is_ktask` → `proc == NULL`：两者本来就是同一件事，删掉一个字段后 trap/切换、
  `make_ready`/`unblock`、栈守卫、ps/threads、ktask syscall 都读已经加载的指针。
- 精确性：`task_release()` 与 `stack_grow()` 的早退只针对**工作线程**
  (`p->proc && !thread_is_main(p)`)，内核线程仍走原来的路径（它们有栈高水位要记，
  没有 fd 要关）。
- 兄弟循环从 `q->leader == leader` 改成 `q->proc == p->proc && &q != p`，并在
  `p->proc == NULL`（内核线程退出）时不进循环——否则会匹配到所有空槽的
  `proc == NULL`。

## 没跑的

- **M33 / RP2040 / Pico 2 真机**：本机只有 C3 的 COM4；真机清单在
  `docs/OPEN-ITEMS.md` §22。
- **T4**：没有对应测试项。
- **x86**：`MKFS: No more free space`，HEAD 上同样失败（§17）。
- **WiFi 镜像**：需要外部 lib 目录，本轮未构建。
- E2-B2（`proc->state` 状态机、信号归属）尚未开始。
