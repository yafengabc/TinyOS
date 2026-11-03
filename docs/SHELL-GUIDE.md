# Shell 使用指南：脚本与优先级

本文是**使用者视角**的说明：每个行为都对应现在的实现，示例输出都是实跑抓下来的
（x86 与 rv32 行为一致，下面不再分别标注）。设计动机与实现细节在
`docs/PLATFORM-DESIGN.md` §6，这里只讲"怎么用、边界在哪"。

```
tos> help
TinyOS shell commands:
  ls                 list files in the ramdisk
  cat <file>         print a file
  echo [args...]     print arguments
  hello              demo program
  mem                heap / sbrk demo
  crash              run an illegal instruction (kills that process)
  uptime             time since boot
  ps                 list tasks
  run <prog> [args]  run a program from /bin
  run <prog> ... &   run it in the background
  run sh <file>      run the commands in a file, as a task
  source <file>      run the commands in a file, in this shell
  nice <n> <prog> .. run it at priority n (0 high, 2 low)
  prio <pid> <n>     move a running task to priority n
  help               this text
  reboot             reset the machine and boot again
  exit [code]        shut TinyOS down (end the script, in a script)
```

命令分两类：**内建**（`ls` 之外上面列出的这些，由 shell 自己解释）和
**程序**（`bin/` 下的每个 ELF）。两者在命令行上没有区别——不是内建的名字
会被当成 `bin/<名字>` 去 exec，所以 `hello alpha` 和 `run hello alpha` 等价。

---

## 1. 脚本

### 1.1 语法：就是控制台

脚本没有第二种语法。shell 用同一个 `split()`、同一个派发函数读文件，
只是换了个输入源并把提示符关掉：

- 一行一个命令；**空行跳过**；**`#` 在行首时整行是注释**——只认行首，命令中间的
  `#` 会被当成普通参数传给程序（`echo a #b` 打印 `a #b`）；
- 行尾的 `&` 让命令进后台，和控制台一样；
- `exit [N]` 在脚本里表示**结束本脚本**并返回 N（只保留低 8 位，`N & 255`）；
  只有在控制台（不在任何脚本里）时它才是关机。

```sh
# a script, any name under the filesystem root
echo [rc] hello from the boot script
uptime

run sh other.sh &
exit 3
```

### 1.2 两种跑法

| 写法 | 做什么 | 任务窗口 | 适合 |
| --- | --- | --- | --- |
| `source <file>` | 在**当前 shell 进程**里递归读文件，不 `exec` | 不占 | 嵌套脚本；C3 上跑任何东西 |
| `run sh <file>` | `exec` 一个新的 sh，文件名作参数 | 占 1 个 | 崩溃隔离、`&`、要退出码 |

选哪个的判断只有一条：**要不要多一个任务**。

- `run sh <file>` = `exec("bin/sh", {"sh", file})`。脚本跑飞只会带走那个 sh，
  shell 毫发无损；它也是一个普通任务，可以 `&` 进后台，退出码经 `wait()` 返回
  （shell 目前不打印子任务的退出码）。
- `source <file>` 是在 shell 自己的身体里读文件。没有隔离——脚本里一句 `reboot`
  就是真重启，一个 crash 也一样（crash 的非法指令只杀当前任务，而"当前任务"
  就是你的 shell）。它换来的是**不占任务窗口**。

为什么"不占窗口"重要：**ESP32-C3 一共只有 3 个任务窗口**（shell、前台、备用）。
`run sh a.sh` 自己吃掉一个，a.sh 里再跑一个程序就是第三个，再嵌一层内核只会回
`exec: out of task windows`——这是真机实测的结论，QEMU 上窗口宽裕，完全看不出来。
所以**在 C3 上写嵌套脚本一律用 `source`**。

### 1.3 路径与参数

- `<file>` 是**文件系统根的相对路径**，带不带前导 `/` 都行：`source etc/rc` 与
  `source /etc/rc` 等价（实测）。
- `run sh <file>` 里只有 `sh` 这个名字吃 `bin/` 前缀，文件名参数原样传给 sh，
  所以同样写 `run sh etc/rc`。
- 脚本**不能带自己的参数**：`run sh a.sh foo` 里 `foo` 会被 sh 忽略（它只读 `argv[1]`）。
  需要参数化的话，用 `wfile` 生成脚本内容。
- 文件名（含路径）上限 63 字节；命令行上限 256 字节。脚本行超过 256 字节会被
  **截断并明说**：`sh: script line too long, truncated`——脚本不能重打，所以不静默。

### 1.4 嵌套与深度

`source` 可以嵌套：a.sh 里 `source b.sh`，b.sh 里再 `source c.sh`……
上限 **`MAX_SOURCE = 4`** 层（控制台本身算第 0 层），到顶报：

```
tos> source a.sh          # a→b→c→d→e 五层链
sh: scripts nested too deep
```

