# 栈区：stack_area 归 chip/ISA baseline，RP2350 双 ISA 移植（T3 待上板）

```text
commit:            be5bbba  build: stack_area is a chip/ISA fact, checked where
                            the other window geometry is
                    6394cfe  rp2350: enable the stack area on the M33 and
                            Hazard3 baselines
                    dc78e5d  docs: stack area is on C3 and both RP2350
                            baselines; RP2350 T3 pending
change:            PLAT_STACK_AREA 16 KiB 写进 rp2350-generic（M33/PMSAv8）和
                   rp2350rv-generic（Hazard3/NAPOT）两个 baseline；具体板子
                   通过 base = 继承。board.conf 增加 [window] stack_area，
                   boardconf/geometry/boardlayers 三层构建期校验；board_check
                   启动时和 layout.c 对账。M33 栈段/landing/guard 与
                   deep 3 的 TOS_STACK 提示、Hazard3 的 NAPOT 映射和 host
                   harness 一起补上。x86 启动预算 202 -> 203 扇区。
target/board:      ESP32-C3（COM4，实机回归）；RP2350 M33 / Hazard3 的
                   rpi-pico2*、atk-dnr2350am* 构建；QEMU rv32/rv32smp/arm/x86
toolchain:         go 1.27.3 windows/amd64；clang 23.1.4；LLD 23.1.4
T0:                PASS（go test ./...、go vet；含 geometry/boardlayers/
                   syscall 表/快照/PMP host harness）
T1:                PASS 18/18
T2:                PASS smoke rv32 / rv32smp / arm / x86；soak 3/3
                   （rv32/rv32smp/arm，`scripts/soak.py --cycles 3`）；
                   compat rv32,arm PASS
T3:                C3 PASS（默认镜像 accept 42/42；free 的 stack area、
                   deep 3 的 TOS_STACK 提示在板上确认）。
                   RP2350 M33 / Hazard3: **NOT RUN，等上板**。
                   RP2040: NOT RUN（下一步）。
T4:                NOT RUN
logs:              build/t-p4-*.log（本地，不入库）
```

## 做了什么

- **stack_area 是 chip/ISA 事实**：`PLAT_STACK_AREA` 在 `rp2350-generic` 和
  `rp2350rv-generic` 的 `layout.c` 各写 16 KiB；`[window] stack_area = 16k`
  也只写在 baseline 的 `board.conf`。具体板子的 `board.conf` 不写它，
  `LoadBoardConfig` 沿 `Bases` 继承（ISA overlay 优先于 chip baseline）。
