# mkdir/rmdir 系统调用 + 小命令合并成 /bin/tools（2026-11-28）

```text
commits:           ba05eda  fs: mkdir/rmdir 系统调用（lfs_mkdir、空目录 lfs_rmdir）
                   64c7c4d  user: 19 个小命令合并成一个 multi-call 二进制
change:            新增 SYS_MKDIR 57 / SYS_RMDIR 58；/bin/tools 承载
                   cat cp dd df du echo flashinfo free grep head kill mkdir
                   rmdir stat tail tee uptime wc wfile；内核 exec 对裸名增加
                   第三处回退 /bin/tools（argv[0] 保留原名）
target/board:      ESP32-C3（COM4）；18 个构建目标；QEMU smoke
toolchain:         go 1.27.x；clang/ld.lld 23.1.4
T0:                PASS（go test ./...、go vet、goc32 测试 + 单文件编译、
                   native32 测试；ABI 快照与 CSV 一致）
T1:                PASS 18/18
T2:                PASS smoke rv32 / rv32smp / arm
T3:                PASS（C3 默认镜像，accept 42/42）
T4:                NOT RUN（纯用户态/FS 工具；不碰调度/窗口/flash 压力路径）
logs:              build/t-mc3-*.log、build/t-bb*.log（本地，不入库）
```

## mkdir / rmdir

- `SYS_MKDIR` → `ramdisk_mkdir` → `lfs_mkdir`；
- `SYS_RMDIR` → `ramdisk_rmdir`：先 `lfs_stat` 必须是目录，再用
  `lfs_dir_open/read`（跳过 `.`/`..`）确认**空目录**，最后 `lfs_remove`；
  非空、设备路径、或路径包含调用者 cwd 都拒绝；
- `lfs_remove` 对目录是递归的，所以 `unlink()` 仍然拒绝目录，`rm` 不会误删 `/bin`；
- ABI 追加在末尾（57/58），`abi/syscalls.csv` 和 goc32 的 snapshot 一起更新；
  `go test` 的快照比对通过，goc32 单文件编译通过。

## multi-call

- 源文件移进 `user/tools/`（目录程序 → `/bin/tools`），`main` 改成
  `applet_<name>`，`dispatch.c` 按 `argv[0]` 分发；`tools <name>` 等价，
  `tools` 无参列出 applet；
- **命令名到 `/bin/tools` 的映射放在内核 `exec()`**：裸名先 `./name`，再
  `/bin/name`，都不在则 `/bin/tools`，`argv[0]` 保持用户输入。放在 shell 里
  会把 msh 的镜像从 8 KiB 推过线、窗口变成 20 KiB（RP2040 上构建直接拒绝），
  内核回退只多几十字节，两个 shell 不用动；
- `ls -l /bin` 追加 `name -> tools` 行；默认 `ls /bin` 保持原始列表（脚本按
  名字计数），`ls` 本身保持独立二进制；
- applet 清单只有一份 `user/tools/applets.h`，`dispatch.c` 和 `ls.c` 共用。

## C3 实测（`df`）

| | 合并前 | 合并后 |
|---|---|---|
| `df` used | 421,888 B | **323,584 B** |
| 节省 | — | **96 KiB** |
| `/bin` 文件 | 19 个独立工具 | 1 个 `tools`（10,902 B，3 个 block） |

分两步：前 10 个工具 421,888 → 352,256（68 KiB），再加 9 个文件/文本工具
352,256 → 323,584（28 KiB）。

逐命令验证（C3，默认镜像）：

```text
tos> du /tests                 -> 4491 /tests
tos> cat /readme.txt | wc      -> 37 244 1402
tos> dd of=/y bs=4096 count=1  -> wrote 4096 bytes
tos> dd if=/y bs=4096 count=1  -> read 4096, pattern verified
tos> df / free / uptime / echo / wfile / flashinfo / stat / kill / mkdir / rmdir  全部正常
tos> ls -l /bin                -> 19 行 `l  name -> tools`
tos> nosuchcmd                 -> tools: unknown applet nosuchcmd
```

## 没有合并的

`ls`（目录列表器，2.4 KB，保留真实路径）、`msh`/`sh`（内核直接启动）、
`csc`/`rvm`/`rcc`、`edit`、`rx`（tosput/tosedit 的关键路径），以及硬件
（`gpio adc spi i2c pwm uart`）和网络（`ping host net http dhcp wificonn
wifiscan`）命令——后两组要按 feature 门控，留作第二轮。

代价：`tools` 是 10.9 KB，每次调用按整个 span 开窗口（约 16 KiB），比单独的
`cat`（4–8 KiB）大；C3 无压力，RP2040 的 264 KiB 上单/双个并发仍可接受。
