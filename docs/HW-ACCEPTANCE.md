# 上机验收清单（RP2040 / RP2350）

给**另一台连着板子的机器**上的 agent 执行。这份清单把仓库里所有"代码已实现、
只有构建通过"的项（README 里标 ◐ 的）转成可以在真机上判 PASS/FAIL 的步骤。

执行完请按第 5 节回报原始输出，不要只给结论。

```sh
git log --oneline -1     # 应为 102ca7d 或更新（PIO 字段顺序修复必须先到位）
```

两块板的烧写入口不一样：RP2040 是 UF2 盘 `RPI-RP2`，RP2350 是 `RP2350`
（旧 ROM 叫 `RPI-RP2`）。都不支持 UART 下载，BOOTSEL 按键是关键操作，agent
没法代按——请让现场的人配合按一下。

不要提交 `tos.conf`（本机 menuconfig 状态）；构建产物在 `build/`，已被 git 忽略。

---

## 0. 串口工具

agent 需要能读控制台。存成 `/tmp/tos_serial.py`：

```python
import serial, sys, time

def run(port, cmd, settle=1.2):
    with serial.Serial(port, 115200, timeout=0.1) as s:
        s.write(b"\r\n")
        time.sleep(0.4)
        s.reset_input_buffer()
        s.write(cmd.encode() + b"\r\n")
        end, idle, buf = time.time() + settle + 2.0, 0.0, b""
        while time.time() < end:
            chunk = s.read(4096)
            if chunk:
                buf += chunk
                idle = 0.0
            else:
                idle += 0.1
                if idle > 0.5 and buf:
                    break
        return buf.decode(errors="replace")

if __name__ == "__main__":
    print(run(sys.argv[1], " ".join(sys.argv[2:])))
```

依赖 `pip install pyserial`（`scripts/rp2350/flash.py` 也要用）。

串口：RP2040 板是 CH343 → Windows `COMx` / Linux `/dev/ttyUSB0`；RP2350 板同时
有 USB CDC（`2e8a:000a`）和 UART0（GPIO0/1）。115200 8N1，无流控。RP2040 板的
CH343 的 DTR/RTS 没接 BOOTSEL/RESET，**打开串口不会复位板子**，可以随时重连。

---

## 1. RP2040

### 1.1 构建与烧写

```sh
go run ./cmd/tos build -config configs/rp2040.conf
# -> build/kernel-rp2040.elf, build/rp2040.uf2 (约 2.2 MiB)

python scripts/rp2040/autoflash.py --uf2 build/rp2040.uf2     # 按住 BOOTSEL 上电
```

`autoflash.py` 只扫盘符（Windows 专用）。Linux 上手动：

```sh
cp build/rp2040.uf2 /media/$USER/RPI-RP2/ && sync
```

### 1.2 关卡 A：镜像自检

```
cat /proc/dev
```

必须出现（`pio` 特性没编进去时 `/bin/pio` 和这两行都会消失）：

```
pio0     pio      -       4 state machines [PIO0]
pio1     pio      -       4 state machines [PIO1]
```

`ls /bin/pio` 确认命令在。

### 1.3 关卡 B：PIO 回环（不需要接线）

GP4 同时做 OUT 和 IN，`pio tx` 写进去的 bit0 会被驱动到 GP4，`in pins,1` 从 pad
读回来。

```
pio asm /pio/loopback.pio 0 0
```

期望**逐字**等于（这 5 个字是在 x86 上核算过的）：

```
pio: 5 word(s), origin 0, wrap 4..1, side_set 0, clkdiv 100+0/256, pins gp4..4
  0000: e081
  0001: 80a0
  0002: 6001
  0003: 4001
  0004: 8020
pio: loaded into pio0 sm0
```

```
pio start 0 0
pio tx 0 0 1 0 1 1
pio rx 0 0 4
```

期望 `pio: wrote 4 word(s)` 和

```
pio: read 4 word(s): 00000001 00000000 00000001 00000001
```

`rx` 是非阻塞的；返回字数少了就隔 200 ms 再执行一次（SM 一直跑，FIFO 不会因为
没读而丢数据）。

**PASS**：4 个读回值与写入的 4 个 bit 一一对应。

### 1.4 关卡 C：PIO 板载 LED

```
pio asm /pio/blink.pio 0 0
pio start 0 0
```

期望 11 个字（`e081 e001 e03f e05f 1f84 0043 e000 e03f e05f 1f89 0048`），
GP25 约 2 Hz 闪烁。**PASS**：肉眼可见闪烁。结束后 `pio rel 0 0`。

