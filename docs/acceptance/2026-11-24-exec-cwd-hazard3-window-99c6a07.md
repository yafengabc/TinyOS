# exec 的 cwd 解析 + Hazard3 去 RVC 的两处收尾（2026-11-24）

```text
TinyOS commit:     b1921a2（exec 路径规则）、99c6a07（msh 窗口承诺 / PMP 说明）
script commit:     090e022（rcc/thumb1 SP 立即数同步，见文末）
target/config:     esp32c3-generic；rv32 / rv32smp / arm / x86 QEMU；全 18 目标
board/port:        ESP32-C3（COM4）；RP2350 / RP2040 真机 NOT RUN（不在本机）
toolchain:         go 1.27.1 windows/amd64；clang 23.1.4
host:              Windows 11 / EKSOFT
T0:                PASS — go test -count=1 ./...
T1:                PASS — go run ./cmd/tos build all -> 18/18（修前 16/18）
T2:                PASS — smoke rv32/rv32smp/arm/x86；soak 3/3
T3:                PASS — C3 accept.py 42/42；RP2350/RP2040 NOT RUN
T4:                NOT RUN
```

## 1. `exec()` 的路径规则（`b1921a2`）

`exec()` 以前是唯一不认 cwd 的路径调用：`./h3` 打不开（没人折叠 `.`），`sub/x`
永远从根解析，两个 shell 靠拼 `/bin/`、`bin/` 绕过去（只在“文件系统也忽略 cwd”
时成立）。现在：

| 输入 | 解析 |
|---|---|
| `/h2` | 绝对路径 |
| `./h3`、`src/prog`、`../bin/x` | 相对 cwd（`resolve_path()`，和 `open()`/`stat()` 同一个） |
| `hello`（无 `/`） | 先看当前目录，没有再找 `/bin/hello`；都没有时报 `/bin/hello` |

msh 和 sh 都改成直接 `exec(argv[0])`（msh 因此小了），`piperace`/`smp`/`smpbench`/
`mailtest` 里的 `bin/x` 改成 `/bin/x`，内核起 shell 用 `/bin/msh`。

C3 上逐条实测（COM4）：

```text
tos> cat /bin/hello > /h2
tos> h2                     hello from ring 3!  argv[0] = "h2"      <- cwd 优先
tos> /h2                    hello from ring 3!  argv[0] = "/h2"
tos> cd /tests
tos> cat /bin/hello > /tests/h3
tos> ./h3                   hello from ring 3!  argv[0] = "./h3"     <- 相对带 /
tos> h3                     hello from ring 3!  argv[0] = "h3"
tos> cd /bin ; hello        hello from ring 3!                        <- cwd 就是 /bin
tos> cd /   ; hello         hello from ring 3!                        <- 回落 /bin
tos> nosuchprog             exec: '/bin/nosuchprog' not found ...
tos> piperace 1 8           piperace: ok                              <- 子进程 /bin/piperace
```

验收脚本加了 3 项固定这个行为（`./名字`、当前目录裸名字、删掉），C3 从 39 项变
42 项；RP2040 的 `accept.py` 也加了同一项。

## 2. Hazard3 去 RVC 的两处收尾（`99c6a07`）

对 `fa5e66d`（用户侧 `-march=rv32ima`）的复核结论：

- 方向对：RISC-V 只允许“单个 PMP 条目覆盖整个访问”，而 rp2350rv 的映射是页粒度
  NAPOT，`.text` 会被切成多块；RVC 让 32 位指令落在 `page_end-2` 时一次取指跨两个
  条目，必然 fault。这是规范行为，不是 Hazard3 勘误；C3 用 TOR（4 字节粒度），所以
  同一问题在 C3 上不会出现——这也解释了为什么只改 rp2350 是对的。
- **发现一个真问题**：msh 的窗口承诺没跟着镜像长。`fa5e66d` 让 msh 镜像
  8 KiB → 12 KiB，`TOS_WINDOW(16 KiB)` 装不下 12 + 4 + 4 = 20 KiB；新增的
  `grain >= 16 KiB` 分支救不了 ATK 的两个 Hazard3 目标（4 KiB grain）：

  ```text
  tos: atk-dnr2350am-rv32: bin/msh: TOS_WINDOW(16 KiB) no longer fits:
  image 12288 + stack 4096 + heap 0 + guard 4096 = 20480 bytes, so it would
  take a 20 KiB window
  ```

  即 `tos build all` 只有 16/18。窗口需求跟镜像走、镜像跟 ISA 走，不跟 grain 走：
  build 在去掉 C 扩展时同时传 `-DTOS_USER_NO_RVC=1`，msh 的承诺按它分支
  （4 KiB grain 24 KiB、16 KiB grain 32 KiB）。修后 18/18。
- 旁证（`rpi-pico2-rv32-smp` 的对象属性，`readelf` 语义）：

  ```text
  user: msh.o / lib.o / syscall.o / rcc.o / csc.o   rv32i2p1_m2p0_a2p1_...（无 c2p0）
  kernel: rv32_platform.o                            rv32i2p1_m2p0_a2p1_c2p0_...（保留 RVC）
  C3: msh.o / lib.o                                  rv32i2p1_m2p0_c2p0_...（不受影响）
  ```

## 3. pinned 工具链漂移（外部仓库 `090e022`）

`user/rcc/thumb1.inc` 的 SP 立即数修复只落在 TinyOS 一侧，`TestPinnedToolchainSources`
变红（“copy it across and commit both”）。已把同一改动同步进 `tinyos-script`
（`src/rcc/thumb1.inc`，`090e022`），脚本仓库 `tools/test.py` ALL TESTS PASS、
`check_target_build.py` 三个源 PASS、`check_docs.py` OK；T0 恢复绿。

## 4. 尚未覆盖 / 留意

- **RP2350 / RP2040 真机未跑**：本机没有板。`fa5e66d` 的板级证据在对面的
  `2026-11-23-pico2-matrix-f91db82.md`（rv32 单/双核三条 CSC 链）；本轮改的
  msh 窗口只影响窗口大小，未上板确认 `run sh` 嵌套深度。
- **容量**：Hazard3 上 msh 32 KiB、rcc 80 KiB 窗口，`run sh` 的嵌套层数会比以前少一层；
  `docs/SHELL-GUIDE.md` 的窗口预算段落没写这个差异。
- **PMP 的根限制仍在**：页粒度 NAPOT、7 块预算、代码/数据同页即 RWX（OPEN-ITEMS 31.0）。
  去 RVC 只解决“取指跨块”，不解决这些。
- 用户侧不再用 RVC，Hazard3 用户代码大 15–20%；C3 / QEMU 不受影响。

## 重跑

```sh
go test -count=1 ./...
go run ./cmd/tos build all
go run ./cmd/tostest smoke -target rv32         # rv32smp / arm / x86 同
python scripts/soak.py --cycles 3 --targets rv32,rv32smp,arm
go run ./cmd/tos build -target esp32c3-generic
python scripts/esp-c3/flash.py --build -p COM4
python scripts/esp-c3/accept.py -p COM4 --no-reset
```
