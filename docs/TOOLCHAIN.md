# 脚本工具链：csc / rvm / rcc

TinyOS 里的 C 脚本链路是三段式：

```text
source.c --csc--> RIR2 镜像 --rvm--> 解释执行
                             \--rcc--> 原生 ELF/LZ4TOS（RV32 或 Thumb-2）
```

`csa` + `pvm` 是旧的 AMX 对照实现，暂时保留；最终的链路是 `csc` + `rvm` +
`rcc`。详细设计写在独立仓库 `yafengabc/tinyos-script`：

- [`docs/csc.md`](https://github.com/yafengabc/tinyos-script/blob/main/docs/csc.md)
  — 流式两遍编译、优化 pass、56 字节 AST、限制与诊断
- [`docs/rvm.md`](https://github.com/yafengabc/tinyos-script/blob/main/docs/rvm.md)
  — 解释器用法、汇总行、256 IR 上限、作为 golden model 的角色
- [`docs/rcc.md`](https://github.com/yafengabc/tinyos-script/blob/main/docs/rcc.md)
  — 后端选择、ELF/LZ4TOS 布局、RV32/Thumb-2 ABI、寻址技巧、限制、QEMU 流程
- [`spec/rir2.md`](https://github.com/yafengabc/tinyos-script/blob/main/spec/rir2.md)
  — RIR2 字节格式与 opcode 语义

## 本仓库里的位置

| 程序 | 路径 |
|---|---|
| `csc` | `user/csc/csc.c` |
| `rvm` | `user/rvm/rvm.c` |
| `rcc` | `user/rcc/rcc.c` + `user/rcc/rv32.inc` / `user/rcc/thumb2.inc` |

三者都在 `internal/build` 的 `defaultUserExclude` 里，只有配置打开
`[features] rir = true` 时才会进入镜像；发布镜像默认用 `csa` + `pvm`。

测试/实验配置：

| 配置 | 用途 |
|---|---|
| `configs/esp32c3-rir.conf` | ESP32-C3，板上 `csc -> rvm -> rcc` |
| `configs/rv32-rir.conf` | QEMU rv32，原生 RV32 冒烟 |
| `configs/arm-rir.conf` | QEMU mps2（Cortex-M3），Thumb-2 后端 |

## 板级能力（`csc`/`rcc` 的静态预算）

| 项 | 值 |
|---|---|
| 函数表 | 256 |
| 单函数 AST | 256 节点 |
| 单函数 IR | 256 条 |
| `rcc` RIR2 输入 | 40 KiB（约 2000 IR），按 4 KiB 分块读 |
| `rcc` 标签 | 4096 |
| `rcc` text | 48 KiB |
| `rcc` 窗口 | 88 KiB（C3/rv32） |

`rcc` 有三个后端：`rv32.inc`、`thumb2.inc`（ARMv7-M/ARMv8-M Mainline）和
`thumb1.inc`（ARMv6-M，Cortex-M0/M0+）。Thumb-1 用 r0..r7 作操作数、软件除法/
乘法、`bl` 长分支；改动在 `RCC_ARCH_THUMB1` 下，故事不影响前两个后端。

超过单函数 AST 的程序（例如 90 条语句的 `bigbody`）在板上会报
`AST full`，需要在宿主 `csc` 生成 RIR2。`rvm` 已改成按页读取指令区（宿主 8 页、
板上 2 页，每页 256 条），不再有“整程序 256 条 IR”的上限，只剩换页开销。
两者都记录在 `OPEN-ITEMS.md` §12。

## 移植注意

TinyOS 保存的是独立仓库的**固定副本**。用 PowerShell 复制时
`Copy-Item` 会保留源文件的 mtime，而构建只会在源比目标新时重编，所以复制后
要 touch：

```powershell
Copy-Item ..\tinyos-script\src\rcc\thumb2.inc user\rcc\thumb2.inc -Force
(Get-Item user\rcc\thumb2.inc).LastWriteTime = Get-Date
```

不这么做会得到“源码已更新、镜像里还是上一版后端”的假象。当前 pin 写在
`tinyos-script/port/tinyos/README.md`。

## 测试

- 宿主：`tinyos-script/tools/test.py`（csc + rvm + rcc，40 个程序用例）
- QEMU：本仓库 `docs/TESTING.md` §4.4（RV32）与 §4.5（Thumb-2）
- 板上：`docs/TESTING.md` §5，C3 与 RP2350
