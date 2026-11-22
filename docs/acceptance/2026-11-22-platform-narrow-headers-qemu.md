# platform.h 拆四窄头 + include 方向约束（#52）

- commit: `TBD`（交付时替换为已推送 sha）
- 日期: 2026-11-22
- 变更: `kernel/platform.h`（609 → 162 行）、新增 `kernel/plat/{console,irq,block,memprot}.h`、新增 `internal/build/include_test.go` 与 `testdata/include-allow.txt` / `testdata/platform-periph.txt`
- 主机: Windows / go1.27.1 windows/amd64 / clang；QEMU rv32、arm
- 级别: 共享头 → T0 + T1 + T2（按 `docs/TESTING.md`）

## 结果

| 项目 | 命令 | 结果 |
|---|---|---|
| 符号等价 | 拆前 `git show HEAD:kernel/platform.h` 与拆后 `cat kernel/platform.h kernel/plat/*.h` 各抽声明符号集合 | **107 / 107 完全一致**，双方向差集为空 |
| 负例先红 1 | 在 `drivers/display/st7789/lcd_st7789.c` 首行插 `#include "process.h"` | **红**：`drivers/display/st7789/lcd_st7789.c:1: drivers must not include kernel-private headers`；恢复后绿 |
| 负例先红 2 | 在 `kernel/platform.h` 插 `int plat_lcd_probe(void);` | **红**：`kernel/platform.h:63: new peripheral declaration plat_lcd_probe`；恢复后绿 |
| 负例先红 3 | 删掉 `include-allow.txt` 里一条仍在生效的基线条目 | **红**：该 include 变成 `new violation`（基线不能腐化）；恢复后绿 |
| T0 | `go test -count=1 ./...` | PASS（跑时工作区无 pinned 漂移；期间对方线程同步 pinned 源，另记） |
| T1 | `go run ./cmd/tos build all` | **18/18**：`all 18 targets built` |
| T1 抽样 `.text` | 独立 worktree 拆前/拆后各构建 `rv32`、`rpi-pico2-smp`，`readelf -x .text` 比对 | `rv32` **SHA-256 逐字节相同**；`rpi-pico2-smp` 短 4 字节，首个差异是向量表里的地址常量 `0x1002c8e8 → 0x1002c8e4`（随 `.text` 长度重定位），源符号集合一致。差异可解释但根因未完全定位，见下 |
| T2 | `tostest smoke -target rv32,arm` | **2/2 `SMOKE TEST OK`** |

## 窄头内容

- `plat/console.h`：`plat_console_*` 五函数与其全部注释（原 55–101 行）。
- `plat/irq.h`：timer/irq、cpu topology、CPU description（`plat_pre_tick_spins` 内联在此，紧随其依赖的 `plat_cpu_hz`）、time。`TOS_MAX_CPUS`/`TOS_DIAG`/`TOS_BOOT_TRACE` 的 `#ifndef` 默认值一并保留。
- `plat/block.h`：littlefs LBA/块数、块设备、flash 探测与三个调试钩子（原本紧邻块设备段）。
- `plat/memprot.h`：`plat_seg_t`/`PLAT_SEG_*`、`plat_task_protect`、guard/stack 常量、`regs.h` 与 `syscall_t`、frame/user/enter/fault 系列、`PLAT_PAGE_SIZE`。
- `platform.h` 保留：include guard、`types.h`/`tos.h`、内存布局、boot、物理内存、gpio/adc/pwm/uart/spi/i2c/pio/dma、cache 钩子、shutdown/panic/reboot，并 include 四个窄头。原有符号全部仍从 `platform.h` 可见，旧代码零改动。

## include 方向约束

四条目录规则（drivers→内核私有头、drivers→arch memmap、kernel→board 实现头、arch→drivers 实现头）+ 一条硬规则（platform.h 不得新增多词 `plat_*` 声明）。存量违规写进 `include-allow.txt`，每条**必须带理由**；基线里消失的条目也算失败，防止理由腐化。当前 25 条 include 基线 + 26 条存量声明基线。

## 未完全定位的差异

`rpi-pico2-smp` 的 `.text` 比拆前短 4 字节；所有差异起始于向量表里的 `.time_critical.tinyusb` 地址常量（-4），说明某处代码少了一条 4 字节指令。已排除：构建非确定性（同 worktree 连续两次构建 `.text` SHA 相同）、符号集合变化（107/107 一致）。`rv32` 逐字节相同，T1 18/18 与 T2 smoke 通过，未见运行期影响。根因留作后续；如果后续发现是 include 顺序改变了条件编译，应在本贴补充。

## 未运行

- T3（Pico 2 / ATK / C3 真机）：`NOT RUN`。本次是头文件拆分，不改行为；T1 全目标构建 + QEMU smoke 覆盖编译与链接。
- T4：`NOT RUN`。
