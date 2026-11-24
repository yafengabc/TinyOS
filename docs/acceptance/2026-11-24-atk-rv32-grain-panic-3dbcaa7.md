# ATK RISC-V 的 grain 是 4 KiB：一个自 2026-11-21 起就 panic 的镜像（2026-11-24）

```text
TinyOS commit:     3dbcaa7
target/config:     atk-dnr2350am-rv32 / -smp；其余 16 个目标回归；rv32/rv32smp/arm/x86 QEMU
board/port:        ESP32-C3（COM4）；ATK / Pico 2 / RP2040 真机 NOT RUN（不在本机）
toolchain:         go 1.27.1 windows/amd64；clang 23.1.4
T0:                PASS
T1:                PASS — 18/18
T2:                PASS — smoke 4/4；soak 3/3
T3:                PASS — C3 accept 42/42；RP2350/RP2040 NOT RUN
T4:                NOT RUN
```

## 问题

问题来自一个提问：“Pico 2 是 16 KiB、ATK 是 4 KiB，两者不都是 RP2350 吗？”

- RP2350 的 Hazard3 只有 NAPOT（TOR 关），PMP 端口把**栈**放在自己的 NAPOT 区域，
  所以 `stack_request_ok()` 要求栈提示**整除** `PLAT_APP_GRAIN`；板上工具 `csc`/`rcc`
  要 16 KiB 栈 → grain 必须是 16 KiB（`0b5e224`，2026-11-21）。
- 那次只改了 `board/rp2350rv-generic/{board.conf,layout.c}`，
  **`board/atk-dnr2350am-rv32/board.conf` 还写着 `grain = 4k`、没有 `stack_align`**。
- ATK 的 RISC-V 镜像继承 `board/rp2350rv-generic/layout.c`
  （`PLAT_APP_GRAIN = 0x4000`），而 `board_config_check()` 在 `kmain` 里**无条件**比较，
  不一致就 panic：

  ```text
  panic: board_config.h (atk-dnr2350am-rv32) disagrees with layout.c:
         grain 4096/16384 stack 4096/4096 guard 4096/4096
  ```

没有 RP2350 的 QEMU 模型、ATK 又不是关闭门槛，所以这个镜像从 11-21 起就是
**刷进去也起不来**，直到今天才被问出来。

## 修法

1. **构建期比较**：新增 `internal/build/layoutcheck.go`，从“这个板子实际会链接的
   layout.c”（变体目录优先，然后 `base` 链）里读出 `PLAT_APP_GRAIN`、
   `PLAT_STACK_BYTES`、`PLAT_HEAP_GUARD`，与 `board.conf` 的三个数比较；不一致直接
   拒绝构建。解析不了也算错误（不静默跳过）。
   `TestLayoutAgreesWithBoardConf` 对 `board/` 下每个目录跑一遍，以后改 layout.c 或加
   变体先在 `go test` 里红，而不是在板上 panic。
2. **修 ATK 的 conf**：`grain = 16k`、`stack_align = 1`（grain 是**每镜像**的事——
   同一块板的 ARM 半边仍是 4 KiB）。

负例（旧 conf）与修复后：

```text
tos: board\atk-dnr2350am-rv32\board.conf: [window] grain = 4096 but
     board/rp2350rv-generic/layout.c has 16384; the kernel panics at boot ...
（修后）all 18 targets built
```

## 副作用（好的）

ATK 的 `[user] rvc = auto` 现在也按 16 KiB 粒度判断，回退集合与其它 Hazard3 板一致：
**只有 `csc`**（之前 4 KiB 粒度下是 6 个）。五次回退总数从 14 降到 4。

## 仍未做

- ATK 真机复验：`atk-dnr2350am-rv32` / `-smp` 需要重刷并跑
  `accept33.py --image atk-dnr2350am-rv32-smp --dd-rounds 100`，确认 11-21 之后第一次
  真正能启动；本轮只证明了构建期检查与 18/18 构建。
- 同类“两份声明”的地方（`PLAT_RAM_*`、flash 几何）由运行期 `board_config_check()`
  兜着，构建期还没查；本轮只做了窗口三个数。
