# toscc：帧大于栈时提示 `TOS_STACK`，而不是拒绝数组（2026-11-28）

```text
commit:            d19e526  toscc: suggest TOS_STACK instead of rejecting a
                            large frame（取代 b088b73 的 -stack-guard 语义）
change:            后端记录最大帧和函数名；链接后用程序声明的 TOS_STACK
                   （没有则用 -stack-default，默认 4096）比较，不够就报错并
                   给出应声明的栈大小；帧够大（声明了足够栈）时照常编译
target/board:      工具链（toscc），镜像字节不变
toolchain:         go 1.27.x；toscc 自带后端
T0:                PASS（gofmt、go build、go vet、goc32 + native32 测试、
                   三个样例 + hello.c、rv32 镜像构建）
T1–T4:             NOT RUN（只改 toscc）
logs:              build/t-fit-*.log、build/frames8k*.c（本地，不入库）
```

## 为什么不是“帧 ≥ guard 就拒绝”

8 KiB 的局部数组本身没问题；有问题的是**帧比栈大**：`sub sp, sp, #N` 一步
跨过 guard 洞，第一笔写落在洞下面可写的 image 里，缺页不会发生。所以判据是
`帧 + 余量 > 栈`，栈来自程序自己的 `TOS_STACK`，没有声明时才用板级默认值
（4 KiB）。声明了 16 KiB 栈的程序，8 KiB 数组照常编译。

## 三种情况的实跑输出

```text
# 没有 TOS_STACK
goc32: function "main" needs a 8208-byte stack frame; this program gets 4096
bytes (no TOS_STACK); add TOS_STACK(16384), or move the array to .bss or the heap

# 声明了 TOS_STACK(4 * 1024)
goc32: function "main" needs a 8208-byte stack frame; this program declares
TOS_STACK(4096); raise it to TOS_STACK(16384), or move the array to .bss or
the heap

# 声明了 TOS_STACK(16 * 1024)
build/frames8k-tos.tos: 785 -> 512 bytes (LZ4TOS, hint 4096/16384/0)
```

静态数组（`static char big[8192]`）进 `.bss`：`689 -> 474 bytes (hint
12288/0/0)`，帧很小，不受影响。`user/hello.c` 照常：`1793 -> 1214`。

## 实现

- `native32.MaxFrame` / `MaxFrameFunc` + `ResetFrames()`：后端在 `e.frame`
  定稿后记录高水位，RV32 和 Thumb 共用；
- 链接后、写文件前用 `lz4tos.WindowHint()` 读 `.note.tos` 的 `stack_ask`，
  与 `MaxFrame` 比较；`nextStack()` 取不小于 `帧 + 512` 的 2 的幂（内核
  `stack_request_ok` 的要求，最小 4 KiB）；
- `-stack-default N`（默认 4096）只用于“没有 TOS_STACK”的程序；
- 512 字节余量留给 argv 块和最深帧之上的返回地址。

## 测试

- `TestFrameHighWater` / `TestArmFrameHighWater`：8 KiB alloca 被记录为
  `main` 的帧，16 B 的不算；
- `TestNextStack`、`TestCheckStackFit`：三种消息 + 余量边界；
- 三个样例（无声明、声明过小、声明足够）+ 静态数组 + `hello.c` 全部实跑。

## 仍然不做

- **深调用链**：只比较单个最大帧。链深导致的溢出仍是 guard 的正常工作
  （fault → grow 或 kill），不是这个检查的目标；
- **VLA**（`char buf[n]`）是动态帧，静态检查看不见；
- clang 路径（`-fstack-usage` 构建审计）和 `rcc` 的断言还没做。
