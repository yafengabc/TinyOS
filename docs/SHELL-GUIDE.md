# Shell 使用指南：脚本与优先级

本文是**使用者视角**的说明：每个行为都对应现在的实现，示例输出都是实跑抓下来的
（x86 与 rv32 行为一致，下面不再分别标注）。设计动机与实现细节在
`docs/PLATFORM-DESIGN.md` §6，这里只讲"怎么用、边界在哪"。

```
tos> help
TinyOS shell commands:
  ls [dir|file]      list a directory, or show a file
  cd [dir]           change directory (default /)
  pwd                print the working directory
  cat <file>         print a file
  edit <file>        full screen editor (^S save, ^X exit)
  echo [args...]     print arguments (builtin)
  hello              demo program
  mem                heap / sbrk demo
  crash              run an illegal instruction (kills that process)
  uptime             real time since boot (hardware clock)
  sleep <ms>         wait without using the CPU (builtin in scripts)
  gpio <pin> [0|1]   read or drive a pin (gpio blink <pin>)
  adc [chan] [n]     sample the analog inputs
  smp                run one worker per CPU, in parallel
  reboot             reset the machine and boot again
  df                 filesystem usage
  dd of=f bs=n count=n  write n blocks of a test pattern; with
                     if=f instead of of= read back and verify
  rm <file>...       remove files
  free               RAM map and kernel heap usage
  ps                 list tasks
  cat /proc/ps       the same table as a file (also /proc/mem,
                     /proc/uptime) - `ps > f` writes it to f
  echo 1 > /dev/gpio4  pins and ADC channels are files: write to
                     drive, cat to read, `gpio dev` for ioctl
  cmd > file         send stdout to a file or a device
  A | B              send A's stdout to B's stdin
  run <prog> [args]  run a program from /bin
  run <prog> ... &   run it in the background
  run sh <file>      run a script as a task
  source <file>      run a script in this shell
  nice <n> <prog> .. run it at priority n (0 high, 2 low)
  prio <pid> <n>     move a running task to priority n
Script syntax (in a script file; see docs/SHELL-GUIDE.md):
  name=text          set a variable (also `name=$(prog args)`)
  incr name [n]      add n (default 1) to an integer variable
  $name  ${name}  $? text substitution; $(prog) captures stdout
  if <a> == <b>     also != -eq -ne -lt -gt -le -ge, -n a, -z a
  else / fi         optional else branch / end of if
  while <cond>      loop while the condition holds
  done / break      end of loop / leave the innermost loop
  true / false      set $? to 0 / 1
  help               this text
  exit [code]        shut TinyOS down (end the script, in a script)
```

命令分三类：**内建**（`help`、`cd`、`pwd`、`rm`、`ps`、`exit`、`reboot`、
`source`、`run`、`nice`、`prio`，以及脚本用的 `incr`、`true`、`false`）、
**脚本内内建 + 控制台/后台程序**（`echo`、`sleep`）和**程序**（`bin/` 下的每个
ELF：`ls`、`cat`、`uptime`……）。不是内建的名字会被当成 `bin/<名字>` 去
exec，所以 `hello alpha` 和 `run hello alpha` 等价。

**为什么 `sleep` 有两条路径**：脚本里每毫秒采样一次时，为一次 `sleep 1`
付一次 exec + ELF 重定位是不可接受的。但内建会停住 shell 自己，而内核**明确拒绝
向 init 投递信号**（`process_signal()` 对 `init_proc` 返回 -1，
`process_ctrl_target()` 也跳过它），所以 `source` 脚本里的内建 sleep 一旦很长，
`^C` 就再也收不回来了。因此条件很明确：**只在脚本里（`script_depth > 0`）、
且不带 `&` 时内建**；控制台上敲 `sleep 3000`、`sleep 500 &`、
`nice 2 sleep 500`、`run sleep 500` 全部仍是 `bin/sleep` 程序。控制台的前台
sleep 因此保住了 `^C`：shell 阻塞在 `wait()`，属于
`foreground_job()` 能找到的目标，实测发 0x03 后立刻 `[pid N] killed` 并回到提示符。
`echo` 也分两条路（不带 `&` 是内建），但它不阻塞，没有这层顾虑。

