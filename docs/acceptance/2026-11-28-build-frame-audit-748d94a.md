# 构建期帧审计：clang 程序也按声明的栈检查（2026-11-28）

```text
commit:            748d94a  build: check every user program's frames against
                            the stack it gets
change:            用户程序加 -fstack-usage；链接后 checkUserStack() 读 .su 和
                   程序的 TOS_STACK（没有则板级默认），帧 + 512 B 超过就报错并
                   给出应声明的栈；rcc 的 frame_for() 加 ≥4 KiB 断言（含
                   tinyos-script 镜像副本）
target/board:      ESP32-C3（COM4，实机）；18 个构建目标；QEMU smoke
toolchain:         go 1.27.x；clang/ld.lld 23.1.4
T0:                PASS（gofmt、go build/vet、全部测试，含 pinned toolchain
                   sources；goc32/native32 测试）
T1:                PASS 18/18
T2:                PASS smoke rv32 / rv32smp / arm
T3:                C3 PASS（accept 42/42）；rp2040 / rp2350 NOT RUN（板子未接）
T4:                NOT RUN（编译期检查，不碰运行时路径）
logs:              build/t-audit-*.log、build/su/（本地，不入库）
```

## 补的是什么洞

之前只有 toscc 有帧检查；**发布镜像全部是 clang 编译的**，`user/foo.c` 里写
`char big[8192]` 照样能构建，只有在板子上跑才会出问题（或者干脆跨过 guard
洞静默写坏）。现在构建阶段就拦住：

```text
tos: bin/frames8k: main has a 8208-byte stack frame; the program gets the
board default 4096 bytes; add TOS_STACK(16384), or move the array to .bss
or the heap
```

声明过小：

```text
tos: bin/frames8k: main has a 8208-byte stack frame; the program declares
TOS_STACK(4096); raise it to TOS_STACK(16384), or move the array to .bss
or the heap
```

声明 `TOS_STACK(16 * 1024)`：构建通过，`span 4 KiB / 栈 16 KiB`，与 toscc
的规则一致。

## 实现

- `userCFlags` 增加 `-fstack-usage`（zig cc 不保证 `.su`，所以只对 clang 加；
  没有 `.su` 的对象由检查跳过）；
- clang 把 `.su` 写在 object 旁边（`build/user/foo.su`），格式
  `file:line:func<TAB>bytes<TAB>static`；
- 链接后 `checkUserStack()`：
  - 读该程序所有对象（common + 自己的）的 `.su`，取最大帧；
  - 用 `windowNote()` 从链接出的 ELF 读 `TOS_STACK`（type 1），没有就用
    `WindowGeometry()` 的板级默认栈；
  - `帧 + 512 > 栈` 就报错，建议值取不小于 `帧 + 512` 的 2 的幂；
  - `kind != static`（动态帧）直接报错——VLA 已被前端拒绝，这是兜底；
- `rcc` 的 `frame_for()` 末尾：`frame >= 4096` 就 `die()`。csc 的溢出槽让
  实际帧在 1 KiB 左右（`VREG_MAX=256`），断言今天是休眠的。

## 验证

- 临时 `user/frames8k.c` 三次构建：无声明 → “add TOS_STACK(16384)”；
  `TOS_STACK(4K)` → “raise it”；`TOS_STACK(16K)` → 通过（span 4 KiB，
  note 16 KiB）；文件已删除，不入库；
- T1 18/18：所有目标的每个程序都过了这项检查（树里没有超限帧）；
- C3 accept 42/42（rcc 断言不影响现有程序；accept 里的 csc/rcc 路径正常）；
- `TestPinnedToolchainSources` 要求 `tinyos-script/src/rcc/rcc.c` 同步，
  镜像副本已在那边单独提交。

## 没有做的

- **深调用链**：仍然只比单帧；链深溢出是 guard 的日常工作（fault → kill），
  自动按调用图算 `TOS_STACK` 是另一个特性；
- `.su` 的 `bounded`/`dynamic` 目前只会报错，不会去猜；
- `edit`/`rx` 等程序还没声明 4 KiB 栈（量过 `stk` 才能做）。
