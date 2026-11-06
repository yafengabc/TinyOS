# Berry 能不能进 TinyOS，以及「Berry AOT」到底是什么

> 2026-11-04。接 `docs/SCRIPT-DESIGN.md` 的那一轮：那边量的是自研 VM，这里用
> **完全相同的尺子**量 Berry。所有体积是本机 clang 23.1.4 / rv32imc / `-Oz`
> 实测，不是官网数字，也不是估算。

## 0 结论

1. **Berry 在 TinyOS 的小板上不合适，在 PC / QEMU 目标上可以。** 把编译器和全部
   可选模块都关掉、整数降到 32 位、浮点降到单精度之后，仍是 **71.7 KiB** 代码+常量，
   是自研「宿主编译 + VM」方案（3.3 KiB）的 **22 倍**。C3 用户区 192 KiB 理论上塞得下
   一个 96 KiB 窗口的程序，但会吃掉一半用户区并挤掉脚本对象堆；RP2040 的 136 KiB
   用户区则基本出局。
2. **比体积更硬的是运行时依赖**：Berry 的 mark-sweep GC 需要真正能 `free()` 的堆，
   而 TinyOS 用户态 `free()` 是空操作；异常处理需要 `setjmp/longjmp`，目标代码里
   一个都没有；即便 `BE_USE_SINGLE_FLOAT=1`，仍要 `__addsf3` 一族软浮点。
   这三项不是「裁剪一下就好」，是要先改语言运行时或内核。
3. **官方没有原生码 AOT。** 源码里 `aot|jit|native code|machine code` 零命中。
   所谓 solidify 生成的是 `const binstruction[] { 0x80060000, // RET }`
   —— **还是字节码**，只是从「文件」变成「C 常量数组」，VM 照跑。
4. **真 AOT 技术上能做，但在 TinyOS 上收益为负**：代码膨胀、运行时一个字节都省不掉、
   而瓶颈本来就是 RAM 不是 CPU。详见 §5。

## 1 实测方法（与 SCRIPT-DESIGN.md §3 同口径）

```
clang --target=riscv32-unknown-elf -march=rv32imc -mabi=ilp32 -std=c99 -Oz \
      -ffreestanding -fno-builtin -fno-pic -fno-pie \
      -fno-unwind-tables -fno-asynchronous-unwind-tables -w
```

- 源码：`berry-lang/berry` master（浅克隆在 `.tmp/berry/`，该目录在 `.gitignore` 内）。
- 先跑 `python tools/coc/coc -o generate src default -c default/berry_conf.h` 生成常量表。
- 编译 `src/*.c` 全部 40 个 + `default/be_modtab.c`，共 **41 个编译单元**；
  `default/be_port.c` 是宿主 OS 适配层（POSIX 文件/目录），移植时本来就要按
  `be_sys.h` 重写，因此不计入核心体积，
  **逐 `.o` 读 ELF32 section header 累加 `.text` / `.rodata`**（与 SCRIPT-DESIGN 一样，
  不做链接期去重，因此是上界）。
- 目标环境缺 libc，所以用一组只含函数声明的 stub 头让代码编过；stub 只影响符号
  **声明**，不改 Berry 自身代码，也不影响生成代码体积。
- 外部依赖清单来自逐个 `.o` 的 `SHT_SYMTAB` 里 `st_shndx == SHN_UNDEF` 的符号。
- 复现脚本：`.tmp/berry/probe.py`（未入库）。

两种配置都通过 `-include` 预包含自定义 `berry_conf.h`（利用其 include guard 覆盖默认值）：

| 配置 | 改动 |
|---|---|
| `full` | 仓库默认 `default/berry_conf.h` |
| `min` | `BE_USE_SCRIPT_COMPILER=0`、`BE_USE_BYTECODE_SAVER=0`、`BYTECODE_LOADER=1`、`BE_USE_SHARED_LIB=0`、`BE_USE_FILE_SYSTEM=0`、`BE_INTGER_TYPE=0`（32 位 int）、`BE_USE_SINGLE_FLOAT=1`、全部 `BE_USE_*_MODULE=0`、调试/变量信息/性能计数器/重载哈希全关，`BE_STACK_START=32` |

## 2 体积：官网「< 40 KiB」为什么对不上

| | `.text` | `.rodata` | 代码+常量 | 相对自研 VM |
|---|---:|---:|---:|---:|
| 自研「宿主编译 + VM」（SCRIPT-DESIGN §3） | — | — | **3.3 KiB** | 1× |
| 自研「片上编译 + VM」 | — | — | 9.5 KiB | 2.9× |
| **Berry `min`**（41/41 编过） | 61,596 B | 11,829 B | **71.7 KiB** | **21.7×** |
| **Berry `full`**（41/41 编过） | 118,324 B | 25,520 B | **140.5 KiB** | **42.6×** |

