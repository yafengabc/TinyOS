# csc 状态机模式：能否撑住、上限在哪（2026-11-23）

```text
commit:            43ada9d（files/tests/state.c、state.expected、accept 检查）
target/config:     esp32c3-generic；rv32 QEMU；全 18 目标
board/port:        ESP32-C3（COM4）
toolchain:         go 1.27.1 windows/amd64；clang 23.1.4；板上 csc/rvm/rcc
host:              Windows 11 / EKSOFT
T0:                PASS — go test -count=1 ./...
T1:                PASS — go run ./cmd/tos build all -> 18/18
T2:                PASS — QEMU rv32 同流程；arm smoke
T3:                PASS — C3 accept.py 34/34（含 state 五项）
T4:                NOT RUN
```

## 模式

```c
struct job { int state; int n; int total; };
struct job job_step(struct job j) { switch (j.state) { ... return j; } }
```

- 状态用**标量结构体**，按值传入/返回（csc 不支持结构体指针，也不支持结构体里的数组）；
- 每个 `step()` 只做一个单位的工作，返回后事件循环继续；
- 事件循环用 `sleep(1)` 让出——csc 的 native 表里没有 `yield`/`poll`（`OPEN-ITEMS` 16.15）。

## 实测：按函数编译的上限

板上 csc 每函数 256 AST / 256 IR。用单函数 `switch` 压测：

| 单函数 case 数 | 结果 |
|---|---|
| 20 | 编译通过 |
| 35 | `csc: AST full at line 104` |
| 50 | `csc: AST full at line 104` |

所以“会不会炸内存”的答案是：**不会 OOM，会在固定 AST/IR 数组上明确报错**；一个函数装不下一个大协议，但把状态机按任务/阶段拆成多个 `step()` 就没有问题。`state.c` 的形状就是按这个规则写的。

## 运行结果

板上与 QEMU rv32 都得到：

```text
csc: /tests/state.c -> /tests/state.rir
rvm: turns=8 / sum=10 / readsum=405
rcc: 5244 text, 5504 elf, 5555 tos, 279 labels
state: turns=8 / sum=10 / readsum=405
```

`rvm` 与原生输出逐行一致；C3 `accept.py` **34/34**。

## 建议

- 一个 `step()` 控制在十几到二十个状态以内；协议类按 begin/header/body/end 拆函数；
- 需要多个实例时用结构体按值传递（不要指针）；数组放外面或用标量累加；
- 真要非阻塞 fd，需要给 csc/rvm 的 native 表加 `poll`（和 `yield`），属于运行时小改，不是语言改动。
