// hello.bpf.c —— 第一个内核态 eBPF 程序
//
// 作用：每当有进程被启动（execve 系统调用），就把这个进程的名字打印到内核的 trace_pipe 里。

#include "vmlinux.h"
#include <bpf/bpf_helpers.h>   // bpf_printk / SEC / bpf_get_current_comm 都在这里

// 许可证。写 "GPL" 是惯例，部分内核 helper 只对 GPL 程序开放。
char LICENSE[] SEC("license") = "GPL";

// SEC("...") 里的字符串就是"挂载点声明"：
//   格式 = 类型 / 类别 / 名字
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