实测：四层链（a→b→c→d）正常执行，第五个 `source` 被拒，深层文件不再打开，
外层脚本从下一行继续。设上限的真正理由不是栈（每层约 110 B，8 KiB 默认栈付得起），
而是**两个脚本互相 `source` 这种死循环**。

`run sh` 的嵌套不受这个数管，它受**任务窗口**管：每层一个窗口，x86/rv32
足够宽裕，C3 的 192 KiB 在按需分配 + 8 KiB 默认栈下能装 12 个 16 KiB 的窗口。

### 1.5 启动脚本 `etc/rc`

pid 1 的 shell 在打出 banner、第一个提示符**之前**读一次 `files/etc/rc`
（随 `tos build` 打包进镜像，板上路径 `etc/rc`）。注意：

- **只有 init（pid 1）读它**。你后来 `run sh` 起的每个 shell 都不再读——否则
  每层 `run sh` 都会重放一遍启动命令。
- 发行版里它是**空的（纯注释）**，这是刻意的：开机自己说话的机器没法做差分
  测试，而这里打印的每个字节都会落在所有验收用例前面。想加启动动作，把
  `files/etc/rc` 里的示例行取消注释，或者写自己的，然后重新 `tos build`。

### 1.6 错误报告

- `source nosuch` / `run sh nosuch` → `sh: cannot open nosuch`（点名的就是你给的路径）；
- 嵌套超限 → `sh: scripts nested too deep`；
- 行太长 → `sh: script line too long, truncated`；
- 脚本里某个命令 exec 失败（比如 `bin/` 下没有这个名字）→ 由内核报具体原因
  （not found / corrupt / out of task windows），shell 不重复打印。

### 1.7 一个完整的例子

```sh
# files/etc/rc —— 开机把低优先级的采集任务挂到后台，然后交出控制台
nice 2 bench &          # 2 级（最低）跑基准，不挡 shell
echo [rc] up and running
```

调试脚本不用重烧镜像——板上现写：

```
tos> wfile t.sh echo hello from a script
tos> source t.sh
hello from a script
```

（`wfile <file> <内容>` 写一行；要多行脚本就分几次写不同文件，或 `source` 串起来。）

---

## 2. 优先级

### 2.1 模型

三级，**0 最高**：`PRIO_HIGH 0` / `PRIO_NORMAL 1` / `PRIO_LOW 2`。
三条规则，全部是刻意的：

1. **继承**：子任务出生在父任务的级别上。shell 是 0 级，所以从 shell 启动的
   一切默认 0 级。
2. **只能降，不能升过自己**：`setprio` 把目标级别调到**高于调用者自己的级别**
   时直接拒绝（不是 clamp）。没有 aging 的世界里，一个能把自己抬到 shell 之上的
   程序就锁死了整个控制台；不给学生这个选项，最坏也就是同级轮转——和没有
   优先级之前一样。
3. **级间严格，级内轮转**：`sched_pick()` 从高到低扫三级，**只要高一级有一个
   可运行的任务，低一级一个 tick 都拿不到**；同级的任务从上次运行的位置往后
   轮转。**没有 aging**：低优先级饿死是定义内的行为，不是 bug——你申请级别，
   就是在申请"别人永远让着我"。

时间片与以前一样：100 Hz tick，`QUANTUM = 5` 即 **50 ms 一片**，只在用户态帧上
抢占（syscall 执行到一半不会被换走）。

### 2.2 命令

```
nice <0-2> <program> [args...] [&]     以指定级别启动一个程序
prio <pid> <0-2>                        移动一个已经在跑的任务
```

- `nice` 先 `exec`（子任务此刻 READY、**一条指令都没跑过**）再设级，所以不存在
  "用错误级别偷跑一片"的窗口。
- `prio` 对任何在跑/在等/在睡的任务都有效，设完当场生效：如果被升级的任务比
  当前 CPU 上的任务级别高，**下个 trap 收尾就切换**，不等这一片用完。
- `nice 9 hello` → `usage: nice <0-2> <program> [args...] [&]`；
  `prio 99 1` → `prio: no such task, or a level you cannot set`
  （这句话的两种含义是同一个报错：pid 不存在，或级别越过了调用者）。

用户程序也能用：`setprio(pid, n)`（`pid 0` = 自己）系统调用，同一套规则——
进程可以把自己**降到**任意级别自首，升不上去。

### 2.3 ps 怎么读

```
tos> nice 1 spin &
[2]
tos> run spin &
[3]
tos> ps
 pid ppid prio state  c cpu% cpums wink stk/sz  name
   1    0    0 run    -    0.0     0   32    0.6/8  sh
   2    1    1 rdy    -   51.3     0   32    0.2/8  spin
   3    1    0 rdy    -  100.0     0   32    0.2/8  spin
```

