# B-1d：镜像清单变成数据（2026-11-17）

```text
commit:            45fc869
change:            targets/*.conf + 加载器；删除 boardChips；UF2/镜像格式按 (chip, arch) 分派
baseline:          d663bde
target/config:     13 个目标 + configs/rv32-fsro.conf
board/port/image:  无真机
toolchain:         go 1.27.3, clang 23.1.4, qemu 11.1.3, python 3.14.9
host:              Windows NT 10.0.17763 (MSYS2 UCRT64)
T0:                PASS（新增 targets_test.go 三组测试）
T1:                PASS 13/13 + fs_ro；每个目标的 build-config.txt 与改动前逐字节相同
T2:                PASS（smoke 4/4）
T3:                NOT RUN（无板）
T4:                NOT RUN
```

## 判据

| 判据 | 结果 |
|---|---|
| 加目标不再改 Go | `targets/<name>.conf`；`-target` 取文件名 |
| 13 个既有目标的值不变 | 各自 `build-config.txt` 与改动前 **逐字节相同**（13/13） |
| 板名不再出现在构建选择里 | `boardChips` 删除；UF2 分派改为 `(chip, arch)` |
| 失败模式 | 目标名未知 → 现有报错；chip/arch 无格式 → 明确错误；`targets/` 里混入非 conf → 测试失败 |

## 加载与校验

```text
Targets 在 init 时由 LoadTargets() 填充，全有或全无
  name 省略时取文件名；board 必填；arch 必填（当前 13 份都写）
  cpus/max_procs 整数；heap/kstack 支持 k/m 后缀
测试：13 组 name/board/arch/march/cpus/heap；board 可解析；ISA 在芯片上；
      targets/ 目录只放 *.conf 与 README.md
```

## 为什么删 `boardChips`

它编码的是“这个板目录只允许这几颗芯片”。现在 board.conf 自己声明 chip；剩下的真实约束是
**variant 与 base 同芯片**、**目标的 ISA 在该芯片上**，两条都有测试。至于“conf 里写了一颗
与板级代码不符的芯片”，那类错误会在编译期立刻暴露——板级 platform 代码用的是对应移植端的
头文件，芯片写错就编不过。

## 没做

- **板目录改名/分层**（`rp2350-generic` + `atk-dnr2350am` 等）：HISTORY §74 末尾记了两个
  已知约束（`layout.c` 的 `PLAT_LFS_*` 需要 `#if TOS_HAS_FLASH` 分支；RV32 generic 需自带
  `platform.c`/`kernel.ld`/`image_def.S`）。
- `menu` 的目标排序仍依赖 map 遍历（既有行为）。