这一关验证 wrap、5 位 delay 和双层循环；如果 B 过而 C 不闪，问题集中在 delay。

### 1.5 关卡 D：PIO WS2812（可选，需要灯带接 GP4）

```
pio asm /pio/ws2812.pio 0 0
```

期望 4 个字，且必须与 pico-sdk `pioasm` 的 `ws2812.pio.h` 一致：

```
  0000: 6221
  0001: 1123
  0002: 1400
  0003: a442
```

```
pio start 0 0
pio tx 0 0 0x00ff0000
```

**PASS**：第一颗灯珠亮红。颜色是 GRB 左移 8 位：红 `0x00ff0000`、绿
`0xff000000`、蓝 `0x0000ff00`。没有灯带就跳过，回报里注明"未测"。

这一关专门验证 `.side_set` 与 EXECCTRL.SIDE_EN（`102ca7d` 修的两个 bug）。

### 1.6 关卡 E：外设与文件系统（都是 ◐）

```
pwm 25 1000 50          # GP25 变暗，1 kHz 50%
uart                    # UART1 TX=GP4 RX=GP5，115200
uart send HELLO
spi / i2c / dma         # 各自打印用法后接一个自发自收/回环用例
```

littlefs 写（这是"挂载 ✅ / 写 ◐"里缺的那一半）：

```
dd if=/bin/hello of=/ddfile bs=512 count=8
ls /ddfile
reboot
ls /ddfile              # 重启后还在 -> flash 写/擦路径可信
```

如果 `/ddfile` 重启后不见了，说明写还在 RAM 层或擦除路径有问题，把 `dd` 和 `ls`
的输出都贴回来。

### 1.7 关卡 F：`rp2040smp`

```sh
go run ./cmd/tos build -target rp2040smp
python scripts/rp2040/autoflash.py --uf2 build/rp2040smp.uf2
```

```
smp                     # 应打印 "smp: 2 CPU(s)"，两个 worker 报告不同的 cpu id
smpbench                # 报告 speedup
```

RP2040 只有 SIO FIFO 握手，没有完整验收记录，**没有基线数字**：把 `smp` 的
topology 和 `smpbench` 的 `speedup: x.xx` 原样回报即可。若两个 worker 都报告
`cpu 0` 或 speedup ≈ 1.00，说明 core 1 实际没在跑用户代码。

---

## 2. RP2350 / Pico 2 / ATK-DNRP2350AM

RP2350 的大部分功能已经是 ✅ 实机（USB CDC + UART0 双控制台、littlefs、MPU
`prot`、ST7789 屏、SMP）。**这里不要重复做全量回归**，只测下面这些新增/未验项。

### 2.1 构建与烧写

```sh
go run ./cmd/tos build -target rp2350
python scripts/rp2350/flash.py --smoke
```

`--smoke` 会在看到 `tos> ` 后依次跑 `echo / ls / free / uptime / cat /proc/uptime /
gpio / reboot`；`--port auto` 靠 VID/PID 找 CDC 口。按住 BOOTSEL 插 USB（或
BOOTSEL + RESET）出现 `RP2350` 盘时脚本会自动拷贝。

`python scripts/rp2350/flash.py --list` 可以先看串口和盘符。

### 2.2 关卡 G：SMP 回归（`rp2350smp`，已有基线）

```sh
go run ./cmd/tos build -target rp2350smp
python scripts/rp2350/flash.py --uf2 build/rp2350smp.uf2 --smoke
```

```
smp
smpbench
```

**PASS**：`smp` 打印 `smp: 2 CPU(s)`，两个 worker 的 cpu id 不同；`smpbench` 的 speedup
在 **1.8–2.0x**（历史记录 1.89–2.01x）。明显低于 1.8x 就是回归。

同一条链路在 RISC-V 核上也验一遍：

```sh
go run ./cmd/tos build -target rp2350rvsmp
python scripts/rp2350/flash.py --uf2 build/rp2350rvsmp.uf2 --smoke
```

### 2.3 关卡 H：内核线程 + SMP（重点，从未上过真机）

`configs/rp2350smp-kthreads.conf`（Cortex-M33 ×2）和
`configs/rp2350rvsmp-kthreads.conf`（Hazard3 ×2）目前**只构建过**。

```sh
go run ./cmd/tos build -config configs/rp2350smp-kthreads.conf
python scripts/rp2350/flash.py --uf2 build/rp2350smp.uf2 --smoke
```

```
kthreadtest     # 期望最后一行 "kthreadtest: all ok (0 failure(s))"，17 项
threadbench     # 期望 ratio 接近 100%，以及 "sleep() in main, sibling kept running"
prodcons        # 期望校验和一致，无阻塞卡死
```

