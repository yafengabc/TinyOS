# `[user] rvc = auto`：按程序回退到 no-RVC（2026-11-24）

```text
TinyOS commit:     0ec6c2b
target/config:     rpi-pico2-rv32 / -rv32-smp / atk-dnr2350am-rv32 / -smp /
                   rp2350rv-generic；rv32/rv32smp/arm/x86 QEMU；18 目标
board/port:        ESP32-C3（COM4）；RP2350 / RP2040 真机 NOT RUN（不在本机）
toolchain:         go 1.27.1 windows/amd64；clang 23.1.4
host:              Windows 11 / EKSOFT
T0:                PASS — go test -count=1 ./...
T1:                PASS — go run ./cmd/tos build all -> 18/18
T2:                PASS — smoke rv32/rv32smp/arm/x86；soak 3/3；rvm-cover 3/3（rv32）
T3:                PASS — C3 accept.py 42/42；RP2350 / RP2040 NOT RUN
T4:                NOT RUN
```

## 策略

板级事实只写一行（`board/{rp2350rv-generic,rpi-pico2-rv32,atk-dnr2350am-rv32}/board.conf`）：

```ini
[user]
rvc = auto
```

| 值 | 行为 |
|---|---|
| 不写 | 按 target 的 ISA，允许压缩指令（今天 C3 / QEMU / ARM） |
| `auto` | 先带 C 编译链接；**只**把链接后跨 4 KiB 页边界的那个程序换成 no-RVC 重链 |
| `no` | 整个用户侧不用 C（保留给想要简单规则的端口） |

Go 里没有任何板名/芯片名。新加同形态的板子就是加这一行。

## 实现

`internal/build/build.go`：

- `compileUserTree(udir, incs, base, sub, …)` 编一棵 ISA 变体树：默认树在
  `build/user/`；`auto` 时再编一棵不带 C 的在 `build/user/noc/`（**公共运行库也带一份**，
  因为跨界指令可能落在 libc 里）。
- 链接循环：从默认树链接到 `build/user/bin/<prog>`，用 `checkCodePages()` 的同一个
  指令遍历器读一遍；跨界就再从 noc 树链接一次并复检（4 字节指令不可能跨页，一遍即够），
  打印 `RVC bin/<prog>: rebuilt without compressed instructions`。
- `buildFSImage()` 里的 `checkCodePages()` 仍是最终门禁，门限改成“板子的
  `[user] rvc` 非空且 arch=rv32”。

## 实测（rpi-pico2-rv32）

```text
RVC bin/edit: rebuilt without compressed instructions
RVC bin/pio:  rebuilt without compressed instructions
RVC bin/sh:   rebuilt without compressed instructions
RVC bin/csc:  rebuilt without compressed instructions
RVC bin/rcc:  rebuilt without compressed instructions
RVC bin/rvm:  rebuilt without compressed instructions
```

五个 Hazard3 目标在 `build all` 里一共 27 次按程序重建。

| 对象 | ISA 属性 | 说明 |
|---|---|---|
| `build/user/msh/msh.o` | `…a2p1_c2p0…` | 保留 C，镜像 5 566 B（no-RVC 是 6 962 B） |
| `build/user/noc/rcc/rcc.o` | `…a2p1…`（无 c2p0） | 回退后的那一份 |
| `build/kernel/rv32_platform.o` | `…a2p1_c2p0…` | 内核没动，仍带 C |

因此 **msh 的窗口回到 16 KiB**（不再需要 20/32 KiB），只有被回退的程序变大；内核也不再
为这条策略付出 flash。

## 验证

- T0 PASS；T1 18/18；T2 smoke 4/4、soak 3/3；`scripts/rvm-cover.py` rv32 三条链 PASS。
- C3 accept 42/42（C3 的 board.conf 没有 `rvc`，仍单变体、保留 RVC，行为不变）。
- 新增单测 `TestBoardUserRVCPolicy`：三块 Hazard3 板必须是 `auto`，其余必须是默认。
- 负例（上一版行为仍可复现）：`user/rcc/opt` 里写 `-march=rv32imac` 时，`auto` 会把它
  当作“带 C 的默认树”，链接后照样被检测并回退——门禁仍在最后把关。

## 待办

- **RP2350 真机未跑**：本轮只验证了构建产物与 QEMU/C3。板上应重跑 `rpi-pico2-rv32-smp`
  的三条 CSC 链（`accept33.py` 的 `G` 段）与 heartbeat，确认 `auto` 生成的两套
  per-program ISA 在真实 PMP 下都正常。
- `rvc = no` 目前没有板子用（`auto` 覆盖了），保留是为了让“整镜像不用 C”仍然可表达。
