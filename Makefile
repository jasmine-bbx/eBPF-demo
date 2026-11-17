# 一个最小 eBPF 项目的构建文件
#
# 对外只有三个命令：
#   make        构建出 build/hello
#   make run    用 sudo 跑起来
#   make clean  全部清掉

# ?= 的意思是「如果这个变量还没定义过，就赋这个值」，为什么要这样？因为命令行可以覆盖：make CLANG=/opt/llvm/bin/clang
CLANG     ?= clang
CC        ?= cc
BPFTOOL   ?= bpftool

# := 叫立即展开——定义的时候就把右边算成最终值。对比 = 是延迟展开——每次用到才展开。
OUTPUT    := build

# $(shell ...) 表示「执行这条 shell 命令，把输出当成字符串」。
# x86_64 -> x86, aarch64 -> arm64
ARCH      := $(shell uname -m | sed -e 's/x86_64/x86/' -e 's/aarch64/arm64/')

# -I$(OUTPUT)：hello.c 里 #include "hello.skel.h"，那个头文件在 build/ 下
CFLAGS    := -O2 -g -Wall -I$(OUTPUT)
# libbpf 的静态/内部依赖，写死比依赖 pkg-config 稳
LDLIBS    := -lbpf -lelf -lz

.PHONY: all run clean

all: $(OUTPUT)/hello

# 0) 从当前内核的 BTF 导出内核结构体定义。换内核就得重建。
#    这一步不需要 hello.bpf.c 里用到它，但留着规则省得以后手工敲。
vmlinux.h:
	$(BPFTOOL) btf dump file /sys/kernel/btf/vmlinux format c > $@

# 1) 内核态 C -> BPF 字节码（只有 -c，不链接；目标是 BPF 虚拟机不是 CPU）
$(OUTPUT)/hello.bpf.o: hello.bpf.c vmlinux.h | $(OUTPUT)
	$(CLANG) -O2 -g -target bpf -D__TARGET_ARCH_$(ARCH) -c $< -o $@

# 2) 字节码 -> skeleton 头文件（字节码变成 C 数组 + 几个包装函数）
$(OUTPUT)/hello.skel.h: $(OUTPUT)/hello.bpf.o
	$(BPFTOOL) gen skeleton $< > $@

# 3) 用户态 loader 链接成可执行文件（这里是本机 CPU，不是 BPF）
$(OUTPUT)/hello: hello.c $(OUTPUT)/hello.skel.h | $(OUTPUT)
	$(CC) $(CFLAGS) hello.c -o $@ $(LDLIBS)

$(OUTPUT):
	mkdir -p $@

run: all
	sudo ./$(OUTPUT)/hello

clean:
	rm -rf $(OUTPUT)

# 生成 skel.h 中途失败时，别留下半截文件让下一次 make 误以为它是最新的
.DELETE_ON_ERROR:
