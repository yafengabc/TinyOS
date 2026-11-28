# toscc 增加栈帧检查：帧 ≥ guard 直接拒绝（2026-11-28）

```text
commit:            b088b73  toscc: refuse a function frame that reaches the
                            stack guard
change:            新增 -stack-guard N（默认 4096，0 关闭）；RV32/Thumb 后端在
                   帧布局完成后检查，帧 ≥ N 时报错并给出函数名；goc32/README
                   记录用法
target/board:      工具链（toscc），不涉及镜像；18 个目标未重新构建
toolchain:         go 1.27.x；toscc 自带后端
T0:                PASS（gofmt、go build、go vet、goc32 + native32 测试、
                   user/hello.c 经 toscc 编译、rv32 镜像构建）
T1–T4:             NOT RUN（只改 toscc，镜像字节不变）
logs:              build/t-toscc-*.log、build/frames8k*.c（本地，不入库）
```

## 为什么是在编译期

guard 洞能拦住 `push`，但拦不住一条 `sub sp, sp, #8192`：第一笔写直接落在洞
下面可写的 image/heap 里，缺页不会发生，溢出是静默的。帧大小时编译器已经
知道（所有 alloca + 溢出槽 + outgoing args 都算在 `e.frame` 里），所以在
后端布局完成后检查是最便宜也最准确的位置。

## 实现

- `native32.StackGuard`（字节，0 = 关闭）和 `native32.FrameError`；
- `rv32.go` / `arm.go` 在 `e.frame` 定稿后调用 `checkFrame()`；`EmitRV32`/
  `EmitARM` 入口清空 `FrameError`；
- CLI：`-stack-guard N`，默认 4096；两个 emit 路径（源码编译、`-ll`）都走
  `emitModule()`，出错就带着源文件名返回；
- 默认 4 KiB 对应所有 4 KiB grain 板子的洞；PC/QEMU 是 16 KiB，可用
  `-stack-guard 16384` 放宽（验证时用的是默认值）。

## 验证

```text
$ goc32 -target esp32c3 -o build/frames8k.tos build/frames8k.c
goc32: build/frames8k.c: function "main" has a 8208-byte stack frame and the
stack guard is 4096 bytes; move large arrays to .bss or the heap, or split
the function                                （退出码 1）

$ goc32 -target esp32c3 -o build/frames8k-static.tos build/frames8k-static.c
build/frames8k-static.tos: 689 -> 474 bytes (LZ4TOS, hint 12288/0/0)

$ goc32 -target esp32c3 -o build/hello-toscc.tos user/hello.c
build/hello-toscc.tos: 1793 -> 1214 bytes (LZ4TOS, hint 4096/0/0)
```

单元测试：

- `TestStackGuardRefusesLargeFrame`：`alloca [8192 x i8]`，`StackGuard=4096`
  → `FrameError` 非空且包含函数名；
- `TestStackGuardAcceptsSmallFrame`：`[16 x i8]` → 通过；
- `TestStackGuardZeroDisables`：`StackGuard=0` → 8 KiB 帧也放行。

注意：静态数组（`static char big[8192]`）进 `.bss`，帧不变，所以照常编译，
span 从 4 KiB 涨到 12 KiB——这正是“挪到 .bss”的建议所期望的结果。

## 没有做的

- clang 路径的同类检查（`-fstack-usage` + 构建期审计）还没接；
- VLA（`char buf[n]`）是动态帧，静态检查看不到，需要 probe 或运行时边界；
- `rcc`（板上后端）还没有同样的断言，它的帧受 `VREG_MAX` 限制（约 1.1 KiB），
  但加一行断言更稳妥。