`sleep` 的单位是**毫秒**（内核 ABI、唤醒它的 tick、`samp` 的采样间隔都是毫秒），
上限 3600000 ms——这是内核夹断的那个值，两条路径都先拒绝，免得静默地少睡。

---

## 1. 脚本

脚本 = 命令文件 + 三样控制台没有的东西：**变量**、**命令替换**、**成块的控制语句**。
控制台也认变量和 `$(...)`（方便当场调试），只有 `if` / `while` / `else` / `fi` /
`done` / `break` 需要文件——`while` 必须能跳回开头，而控制台是一条不会回头的流；
把这些关键字敲在提示符里会得到 `sh: if/while/else/fi/done/break need a script`。

实现上没有新系统调用、没有 AST、没有 `malloc()`：整个脚本一次读进 4 KiB 静态池，
按行切开后用行号跳转；`source` 把文件追加到池尾、返回时把池指针退回去，所以循环里
反复 `source` 不会长胖。命令分发仍是控制台那个 `run_command()`。代价是 `bin/sh`
从 7.0 KiB 变成 17.7 KiB ELF（压缩后 14.5 KiB），`ps` 里的窗口仍是 **32 KiB**，
跑过脚本后的栈高水位 `0.6/8 → 1.6/8`。

### 1.1 变量与替换（控制台和脚本通用）

| 写法 | 做什么 |
| --- | --- |
| `name=text` | 赋值；右边是**已经展开过的整段文本**，可以有空格 |
| `$name` / `${name}` | 取值，未定义即空串 |
| `$?` | 上一条命令的退出码（exec 失败是 127） |
| `$(prog args)` | 捕获一条命令的 stdout，见 §1.2 |
| `incr name [n]` | 把变量当整数加 n（默认 1） |
| `true` / `false` | 只把 `$?` 置成 0 / 1 |

第一个词里出现 `=` 且左边是标识符才算赋值（`dd of=f bs=1` 不是）。变量名最长
11 B、值最长 95 B，一共 8 个变量。**没有引号、没有算术表达式、没有布尔运算**：
`incr` 是循环里唯一的算术。这是"简单脚本"，不是第二门语言。

```
tos> x=$(cat /proc/uptime)
tos> echo x-is-$x
x-is-0.004
tos> echo braces=${x}
braces=0.004
```

`$?` 在**展开时**取值：`echo status=$?` 先展开再执行 echo，所以拿到的是上一条命令
的状态，echo 自己随后把它置 0。同理 `false` 后面紧跟 `echo ... $?` 打印 1。

### 1.2 `$(...)`：把程序输出接进脚本

`$(prog args)` 是 shell 自己接的一根 pipe：fd 1 指向写端 → spawn `bin/prog` →
shell 关掉自己那份写端 → 把读端**读到 EOF** → 才 `wait(pid)`。先读完再 wait 不是
优化：内核 pipe 只有 2048 B，写满的生产者会阻塞，先 wait 的 shell 会和自己的孩子
死锁。读到的换行变空格、尾部空白去掉，结果直接拼进所在行。

```sh
name=$(echo tiny)        # echo / pwd 是内建，直接给文本，不 spawn
t=$(cat /proc/uptime)    # 程序输出经 pipe 回到脚本，t = 0.005
if $t != 0
    echo [script] capture-ok
fi
```

边界（都是刻意的）：

- `$(...)` 里只放**一条简单命令**：不支持 `|`、`>`、嵌套 `$(...)`；里面可以有 `$var`。
- 输出超过 255 B 打印 `sh: $(...) output truncated`，但会继续把管道抽干——截断的是
  脚本看到的文本，不是让生产者卡住。
- 捕获程序的退出码进 `$?`；`$(false)` 得到空串且 `$?=1`。
- 程序若从 stdin 读（如 `$(wc)`）会读控制台并阻塞；要统计文件内容请用 `A | B`。

`A | B` 仍在：两个程序经另一根 pipe 串起来（`echo abc def | wc` → `1 2 8`），
和 `$(...)` 的区别是消费者是程序还是脚本自己。

### 1.3 控制语句（只在脚本里）

```sh
if <a> == <b>
    ...
else                # 可省
    ...
fi

while <cond>
    ...
    break           # 可省，可以从 if 里跳
done
```

条件只有一个比较，没有 `&&` / `||` / 引号：

