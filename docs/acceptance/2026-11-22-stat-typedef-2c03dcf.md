# tos_stat_t 统一 + 目标端编译检查（2026-11-22）

```text
tinyos commit:     2c03dcf（user/lib.h: typedef；user/rvm 同步）
script commit:     df7d777（include/lib.h / src/rvm.c / py / host_shim +
                   tools/check_target_build.py + CI）
target/config:     rpi-pico2、esp32c3-generic；全 18 目标；rv32/arm QEMU
board/port:        ESP32-C3（COM4）
toolchain:         go 1.27.1 windows/amd64；clang 23.1.4
host:              Windows 11 / EKSOFT
T0:                PASS — go test -count=1 ./...
T1:                PASS — go run ./cmd/tos build all -> 18/18
T2:                PASS — tostest smoke rv32 + arm（本 run 未单列）；rvm-cover.py PASS
T3:                PASS — C3：cover 九行一致，accept 24/24
T4:                NOT RUN
```

## 背景

`b516695` 用 `#define tos_stat stat` 止血了 pinned rvm 的目标端编译（脚本
40ab506 把 host shim 的 stat 改名 `struct tos_stat`）。这次两边统一成中性类型：

- 脚本 `include/lib.h`：`typedef struct tos_stat { size; type; } tos_stat_t;`
- TinyOS `user/lib.h`：`typedef struct stat tos_stat_t;`
- `src/rvm.c`、`src/py/py_main.c`、`tools/host_shim.c` 使用 `tos_stat_t`

## 新增的目标端检查

脚本仓库 `tools/check_target_build.py --tos <TinyOS>`：按 **riscv32-unknown-elf**
把 csc/rvm/rcc 对着 TinyOS 的 `user/lib.h` / `include` / `kernel` 做
`-fsyntax-only`。ISA 是关键——rvm.c 的 native syscall 块在
`#if defined(__riscv) || defined(__arm__)` 里，用宿主编译器解析会跳过上次坏掉的
那段。CI 里 checkout TinyOS 后运行。

负例：把 TinyOS 的 `tos_stat_t` 改名后，检查器报

```text
FAIL rvm against .../TinyOS
src/rvm.c:645:21: error: use of undeclared identifier 'tos_stat_t'
PASS csc
PASS rcc
```

## 复测

- 脚本：`tools/test.py` ALL TESTS PASS；`check_docs.py` OK；
  `check_target_build.py` TARGET BUILD OK。
- TinyOS：T0 全绿、T1 18/18、`rvm-cover.py` PASS。
- C3（COM4）刷默认镜像：`csc → rvm → rcc → cover` 九行一致；`accept.py` 24/24。

## 余项

Linux CI 需要实际跑一次（本次在 Windows 本地验证）。`b516695` 的宏在同一
提交链里被本提交替换，历史保留。
