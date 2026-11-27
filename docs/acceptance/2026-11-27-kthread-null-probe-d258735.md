# kthreadtest 的地址 0 探针：改成未映射地址（2026-11-27）

```text
commit:            d258735（exp/thread-sched）
change:            user/kthreadtest.c 的死地址探针 0 -> 0xFFFFFFF0
target/config:     rpi-pico2、-smp、-rv32、-rv32-smp，默认配置
board/port/image:  Pico 2（RP2350A，8 MiB W25Q64），UART0 → CH343 COM19
toolchain:         go 1.27.3；clang/ld.lld 23.1.4
T3:                PASS 四张 accept 20/20、22/22、21/21、23/23；
                   kthreadtest 四张全部 all ok (0 failure(s))
T4:                NOT RUN（本次只改测试探针）
logs:              build/fix72/（本地，不入库）
```

## 问题（#72）

`the holder died on its fault` 在两张 RV32 镜像上失败，ARM 两张通过。现场是
**日志里没有任何 fault 打印**：线程正常返回，`join` 的 code 不是 139。

实测（`rpi-pico2-rv32-smp`，`720f28d`）把平台事实钉死了：

```text
prot null            -> survived          # 读地址 0 不 fault（U 态）
prot dev 0x0         -> survived          # 写地址 0 不 fault
prot dev 0x4/0x1000/0x4000 -> survived
prot heap            -> [pid 5] store access fault
prot stack           -> [pid 7] store access fault
prot dev 0x40070000  -> [pid 8] store access fault    # UART0
prot dev 0x30000000  -> [pid 14] store access fault   # 未映射
```

保护本身是好的，只有包含地址 0 的低区被放行。原因写在
`board/rp2350rv-generic/pmp_map.h` 的头注释里：

```text
8..10 are hardwired by the bootrom (ROM and peripherals, RWX) and 11..15
hardwired OFF; writing those is a no-op.
```

地址 0 就是 bootrom 的 ROM 区，`pmp_program()` 只写条目 0..7，写 8..15 被硬件丢弃，
所以内核收不回那个权限。第二层原因：这条探针跑在 kthread 上，而
`arch/rv32/platform.c` 在 `mret` 前把 ktask 的 MPP 强制成 M（PMP 不约束 M 态），
即使没有 bootrom 那条也不会 fault。

ARM 通过是因为 M33 的 MPU 没有覆盖地址 0 的区域，访问就 fault。

## 修复

探针改为写 `0xFFFFFFF0`（32 位空间最后一个字，所有端口都没有映射）：

```c
#define FAULT_PROBE_ADDR 0xFFFFFFF0u
...
*(volatile int *)FAULT_PROBE_ADDR = 1;   /* fault holding the word */
```

这条用例测的是「线程持锁死亡后锁能否释放」，不关心写哪个非法地址；换成死地址后，
fault 由「该地址没有映射」引起，而不是某个 bootrom 恰好留下的 PMP 权限。
`memprot_on()` 的严格分支和 139 判据**没有放宽**。

## 验证

| 镜像 | accept33（dd 100 轮） | kthreadtest |
|---|---|---|
| `rpi-pico2` | 20/20 | all ok (0) |
| `rpi-pico2-smp` | 22/22 | all ok (0) |
| `rpi-pico2-rv32` | 21/21 | **all ok (0)**（修前 1 failure） |
| `rpi-pico2-rv32-smp` | 23/23 | **all ok (0)**（修前 1 failure） |

四张的 `dd+rm 100 rounds` 都 PASS，`memprot: on`，分数与修复前一致（20/22/21/23），
说明改动只影响这一条探针。

## 没有修的东西（重要）

**「U 态可以读写地址 0」这个保护缺口仍然存在。** 本次只改了测试探针，让它不再依赖
平台的 PMP 布局。要真的拒绝 U 态访问低地址，需要占用一个软件可写的 PMP 条目
（编号 0，优先级高于 bootrom 的 8..10）覆盖 `[0, 64 KiB)` 且权限为 0；代价是窗口映射
从 7 个块减到 6 个，大窗口程序可能从 SPLIT/SNAP 降级到 IMAGE/WINDOW（丢掉 RX 或
守卫洞）。这是独立决策，没有在本次改动里做。

顺带记录：同一镜像上 `prot rodata` 也 survived（写 `.rodata` 被接受），属于
`pmp_map()` 的已知限制（wx 页与 .data 同页时整页 RWX，OPEN-ITEMS 31.0），与地址 0 无关。
