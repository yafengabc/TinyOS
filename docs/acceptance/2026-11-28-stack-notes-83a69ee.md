# 无堆程序声明 4 KiB 栈：窗口再省 4 KiB（2026-11-28）

```text
commit:            83a69ee  user: declare 4 KiB stacks for the programs that
                            have no heap
change:            sh / tools / help / ls / sleep 加 TOS_STACK(4 KiB)：
                   内核不再给它们 4 KiB 的 grow 预留（窗口精确到 span+栈+guard）
target/board:      ESP32-C3（COM4，实机）；18 个构建目标；QEMU smoke
toolchain:         go 1.27.x；clang/ld.lld 23.1.4
T0:                PASS（go test ./...、go vet）
T1:                PASS 18/18
T2:                PASS smoke rv32 / rv32smp / arm（含 ps、deep 3、help、ls）
T3:                C3 PASS（accept 42/42）；rp2040 / rp2350 NOT RUN（板子未接）
T4:                NOT RUN（只改窗口尺寸，不碰调度/flash 压力路径）
logs:              build/t-s1-*.log … build/t-s6-*.log（本地，不入库）
```

## 道理

`grow`（`plat_window_grow_reserve()`）是"栈翻倍时可以借用的堆空间"：窗口
多留 4 KiB，`process_stack_grow()` 才能把 4 KiB 栈翻成 8 KiB，同时给堆留下它
要的那份。一个不分配堆、深度也有界的程序，这块空间永远用不上——`TOS_STACK`
就是关掉它的声明（内核看到 stack_ask 就不再给 grow）。

`deep` 和 `heap` 是反例，也是保留这条默认规则的理由：它们不声明 `TOS_HEAP`
却把这块空间当堆 / 栈扩展用，`deep 3` 就是专门测增长的回归；所以不能在内核
里写"heap_ask==0 就不给 grow"，只能由程序自己声明。

## 结果

| 程序 | C3 | ARM |
|---|---|---|
| `sh` | 28 → **24** | 28 → **24** |
| `tools` | 32 → **28** | 36 → **32** |
| `help` | 16 → **12** | 16 → **12** |
| `ls` | 16 → **12** | 20 → **16** |
| `sleep` | 16 → **12** | 16 → **12** |

（窗口 = span + 4 KiB stack + 4 KiB grow + 4 KiB guard；声明后 grow 项为 0。
C3 的 `sh` span 16 KiB，所以 16+4+0+4 = 24。）

板上实测：

```text
tos> ps                 -> sh  ... wink 24  stk 1.3/4
tos> deep 3             -> [pid 2] stack grown to 8 KiB, heap ceiling 0 KiB
                           deep: 3 levels, journal intact
tos> sleep 500 &        -> [12]；ps 里 sleep 行 wink 12, stk 0.2/4
tos> sh etc/demo.sh     -> 全部标记通过（if/while/incr/break/$(...)/管道）
tos> accept.py -p COM4  -> 42/42 passed
```

`deep 3` 仍然能增长，说明没有声明的程序（包括将来新写的）行为没变；这五个
程序溢出时改成在 guard 处被杀并打印 `stack overflow ... killing`，不会再翻倍。

## 没有做的

- `edit`、`rx` 等其它无堆程序没有动：它们的深度没有测过，等哪天需要再按同样
  方法量 `ps` 的 stk 高水位后逐个声明；
- guard 仍是 4 KiB：C3/rp2350rv 的 PMP 是页粒度，一页就是最小值；rp2350 ARM
  的 4 KiB 里有 0xF00 是异常 landing（见 `arch/arm/platform.c`），砍掉会
  MSTKERR；
- rp2040 / rp2350 实机没接，它们的 T3 是 `NOT RUN`。
