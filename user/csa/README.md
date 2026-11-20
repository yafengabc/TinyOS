# csa

`csa` 是跑在 TinyOS 用户态的小型 C 子集编译器。它直接生成 AMX core
P-code（`.amx`），由 `pvm` 执行，不依赖 Pawn 的宿主编译器，也不依赖
`pvm` 的 float/printf 扩展。

```sh
csa /bin/demo.c /bin/demo.amx
pvm /bin/demo.amx
```

## 支持的语法

- 类型：`int`、`void`、`char`、`string`、`float`、`struct`，`typedef`（标量或结构体别名），`enum`。
- 全局变量、函数、递归；函数参数按值传递，结构体/数组参数隐式按引用传递。
- 局部变量必须在函数体开头声明；局部数组支持一维。
- 语句：`if/else`、`while`、`do-while`、C 风格 `for`、`for i in a..b`、
  `break`、`continue`、`switch/case/default`（支持贯穿）、`return`。
- 运算符：算术、比较、逻辑、位运算；复合赋值 `+= -= *= /= %= &= |= ^= <<= >>=`；
  `++`/`--` 前缀与语句级后缀。
- 输出：`print(x)`、`printf("...", ...)`。

## 浮点：源码 `float`，底层 Q16.16

`float` 不是 IEEE 浮点，而是 **Q16.16 定点**：一个 cell 存
`值 × 65536`，范围和精度固定为 ±32767、1/65536。

```c
float a;
float b;

a = 1.5;
b = a * 2.0 + 0.25;
printf("%.2f\n", b);       /* 3.25 */
```

- 小数字面量在编译期转成 Q16.16；
- `+`、`-` 用整数 `ADD/SUB`；
- `*`、`/` 调 `fix16_mul` / `fix16_div`，不启用 soft-float；
- `printf("%f")` / `printf("%.Nf")` 调 `printfix16`；
- 整数和 float 混用时，整数会转成 Q16.16；赋值/返回/传参也会做转换；
- 没有 NaN/Inf、denormal；`sqrt/sin` 需要定点实现或查表。

## 字符串

`csa` 使用自己的**字节字符串 ABI**：1 个字符 1 字节，连续存放并以 NUL
结尾；`string` 是 4 字节句柄（data 段字节地址），`char[N]` 是 N 字节的
可修改缓冲区。Pawn 的 `tos_print` 仍保留，`csa` 用 `tos_prints`。

```c
string s;
char buf[16];

s = "hello";
printf("%s %d\n", s, strlen(s));

strlcpy(buf, 16, s);       /* 第二个参数是字节容量 */
buf[0] = 'H';
strlcat(buf, 16, "!");
printf("%s %d\n", buf, strcmp(buf, "Hello!"));
```

- `string` 赋值是别名/借用，不复制字符；要修改或复制用 `char[]` + `strlcpy`。
- `char a[]` 作为函数参数时按引用传递，和 struct 一样，不写 `*`/`&`。
- 内置函数：`strlen`、`strcmp`、`strlcpy`、`strlcat`、`atoi`。
- `char` 和 `buf[i]` 是单字节 load/store；`string` 句柄仍是 4 字节 cell。
- 未初始化的全局 `string` 是空串；局部 `string` 先赋值再使用。
- 字符串内容是字节，不按 UTF-8 字符索引；容量按字节算。

## 硬件

`pvm` 的 GPIO/ADC/PWM natives 直接暴露给脚本：

```c
int n;
n = gpio_count();
printf("gpio=%d adc=%d pwm=%d\n", n, adc_count(), pwm_count());
if (n > 0) {
    gpio_mode(3, 1);        /* 1 = TOS_PIN_OUT */
    gpio_write(3, 1);
    printf("read=%d\n", gpio_read(3));
    gpio_write(3, 0);
}
printf("adc0=%d vref=%d bits=%d\n", adc_read(0), adc_vref_mv(), adc_bits());
pwm_set(3, 1000, 50);       /* GP3, 1 kHz, 50% */
pwm_off(3);
```

