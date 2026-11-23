# Hazard3 跨页取指的构建期门禁 + 脚本链上板前覆盖（2026-11-24）

```text
TinyOS commit:     dc01958（checkCodePages + rvm-cover 三链 + CI；msh 承诺 20 KiB）
target/config:     rpi-pico2-rv32 / -rv32-smp / atk-dnr2350am-rv32 / -smp 构建门禁；
                   rv32 / arm（rvm-cover）；rv32/rv32smp/arm/x86 QEMU；全 18 目标
board/port:        ESP32-C3（COM4）；RP2350 / RP2040 真机 NOT RUN
toolchain:         go 1.27.1 windows/amd64；clang 23.1.4
host:              Windows 11 / EKSOFT
T0:                PASS — go test -count=1 ./...
T1:                PASS — go run ./cmd/tos build all -> 18/18
T2:                PASS — smoke rv32/rv32smp/arm/x86；soak 3/3；rvm-cover rv32+arm 三链
T3:                PASS — C3 accept.py 42/42；RP2350/RP2040 NOT RUN
T4:                NOT RUN
```

## 为什么 QEMU 复现不了、但必须上板前覆盖

`rcc` 那次是 **Hazard3 的页粒度 NAPOT** 才有：一段 `.text` 被 `pmp_cover()` 拆成
多个块，RISC-V 规定一次访问只有**一个**条目覆盖它才允许，所以 `page_end-2` 的 32 位
指令一条取指跨两个块就 fault（两边都给 X 也一样）。QEMU rv32 与 C3 走 **TOR**，一段
只有一个连续条目，同样的二进制在那上面不崩——实测 `rvm-cover` 与三条链在 QEMU
rv32/rv32smp 全过。所以“QEMU 能复现”这个说法不成立；能复现的是**指令跨页**这个
事实，需要用构建期检查去抓。

## 加了什么

| 位置 | 内容 |
|---|---|
| `internal/build/codepage.go` | `checkCodePages()`：逐个用户镜像走指令流（低两位 `0b11` = 4 字节，否则 2 字节），任何指令跨 4 KiB 边界 → **构建失败**并打印地址；只对 `arch=rv32 && chip=rp2350` 生效 |
| `internal/build/codepage_test.go` | 三种形状单测：`page_end-2` 的 4 字节指令报错、同位置的 2 字节不报、正好结束在边界的 4 字节不报 |
| `scripts/rvm-cover.py` | 由只跑 `cover` 扩成 `cover/state/ready` 三条链（rv32 + arm），`--only` 可选单条 |
| `.github/workflows/images.yml` | rv32 的 smoke job 增跑 `python3 scripts/rvm-cover.py` |
| `user/msh/msh.c` | 承诺从 24 KiB 收紧到**正好 20 KiB**（12 KiB 镜像 + 4 栈 + 4 guard） |
| `docs/TESTING.md` §4.6/§11、`docs/OPEN-ITEMS.md` | 写清跨条目规则、门禁与上板前命令 |

## 门禁自己的证据（负例）

把门限临时放开到所有 rv32 目标（QEMU 用户代码仍带 RVC），构建立即红：

```text
tos: bin/edit: 4-byte instruction at 0x00000ffe crosses a 4 KiB page boundary;
Hazard3 splits the code run at page granularity and a fetch over two PMP entries
faults, so user code must be built without RVC
```

用同一份命令扫描 QEMU rv32 的用户镜像，9 个程序命中（`csa csc csc3 edit irc pio
rcc rvm sh`，地址全是 `0x?ffe`）——也就是说上板前就有一批“地雷”，`rcc` 只是
第一个踩到的。Hazard3 目标上用户代码已去 RVC，扫描为零，门禁保持绿。

## msh 的尺寸（回应“又大于 16 KiB”）

| 目标 | 文件 | 镜像 span（含 .bss，按页取整） | 窗口 |
|---|---|---|---|
| esp32c3-generic / rpi-pico / rpi-pico2 / atk（ARM/C3，带 RVC） | 5.5–6.3 KiB | 8 KiB | 16 KiB（不变） |
| atk-dnr2350am-rv32（4 KiB 粒度） | 6 962 B | **12 KiB**（8 644 B 需要 3 页） | **20 KiB**（原 16） |
| rpi-pico2-rv32 / -rv32-smp（16 KiB 粒度） | 6 962 B | 12 KiB | **32 KiB**（分配按 16 KiB 粒度取整；原 16） |

即：**文件没过 16 KiB，过 16 KiB 的是 Hazard3 上的镜像 span/窗口**。根因是去 RVC
让 text 多约 1.1 KiB，把 8 644 B 推过 2 页线（8 192）。要回到 16 KiB 需要再省
**452 B**：最直接的是 `POOL_MAX 1536 → 1024`（-512 B，命令展开池从 6×行宽降到 4×，
超长展开仍会报 too long）。本轮**没做**这个裁剪——它改变 shell 的行展开上限，要单独
一轮 C3/QEMU 回归；需要的话再开。

## 重跑

```sh
go test -count=1 ./...
go run ./cmd/tos build all
python scripts/rvm-cover.py                 # rv32 三条链
python scripts/rvm-cover.py --target arm
go run ./cmd/tostest smoke -target rv32     # rv32smp / arm / x86 同
python scripts/soak.py --cycles 3 --targets rv32,rv32smp,arm
go run ./cmd/tos build -target esp32c3-generic && python scripts/esp-c3/flash.py --build -p COM4
python scripts/esp-c3/accept.py -p COM4 --no-reset
```