- **构建期校验**：`boardconf.go` 检查 16 KiB 的整数倍、16–128 KiB；
  `geometry_test` 现在遍历每个板子，沿 `Bases` 找到实际编译的 `layout.c`，
  比对 grain/stack/guard/**stack_area** 和 `PLAT_APP_MAX` 的 16 KiB 对齐；
  `boardlayers_test` 拒绝具体板子自己写 `stack_area` 或定义
  `PLAT_STACK_AREA`。`TOS_APP_STACK_AREA` 进 `board_config.h`，
  `board_check()` 启动时和 `layout.c` 对账。
- **M33（PMSAv8）**：`apply_protection()` 输出 code / data / stack 三段并按
  地址排序；`plat_task_protect()` 按位置取出，`win_end` 变成栈顶，region 2
  只映射栈块，块下的 4 KiB 洞是 guard。32 B landing 保留（异常刚越过栈底时
  只有它能压帧；相邻栈顶 32 B 是 `build_initial_stack` 不用的 256 B
  headroom）。普通数据 fault 的 sp 落在 stack_low 以下时，现在报
  `declare TOS_STACK()` + `killing`，不再打印像野指针的寄存器 dump；
  MSTKERR 的消息也补上同一句。
- **Hazard3（NAPOT）**：mapper 的 `win_end` 是栈顶；数据窗和栈之间的大洞
  保持未覆盖；栈块是自然对齐的 2 的幂，一个 NAPOT 条目。host harness 新增
  两个 area 布局（合成窗口和 rcc 的 68 KiB image + 40 KiB 数据窗），都保持
  非 WINDOW 策略，`prot heap`/`prot stack` 的洞还在。
- **`process_stack_grow()`**：有栈区时只在 `sp` 真的离开块时打印
  `declare TOS_STACK()`；`prot heap`/`prot null` 这类 sp 仍在栈内的 fault
  不额外多一行。
- **x86**：内核 103,456 B 超过 202 扇区（103,424），`KERNEL_SECTORS` 改 203，
  littlefs LBA 204（`stage1.S`/`build.go`/`board/pc/layout.c`/guide 同步）。

## 实测

```text
C3（COM4，最终树）:
  accept.py -> 42/42
  prot heap / prot stack -> 单条 store access fault（没有多余的 TOS_STACK 行）
  deep 3 -> declare TOS_STACK + killing，shell 继续

构建:
  rpi-pico2-smp          build/tinyos-rpi-pico2-smp.uf2          2,097,664 B
  rpi-pico2-rv32-smp     build/tinyos-rpi-pico2-rv32-smp.uf2     2,097,664 B
  （rpi-pico2、rpi-pico2-rv32、atk-* 同源构建通过）
  check-stack-area 16384 bytes（两个 baseline 的 build-config）

Host PMP harness（RP2350 Hazard3）:
  rcc: SPLIT does not fit, SNAP chosen（旧布局，回归）
  area: SPLIT kept
  rcc-area: SPLIT kept
  ok

T0 / T1 18/18 / T2 四架构 / compat rv32,arm 全过
```

## 上板要跑什么（RP2350，T3）

先各刷一次，**同一块 RP2350A 一次只能刷一个 ISA 镜像**：

```text
go run ./cmd/tos build -target rpi-pico2-smp
go run ./cmd/tos build -target rpi-pico2-rv32-smp

python scripts/rp2350/accept33.py --image rpi-pico2-smp \
    -p COMx --dtr --uf2 build/tinyos-rpi-pico2-smp.uf2
python scripts/rp2350/accept33.py --image rpi-pico2-rv32-smp \
    -p COMx --dtr --uf2 build/tinyos-rpi-pico2-rv32-smp.uf2
```

重点看：

- `free` 出现 `stack area 16384  16  (live ..., peak ...)`；`stk 4 1000 &`
  时 reserved 涨到 32768，退出后缩回 16384；
- `ps` 的 `sh` `wink` 变小（数据窗不再含栈），`stk/sz` 是独立块；
- `deep 3` 在 M33 和 Hazard3 上都被杀，输出含 `declare TOS_STACK` 和
  `killing`；`prot heap`/`prot stack`/`prot null`/`prot rodata` 仍按
  `accept33` 的期望 fault；
- `kthreadtest` 14 slots / all ok，1000 mixed 4K/16K 回到基线与 peak 32768；
  `prodcons` 32768 B；`threadbench` 两轮后再 `kthreadtest` 仍 all ok；
- Hazard3 启动后不应出现 `[pmp] window cannot be described` 或
  `window map degraded`；M33 的 `[mpu]` 行 region 数不应变成 1（那表示
  退化到整窗 RWX）。

## 上板反馈（2026-11-28，M33 单核）

第一次 `rpi-pico2`（M33 UP）上板时 `deep 3` 没被拦到：旧的
`ARM_STACK_LANDING = 0xF00` 是隐式增长时代的“栈下空洞可写条”，在栈区布局里
它下面就是**下一块栈**。`deep 4` 写到约 **7.33 KiB** 才撞上 256 B 的 guard，
此时相邻栈已经被踩。修复：landing 缩到 `ARM_HW_SIZE`（32 B，只够异常帧），
其余全部是 guard；更深的越界走 MSTKERR，仍然带 `declare TOS_STACK` 杀。
**单核和 SMP 的 T3 证据都要在修复后的 commit 上重跑**；本卡先前的
“RP2350 NOT RUN” 不变，直到新证据进来。

## 没验证什么

- RP2350 的 M33/Hazard3 **没有 QEMU 覆盖**（qemu mps2 是 M3，rv32 virt 是
  TOR）；PMSAv8 的栈块/landing 和 Hazard3 的 NAPOT 实际映射只能上板验。
- RP2040 仍未开栈区，按计划最后做。
- 长跑（T4 10/30 分钟）还没跑。