| 形式 | 含义 |
| --- | --- |
| `a == b` / `a != b` | 字符串比较（`strcmp`） |
| `a -eq/-ne/-lt/-gt/-le/-ge b` | 整数比较（`atoi`；`-gt 0` 看 `"1.234"` 时算 1） |
| `-n a` / `-z a` | 非空 / 为空 |
| `a`（单个词） | 非空且不是 `0` |

`while` 的条件**每轮重算**（`done` 跳回 `while` 那一行而不是只判一次），所以
`incr` 改过的变量能终止循环。块在打开时就先扫到匹配的 `fi` / `done`：条件为假
直接跳过去，假分支里的行既不展开也不执行——包括其中的 `$(...)`。缩进只是好看，
解析器跳前导空白。未定义变量展开成空串会让 `if $x == y` 变成 ` == y`，报
`sh: bad condition` 并当作假；判空请用 `-n` / `-z`。

块嵌套上限 8 层，循环最多回跳 **10000** 次，超了报 `sh: loop limit exceeded`
并结束脚本：脚本跑在 shell 自己身上，没有独立任务可供 `^C` 收拾，死循环得自己兜住。

### 1.4 内置命令与后台

脚本里高频的小命令不该每次付一次 exec + ELF 重定位。**只有脚本里的**
`sleep` / `echo` 是内建：

```sh
i=0
while $i -lt 3
    echo [script] loop $i
    incr i
    sleep 10          # 脚本内：内建，三次循环零个新任务
done
```

控制台上敲的 `sleep 3000`、`sleep 500 &`、`nice 2 sleep 500`、`run sleep 500`
仍走 `bin/sleep` 程序；`echo ... &` 同理。理由见开头：**内建会停住 shell 自己，
而 init 是不可投递信号的目标**，`source` 脚本里一句长 sleep 会让 `^C` 找不到任何
目标。所以脚本里需要"可被 ^C 打断的长等待"时写 `run sleep 3000`——它仍是普通
前台子任务；短采样循环才用内建。两条路径的输入校验和报错文字一致，
`tostest emutest` 的 sleep 检查覆盖的就是控制台那条。

### 1.5 两种跑法

| 写法 | 做什么 | 任务窗口 | 适合 |
| --- | --- | --- | --- |
| `source <file>` | 在**当前 shell 进程**里递归读文件，不 `exec` | 不占 | 嵌套脚本；C3 上跑任何东西 |
| `run sh <file>` | `exec` 一个新的 sh，文件名作参数 | 占 1 个 | 崩溃隔离、`&`、要退出码 |

选哪个的判断只有一条：**要不要多一个任务**。

- `run sh <file>` = `exec("bin/sh", {"sh", file})`。脚本跑飞只会带走那个 sh，
  shell 毫发无损；它也是一个普通任务，可以 `&` 进后台，退出码经 `wait()` 返回。
- `source <file>` 是在 shell 自己的身体里读文件。没有隔离——脚本里一句 `reboot`
  就是真重启，一个 crash 也一样。它换来的是**不占任务窗口**。

为什么"不占窗口"重要：**ESP32-C3 一共只有 3 个任务窗口**（shell、前台、备用）。
所以**在 C3 上写嵌套脚本一律用 `source`**。`run sh etc/demo.sh` 在只有 shell 的
干净系统上也实测跑通了，但脚本里再嵌一层程序就可能回
`exec: ... largest free block is 0 KiB`。

### 1.6 路径、参数与嵌套

- `<file>` 是**文件系统根的相对路径**，带不带前导 `/` 都行：`source etc/rc` 与
  `source /etc/rc` 等价（实测）。
- `run sh <file>` 里只有 `sh` 这个名字吃 `bin/` 前缀，文件名参数原样传给 sh。
- 脚本**不能带自己的参数**：`run sh a.sh foo` 里 `foo` 会被 sh 忽略（它只读
  `argv[1]`）。需要参数化的话，在脚本里 `name=...` 或用 `wfile` 生成内容。
- 文件名（含路径）上限 63 字节；命令行上限 256 字节。
- `source` 嵌套上限 **`MAX_SOURCE = 4`** 层（控制台算第 0 层），到顶报
  `sh: scripts nested too deep`。设上限的真正理由不是栈（实测跑完整个 demo.sh
  是 `1.6/8` KiB），而是**两个脚本互相 `source` 这种死循环**。
