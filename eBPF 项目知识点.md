### 一、判断机器是否能使用 eBPF

执行以下命令：

	uname -r  # 查看内核版本， Ubuntu 26.04 默认 7.0.x
	
	# 判断内核是否支持 BTF（内核有没有"数据结构说明书"），也就是也就是编译时是否开了 `CONFIG_DEBUG_INFO_BTF=y`
	ls -lh /sys/kernel/btf/vmlinux  
	
	grep -E 'CONFIG_(BPF|BPF_SYSCALL|DEBUG_INFO_BTF)=' /boot/config-$(uname -r)

**`/boot/config-*` 是什么**：内核在编译时有几千个开关（`CONFIG_XXX`），决定"这个功能要不要编进去"、编成内置（`=y`）还是可加载模块（`=m`）。Ubuntu 把这套开关的记录留在 `/boot/config-<版本>` 这个文本文件里，方便你查。它是**只读记录**，不是配置文件，改它没用。

| 开关 | 管什么|
|---|---|
|`CONFIG_BPF=y`|eBPF 核心框架本身|
|`CONFIG_BPF_SYSCALL=y`|允许用户程序通过 `bpf()` 系统调用把程序塞进内核|
|`CONFIG_DEBUG_INFO_BTF=y`|编译时生成上面那份 BTF 说明书，即 `/sys/kernel/btf/vmlinux`|


### 二、BTF是什么

BTF（BPF Type Format）是 eBPF 生态里的一种类型信息格式，一种紧凑的二进制格式，用于描述**内核或者 BPF 程序的数据结构**的类型信息；它规定每个结构体字段在什么偏移位置、是什么类型。

BPF 只想知道一件事：

> `struct task_struct` 有多少字节？`comm` 在第几个字节？它是什么类型？

BTF 就是为这个问题定制的格式：**只存类型，不存代码和行号**，体积比 DWARF 小一个数量级。名字里那个 B 是 BPF，因为它是 2018 年为 BPF 程序设计的；后来发现描述内核类型通用，就扩展成了内核的通用类型表（`/sys/kernel/btf/vmlinux` 就是这么来的）。

在编译出的 BPF 目标文件中，它位于 ELF 的 `.BTF` 段；内核自身则在 `/sys/kernel/btf/vmlinux` 暴露出一份完整的内核类型信息。

常用工具：

	# 安装 bpftool
	sudo apt install bpftool        
	
	# 把当前运行的这个内核所携带的类型信息（BTF），翻译成 C 语言写法的类型声明
	bpftool btf dump file /sys/kernel/btf/vmlinux format c > vmlinux.h 


### 三、一个 eBPF 项目的完整流程

#### 1、编写「内核态 eBPF 程序源码」hello.bpf.c

`````C
// hello.bpf.c —— 第一个内核态 eBPF 程序
//
// 作用：每当有进程被启动（execve 系统调用），就把这个进程的名字打印到内核的 trace_pipe 里。

#include "vmlinux.h"
#include <bpf/bpf_helpers.h> // bpf_printk / SEC / bpf_get_current_comm 都在这里

// 许可证。写 "GPL" 是惯例，部分内核 helper 只对 GPL 程序开放。
char LICENSE[] SEC("license") = "GPL";

  

// SEC("...") 里的字符串就是"挂载点声明"：
// 格式 = 类型 / 类别 / 名字
// libbpf 加载时会解析这个字符串，决定把程序挂到哪。
// 这里 = 跟踪点(tracepoint)，系统调用表里的 sys_enter_execve，
// 也就是"一个进程通过 execve 启动的那一刻"。
SEC("tracepoint/syscalls/sys_enter_execve")
int hello_execve(void *ctx)
{
	char comm[16] = {};
	
	// 内核态里没有 printf、没有 malloc、不能调 libc。
	// 能用的只有内核提供的一批函数，叫 helper。
	// bpf_get_current_comm = 取"当前进程的名字"，写进 comm 这个缓冲区。
	bpf_get_current_comm(&comm, sizeof(comm));
	
	// 把结果写到 /sys/kernel/debug/tracing/trace_pipe。
	// 注意：最多 3 个参数，格式串也有长度限制。
	// 这是调试用的土办法，正式项目会用 ringbuf。
	bpf_printk("hello: %s\n", comm);
	
	// 返回 0 表示放行。tracepoint 程序不改返回值也无所谓，但必须 return。
	return 0;
}
`````

#### 2、下载工具

	sudo apt install -y clang llvm libbpf-dev

| 工具         | 作用                                                               |
| ---------- | ---------------------------------------------------------------- |
| clang      |                                                                  |
| libbpf-dev | 提供 `<bpf/bpf_helpers.h>`，没有它会报 `bpf/bpf_helpers.h: No such file` |

#### 3、获取 vmlinux.h

因为需要用到内核数据类型

	# 安装 bpftool
	sudo apt install bpftool        
	
	# 把当前运行的这个内核所携带的类型信息（BTF），翻译成 C 语言写法的类型声明
	bpftool btf dump file /sys/kernel/btf/vmlinux format c > vmlinux.h 

