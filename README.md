# hello-ebpf

一个最小可用的 eBPF 程序：**每当有进程启动（`execve` 系统调用），就把它打印到内核 trace_pipe 里。**

只有三个源文件、一个 Makefile，没有任何框架和脚手架，适合用来理解 eBPF 的三段式结构（内核态程序 → 字节码 → 用户态加载器），也适合当作新实验的起点。

```
$ sudo cat /sys/kernel/debug/tracing/trace_pipe
     ls-12345   [002] d... 1234.567890: bpf_trace_printk: hello: ls
   bash-12346   [001] d... 1234.568901: bpf_trace_printk: hello: bash
   node-12350   [003] d... 1234.570122: bpf_trace_printk: hello: node
```

---

## 目录结构

| 文件            | 作用                                                        |
| --------------- | ----------------------------------------------------------- |
| `hello.bpf.c`   | **内核态**程序。挂到 `sys_enter_execve` 跟踪点，打印进程名。 |
| `hello.c`       | **用户态**加载器。打开/加载/挂载字节码，然后守着进程不退出。 |
| `Makefile`      | 把上面两步串起来的构建规则。                                 |
| `vmlinux.h`     | 从当前内核 BTF 导出、构建时自动生成，**不入库**。            |
| `build/`        | 构建产物（`.bpf.o`、`.skel.h`、可执行文件），**不入库**。     |

## 工作原理

```
  hello.bpf.c
      │  clang -O2 -g -target bpf -c
      ▼
  build/hello.bpf.o
      │  bpftool gen skeleton
      ▼
  build/hello.skel.h ──────┐
                           │  #include
  hello.c ─────────────────┘
      │  cc -Ibuild ... -lbpf -lelf -lz
      ▼
  build/hello
      │  bpf() 系统调用：open → load → attach
      ▼
  内核 verifier 逐条校验字节码
      │  校验通过才允许加载
      ▼
  挂到 tracepoint/syscalls/sys_enter_execve
      │  每次 execve 触发
      ▼
  bpf_printk → trace_pipe
```

三个关键概念，对应代码里的三处注释：

1. **`SEC("...")` 决定挂载点**：字符串格式是 `类型/类别/名字`。`tracepoint/syscalls/sys_enter_execve` 表示「进程 execve 的那一刻」。换一行 `SEC()` 就是另一个程序。
2. **内核态没有 libc**：不能 `printf`、不能 `malloc`，只能用内核暴露的 helper（这里用了 `bpf_get_current_comm` 取进程名）。
3. **用户态只做三件事**：`open` → `load` → `attach`，之后 `sleep` 住。进程一退出，attach 的 link 就被释放，程序自动摘下——所以 loader 不能是「跑完就退」的一次性脚本。

## 环境要求

**Linux only。** eBPF 是内核特性，macOS / Windows 上无法编译运行（见文末「在 macOS 上开发」）。

| 依赖        | 要求                                                          |
| ----------- | ------------------------------------------------------------- |
| 内核        | 建议 ≥ 5.8（BTF + tracepoint link 支持；`/sys/kernel/btf/vmlinux` 必须存在） |
| clang / llvm| ≥ 10，需要支持 `-target bpf`                                  |
| libbpf      | ≥ 0.6（`gen skeleton` 与 `*__open_and_load`）                  |
| bpftool     | 任意近期版本                                                  |
| libelf / zlib | 链接 `-lelf -lz` 需要                                     |
| 权限        | root（或 `CAP_BPF` + `CAP_PERFMON`）                          |

安装：

```bash
# Ubuntu / Debian
sudo apt install clang llvm libbpf-dev libelf-dev zlib1g-dev bpftool

# Fedora
sudo dnf install clang llvm libbpf-devel elfutils-libelf-devel zlib-devel bpftool

# Arch
sudo pacman -S clang llvm libbpf bpftool
```

确认内核有 BTF（这条命令没报错就说明可以）：

```bash
ls /sys/kernel/btf/vmlinux
```

## 快速开始

```bash
make            # 构建出 build/hello
sudo ./build/hello
```

然后**另开一个终端**看输出：

```bash
sudo cat /sys/kernel/debug/tracing/trace_pipe
```

在任意地方执行几条命令（`ls`、`bash -c true`…），trace_pipe 里就会出现 `hello: ls` 这样的行。`Ctrl-C` 结束 loader。

也可以一步到位：

```bash
make run        # 等价于 make && sudo ./build/hello
make clean      # 删掉 build/
```

> trace_pipe 的另一个路径是 `/sys/kernel/tracing/trace_pipe`（没挂 debugfs 时用）。如果两个都不存在：
> `sudo mount -t debugfs none /sys/kernel/debug`。

## Makefile 说明

| 命令            | 实际做了什么                                                |
| --------------- | ----------------------------------------------------------- |
| `make`          | 依次执行下面 4 步，产出 `build/hello`                       |
| `make run`      | `make` 之后 `sudo` 跑 loader                                |
| `make clean`    | 删除 `build/`                                               |
| `make vmlinux.h`| 单独重新从内核 BTF 导出结构体定义（换内核后要重跑）          |

四步构建链：