- `exit [N]` 在脚本里**结束本脚本**并返回 N（只保留低 8 位）；只有在控制台
  （不在任何脚本里）时它才是关机。`source` 的返回值就是 `$?`，实测
  `wfile exit.sh exit 7` + `source exit.sh` + `echo $?` → `7`，shell 还在。

### 1.7 启动脚本 `etc/rc`

pid 1 的 shell 在打出 banner、第一个提示符**之前**读一次 `files/etc/rc`
（随 `tos build` 打包进镜像，板上路径 `etc/rc`）。注意：

- **只有 init（pid 1）读它**。你后来 `run sh` 起的每个 shell 都不再读——否则
  每层 `run sh` 都会重放一遍启动命令。
- 发行版里它是**空的（纯注释）**，这是刻意的：开机自己说话的机器没法做差分
  测试，而这里打印的每个字节都会落在所有验收用例前面。想加启动动作，把
  `files/etc/rc` 里的示例行取消注释，或者写自己的，然后重新 `tos build`。

### 1.8 限制与错误报告

| 限制 | 值 |
| --- | --- |
| 脚本文本池（所有嵌套脚本合计） | 4096 B |
| 单个脚本行数（空行 / 注释不计） | 64 |
| 单行 / 展开后的命令行 | 256 B |
| 变量 | 8 个，名 11 B，值 95 B |
| 块嵌套 / while 回跳 | 8 层 / 10000 次 |

- `source nosuch` / `run sh nosuch` → `sh: cannot open nosuch`；
- 池满 / 行太多 / 行太长 → `sh: script text pool full` / `too many script lines` /
  `script line too long, truncated`（脚本不能重打，所以不静默截断）；
- 缺 `fi` / `done`、`else` / `break` 用错地方 → 点名报错并结束该脚本；
- 脚本里某个命令 exec 失败 → 由内核报具体原因（not found / corrupt / out of
  task windows），shell 不重复打印。

### 1.9 一个完整的例子

`files/etc/demo.sh` 随镜像打包，也是 `tostest emutest` 的回归夹具，覆盖变量、
`$(...)`、if/else、while/incr/break、内建 sleep、程序调用与两段管道：

```
tos> source etc/demo.sh
[script] name=tiny
[script] if-ok
[script] uptime=0.005
[script] capture-ok
hello from ring 3!
pid=3 argc=1 uptime=6 ms
  argv[0] = "hello"
1 2 8
[script] loop 0
[script] loop 1
[script] loop 2
[script] after-loop i=3
[script] break-at-2
[script] after-break j=2
[script] false-status=1
[script] done
tos>
```

调试短脚本不用重烧镜像——板上现写（`wfile <file> <内容>` 一次写一行）：

```
tos> wfile t.sh echo hello from a script
tos> source t.sh
hello from a script
```

---

## 2. 优先级

### 2.1 模型

三级，**0 最高**：`PRIO_HIGH 0` / `PRIO_NORMAL 1` / `PRIO_LOW 2`。
三条规则，全部是刻意的：

1. **继承**：子任务出生在父任务的级别上。shell 是 **1 级（NORMAL）**，所以从
   shell 启动的一切默认 1 级；0 级是留给实时任务的空位（早期版本让第一个任务
   出生在 0 级，结果 `nice 0` 提升不了任何东西——那一级本来就是默认级）。
2. **只能降，不能升过自己**：`setprio` 把目标级别调到**高于调用者自己的级别**
   时直接拒绝（不是 clamp）。没有 aging 的世界里，一个能把自己抬到 shell 之上的
   程序就锁死了整个控制台；不给学生这个选项，最坏也就是同级轮转——和没有
   优先级之前一样。**唯一的例外是 shell 自己**：它是你敲下的那条命令，可以
   `nice 0 prog` 把子任务放到实时级；普通程序提升不了自己，也提升不了子任务。
3. **级间严格，级内轮转**：`sched_pick()` 从高到低扫三级，**只要高一级有一个
   可运行的任务，低一级一个 tick 都拿不到**；同级的任务从上次运行的位置往后
   轮转。**没有 aging**：低优先级饿死是定义内的行为，不是 bug——你申请级别，
   就是在申请"别人永远让着我"。