`min` 里最大的编译单元（口径 = 41 个 `.o` 相加）：`be_byteslib.c` 13.1 KiB、`be_vm.c` 11.1 KiB、
`be_string.c` 6.3 KiB、`be_api.c` 6.0 KiB、`be_listlib.c` 4.8 KiB、
`be_strlib.c` 4.4 KiB、`be_baselib.c` 3.6 KiB、`be_gc.c` 2.4 KiB。
这些是语言核心类型（bytes / string / list / map / class / GC），**关不掉**；
能关掉的编译器（`be_parser` 11.6 KiB、`be_lexer` 6.0 KiB、`be_code` 5.4 KiB）
和 solidify 模块（14.5 KiB）已经在 `min` 里消失了。

官网的「core < 40 KiB / heap < 4 KiB」是 **Cortex-M4 Thumb + ARMCC** 下、只算
interpreter-core 的数字；换 rv32imc + clang、把标准库模块算进来，量级就变了。
不是虚标，是口径不同——所以必须自己量。

## 3 放得下吗：按板子算

窗口公式见 SCRIPT-DESIGN §2：`win = align_up(span + stack + guard, grain)`，
小板的 `grain = 16 KiB`、默认栈 8 KiB、guard 4 KiB。

| 目标 | 用户区 | Berry `min` 需要的窗口 | 判定 |
|---|---:|---:|---|
| PC / QEMU virt (rv32) | 112 MiB / 16 MiB | ~96 KiB（72 + 8 + 4 上取整） | **可行**，余量充足 |
| ESP32-C3 | 192 KiB | ~96 KiB（占一半）；声明 24 KiB 脚本堆则 ~112 KiB | **勉强**，代价是独占半壁用户区、多任务所剩无几 |
| RP2040 (M0+) | 136 KiB | ~112 KiB | **实质出局**，shell + 前台 + 脚本挤不下 |

内核里也放不下：C3 内核 `.text` 在 SRAM（ROM 搬入，非 XIP），余量只有 33 KiB；
RP2040 虽是 XIP、代码不占 SRAM，但 Berry 的 `.bss` 只有 4 B——**所有状态都靠
`malloc`**，会直接压到那个已被 littlefs 缓存和 exec 解压缓冲占着的 32 KiB `kmalloc`
arena 上。

顺带一提：`.bss` 只有 4 B 看着很美，其实是把成本全推给了运行时堆，而这恰恰是
TinyOS 最缺的东西。

## 4 依赖缺口：`nm` 实测出来的硬门槛

从 `min` 配置的未定义符号里摘出的、TinyOS 目标代码**没有**的部分：

| 类别 | 符号 | 后果 |
|---|---|---|
| 软浮点 | `__addsf3 __subsf3 __mulsf3 __divsf3 __eqsf2 __lesf2 __ltsf2 __fixsfsi __floatsisf __floatunsisf __truncdfsf2 __extendsfdf2` | 即使单精度也要 vendored compiler-rt 一族；`full` 还要 20 个 `__*df3` 和 `__divdi3/__moddi3/__ashldi3/__ashrdi3` |
| 分配器 | `malloc free realloc` | **GC 的 sweep 阶段必须真能释放**。TinyOS 用户态 `free()` 是 `(void)p;` 空操作、`sbrk` 只增不减 → 跑几轮就泄漏到窗口上限 |
| 异常 | `setjmp longjmp` | Berry 的 `raise` / 保护调用全靠它；`include/` 里连 `setjmp.h` 都没有 |
| 格式化 | `snprintf strncpy strncat memchr isdigit` | `string.format` 和词法器要用；自研 `printf` 只有 `%d/%i/%u/%x/%c/%s/%%` |
| 文件 | `fopen fread fwrite fseek ftell fclose fflush fgets` | `.bec` 加载路径；要换成 TinyOS 的 `open/read/write` 并自己实现 `be_sys.h` 那层 |
| 其它（`full` 追加） | `strtod`、全套 `sin/cos/pow/log/atan2/...`、`time/localtime/clock`、`system`、`abort/exit` | 关掉对应模块可去掉，但需要 linker 配合 `--gc-sections` |

`BE_EXPLICIT_MALLOC/FREE/REALLOC` 可以重定向到一个自写 allocator，这是唯一
「有官方钩子」的一项；`setjmp/longjmp`、软浮点、格式化则完全没有钩子，只能补或改源码。

## 5 「Berry AOT」的四个层次

### 5.1 现成的：`.bec` 预编译字节码（唯一真正有用的）

`be_bytecode.c` 能把编译好的 `bproto` 序列化成文件（magic `0xBE 0xCD 0xFE`，version 4），
`be_bytecode_load()` 再读回来；配合 `BE_USE_SCRIPT_COMPILER=0` +
`BE_USE_BYTECODE_LOADER=1`，设备端就不带编译器。Tasmota 的 `tasmota.compile("x.be")`
和 `.bec` 文件就是这个。**这正是 SCRIPT-DESIGN §5 推荐的「宿主编译 + 设备端 VM」**，
Berry 原生支持，不用自己造。上面 `min` 的 71.7 KiB 已经是这个形态。