可用函数：`gpio_count`、`gpio_mode`、`gpio_write`、`gpio_read`、
`adc_count`、`adc_read`、`adc_vref_mv`、`adc_bits`、
`pwm_count`、`pwm_set`、`pwm_off`。它们由板级驱动实现；没有该外设的板子
通常返回 0 或 -1。

## 文件/进程基础

第一档系统调用已经暴露（都是 `pvm` 进程权限内的薄 wrapper）：

```c
int fd;
char buf[64];
int n;

fd = create("btest.txt");
write_all(fd, "hello\n", 6);
close(fd);

fd = open("btest.txt");
n = read(fd, buf, 63);
buf[n] = 0;
printf("read=%d [%s]\n", n, buf);
lseek(fd, 0, 0);
close(fd);

getcwd(buf, 64);
readdir_index(0, buf, 64);
printf("pid=%d up=%d\n", getpid(), uptime());
unlink("btest.txt");
```

可用：`open`、`create`、`read`、`write`、`write_all`、`close`、`lseek`、
`unlink`、`chdir`、`getcwd`、`readdir_index`、`readdir_path`、
`sleep`、`uptime`、`getpid`、`yield`、`dup`、`ioctl`。

- `read`/`getcwd`/`readdir_*` 的目标必须是 `char[]`；
- `write`/`write_all` 接受 `string` 或 `char[]`；
- `open`/`create`/`unlink`/`chdir` 接受字节字符串路径；
- 还没有暴露 `stat`/`dfinfo`/`meminfo`（结构体映射）、`exec`/`spawn`、网络、
  `reboot`/flash。

## 结构体与 UFCS

结构体是值类型；作为参数时传递地址，函数内可直接修改调用者的对象。
`a.f(x)` 会展开为 `f(a, x)`，其中 `f` 的第一个参数必须是 `struct`。

```c
struct Point { int x; int y; };

int sum(struct Point p) { return p.x + p.y; }
void shift(struct Point p, int d) { p.x += d; p.y += d; }

int main() {
    Point p;
    p.x = 1;
    p.y = 2;
    p.shift(10);
    printf("sum=%d\n", p.sum());
    return 0;
}
```

## printf

格式串必须是字符串字面量，编译期展开为 `tos_print` / `tos_putint` 调用：

- `%d`、`%i`、`%u`：整数
- `%s`：字符串字面量、`string` 变量、`char[]` 或返回 `string` 的函数
- `%f`、`%.Nf`：Q16.16 float（`%.2f` 保留两位）
- `%%`：百分号

`%f` 走 `printfix16`，和 `pvm` 的可选 float 扩展无关。

## 限制

- 没有指针和多维数组。
- `float` 是 Q16.16 定点，不是 IEEE；范围 ±32767、精度 1/65536。
- 局部变量不能在声明处初始化，需在语句中赋值；全局初始化只接受整数常量。
- `case` 标签必须位于该 `switch` 体的大括号顶层。
- 代码/数据上限为 `CODE_MAX`/`DATA_MAX`（默认 2048 cells / 4096 bytes），
  超出时编译报错而不是生成截断镜像。
- 输出镜像大小受 `out[8192]` 限制；大程序需要同时调整这两个上限。
- 字符串是字节串（1 字符 1 字节），不按 UTF-8 字符索引；`char[]` 容量按字节算。
- 字符串/`char[]` 只需要 `DATA_MAX`，大缓冲区比原来的 cell-per-char 省 4 倍。

## 资源

在 ESP32-C3 上，`csa` 的窗口约 100 KiB（`-Oz` + 流式写 `.amx`：span 82K + 16K 栈 + 4K guard）；`pvm`（整数 core + fix16 + 字节字符串 + 系统调用）约 28 KiB。
两者都作为普通用户程序运行，不进入内核，也不使用 XIP。
