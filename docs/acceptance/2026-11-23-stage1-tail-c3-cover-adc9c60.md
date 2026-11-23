# 阶段 1 收尾 + 板测覆盖 + 工具（2026-11-23）

```text
commits:           5d4bf70 C3 验收加入 csc -> rvm -> rcc -> cover（并修复回显比较）
                   7ddaea8 docs/PLATFORM-CONTRACT.md（阶段 1 第 4 项）
                   adc9c60 mklfs -l / -x / --offset
target/config:     esp32c3-generic；x86/rv32/arm/rv32smp（QEMU）；全 18 目标
board/port:        ESP32-C3（COM4）
toolchain:         go 1.27.1 windows/amd64；clang 23.1.4
host:              Windows 11 / EKSOFT
T0:                PASS — go test -count=1 ./...
T1:                PASS — go run ./cmd/tos build all -> 18/18
T2:                PASS — scripts/soak.py 4/4（见下）
T3:                PASS — esp-c3 accept.py 29/29（含 5 项 cover）
T4:                NOT RUN
```

## C3 验收

`python scripts/esp-c3/accept.py -p COM4 --no-reset` → **29/29 passed**，新增：

```text
PASS  csc compiles /tests/cover.c
PASS  rvm cover output matches cover.expected
PASS  rcc compiles the native image
PASS  native cover output matches cover.expected
PASS  rvm and native agree
```

顺带修了两处比较逻辑：控制台会回显输入，原来直接拿原始输出和
`tests/onboard/cover.expected` 比会多一行。`accept.py` 与 `accept33.py`
都改为丢掉回显行、prompt 行和 rvm 的 `result=` 尾巴（accept33 的同类问题
之前没暴露，因为还没在 Pico 2 上跑过）。

## T2 soak（两段窗口放置之后）

```text
x86      PASS  tasks      133169152    130048  (4 cycles)
rv32     PASS  tasks      20054016     19584  (4 cycles)
arm      PASS  tasks       3342336      3264  (4 cycles)
rv32smp  PASS  tasks      19988480     19520  (4 cycles)

4/4 targets passed
```

`free` 前后一致，分配器在高强度 churn 下没有泄漏；`#55` 的 buddy/grain
两段放置不改变回收行为。

## mklfs 只读视图

- `mklfs -l build/ramdisk-rv32.bin`：42 个文件，名字+大小。
- `mklfs -x ... build/x-att`：抽出 42 个文件；`bin/cat` 597 字节，头是
  `LZ4TOS`，与镜像内大小一致。
- `mklfs --offset 524288 -l build/tinyos-esp32c3-generic-full.bin`：
  直接打开整片镜像里的文件系统。

## 文档

`docs/PLATFORM-CONTRACT.md`：把 `plat_*` 分成三类上下文（临界区/ISR 可调用、
任务上下文同步调用、boot/不返回），并写明 save/restore 配对、平台层不取锁、
不得覆盖 guard hole、`TOS_STACK()` 规则、block 擦除语义。`platform.h` 顶部
指向它。

## 未运行

- **T3 Pico 2 / #46**：无板。#46 已给出代码分析（TX 环 128 B + idle-only
  `tud_task`，单核饿死），需要上板确认计数器后再改。
- RP2040、T4 未参与。
- `win_largest_free()` 的 `grain-1` 高报：复查后确认不可达——窗口大小恒为
  grain 的整数倍、域基址按 grain 对齐，所以间隙长度本身就是 grain 的倍数，
  不需要改。
