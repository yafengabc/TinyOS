# 128 KiB RAM / 512 KiB flash：能不能跑 TinyOS

（评估，2026-11-16；数字取自当前 `main` 的 rp2040/rpi-pico-smp 构建）

> RP2040/Pico 本身是 **264 KiB** SRAM；这里是把 rp2040 构建当作 128 KiB RAM 目标的代理数字，不是 RP2040 的实际容量。

## 结论

**单核能跑，SMP 不建议**。真正卡人的不是 RAM 而是 **512 KiB flash 的布局**：现在
RP2040 把 littlefs 放在 **0x80000 = 512 KiB**，正好等于整片 flash 的末尾，必须把
文件系统前移。RAM 那边 128 KiB 够，但并发窗口从 21 个掉到 **5 个左右**。

## 现在的实测占用（rp2040 目标，本机 `tos.conf`）

| 项 | 单核 | SMP |
| --- | --- | --- |
| flash（boot2 + 内核代码） | **73.3 KiB** | 74.3 KiB |
| RAM（.data + .bss，含 8 KiB 内核堆） | **45.4 KiB** | 53.7 KiB（多一个 8 KiB 内核栈） |
| littlefs 偏移 | 0x80000（512 KiB） | 同 |
| littlefs 分区 | 384 blocks = 1.5 MiB | 同 |
| 打包的 FS 内容 | ~100 KiB（48 文件） | 同 |

## 512 KiB flash 怎么切

```
0x000000  boot2 + 中断向量 + 内核      74 KiB 上限留 128 KiB
0x020000  littlefs 起点（新）
0x080000  512 KiB 末尾
```

- `PLAT_LFS_LBA = 0x20000/512 = 256`，`PLAT_LFS_BLOCKS = (0x80000-0x20000)/4096 = 96`
  （384 KiB 分区），内容 ~100–150 KiB，够；
- 还有一个**看不见的耦合**：`internal/build/uf2.go` 的 `rpLfsOffset = 0x80000` 是
  硬编码，rp2040 / rp2350 / atk-dnr2350am-rv32 共用。新板要把这个常量变成按目标取值，否则
  UF2 里文件系统会被写到 flash 之外；
- `RamdiskSectors`（现在 rp2040 是 1200 = 600 KiB）要降到 ≤ 768（384 KiB），
  `checkLayout()` 会核对它和 `PLAT_LFS_BLOCKS` 是否一致。

## 128 KiB RAM 怎么分

窗口 = 镜像 span + 4 KiB 栈 + 4 KiB 护栏 + 4 KiB 增长余量，所以**每个任务约
16 KiB**（`msh` 实测就是 16 KiB），`sh`/`py` 这类大程序 36–44 KiB。

| 配置 | 内核占 | 剩给窗口 | 并发任务数 |
| --- | --- | --- | --- |
| 单核 | 45.4 KiB | **82.6 KiB** | 5 个普通任务，或 `msh`+`sh`+2 个小工具 |
| SMP | 53.7 KiB | 74.3 KiB | 4 个，`sh` 一来就只剩 2 个 |

结论：单核可用；SMP 在 128 KiB 上勉强，两个 8 KiB 内核栈 + 每个任务 16 KiB，
窗口碎片化以后很容易 `no room`。

## 要改的东西（按顺序）

1. **新板 profile**：`layout.c` 的 `PLAT_APP_MAX`/`PLAT_RAM_TOP` 改成 0x20020000
   （128 KiB）、`PLAT_LFS_LBA`/`BLOCKS` 按上面；
2. **`rpLfsOffset` 参数化**（`uf2.go`），`RamdiskSectors` 改 768；
3. **裁用户集合**：`py` 44 KiB 的窗口在 82 KiB 里放不下，`sh` 还留着，
   建议 `exclude = py,coremark,bench,fb,floattest,lcdconsole,screen,splash`，
   保留 `msh sh cat ls echo dd df free ps rm sleep wfile wc uptime prot deep`；
4. **`heap` 保持 8 KiB**（现在是 12288 的本机配置，128 KiB 上别加）；
   `max_procs` 可以留 8，窗口数才是限制；
5. `PLAT_STACK_BYTES` 仍 4 KiB、`PLAT_HEAP_GUARD` 4 KiB、grain 4 KiB——这套在
   RP2040 上已经验收过，别动。

## 上板前需要重新量的

§30.3 那三条在这块板上会变：`msh` 仍是 16 KiB（没变），但**并发窗口数**从 21 掉到
5，`deep 6` 需要的 32 KiB 连续空间还在（82 KiB 够），所以 `deep 3`/`deep 6` 的判据
不用改。真正要重测的是"`msh`+`sh`+`dd` 同时在场时窗口分配会不会失败"，以及
512 KiB flash 上 littlefs 96 blocks 的写入寿命（`dd` 200 轮会覆盖同一批块）。

## 不建议的

- **128 KiB 上跑 SMP**：收益是 2 个核，代价是 8 KiB 内核栈 + 一半的并发任务，
  在 `dd`/`rm` 这种单线程负载上没有意义；
- **把 grain 降到 1 KiB 换更多窗口**：RP2040 没有 MPU，窗口靠软件守卫，1 KiB
  步长会让护栏只有 1 KiB，栈溢出检测变得不可靠，而且 PMP/MPU 板子那边不通用。
