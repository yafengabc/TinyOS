# B-2b 前半：park 握手顺序共享 + host 模型（2026-11-17）

```text
commit:            1120aac
change:            rp_park.h 新增 RP_PARK_BARRIER / rp_park_begin / rp_park_ack；
                   三块板改调 helper；新增 host 端的 park 规则模型测试
baseline:          ae2759f
target/config:     rp2040(+smp) / rp2350(+smp) / rp2350rv(+rvsmp)；12 目标 + fs_ro；QEMU 4
board/port/image:  无真机：park 只在双核 flash 写时执行，QEMU 不跑
toolchain:         go 1.27.3, clang 23.1.4, qemu 11.1.3, python 3.14.9
host:              Windows NT 10.0.17763 (MSYS2 UCRT64)
T0:                PASS（含新的 host 模型测试）
T1:                PASS 12/12 + configs/rv32-fsro.conf
T2:                PASS（smoke 4 目标；soak 3/3）
T3:                NOT RUN（要 RP2350 双端 1000 轮 dd/rm，归 #17）
T4:                NOT RUN
```

## 落地的是什么

三块板原本各自手写同一套顺序：**新代号 → 归属 → 屏障 → flag**；被挂起的一侧**每一轮
都要重新发布 ack**。这两条正是历史两个 bug 的所在（ack 只发一次 → rp2350rvsmp 的
`dd: write error at block 0 (-1)`；代号与 ack 错配）。现在只有一份：

```c
uint32_t rp_park_begin(struct rp_park_state *s, int me);  /* req++, owner, barrier, flag=1 */
void     rp_park_ack(struct rp_park_state *s);            /* 每轮发布，屏障在 store 后 */
```

屏障按架构拼写：ARM `dmb`、RISC-V `fence rw, rw`。

## 没落地的是什么（以及为什么）

循环本身留在各板。它们**不是同一段代码换拼写**：

| | rp2040 | rp2350 | rp2350rv |
|---|---|---|---|
| park 循环里喂狗 | 无 | 有 | 有 |
| core1 不再返回（BOOTSEL 前） | 无 | 有 | 无 |
| 诊断变量组 | 无 | `g_park_*` | `g_pk_*` |
| 唤醒 | `sev` | `sev` | `slt x0,x0,x1`（编译器绊线） |

要合并就得在 XIP 关键路径上引入回调钩子，这需要台面时间（issue #17）。

## 等价性证据（对象级）

六个目标的 `platform.o` 对比；SMP 差异只出现在被改的两处：

```text
rp2350smp   : .text.plat_flash_exclusive_begin  164 -> 168   （唯一变化的节）
              .text.rom_enter_bootsel            （内联了 rom_stop_other_core）
rp2350rvsmp : .text.plat_flash_exclusive_begin  （尺寸不变，内容重排）
rp2040smp   : .text.plat_flash_exclusive_begin  （尺寸不变，内容重排）
```

`rp_flash_park_loop`、`plat_flash_park_poll` **逐字节相同**；三个单核目标整份对象相同
（park 代码在 `#if TOS_MAX_CPUS > 1` 里编译掉，说明头文件改动没有波及别处）。

变化的部分是同一组语句在屏障之前的重排，屏障与 store 的相对位置未变——这是这件事
在桌面上能给出的最强证据，真正的判据仍是 #17 的 1000 轮 soak。

## 新测试：把规则变成可执行的

`internal/build/testdata/rppark_model.c` + `rpparkmodel_test.go`：用 host 编译器把
`rp_park.h` 编进来，模拟两个核，检查 15 条断言：

```text
ok   first generation is 1
ok   not acked before the other core answers
ok   the other core must park / the owner must not park itself
ok   acked after one publish
ok   release drops flag and ownership
ok   generation advances / the previous ack does not count for this one
ok   a core still in its loop acks the new request      ← 只发一次的 bug 卡在这里
ok   stale ack value does not ack this request
ok   a real publish does
MODEL OK
```

反向验证：把 `rp_park_ack()` 改成"只在第一代发布"，三条断言立刻 FAIL——正是
HISTORY §35 记录的那个缺陷。