1. **`vmlinux.h`** ← `bpftool btf dump file /sys/kernel/btf/vmlinux format c`。把当前内核的结构体定义导出成本地头文件，这是 CO-RE 的基础，**换内核必须重建**，所以它不纳入版本管理。
2. **`hello.bpf.o`** ← `clang -O2 -g -target bpf -c`。注意目标是 **BPF 虚拟机**，不是 CPU；并且**不链接**。`-g` 不能省，BTF 信息靠它。
3. **`hello.skel.h`** ← `bpftool gen skeleton`。把字节码转成 C 数组，并生成 `hello_bpf__open_and_load()` / `__attach()` / `__destroy()` 这些包装函数。有了它，`hello.c` 就不用自己 `fopen` 那个 `.o`、不用手写 `bpf_object__open_file`。
4. **`build/hello`** ← `cc` 编译链接。这一步是本机 CPU 的普通程序，跟第 2 步的交叉目标不同。

Makefile 里几个刻意的选择：

- 工具链变量用 `?=`，所以可以临时换版本：`make CLANG=/opt/llvm/bin/clang`
- `ARCH` 由 `uname -m` 推导（`x86_64→x86`、`aarch64→arm64`），喂给 `-D__TARGET_ARCH_$(ARCH)`，跨架构才不会编译失败
- `LDLIBS` 写死 `-lbpf -lelf -lz`，比依赖 `pkg-config` 稳
- `.DELETE_ON_ERROR`：生成 `skel.h` 中途失败时删掉半截文件，避免下次 `make` 误以为它是最新的

## 排查

| 现象                                                | 原因与处理                                                                                     |
| --------------------------------------------------- | ---------------------------------------------------------------------------------------------- |
| `加载失败: Operation not permitted`                  | 没用 root，或容器没给 `--privileged` / `CAP_BPF`                                                |
| `加载失败` 且 stderr 有 verifier 报错                | 校验器拒绝了字节码。loader 只压掉了 `DEBUG` 级日志，verifier 的完整原因照常输出，按行号查 `hello.bpf.c` |
| `make` 报 `No such file or directory: /sys/kernel/btf/vmlinux` | 内核没开 BTF。换 >= 5.8 的发行版内核，或手工提供 `vmlinux.h`（见下）                    |
| `make` 报 `fatal error: 'vmlinux.h' file not found` | 先单独跑 `make vmlinux.h`，确认 `bpftool` 在 `PATH` 里；若残留了空的 `vmlinux.h`，删掉重来      |
| trace_pipe 一片空白                                 | ① 没跑过任何新进程 ② debugfs 没挂载 ③ 看错了路径（`tracing` 在 `debug` 下或直接在 `/sys/kernel/` 下） |
| `error: unknown target triple 'bpf'` / `unrecognized command-line option '-target'` | 用的是 Apple clang 或 gcc。必须是 Linux 上的 LLVM clang                        |
| `undefined reference to bpf_object__...`            | 缺 `libbpf-dev`，或链接顺序里 `-lbpf` 没放在源文件后面                                          |

没有内核 BTF 时的退路：把 `hello.bpf.c` 里的 `#include "vmlinux.h"` 换成

```c
#include <linux/bpf.h>
#include <linux/ptrace.h>
```

并在编译时加 `-I/usr/include/$(uname -m)-linux-gnu`。

## 局限与下一步

这份代码是**教学用**的，刻意简单：

- `bpf_printk` 走的是 trace_pipe，所有程序共享一条管道，最多 3 个参数，高频调用会拖慢系统。生产代码应该用 **ringbuf**（`BPF_MAP_TYPE_RINGBUF` + `bpf_ringbuf_output`），在 loader 里用 `ring_buffer__poll` 收。
- 没做任何过滤，系统里**所有** execve 都会打印。可以加一个 map（`BPF_MAP_TYPE_HASH`），只在 loader 里把关心的进程名或 UID 塞进去，内核态先查表再打印——这是「用户态配置、内核态执行」的标准姿势。
- 只有打印，没有统计。加一个 `BPF_MAP_TYPE_ARRAY` 或 per-CPU counter，按进程名累加 execve 次数，就是最小可用的审计工具。
- 挂载点是 syscall tracepoint，会漏掉内核线程的创建。换成 `fentry/do_execveat_common`（需要 BTF + `bpf_trampoline`）覆盖更全。

改完这些，项目就从「hello world」变成「能用的工具」了。

## 在 macOS 上开发

本仓库在 macOS 上只能改代码，**不能 make**：没有 `/sys/kernel/btf/vmlinux`，没有 `bpftool`，Apple clang 也不支持 `-target bpf`。需要在 Linux 环境里构建：

- **Lima / UTM 虚拟机**（推荐，内核完整）：`limactl start --vm-type=vz template://ubuntu`，然后把目录挂进去
- **Docker Desktop / OrbStack 的 Linux VM**：`docker run --privileged --pid=host -it -v "$PWD":/src ubuntu bash`，注意需要挂载 `/sys/kernel/debug` 才能读 trace_pipe
- 真机 / 云主机上的 Linux 最省事

## 许可证

`hello.bpf.c` 里声明的是 `GPL`。这是 eBPF 的惯例：一部分 helper 和 kfunc 只对 GPL 程序开放，`char LICENSE[] SEC("license") = "GPL";` 这行不能删。
