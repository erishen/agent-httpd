// 榨性能案例：在缓冲区中查找/统计 "\r\n"（HTTP header 行结束符）
//
// 这是 agent-httpd 做 HTTP 解析时真实要干的事：扫描请求/响应头，找 CRLF。
// 演示两种写法：
//   1) count_crlf_scalar  —— 朴素逐字节标量循环（编译器在 -O2 下可能自动向量化）
//   2) count_crlf_neon    —— 手写 NEON intrinsics：一次读 16 字节，用 vmaxvq 判断
//                            这一块里有没有 '\r'，没有就整块跳过，有再在块内标量精查
//
// 你的 Mac 是 Apple Silicon(ARM64)，所以用 ARM 的 NEON，不是 x86 的 SSE。
// 想看编译器到底生成了什么 NEON 指令，跑：
//   clang -S -O2 -fno-vectorize crlf_scan.c -o crlf_scan.s
// 然后搜 cmeq / umaxv / ldr q 即可。

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <arm_neon.h>

// 朴素标量版：每次比较当前字节和下一个字节
static long count_crlf_scalar(const char *buf, size_t len) {
    long c = 0;
    for (size_t i = 0; i + 1 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n') c++;
    }
    return c;
}

// NEON 向量化版：16 字节一块，先判断块内有没有 '\r'
static long count_crlf_neon(const char *buf, size_t len) {
    long c = 0;
    size_t i = 0;
    for (; i + 16 <= len; i += 16) {
        uint8x16_t chunk = vld1q_u8((const uint8_t *)buf + i);
        uint8x16_t cr    = vdupq_n_u8('\r');
        uint8x16_t eq    = vceqq_u8(chunk, cr);   // 每块字节 == '\r' 则对应字节 = 0xFF
        if (vmaxvq_u8(eq) != 0) {                 // 块内任一字节是 '\r' 时 max == 0xFF
            for (size_t j = 0; j + 1 < 16; j++) {
                if (buf[i + j] == '\r' && buf[i + j + 1] == '\n') c++;
            }
        }
    }
    // 尾部不足 16 字节，标量收尾
    for (; i + 1 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n') c++;
    }
    return c;
}

#define BUF_SIZE   (32 * 1024)
#ifndef N_ITERS
#define N_ITERS    200000
#endif

// 关键：Apple Silicon 上向量加载/存储(ldr q / str q)要求 16 字节对齐，否则直接
// SIGBUS。用栈上局部数组 + aligned(16)（ABI 保证栈 16 对齐），比全局/动态分配稳。

int main(void) {
    // 32KB 栈缓冲 + 显式 16 字节对齐：向量加载/存储 ldr q / str q 才安全
    char g_buf[BUF_SIZE] __attribute__((aligned(16)));
    // 构造稀疏数据：每行 64 字节文本 + "\r\n"，'\r' 密度 ≈ 1/66 ≈ 1.5%
    memset(g_buf, 'A', sizeof(g_buf));
    for (size_t i = 0; i + 2 < BUF_SIZE; i += 66) {
        g_buf[i + 64] = '\r';
        g_buf[i + 65] = '\n';
    }

    // 1) 正确性校验：两版结果必须一致
    long s = count_crlf_scalar(g_buf, BUF_SIZE);
    long n = count_crlf_neon(g_buf, BUF_SIZE);
    printf("scalar CRLF count = %ld, neon CRLF count = %ld -> %s\n",
           s, n, s == n ? "MATCH" : "MISMATCH");

    // 2) 性能对比（关掉自动向量化以公平对照：标量保持朴素，NEON 靠 intrinsics 向量化）
    long acc = 0;
    clock_t t0 = clock();
    for (int it = 0; it < N_ITERS; it++) acc += count_crlf_scalar(g_buf, BUF_SIZE);
    clock_t t1 = clock();
    double ds = (double)(t1 - t0) / CLOCKS_PER_SEC;

    long acc2 = 0;
    t0 = clock();
    for (int it = 0; it < N_ITERS; it++) acc2 += count_crlf_neon(g_buf, BUF_SIZE);
    t1 = clock();
    double dn = (double)(t1 - t0) / CLOCKS_PER_SEC;

    printf("scalar: %.3f s (acc=%ld)\n", ds, acc);
    printf("neon  : %.3f s (acc=%ld)\n", dn, acc2);
    if (dn > 0) printf("speedup: %.2fx\n", ds / dn);
    return 0;
}