| 列 | 含义 |
| --- | --- |
| `pid` / `ppid` | 任务号 / 父任务号（shell 的孩子 ppid=1） |
| `prio` | 当前级别，0–2。**就是这一列在决定谁先跑** |
| `state` | `run` 在 CPU 上；`rdy` 可运行等轮转；`wait` 等子任务；`tty` 等一整行输入；`sleep` 睡到某个 tick；`stop` 被 `^Z` 停住（保留窗口和现场，**再按一次 `^Z` 恢复**，内核打 `[pid N] continued`） |
| `cpu%` | 最近一段的 CPU 占用 |
| `cpums` | 累计 CPU 毫秒 |
| `wink` | 这个任务实际占用的窗口（KiB）。窗口按需分配，镜像越大占得越多；8 KiB 默认栈下，300 字节的程序是 16、shell 是 32、`edit` 也是 32；空闲窗口不进这一列 |
| `stk/sz` | 分子是栈高水位（KiB，一位小数）：这个任务至今最深用到的栈。`exec` 把整块栈填 `0xCC`，从底部往上找第一个被改过的字节就是答案。实测 shell 0.6、`spin` 0.2、`stk 6` 12.3；历史上最深的那次记在 `cat /proc/stack`。分母是这个任务**现在**的栈：板子的默认值、它自己 `TOS_STACK()` 要的，或者踩出栈底后内核翻倍给它的（内核会打一行 `[pid N] stack grown to 16 KiB`）。分子是"够不够"的唯一依据，分母说明在给哪个尺寸打分 |

### 2.4 实测：级间是什么样子

0 级和 1 级各跑一个 `spin`（每个打 6 条输出、间隔约 400 ms）：

```
spin[3] 1/6 at 1680 ms      ← 0 级先跑
spin[3] 2/6 … 6/6           ← 1 级一条都插不进来
spin[3] done
spin[2] 1/6 at 3680 ms      ← 0 级一退出，1 级立刻补上
spin[2] 2/6 … 6/6
spin[2] done
```

把两个都放 0 级（或都 1 级），则是标准的 50/50 轮转，输出交错。

**唤醒抢占**单独验证：一个 0 级任务在 `tty` 上睡着，你在它睡觉时敲命令——
那行输入到达的当拍它就被叫醒并拿回 CPU，2 级的后台任务不用等自己这一片跑完。
子任务退出、sleep 到点同理。切换永远发生在 trap 收尾，内核调用不会被打断。

### 2.5 典型用法

```
tos> nice 2 bench &        # 后台长任务降到最低：shell 和前台永远先跑
tos> nice 1 fetch &        # 中间级：比垃圾任务重要，比交互不重要
tos> prio 2 0              # 需要它赶紧跑完时临时提级（0 级调用者才能提）
tos> prio 2 2              # 用完降回去
```

给程序内部用（不经过 shell）：

```c
#include "lib.h"
setprio(0, 2);             /* 我是后台任务，自降为最低 */
```

### 2.6 陷阱

- **饿死**：只要 0 级有一个永远 `rdy` 的任务，1/2 级就永远拿不到 CPU。这是
  特性。真被卡住了，`^C` 杀掉那个 0 级任务，或 `^Z` 停住它（`^Z` 是切换键：
  再按一次继续）——杀任务不需要特权。
- **shell 自降升不回**：在 shell 里 `prio 1 2` 把**自己**降到 2 级之后，本会话
  内谁也不能把它升回 0（升级被"不能高于调用者"挡住，而此时调用者就是 2 级的
  shell 自己）。重启（`reboot`）是唯一的恢复方式。所以别拿 pid 0 开玩笑。
- **`nice` 也会失败**：0 级的 shell 用 `nice` 永远成功；但一个已经被降到 2 级的
  shell 里 `nice 0 prog` 会被拒绝，报 `nice: cannot move that task to that level`
  ——和 `prio` 是同一条规则。
- **低级别不改变阻塞语义**：`sleep`/`wait`/读 tty 的任务不占 CPU（阻塞而非
  忙等），所以"低级别的任务在睡觉"完全不妨碍高级别任务全速跑。

---

## 3. 速查

```
source <file>              在本 shell 里跑命令文件（不占窗口，C3 首选）
run sh <file>              起新任务跑命令文件（隔离、可 &、有退出码）
run sh <file> &            后台跑脚本
nice <0-2> <prog> [&]      以指定级别启动
prio <pid> <0-2>           移动在跑的任务（只能向下或平级）
exit [N]                   结束脚本（返回 N&255）；控制台上才是关机
reboot                     真复位
```

| 规则 | 值 |
| --- | --- |
| 优先级 | 3 级，0 最高，继承父任务，不能升过调用者 |
| 时间片 | 50 ms（100 Hz tick × QUANTUM 5） |
| 调度 | 级间严格从高到低；级内轮转；唤醒即抢占（trap 收尾切换） |
| `source` 嵌套 | ≤ 4 层（每层约 110 B 栈） |
| 脚本行 / 文件名 | ≤ 256 B（截断会报错）/ ≤ 63 B |
| 启动脚本 | `files/etc/rc`，仅 pid 1 读一次，默认空 |
| 任务总数 | 16（`MAX_PROCS`）；C3 任务窗口只有 3 个 |
