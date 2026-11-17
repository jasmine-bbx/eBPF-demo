// hello.c —— 用户态 loader
//
// 职责：把 hello.bpf.c 编译出来的字节码送进内核，挂到挂载点上，然后守着。
//
// 三步走（对应 bpf() 系统调用背后的三段流程）：
//   1. open  —— 读 hello.bpf.o，解析 SEC() 名、重定位
//   2. load  —— 字节码进内核，校验器（verifier）逐条检查，不过就拒绝加载
//   3. attach—— 挂到 tracepoint/syscalls/sys_enter_execve，事件发生才触发
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
	printf("  sudo cat /sys/kernel/debug/tracing/trace_pipe\n");
	fflush(stdout);

	// 停住不退出：进程一旦退出，attach 的 link 就被释放，程序也就摘下来了
	while (!exiting)
		sleep(1);

cleanup:
	// 卸载程序、撤销挂载、释放 map，一个调用收尾
	hello_bpf__destroy(skel);
	return err ? 1 : 0;
}