#### 4、编译「内核态 eBPF 程序源码」hello.bpf.c

	clang -O2 -g -target bpf -c hello.bpf.c -o hello.bpf.o

把 C 翻译成「内核里的 BPF 虚拟机」能执行的指令，并顺手把挂载点、许可证、调试信息打进 ELF。

和普通编译有 4 个关键差别：

**1）目标不是你的 CPU**。`-target bpf` 让 clang 生成的是 **BPF 虚拟 ISA（Instruction Set Architecture）** 的指令——一套只有 11 个寄存器（r0–r10）的抽象指令集。它和真实 CPU 指令没有对应关系。

**2）产物不能执行。** `hello.bpf.o` 是 ELF 文件，但没有 `main`、没有入口点，`./hello.bpf.o` 只会报 exec format error。唯一能"运行"它的东西是内核的 `bpf()` 系统调用。

**3）变成机器码是内核干的。** 加载时内核先跑 **verifier** 逐条检查，通过后才由 **JIT** 翻译成真实 CPU 指令挂上去执行。所以：

```
hello.bpf.c  --[clang -target bpf]-->  hello.bpf.o (字节码+元数据)  --[loader+bpf()]-->  verifier -> JIT -> 机器码
```

**4） 编译还顺带打包了三个"加载必需"的东西**，少了任何一个都加载不了：

- `SEC("tracepoint/...")` → 变成 ELF 的 section 名，loader 靠这个字符串知道挂到哪
- `SEC("license") = "GPL"` → 变成 license 段，内核靠它决定能不能用 GPL 专属 helper
- `-g` → 生成 BTF 调试信息，verifier 报错定位和 map 类型都靠它

#### 5、把 `.bpf.o` 变成 skeleton 头文件

	bpftool gen skeleton hello.bpf.o > hello.skel.h

**为什么需要这一步**：`.o` 是二进制，C 代码没法 `#include` 它。skeleton 干两件事——把字节码包装成一个大 C 数组塞进 `hello.skel.h`，再生成 `hello_bpf__open_and_load()` / `hello_bpf__attach()` / `hello_bpf__destroy()` 这几个函数（**函数名由传给 bpftool 的 .o 文件名（basename）自动推导**）。这样 loader 只要包含这个头文件，就能把程序加载进内核。

`hello.skel.h` 本质就是「一个字节数组 + 几个包装函数」。

#### 6、编写用户态 loader : `hello.c`

```C
// hello.c —— 用户态 loader
// 职责：把 hello.bpf.c 编译出来的字节码送进内核，挂到挂载点上，然后守着。
//
// 三步走（对应 bpf() 系统调用背后的三段流程）：
// 1. open —— 读 hello.bpf.o，解析 SEC() 名、重定位
// 2. load —— 字节码进内核，校验器（verifier）逐条检查，不过就拒绝加载
// 3. attach—— 挂到 tracepoint/syscalls/sys_enter_execve，事件发生才触发
//
// skeleton 模式：bpftool gen skeleton 已把 hello.bpf.o 打包进 hello.skel.h，
// 所以这里不需要手动 fopen 那个 .o 文件。

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <bpf/libbpf.h>
#include "hello.skel.h"

static volatile sig_atomic_t exiting = 0;

static void on_signal(int sig)
{
	(void)sig;
	exiting = 1;
}

// 只压掉 DEBUG 级（太吵），警告和错误照常输出。
// 加载失败时你能看到校验器的完整报错，这是排查 bpf.c 问题的主要信息源。
static int quiet_libbpf(enum libbpf_print_level level, const char *fmt, va_list args)
{
	if (level == LIBBPF_DEBUG)	
		return 0;
	return vfprintf(stderr, fmt, args);
}

int main(void)
{
	struct hello_bpf *skel;
	int err = 0;
	
	libbpf_set_print(quiet_libbpf);
	
	// Ctrl-C / kill 时优雅退出，而不是让内核里的挂载点悬空
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	
	// 1 + 2：打开并加载。失败时校验器会往 stderr 打原因。
	skel = hello_bpf__open_and_load();
	if (!skel) {
		fprintf(stderr, "加载失败: %s\n", strerror(errno));
		return 1;
	}
	
	// 3：按 SEC("tracepoint/syscalls/sys_enter_execve") 自动挂载
	err = hello_bpf__attach(skel);
	if (err) {
		fprintf(stderr, "挂载失败: %s\n", strerror(-err));
		goto cleanup;
	}
	
	printf("已挂载 sys_enter_execve，正在监听...（Ctrl-C 退出）\n");
	printf("另开一个终端看输出：\n");
	printf(" sudo cat /sys/kernel/debug/tracing/trace_pipe\n");
	fflush(stdout);
	
	// 停住不退出：进程一旦退出，attach 的 link 就被释放，程序也就摘下来了
	while (!exiting)
		sleep(1);
		
cleanup:
	// 卸载程序、撤销挂载、释放 map，一个调用收尾
	hello_bpf__destroy(skel);
	return err ? 1 : 0;
}
```

#### 7、把 `hello.c` 编译链接成可执行文件 `hello`

	cc -O2 -g -Wall hello.c -o hello -lbpf -lelf -lz

