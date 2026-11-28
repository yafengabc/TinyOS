# 栈区：RP2350 四镜像 T3（Pico 2），M33 landing 收敛到 32 B

```text
commit:            21c90a8  arm: M33 landing is the hardware frame only, not
                            0xF00
                    c7eafcc  test: soak counts deep 3 after the no-grow change
change:            ARM_STACK_LANDING 0xF00 -> 32：栈底以下只留一个异常帧
                   （8 字）可写，洞的其余部分仍是 guard。原来的 3.75 KiB
                   可写条带让 M33 的 4 KiB 栈实际可写约 7.75 KiB，deep 3
                   （3 x 2064 B）跑完而不是被杀；在栈区里它还会压住下一个
                   栈块的顶部。
target/board:      Raspberry Pi Pico 2（COM19 UART0 / COM27 CDC）：
                   rpi-pico2、rpi-pico2-smp、rpi-pico2-rv32、rpi-pico2-rv32-smp
toolchain:         go 1.27.3 windows/amd64；clang 23.1.4；LLD 23.1.4
T0:                PARTIAL：go vet PASS；go test 只有
                   TestPinnedToolchainSources 失败（user/csc|rvm|rcc 与同级
                   tinyos-script 不一致，改动前的主树同样失败）
T1:                PASS 四目标
T2:                NOT RUN（本轮只做 T3）
T3:                M33 UP 19/19 + kthreadtest all ok；
                   M33 SMP accept33 21/21（smpbench 1.98x），
                   kthreadtest FAILED (2)（cwd 两项，见下）；
                   Hazard3 UP 20/20；
                   Hazard3 SMP 22/22（smpbench 2.00x）+ kthreadtest all ok ×2
T4:                NOT RUN
logs:              本地 temp，不入库
```

## 修复

`arch/arm/platform.c` 的 `mpu_region(6, stack_low - ARM_STACK_LANDING, ...)`
沿用了旧布局的 0xF00 可写条带：旧布局里栈底以下的 4 KiB 洞不属于任何人，
软件增长要用；栈区里这些字节就是下一个栈块（或洞），2 KiB 的帧会在 256 B
guard 之前写穿邻居。`21c90a8` 把 `ARM_STACK_LANDING` 改为 `ARM_HW_SIZE`
（32 B，一个异常帧）。本轮上板固件由等价的本地改动构建（与 `21c90a8` 只差
注释），测量结论对应 `21c90a8`。改成 32 B 后：

- `deep 3` 的第三层帧一落到栈底以下就 fault，按 `declare TOS_STACK()` +
  `killing` 只杀该任务；
- 栈区里当前线程不再能写相邻栈块顶部的 3.75 KiB；
- 异常刚越过栈底时仍有 8 字可压帧；更深的越界走 MSTKERR 分支，输出同一条消息。

## 实测（最终树，四个镜像各刷一次）

```text
rpi-pico2（M33 UP，accept33 19/19）:
  deep 3      -> declare TOS_STACK + killing，shell 继续
  kthreadtest -> all ok ×2；1000 mixed live 8192->8192 / reserved 16384->16384
  free        -> stack area 16384 16 (live 8192, peak 32768)
  stk 4 1000 &-> reserved 32768 / live 24576，退出后回基线

rpi-pico2-smp（M33 SMP，accept33 21/21）:
  smpbench 1.98x；smp -s 两 worker 分别在 cpu 1 / cpu 0 then 1
  [mpu] regions=3，栈块 2007e000..2007f000
  kthreadtest -> FAILED (2)：thread saw the leader's cwd / chdir moved
                 （溢出用例之后的新线程；见 OPEN-ITEMS §32）

rpi-pico2-rv32（Hazard3 UP，accept33 20/20）:
  deep 3 -> declare TOS_STACK + killing

rpi-pico2-rv32-smp（Hazard3 SMP，accept33 22/22）:
  smpbench 2.00x
  kthreadtest -> all ok ×2；溢出打印 sp 0x2007cff0 left the stack，只杀线程
```

## M33 SMP 的未解问题（已单独记 OPEN-ITEMS §32）

- 对照组 `6394cfe^`（M33 还没有栈区）同一 `kthreadtest` all ok；
- 当前树把溢出用例临时 `#if 0` 掉后 cwd 两项 PASS → 触发点是“溢出线程被杀”；
- 诊断镜像显示故障归属正确：CPU1 上 `main=0` 的 worker，`psp=0x2007cfd0`
  在 `2007d000..2007e000` 下方 0x30，`cfsr=0x10`（MSTKERR）；随后新线程落在
  同一块，在第一个 SVC 处异常（`psp=0x2007dfe0` 仍正常）；
- 把 landing 退回 `0xF00` 仍能复现（单独跑两轮都在溢出后整进程退出），所以
  与 `21c90a8` 的 landing 修复无关，是 6394cfe 栈区移植暴露的 SMP 回收/复用
  竞态。

## 重跑

```text
go run ./cmd/tos build -target <rpi-pico2|rpi-pico2-smp|rpi-pico2-rv32|rpi-pico2-rv32-smp>
python scripts/rp2350/accept33.py --image <target> -p COM19 --uf2 build/tinyos-<target>.uf2
python scripts/rp2350/kthread_accept.py --uf2 build/tinyos-rpi-pico2.uf2 --no-flash \
    --port COM19 --commands "free,stk 4 1000 &,sleep 6,free,ps,kthreadtest@180,prodcons@120,threadbench@400,kthreadtest@180,free"
```

`accept33` 用 UART0（COM19）；`kthreadtest` 走同一控制台。