### 5.2 现成的：solidify（名字像 AOT，其实不是）

`import solidify; solidify.dump(f)` 输出的是可编译进固件的 C 源码，但内容长这样
（官方文档 Chapter 8 与 `src/be_solidifylib.c:405-410` 双重确认）：

```c
be_local_closure(f, be_nested_proto(
    ... ,
    ( &(const binstruction) { /* code */
        0x80060000,  // 0000 RET 1 K0
    }) ) );
```

`const binstruction[]` 还是 **Berry 字节码数组**，VM 的 dispatch loop 一条不少。
它省的是「读文件 + 反序列化 + 运行时编译」，不是执行。solidify 模块自己就 ~10 KiB
（实测 `be_solidifylib.c` 14.5 KiB），官方因此默认不编。

**在 TinyOS 上它连 RAM 都省不了**：Tasmota 的收益来自 ESP32 固件 XIP，const 段在
flash 里直接读、零 SRAM；而 TinyOS 的用户 ELF 是 `elf_load()` 整体 `memcpy` 到
SRAM 窗口执行的（只有 RP2040 的**内核**才是 XIP），`.rodata` 一样由 RAM 出账。
C3 内核也非 XIP。所以这里 solidify 唯一收益是少一次文件 IO。

### 5.3 不存在的：字节码 → 原生 RISC-V 指令

全库检索 `aot|jit|native code|machine code` 零命中；没有第三方后端。要做只能自己写：

- 现成条件不差：`be_solidifylib.c` 已有逐指令遍历 + `be_decoder.h` 的译码，
  把它改成宿主侧 C 代码生成器是可行的；项目也已有 Go 构建器、clang 交叉编译、
  PIE ELF 加载器和 littlefs 分发链路。
- 但生成的代码仍要链接 **整个 Berry 运行时**（GC / 字符串 / list / map / class /
  闭包 / 异常），因为动态类型的每个 opcode 都要做类型检查并回调运行时 helper。
  一个字节都省不掉，还要多出生成代码本身：304 B 的 fib 字节码展开成原生码通常是
  5–20 倍膨胀。
- 闭包 upvalue、异常栈、GC 根扫描要求生成代码与 `bvm` 的栈布局字节级一致，
  等于把 VM 后端重写一遍，且从此跟 Berry 上游的每次 opcode 改动耦合。
- 换来的只是去掉 dispatch loop（~2–3× 速度）。而 TinyOS 的脚本场景是配置、
  IO、gpio、命令编排，**瓶颈是内存不是 CPU**。

结论：不是「不可行」，是**投入产出比为负**——体积更大、依赖一个没少、只买到用不上的速度。

### 5.4 最像 AOT 的一条，但那就不叫脚本了

宿主直接把脚本编成原生 ELF（C / 现有工具链）走 `exec()`。项目已经有完整通路
（`internal/build/build.go` → littlefs → `elf_load` → LZ4 → 重定位），做起来最快；
代价是没有热更新、没有沙箱隔离、没有动态类型和 GC。

## 6 建议

| 目标 | 建议 |
|---|---|
| 只想在 C3 / RP2040 上跑脚本 | 沿用 `SCRIPT-DESIGN.md` §5 的 3.3 KiB 自研 VM，别引入 Berry。Berry 能贡献的只有 `.bec` 格式和宿主编译器，运行时仍得自己写 |
| 想在 PC / QEMU 上要一门完整语言 | 可以上 Berry，但先做四件事：补 `setjmp/longjmp`、把 `BE_EXPLICIT_*` 接到一个真能 free 的 allocator、提供软浮点 helper、把 `be_sys.h` 的文件层接到 TinyOS syscall。预算按 ≥ 72 KiB 代码 + ≥ 16 KiB 运行时堆算 |
| 想借 Berry 的工具链但不要它的 VM | 用宿主 `berry -c` 产 `.bec`，自己写一个只认所需 opcode 的设备端 VM。相当于把 SCRIPT-DESIGN 的 `mkscript` 换成 Berry 编译器，工程量主要在写字节码解码器 |
| 真的需要 AOT | 走 §5.4（宿主编成 ELF）或者另选有真 AOT 的运行时（如 wasm2c 一类）；别在 Berry 上改后端 |

## 7 复现

```bash
cd D:/projects/TinyOS
git clone --depth 1 https://github.com/berry-lang/berry .tmp/berry
cd .tmp/berry
python tools/coc/coc -o generate src default -c default/berry_conf.h
python probe.py        # 生成 stub 头 + 两种 berry_conf.h，编译 41 个 .o 并量体积
```

`probe.py` 打印每种配置的 `.text/.rodata/.data/.bss`、最大的 8 个编译单元、
以及全部 `SHN_UNDEF` 外部符号。体积是逐 `.o` 累加（上界）；实际链接若开
`-ffunction-sections --gc-sections` 会再小一些，但不会改变 §3 的结论。