时间片：1000 Hz tick，`QUANTUM = 1` 即 **1 ms 一片**，只在用户态帧上抢占
（syscall 执行到一半不会被换走）。这样 `sleep(1)` 就是 1 ms，采样循环能跟住
毫秒节奏；代价是 tick 本身占约 1.6%，1 ms 片再加约 1.6%（CPUBench 实测）。

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
  特性，但它是**可收回**的：`^C` 会按三级顺序找目标——先在读控制台的任务，其次
  某个阻塞父任务正在等的子任务，最后是级别高于 shell 的那个任务。第三级专为
  这种局面存在：`nice 0 spin &` 既不读控制台也不被谁等待，没有它就没有 `^C`
  打得中的目标。投递发生在 tick 里（M 态），所以**不需要 shell 能跑起来**。
  实测：前台 `run spin` / `nice 0 spin` / `sleep 3600000` 的 `^C` 都在 30 ms 内回来。
  `^Z` 走同一条路（停住，再按继续）。
- **睡眠中的后台任务收不回来**：第三级只挑 `rdy`/`run` 的，`nice 0 sleep 3600000 &`
  既不占 CPU 也不被等，`^C` 找不到它，只能等它醒（它也不会把 shell 锁死，因为
  睡着的任务不拿 CPU）。**前台的 `sleep` 不一样**，它走第二级——shell 正 `wait`
  着它，所以 `^C` 立刻生效（控制台的 `sleep` 因此始终是程序）。
- **脚本里的内建 `sleep` 收不回来**：`source` 脚本运行时"当前任务"就是 init 本身，
  `process_signal()` 拒绝向 init 投递信号，`^C` 的三级目标也跳过它。短采样 sleep
  无所谓；脚本里需要可中断的长等待就写 `run sleep N`，它变回普通前台子任务。
- **自降升不回的是程序，不是 shell**：一个程序把自己降到 2 级之后升不回去
  （越过不了自己的级别）。shell 是这条规则的唯一例外——它可以把任何任务提到
  任何级别，包括把自降后的自己提回来。所以 `prio 1 2` 之后
  `prio 1 <自己>` 就能恢复，不必重启。
- **`nice` 的失败信息**：只有非 shell 的调用者才会收到
  `nice: cannot move that task to that level`（它想越过自己的级别）。
  `nice 9 hello` 是参数错误，另一条 usage。
- **低级别不改变阻塞语义**：`sleep`/`wait`/读 tty 的任务不占 CPU（阻塞而非
  忙等），所以"低级别的任务在睡觉"完全不妨碍高级别任务全速跑。

---

## 3. 速查

```
source <file>              在本 shell 里跑脚本（不占窗口，C3 首选）
run sh <file>              起新任务跑脚本（隔离、可 &、有退出码）
run sh <file> &            后台跑脚本
name=text                  变量；$name / ${name} / $? 取值
name=$(prog args)          用管道捕获程序 stdout
incr name [n]              整数加 n（默认 1）
if <cond> / else / fi      条件；== != -eq -ne -lt -gt -le -ge -n -z
while <cond> / done        循环；break 跳出最内层
true / false               把 $? 置 0 / 1
A | B                      两个程序之间的管道
nice <0-2> <prog> [&]      以指定级别启动
prio <pid> <0-2>           移动在跑的任务（只能向下或平级）
exit [N]                   结束脚本（返回 N&255）；控制台上才是关机
reboot                     真复位
```

| 规则 | 值 |
| --- | --- |
| 优先级 | 3 级，0 最高，继承父任务，不能升过调用者 |
| 时间片 | 1 ms（1000 Hz tick × QUANTUM 1） |
| 调度 | 级间严格从高到低；级内轮转；唤醒即抢占（trap 收尾切换） |
| `source` 嵌套 | ≤ 4 层（跑完 demo.sh 的栈高水位 1.6/8 KiB） |
| 脚本池 / 行数 / 行宽 | 4096 B（嵌套合计）/ 64 行 / 256 B |
| 变量 / 块 / 循环 | 8 个（值 95 B）/ 8 层 / 10000 次回跳 |
| 脚本文件名 | ≤ 63 B |
| 脚本示例 / 回归夹具 | `files/etc/demo.sh`，`tostest emutest` 的 `script` 检查 |
| 启动脚本 | `files/etc/rc`，仅 pid 1 读一次，默认空 |
| 任务总数 | 16（`MAX_PROCS`）；C3 任务窗口只有 3 个 |