**PASS**：`kthreadtest` 全过；`threadbench` ratio 不高于 ~110%（QEMU mps2 与 C3
的实测都接近 100%）；`prodcons` 输出校验和且能返回提示符。

已知的两个 SMP 边界（`docs/REAL-THREADS.md`）：`sbrk` 只重编程当前核的 PMP，
所以堆必须在 `kthread_create` 之前分配；`close()` 撞上阻塞中的 `read/write` 没有
引用计数保护。如果 `kthreadtest` 在这两项上挂住，**不是新 bug**，记录现象即可。

然后同样跑一遍 RISC-V 双核：

```sh
go run ./cmd/tos build -config configs/rp2350rvsmp-kthreads.conf
python scripts/rp2350/flash.py --uf2 build/rp2350rvsmp.uf2 --smoke
```

### 2.4 关卡 I：板载件（只看一眼，均已实机通过）

```
screen                   # 屏幕上出状态卡
echo hi > /dev/lcd0      # 屏上出现 hi
cat /dev/key0            # 1 = 按下
prot rodata              # 期望被 MPU 当场拦下（"prot: caught" 一类输出）
```

`prot` 在这块板上是**真保护**，跑完记得 `reboot` 复位。

### 2.5 关卡 J：复位之后的 flash 写（回归，ARM 与 RISC-V 都要做）

早于 `64a11ce` 的镜像这一步会把板子写死：写本身成功，死在写完之后。原因是
`flash_enter_cmd_xip` 回放的 SCLK 分频是为 ROM 启动时的 clk_sys 算的，而内核
已经跑到 150 MHz（见两块板 README 的"已知限制"）。**刷完机立刻测是测不出来的**，
必须在复位之后测。

```
dd if=/bin/hello of=/x bs=512 count=8
df
reboot
dd if=/bin/hello of=/y bs=512 count=8
df
ls /x /y
```

**PASS**：两次 `dd` 都成功，`reboot` 之后还能回到 `tos>`，`/x`、`/y` 都在。
`rp2350`、`rp2350smp`、`rp2350rv`、`rp2350rvsmp` 四个镜像都要过。

### 2.6 关于 PIO

RP2350 上**没有**实现 PIO（`board/rp2350/` 没有 pio.c，RP2350 是每块 3 个 SM /
共 12 个，寄存器和 RP2040 不完全相同）。在 RP2350 上敲 `pio` 得到

```
pio: this board has no PIO support
```

这是**预期行为**，不要当 bug 记。

---

## 3. 失败的常见原因

| 现象 | 含义 |
| --- | --- |
| `pio: assembled, but not loaded` | `pio` 特性没开，或该 SM 被别的 pid 占着（先 `pio rel`） |
| `pio: failed (not owner, or no PIO)` | SM 被另一个进程装载过；`pio rel <block> <sm>` 或复位 |
| `pio: cannot open /pio/xxx.pio` | `pio` 特性关闭时示例文件不打包 |
| 汇编字与本文档不一致 | 汇编器回归，贴完整输出 |
| 字对但引脚无反应 | 引脚被复用，检查输出里的 `pins gpN..M` |
| `flash.py` 找不到 CDC | 用 `-p COMx` 指定 UART0，或 `--list` 排查 |
| UF2 拷完不启动 | family/IMAGE_DEF 不对，贴 `flash.py` 的完整日志 |

---

## 4. 收尾

```
pio rel 0 0     # RP2040：把 SM 和引脚交回
reboot          # RP2350：prot/screen 之后复位回干净状态
```

PIO 的指令存储和 SM 配置都是易失的，重烧或复位后全部丢失——这本身也是
`docs/PIO.md` 里"重启会丢"那条规则的实测确认。

---

## 5. 回报格式

按关卡列，每关给 **PASS / FAIL / 未测**，并附原始输出。至少包含：

1. `git log --oneline -1`
2. `cat /proc/dev` 原文
3. 1.3 / 1.4 / 1.5 里 `pio asm` 的完整字表，以及 `rx` 的读回值
4. `smp` 的 topology、`smpbench` 的 speedup、`threadbench` 的 ratio、
   `kthreadtest` 的结尾行（RP2040 与 RP2350 两个架构都要）
5. 只能靠眼睛判断的项：GP25 是否闪、灯珠颜色、屏幕是否出图、按键读数
6. 失败时的**现象**：报错原文 / 静默无输出 / 数值不符，以及复现步骤

只贴"PASS"而没有原始输出的，回头还是得再问一遍。
