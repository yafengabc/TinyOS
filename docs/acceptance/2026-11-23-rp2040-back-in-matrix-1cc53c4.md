# RP2040 重新进 T3 矩阵 + 一键验收脚本（2026-11-23）

```text
TinyOS commit:     1cc53c4（AGENTS/TESTING 策略 + scripts/rp2040/accept.py）
target/config:     文档与宿主机脚本（不改固件、不改构建）
board/port:        RP2040 NOT RUN（测试台无板；只有 C3 的 CH343 在 COM4）
toolchain:         go 1.27.1 windows/amd64；python 3.13（pyserial）
host:              Windows 11 / EKSOFT
T0:                PASS — go test -count=1 ./...
T1:                NOT RUN（无固件改动，按矩阵不需）
T2:                NOT RUN（同上）
T3:                NOT RUN — 无 RP2040 板，阻塞记录在 OPEN-ITEMS §2c
T4:                NOT RUN
```

## 改了什么

| 文件 | 内容 |
|---|---|
| `AGENTS.md` | “RP2040 不作为关闭依据”改成“重新进入 T3 矩阵（2026-11-23）”：改 `arch/arm`/板级/驱动/内核/控制台/共享头时必跑受影响的镜像，且**走板载 CDC**；按键是唯一人工步骤，不是跳过理由；Pico 2 仍是 RP2350 的关闭门槛 |
| `docs/TESTING.md` §5.0.1 | 同上，写成矩阵规则 |
| `docs/TESTING.md` §5.3 | 命令改成一键 `accept.py`；写明判据、CDC 要求、身份检查失败即退 |
| `docs/TESTING.md` §11 | CI 不覆盖项里点明：镜像在 CI 构建，硬件只能人工 |
| `docs/OPEN-ITEMS.md` §13b | 刷机段落指向新脚本；§2c 记录本轮阻塞 |
| `scripts/rp2040/accept.py` | 新增，一条命令：等 BOOTSEL → 刷 UF2 → 全项检查 |
| `scripts/rp2040/autoflash.py` | docstring 指向 `accept.py`（它只抓 transcript） |

## 脚本检查项（`ACCEPT OK n/n`）

身份（`/proc/cpuinfo` 必须是 RP2040 + 板名）、时钟（两次 `uptime` 对齐墙上时间）、
堆、`ls /bin`、`hello`、`wfile/cat/rm`、`crash` 隔离、**两处长输出**、**两轮 `dd`/`rm`**、
收尾仍应答、全程无 panic/fault。

长输出两处：

1. `ls /bin` 的 token 数 == `ls /bin | wc` 的单词数（管道路径不会截断，控制台丢字节就
   对不上）；
2. `cat /readme.txt` 逐字节对账：控制台把每个 `\n` 变成 `\r\n`，所以
   `len(正文) == wc 的 bytes + lines`，行数、单词数也对账。

## 没有板时能给的证据

- **解析层**：用合成的 console 抓包跑 `accept.py` 里的三个纯函数（不联网、不碰板）：

  ```text
  good : len=1439 want=1439 lines=37/37 words=244/244
  trunc: len=192 want=1439          <- 192 B 截断被抓到
  tokens: ['msh', 'sh', 'wc', 'hello', 'dd', 'crash', 'ls', 'cat']   <- prompt 不混进 token
  ```

  （1439 = `/readme.txt` 的 1402 B + 37 个换行。）

- **插桩层**：把脚本对着台上的 C3（COM4）跑一遍，只验证串口/读 prompt/解析：

  ```text
  PASS  boot: shell prompt                       prompt
  FAIL  identity: RP2040 + board name            ESP32-C3 / rv32imc
  this is not an RP2040 image (model=ESP32-C3 isa=rv32imc); stopping
  before it touches that board's filesystem.
  exit=2
  ```

  这正是身份检查先跑、失败即退的目的：台上只有一根 CH343 时它可能是 C3，脚本不能
  继续去写那块板的文件系统。

- `python -m py_compile` 两个脚本通过；T0 全绿。

## 板上怎么跑（T3 待做）

```sh
python scripts/rp2040/accept.py --uf2 build/tinyos-rpi-pico.uf2      # 按键进 BOOTSEL
python scripts/rp2040/accept.py --uf2 build/tinyos-rpi-pico-smp.uf2
python scripts/rp2040/accept.py -p COM17 --dtr                       # 已刷好时
```

判据：末行 `ACCEPT OK n/n`、退出码 0，且 `console: usb-cdc`（UART0 那次不算 USB
证据）。失败时逐项看 `FAIL` 行：`identity` → 刷错镜像；`long output` → 截断（先查
`/proc/usbboot`、`g_usb_irq_count`）；`dd/rm` → flash 或分配器。

## 没做的

- 真实的 RP2040 运行（无板，见 OPEN-ITEMS §2c）；
- 把 `accept.py` 扩展成 24 项“平权级”表（`prot`/PIO 目视/`reboot` 仍在各自的脚本里）。
