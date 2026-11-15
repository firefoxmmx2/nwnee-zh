/*
 * nwzh.c — NWN:EE (Linux x86-64, build 8193.37-17) 中文渲染补丁（LD_PRELOAD 版）
 *
 * 全部只改进程内存，不写磁盘上任何游戏文件；不设 LD_PRELOAD 即完全恢复。
 *
 * ── 两组补丁 ─────────────────────────────────────────────────────────────
 *
 *  G1  Nuklear 图集字形范围（第一阶段已实测有效）
 *      0xe06961   e8 aa d4 fd ff  call nk_font_default_glyph_ranges
 *              → e8 ba d4 fd ff  call nk_font_chinese_glyph_ranges
 *      只改 1 个字节。实测：中文字体图集 512x128 → 1024x8192。
 *
 *  G2  Aurora TTF 图集字形容量 256 → 21248（第二阶段，UTF-8 直算索引）
 *
 *      A1  0x1124b96  48 89 10           mov [rax],rdx    → 90 90 90
 *          CAuroraInterface 构造函数会把 AurGetTTFTexture 指针写回本体，必须让它别覆盖我们的钩子。
 *
 *      A2  0x19abdd0（.bss 8 字节函数指针 AurGetTTFTexture）
 *          原来指向 0x1126000 GetTTFTexture → 指向本文件的 nwzh_getttf。
 *          全二进制只有 1 处间接调用（0x4876f2）、1 处赋值（0x1124b96，已被 A1 废掉）。
 *
 *      B1  0x1132a6d  bb 01 00 00 00     mov ebx,1        → mov ebx,<side>（每轮烘焙前现写）
 *      B2  0x1132a98  01 db              add ebx,ebx      → jmp +4（跳过 next_pow2 循环）
 *      B3  0x1132aad  41 bc 01 00 00 00  mov r12d,1       → mov r12d,<side>（同上）
 *      B4  0x1132ab8  45 01 e4           add r12d,r12d    → jmp +6（同上）
 *          原来图集宽 = next_pow2(count*3+3 + (int)(count*fontsize))，高 = next_pow2(fontsize+6)。
 *          count=21248 时宽会算成 425088，必然分配失败；改成按每轮实际字号现算边长。
 *          B1 的 imm32 在 0x1132a6e，B3 的 imm32 在 0x1132aaf，nwzh_getttf 每轮覆盖一次。
 *
 *      C1  0x4856e0   81 fe ff 00 00 00  cmp esi,0xff     → cmp esi,0x52ff  (21247)
 *      C2  0x485750   81 fe ff 00 00 00  cmp esi,0xff     → cmp esi,0x52ff
 *          GetSymbolCoords / SetSymbolCoords 的硬上界。
 *
 * ── 为什么不用「改 0x487636 的 0x100」那条路 ────────────────────────────
 *  0x4875f2  lea r14,[rbp-0x440]  —— 码点数组在**栈上**，rbp-0x440 到 rbp-0x40
 *  正好 1024 字节 = 256 个 int，紧挨 [rbp-0x38] 的 stack canary。把 0x487636 的
 *  立即数改成 21248，那个循环会往上写 83KB，第一条就越过 canary → __stack_chk_fail。
 *  所以改走函数指针钩子：由 nwzh_getttf 用静态数组喂 21248 个码点给真 Load。
 *
 * ── 索引方案（UTF-8 直算，纯算术，无码点表）──────────────────────────────
 *      index = 256 + (cp - 0x4E00)        cp ∈ U+4E00..U+9FFF
 *  槽位 0..255     = 原来那 256 个 Latin-1 码点（由调用方传入的 codepoints 数组）
 *  槽位 256..21247 = U+4E00..U+9FFF（20992 个基本汉字区码点，顺序填充）
 *
 *  第三步的 TextOut 解码只要算出同一个 index 就能命中：
 *      UTF-8 三字节（E4..E9 打头）→ cp = (b0&0x0F)<<12 | (b1&0x3F)<<6 | (b2&0x3F)
 *      → 若 0x4E00 <= cp <= 0x9FFF 则 index = 256 + (cp - 0x4E00)，循环步进 3
 *  本轮（容量阶段）不实现这一步；范围外码点按「跳过 + 计数」处理。
 *
 * ── 图集边长策略 ────────────────────────────────────────────────────────
 *  面积估算：area = slots * (fontsize + 6)^2（stb 每格 = 字形框 + 两侧各 3px padding）
 *  fill = area / side^2
 *    默认 4096（RGBA 64MB）；
 *    仅当 fill > 70% 且 fontsize > 18 时升 8192（RGBA 256MB），升之前打 escalate 日志；
 *    NWNZH_ATLAS=<边长> 可手工强制（必须是 2 的幂）。
 *
 * 编译：./build.sh
 */

#define _GNU_SOURCE
#include <fcntl.h>
#include <link.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

/* ── 常量 ─────────────────────────────────────────────────────────────── */
#define NWZH_N_LATIN    256
#define NWZH_CJK_FIRST  0x4E00
#define NWZH_CJK_LAST   0x9FFF
#define NWZH_N_CJK      (NWZH_CJK_LAST - NWZH_CJK_FIRST + 1)  /* 20992 */

/* 中文标点 / 全角 ASCII / 弯引号 三段，紧接汉字区之后 */
#define NWZH_I_PUNCT    (NWZH_N_LATIN + NWZH_N_CJK)           /* 21248 */
#define NWZH_PUNCT_FIRST 0x3000
#define NWZH_PUNCT_LAST  0x303F
#define NWZH_N_PUNCT    (NWZH_PUNCT_LAST - NWZH_PUNCT_FIRST + 1)  /* 64 */

#define NWZH_I_FW       (NWZH_I_PUNCT + NWZH_N_PUNCT)         /* 21312 */
#define NWZH_FW_FIRST   0xFF01
#define NWZH_FW_LAST    0xFF5E
#define NWZH_N_FW       (NWZH_FW_LAST - NWZH_FW_FIRST + 1)    /* 94 */

#define NWZH_I_QUOTE    (NWZH_I_FW + NWZH_N_FW)               /* 21406 */
#define NWZH_N_QUOTE    4

/* 符号区（经典 GUI 实际会用到、原来落在范围外变成空格/方块的） */
#define NWZH_I_DASH     (NWZH_I_QUOTE + NWZH_N_QUOTE)         /* 21410 */
#define NWZH_DASH_FIRST 0x2010
#define NWZH_DASH_LAST  0x2015                                /* — – ‐ ‑ ‒ ― */
#define NWZH_N_DASH     (NWZH_DASH_LAST - NWZH_DASH_FIRST + 1) /* 6 */

#define NWZH_I_ARROW    (NWZH_I_DASH + NWZH_N_DASH)           /* 21416 */
#define NWZH_ARROW_FIRST 0x2190
#define NWZH_ARROW_LAST  0x21FF                               /* ← ↑ → ↓ ↔ … */
#define NWZH_N_ARROW    (NWZH_ARROW_LAST - NWZH_ARROW_FIRST + 1) /* 112 */

#define NWZH_I_GEO      (NWZH_I_ARROW + NWZH_N_ARROW)         /* 21528 */
#define NWZH_GEO_FIRST  0x25A0
#define NWZH_GEO_LAST   0x25FF                                /* ▪ ▫ ■ □ ▲ ► ● ○ … */
#define NWZH_N_GEO      (NWZH_GEO_LAST - NWZH_GEO_FIRST + 1)  /* 96 */

#define NWZH_I_SYM      (NWZH_I_GEO + NWZH_N_GEO)             /* 21624 */
#define NWZH_SYM_FIRST  0x2600
#define NWZH_SYM_LAST   0x26FF                                /* ♀ ♂ ★ ☆ ☀ ☁ … */
#define NWZH_N_SYM      (NWZH_SYM_LAST - NWZH_SYM_FIRST + 1)  /* 256 */

#define NWZH_N_TOTAL    (NWZH_I_SYM + NWZH_N_SYM)             /* 21880 */
#define NWZH_MAX_INDEX  (NWZH_N_TOTAL - 1)                    /* 21879 = 0x5577 */

#define ATLAS_SIDE_MIN  4096
#define ATLAS_SIDE_BIG  8192
#define ATLAS_FILL_MAX  0.70        /* 超过就认为 4096 装不下 */
#define ATLAS_ESCALATE_FONTSIZE 18.0f

/* 目标虚拟地址（.text 的 vaddr == 文件偏移） */
#define VA_G1              0x0e06961UL
/* nk_font_default_glyph_ranges 里那条 lea 的 disp32（0xde3e14）：
 *   0x78 -> default ranges (0x136e090)
 *   0x48 -> chinese ranges (0x136e060)，因为 next-ip = 0xde3e18
 * 改 1 字节即让 nk_font_config / nk_font_atlas_bake / nk_sdl_refresh_config
 * 三个调用者同时拿到 CJK ranges（jbmonoreg 这类兜底字体靠它）。 */
#define VA_G1B             0x0de3e14UL
/* CAuroraTTFTexture::Load 里 stbtt_PackBegin 的第 6 参（padding）：
 *   0x1132b23: 41 b9 03 00 00 00   mov r9d,0x3
 * 每个字形周围留 3px。字形格子是按「行框」取的，行框比墨迹高得多，
 * 3px 不够时上一行字形的笔画会落进本格（空格/连字符这类矮字最明显）。 */
#define VA_PACK_PAD        0x1132b25UL
#define VA_B1              0x1132a6dUL      /* mov ebx,imm32   —— imm 在 +1 */
#define VA_B2              0x1132a98UL
#define VA_B3              0x1132aadUL      /* 41 bc imm32     —— imm 在 +2 */
#define VA_B4              0x1132ab8UL
#define VA_B1_IMM          (VA_B1 + 1)
#define VA_B3_IMM          (VA_B3 + 2)
#define VA_A1              0x1124b96UL
#define VA_A2_PTR          0x19abdd0UL
#define VA_GETTTF_REAL     0x1126000UL
#define VA_C1              0x4856e0UL
#define VA_C2              0x485750UL

/* ── 日志 ─────────────────────────────────────────────────────────────── */
static void sayf(const char *fmt, ...)
{
    char buf[640];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        ssize_t ignored = write(2, buf, (size_t)(n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1));
        (void)ignored;
    }
}

static int tid_now(void)
{
    return (int)syscall(SYS_gettid);
}

/* ── 主程序映像 ───────────────────────────────────────────────────────── */
struct main_exe {
    uintptr_t base;
    uintptr_t lo;
    uintptr_t hi;
};
static struct main_exe g_main;

static int find_main(struct dl_phdr_info *info, size_t size, void *data)
{
    (void)size;
    if (info->dlpi_name != NULL && info->dlpi_name[0] != '\0') return 0;

    struct main_exe *m = (struct main_exe *)data;
    m->base = (uintptr_t)info->dlpi_addr;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
        if (ph->p_type != PT_LOAD) continue;
        uintptr_t lo = m->base + (uintptr_t)ph->p_vaddr;
        uintptr_t hi = lo + (uintptr_t)ph->p_memsz;
        if (m->lo == 0 || lo < m->lo) m->lo = lo;
        if (hi > m->hi) m->hi = hi;
    }
    return 1;
}

static int in_main(const void *p, size_t len)
{
    uintptr_t a = (uintptr_t)p;
    return a >= g_main.lo && (a + len) <= g_main.hi;
}

static long page_size(void)
{
    long p = sysconf(_SC_PAGESIZE);
    return p > 0 ? p : 4096;
}

/* 把 [p, p+len) 所在页改成可写可执行，填新字节，再恢复 r-x 并回读校验 */
static int put_bytes(const void *p, const unsigned char *repl, size_t len)
{
    long ps = page_size();
    uintptr_t page = (uintptr_t)p & ~((uintptr_t)ps - 1);
    size_t span = (size_t)((uintptr_t)p + len - page);

    if (mprotect((void *)page, span, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) return 0;
    memcpy((void *)p, repl, len);
    __builtin___clear_cache((char *)p, (char *)p + len);
    int ok = memcmp(p, repl, len) == 0;
    if (mprotect((void *)page, span, PROT_READ | PROT_EXEC) != 0) ok = 0;
    return ok;
}

/* 校验原字节 → 改 → 日志。任一步不符就跳过这一处，绝不半途崩游戏。 */
static int patch_code(const char *name, uintptr_t va,
                      const unsigned char *orig, const unsigned char *repl, size_t len)
{
    void *p = (void *)(g_main.base + va);
    if (!in_main(p, len)) {
        sayf("[nwzh] patch %s @0x%lx skipped: outside main image\n", name, (unsigned long)va);
        return 0;
    }
    if (memcmp(p, orig, len) != 0) {
        sayf("[nwzh] patch %s @0x%lx skipped: unexpected bytes\n", name, (unsigned long)va);
        return 0;
    }
    int ok = put_bytes(p, repl, len);
    sayf("[nwzh] patch %s @0x%lx %s\n", name, (unsigned long)va, ok ? "ok" : "FAILED");
    return ok;
}

/* 原地改一个 imm32（用于每轮烘焙前调整图集边长） */
static int write_imm32(uintptr_t va, unsigned side)
{
    unsigned char b[4];
    b[0] = (unsigned char)(side & 0xff);
    b[1] = (unsigned char)((side >> 8) & 0xff);
    b[2] = (unsigned char)((side >> 16) & 0xff);
    b[3] = (unsigned char)((side >> 24) & 0xff);
    void *p = (void *)(g_main.base + va);
    if (!in_main(p, 4)) return 0;
    return put_bytes(p, b, 4);
}

/* ── RSS（KB）─────────────────────────────────────────────────────────── */
static long rss_kb(void)
{
    int fd = open("/proc/self/statm", O_RDONLY);
    if (fd < 0) return -1;
    char b[128];
    ssize_t n = read(fd, b, sizeof(b) - 1);
    close(fd);
    if (n <= 0) return -1;
    b[n] = '\0';
    long total = 0, resident = 0;
    if (sscanf(b, "%ld %ld", &total, &resident) != 2) return -1;
    (void)total;
    return resident * page_size() / 1024;
}

/* ── Aurora TTF 钩子 ──────────────────────────────────────────────────── */
/* 真函数签名（由 0x1126000 的反汇编反推，静态成员函数，无 this）：
 *   void GetTTFTexture(char const* name, float fontsize, int* codepoints, int count,
 *                      float a, float b, float c, AurTTFData* out)                */
typedef void (*getttf_fn)(const char *, float, int *, int, float, float, float, void *);

/* AurTTFData 布局（由 0x1132ecf 起的写回序列确认） */
struct aur_ttf_data {
    void *pixels;    /* +0x00 像素缓冲 */
    int   height;    /* +0x08 */
    int   width;     /* +0x0c */
    void *coords_a;  /* +0x10 Vector* 数组 A（stride 12） */
    void *coords_b;  /* +0x18 Vector* 数组 B */
    int   count;     /* +0x20 实际字形数 */
    int   baseline;  /* +0x24 */
};

static getttf_fn       g_real_getttf;
static const unsigned short g_quote_cps[NWZH_N_QUOTE] = {0x2018, 0x2019, 0x201C, 0x201D};

/* 码点 -> 槽位；不在四个区间内返回 -1 */
__attribute__((visibility("hidden")))
int nwzh_cp_to_index(unsigned cp)
{
    if (cp >= NWZH_CJK_FIRST   && cp <= NWZH_CJK_LAST)   return NWZH_N_LATIN + (int)(cp - NWZH_CJK_FIRST);
    if (cp >= NWZH_PUNCT_FIRST && cp <= NWZH_PUNCT_LAST) return NWZH_I_PUNCT   + (int)(cp - NWZH_PUNCT_FIRST);
    if (cp >= NWZH_FW_FIRST    && cp <= NWZH_FW_LAST)    return NWZH_I_FW      + (int)(cp - NWZH_FW_FIRST);
    for (int i = 0; i < NWZH_N_QUOTE; i++)
        if (cp == g_quote_cps[i]) return NWZH_I_QUOTE + i;
    if (cp >= NWZH_DASH_FIRST  && cp <= NWZH_DASH_LAST)  return NWZH_I_DASH  + (int)(cp - NWZH_DASH_FIRST);
    if (cp >= NWZH_ARROW_FIRST && cp <= NWZH_ARROW_LAST) return NWZH_I_ARROW + (int)(cp - NWZH_ARROW_FIRST);
    if (cp >= NWZH_GEO_FIRST   && cp <= NWZH_GEO_LAST)   return NWZH_I_GEO   + (int)(cp - NWZH_GEO_FIRST);
    if (cp >= NWZH_SYM_FIRST   && cp <= NWZH_SYM_LAST)   return NWZH_I_SYM   + (int)(cp - NWZH_SYM_FIRST);
    return -1;
}

static int             g_codepoints[NWZH_N_TOTAL];
static int             g_cps_ready;
static int             g_atlas_side_now = ATLAS_SIDE_MIN;
static int             g_atlas_forced;
static int             g_atlas_dbg;
static const char     *g_dump_path;        /* NWNZH_ATLAS 是否生效 */
static const char     *g_coords_path;      /* NWNZH_DUMP_COORDS：把坐标表写成文本 */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static __thread int    t_in_shim;

/* 统计坐标数组里 xy 非零的字形数 —— stb 打包失败时全留 0，这是最直接的判据 */
static int count_nonzero_coords(const void *arr, int n)
{
    if (arr == NULL || n <= 0) return -1;
    const float *f = (const float *)arr;
    int nz = 0;
    for (int i = 0; i < n; i++)
        if (f[i * 3 + 0] != 0.0f || f[i * 3 + 1] != 0.0f) nz++;
    return nz;
}

/* 统计「零宽」字形：z==0 或 x 上下界相等 —— 这种字会画成 1px 竖线 */
static void stat_degenerate(const void *a, const void *b, int n, int *zero_z, int *flat_x)
{
    *zero_z = *flat_x = -1;
    if (a == NULL || b == NULL || n <= 0) return;
    const float *fa = (const float *)a, *fb = (const float *)b;
    int z = 0, f = 0;
    for (int i = 0; i < n; i++) {
        if (fb[i * 3 + 2] == 0.0f) z++;
        if (fb[i * 3 + 0] - fa[i * 3 + 0] <= 0.0f) f++;
    }
    *zero_z = z;
    *flat_x = f;
}

static void build_codepoints(int *incoming, int nin)
{
    for (int i = 0; i < NWZH_N_LATIN; i++)
        g_codepoints[i] = (incoming != NULL && i < nin) ? incoming[i] : 0;
    for (int i = 0; i < NWZH_N_CJK; i++)
        g_codepoints[NWZH_N_LATIN + i] = NWZH_CJK_FIRST + i;
    for (int i = 0; i < NWZH_N_PUNCT; i++)
        g_codepoints[NWZH_I_PUNCT + i] = NWZH_PUNCT_FIRST + i;
    for (int i = 0; i < NWZH_N_FW; i++)
        g_codepoints[NWZH_I_FW + i] = NWZH_FW_FIRST + i;
    for (int i = 0; i < NWZH_N_QUOTE; i++)
        g_codepoints[NWZH_I_QUOTE + i] = g_quote_cps[i];
    for (int i = 0; i < NWZH_N_DASH; i++)  g_codepoints[NWZH_I_DASH  + i] = NWZH_DASH_FIRST  + i;
    for (int i = 0; i < NWZH_N_ARROW; i++) g_codepoints[NWZH_I_ARROW + i] = NWZH_ARROW_FIRST + i;
    for (int i = 0; i < NWZH_N_GEO; i++)   g_codepoints[NWZH_I_GEO   + i] = NWZH_GEO_FIRST   + i;
    for (int i = 0; i < NWZH_N_SYM; i++)   g_codepoints[NWZH_I_SYM   + i] = NWZH_SYM_FIRST   + i;

    int out_of_range = 0;
    for (int i = NWZH_N_LATIN; i < NWZH_N_TOTAL; i++) {
        int cp = g_codepoints[i];
        if (nwzh_cp_to_index((unsigned)cp) != i) out_of_range++;
    }
    g_cps_ready = 1;
    sayf("[nwzh] cps map: latin=%d cjk=%d punct=%d fw=%d quote=%d dash=%d arrow=%d geo=%d sym=%d slots=%d out_of_range=%d\n",
         NWZH_N_LATIN, NWZH_N_CJK, NWZH_N_PUNCT, NWZH_N_FW, NWZH_N_QUOTE,
         NWZH_N_DASH, NWZH_N_ARROW, NWZH_N_GEO, NWZH_N_SYM,
         NWZH_N_TOTAL, out_of_range);
    sayf("[nwzh] cps seg: cjk@%d U+%04X-U+%04X | punct@%d | fw@%d | quote@%d | dash@%d U+%04X-U+%04X"
         " | arrow@%d | geo@%d | sym@%d U+%04X-U+%04X | max=%d(0x%X)\n",
         NWZH_N_LATIN, NWZH_CJK_FIRST, NWZH_CJK_LAST, NWZH_I_PUNCT, NWZH_I_FW, NWZH_I_QUOTE,
         NWZH_I_DASH, NWZH_DASH_FIRST, NWZH_DASH_LAST, NWZH_I_ARROW, NWZH_I_GEO,
         NWZH_I_SYM, NWZH_SYM_FIRST, NWZH_SYM_LAST, NWZH_MAX_INDEX, NWZH_MAX_INDEX);
    if (0) sayf("%d %d %d %d\n",
         NWZH_N_LATIN, NWZH_CJK_FIRST, NWZH_CJK_LAST, NWZH_N_CJK,
         NWZH_PUNCT_FIRST, NWZH_PUNCT_LAST, NWZH_N_PUNCT,
         NWZH_FW_FIRST, NWZH_FW_LAST, NWZH_N_FW, NWZH_N_QUOTE,
         NWZH_N_TOTAL, out_of_range);
}

/* 按本轮字号选图集边长；返回边长，area/ratio 供日志使用 */
static unsigned pick_atlas_side(float fontsize, int slots, double *area_out, double *ratio_out)
{
    double cell = (double)fontsize + 6.0;
    double area = (double)slots * cell * cell;
    double cap = (double)ATLAS_SIDE_MIN * (double)ATLAS_SIDE_MIN;
    double ratio = area / cap;
    unsigned side = ATLAS_SIDE_MIN;

    if (ratio_out != NULL) *ratio_out = ratio;
    if (area_out != NULL) *area_out = area;

    if (g_atlas_forced) return (unsigned)g_atlas_side_now;

    if (ratio > ATLAS_FILL_MAX && fontsize > ATLAS_ESCALATE_FONTSIZE) {
        side = ATLAS_SIDE_BIG;
        sayf("[nwzh] atlas escalate %u->%u area=%.1fM fill=%.0f%% fontsize=%.2f rss=%ldKB\n",
             (unsigned)ATLAS_SIDE_MIN, side, area / 1e6, ratio * 100.0,
             (double)fontsize, rss_kb());
    }
    return side;
}

/* 每个字形格子的高度 = 字号（引擎按「行框」切图集，不是按墨迹盒），
 * 于是上一行字形的笔画会落进本格顶部。实测 fnt_default_hr 16 的 `-`
 * （idx 45）格子 7x16：第 0..5 行是邻字笔画，真正的连字符在第 7..10 行；
 * 空格（idx 32）格子 2x16 整格都是邻字的竖笔画，于是空格被画成一条竖线。
 *
 * 修法：ASCII 段（idx < 256）逐个格子检查第 0 行——有墨就往下清，
 * 清到出现整行空白为止（最多 8 行）。真实字形不会在格子顶边被切断，
 * 所以这条判据只会伤到「本来就不该出现在格顶」的邻字残影。
 * NWNZH_ASCII_CLEAN=0 可关掉。 */
static int g_ascii_clean = 0;   /* 默认关：擦像素会伤害共享同一像素的邻居，改由搬墨处理 */
static int g_relocate = 1;

__attribute__((visibility("hidden")))
int nwzh_ascii_clean_atlas(void *out, int max_idx)
{
    struct aur_ttf_data *d = (struct aur_ttf_data *)out;
    if (d == NULL || d->pixels == NULL || d->coords_a == NULL || d->coords_b == NULL)
        return -1;
    const int W = d->width, H = d->height;
    if (W <= 0 || H <= 0) return -1;
    const float *fa = (const float *)d->coords_a;
    const float *fb = (const float *)d->coords_b;
    unsigned char *px = (unsigned char *)d->pixels;
    int cleaned = 0;
    long long px_cleared = 0;

    static unsigned char vis[64 * 64];
    static int stk[64 * 64 + 16];      /* (x,y) 对 */
    static int hit[64 * 64 + 16];
    static const int dx4[4] = {1, -1, 0, 0};
    static const int dy4[4] = {0, 0, 1, -1};

    int lim = d->count < max_idx ? d->count : max_idx;
    for (int i = 0; i < lim; i++) {
        int x0 = (int)(fa[i * 3 + 0] * (float)W);
        int y0 = (int)(fa[i * 3 + 1] * (float)H);
        int x1 = (int)(fb[i * 3 + 0] * (float)W);
        int y1 = (int)(fb[i * 3 + 1] * (float)H);
        if (x1 <= x0 || y1 <= y0) continue;
        int w = x1 - x0, h = y1 - y0;
        if (w > 64 || h > 64) continue;                  /* 只处理小格子 */
        if (i == 0x20 || i == 0xA0 || i == 0x09 || i == 0x0A || i == 0x0D) {
            /* 空白字符：格子里任何墨都是邻居的，整格清空 */
            int had = 0;
            for (int y = y0; y < y1; y++)
                for (int x = x0; x < x1; x++) {
                    unsigned char *q = &px[((size_t)y * W + x) * 4];
                    if (q[3]) { memset(q, 0, 4); had++; }
                }
            if (had) { cleaned++; px_cleared += had; }
            continue;
        }

        /* 只对「墨迹整体落在下半格」的 ASCII 标点做残影清理；
         * 字母/数字一律不动——d/b/h/p/q 的竖笔会顶到格顶、并与格上方的残影连成
         * 一整片，按连通块清会把真笔画一起清掉（实测 maintext 17 的 'd' 从 107px
         * 掉到 76px，屏幕上只剩右半边像 ')'）。 */
        {
            static const unsigned char punct_ok[256 / 8] = {
                0x00, 0x00, 0x00, 0x00,   /* 0x00-0x1F */
                0x00, 0x2C, 0x00, 0x00,   /* 0x20-0x3F: 0x21 '!'、0x22 '"'、0x25 '%'、0x27 '\''、0x2C ','、0x2D '-'、0x2E '.'、0x3A ':'、0x3B ';'、0x3D '=' */
                0x00, 0x00, 0x00, 0x00,   /* 0x40-0x5F: 0x5F '_' 见下 */
                0x00, 0x40, 0x00, 0x00,   /* 0x60-0x7F: 0x7E '~' */
            };
            (void)punct_ok;
            int ok = 0;
            switch (i) {
            case 0x21: case 0x22: case 0x25: case 0x27:
            case 0x2C: case 0x2D: case 0x2E:
            case 0x3A: case 0x3B: case 0x3D:
            case 0x5F: case 0x7E: ok = 1; break;
            default: break;
            }
            if (!ok) continue;
        }

        /* 判据：贴着格子顶边、且与格子正上方的墨相连通的像素 = 上一行字形的残影。
         * 真实字形即使顶到第 0 行，它上方（y0-1）也是打包留白，不会被种子选中。
         * 若连通块之外格子里还有别的墨（真字形），这块就是残影，清掉；
         * 若整格墨都属于这块，那要么是正常竖长字形（保留），要么是空白字符格（清空）。 */
        int ns = 0, npt = 0;
        memset(vis, 0, (size_t)w * (size_t)h);
        for (int xx = 0; xx < w; xx++) {
            if (y0 <= 0) break;
            int gx = x0 + xx;
            if (px[((size_t)y0 * W + gx) * 4 + 3] == 0) continue;
            if (px[((size_t)(y0 - 1) * W + gx) * 4 + 3] == 0) continue;
            vis[xx] = 1;
            stk[ns++] = xx;
            stk[ns++] = 0;
        }
        if (ns == 0) continue;
        while (ns > 0) {
            int cy = stk[--ns], cx = stk[--ns];
            if (npt + 2 > (int)(sizeof(hit) / sizeof(hit[0]))) { npt = -1; break; }
            hit[npt++] = cx;
            hit[npt++] = cy;
            for (int k = 0; k < 4; k++) {
                int nx = cx + dx4[k], ny = cy + dy4[k];
                if (nx < 0 || nx >= w || ny < 0 || ny >= h) continue;
                if (vis[ny * w + nx]) continue;
                if (px[((size_t)(y0 + ny) * W + (x0 + nx)) * 4 + 3] == 0) continue;
                vis[ny * w + nx] = 1;
                stk[ns++] = nx;
                stk[ns++] = ny;
            }
        }
        if (npt <= 0) continue;
        int total = 0;
        for (int y = y0; y < y1; y++)
            for (int x = x0; x < x1; x++)
                if (px[((size_t)y * W + x) * 4 + 3] != 0) total++;
        int region = npt / 2;
        if (!(region < total && region <= 200))
            continue;                                    /* 判定为真字形，一点不动 */
        for (int k = 0; k < npt; k += 2) {
            int gx = x0 + hit[k], gy = y0 + hit[k + 1];
            memset(&px[((size_t)gy * W + gx) * 4], 0, 4);
        }
        cleaned++;
        px_cleared += npt / 2;
    }
    if (cleaned > 0)
        sayf("[nwzh] ascii clean: %d glyph cells, %lld px cleared\n", cleaned, px_cleared);
    return cleaned;
}

/* ── 搬墨重建：把「墨迹高度不足半格」的矮字形搬到图集空白处 ──────────────────
 * 引擎给每个字形的 UV 格子高度恒等于字号（是从基线往上开的行框带，不是墨迹盒），
 * 矮字形（，。、；：！？空格…）格子上部装的是图集里上一行字形的像素，画出来就和
 * 真字形叠在一起。抹掉那些像素会连带破坏别的字形（同一像素被两个 quad 采样），
 * 所以这里改成「搬走」：在既有格子下方的空白区复制一份只含它自己墨迹的整高格子
 * （墨迹行号保持不变，基线对齐），再把该槽位的 u 改指过去。
 * 原槽位一个像素都不改；其它槽位的坐标条目一个都不动。NWNZH_RELOCATE=0 关闭。 */
/* 参考墨迹缓存：某些字体（实测 fnt_maintext 17 的 U+FF0C）烘出来的格子里
 * 根本没有字形墨迹（只有上一行的残影），搬过去就是一片空白。这里把「下半格确实
 * 有墨」的矮字形格子按索引缓存下来，遇到空壳格子就按最近邻缩放补贴一份。 */
#define NWZH_REF_MAX 96
struct nwzh_ref { int idx; int w, h; unsigned char *px; };
static struct nwzh_ref g_ref[NWZH_REF_MAX];
static int g_ref_n;

static struct nwzh_ref *nwzh_ref_find(int idx)
{
    for (int i = 0; i < g_ref_n; i++)
        if (g_ref[i].idx == idx) return &g_ref[i];
    return NULL;
}

/* 缓存某格 [b,h) 的墨迹（w 像素宽、hb 行） */
static void nwzh_ref_store(int idx, const unsigned char *px, int W, int x0, int y0, int w, int h, int b)
{
    if (g_ref_n >= NWZH_REF_MAX) return;
    int hb = h - b;
    unsigned char *buf = (unsigned char *)malloc((size_t)w * hb * 4);
    if (buf == NULL) return;
    for (int y = 0; y < hb; y++)
        memcpy(buf + (size_t)y * w * 4, &px[((size_t)(y0 + b + y) * W + x0) * 4], (size_t)w * 4);
    struct nwzh_ref *e = nwzh_ref_find(idx);
    if (e != NULL) { free(e->px); }
    else { e = &g_ref[g_ref_n++]; e->idx = idx; }
    e->w = w; e->h = hb; e->px = buf;
}

__attribute__((visibility("hidden")))
int nwzh_relocate_short_glyphs(void *out)
{
    struct aur_ttf_data *d = (struct aur_ttf_data *)out;
    if (d == NULL || d->pixels == NULL || d->coords_a == NULL || d->coords_b == NULL)
        return -1;
    const int W = d->width, H = d->height;
    if (W <= 0 || H <= 0 || d->count <= 0) return -1;
    float *fa = (float *)d->coords_a;
    float *fb = (float *)d->coords_b;

    /* NWNZH_RELOCATE=2：坐标重定向判别实验——让 U+FF0C(21323) 借用 U+3002(21250) 的格子。
     * 若屏幕上逗号位置画出圆圈，说明引擎用的是我们改过的坐标数组；若仍是 ' ，说明坐标被复制走了。 */
    if (g_relocate == 2) {
        if (d->count > 21323) {
            fa[21323*3+0] = fa[21250*3+0]; fa[21323*3+1] = fa[21250*3+1]; fa[21323*3+2] = fa[21250*3+2];
            fb[21323*3+0] = fb[21250*3+0]; fb[21323*3+1] = fb[21250*3+1]; fb[21323*3+2] = fb[21250*3+2];
            sayf("[nwzh] reloc probe: 21323 -> cell of 21250 (u0=%.5f v0=%.5f)\n", fa[21323*3+0], fa[21323*3+1]);
        }
        return 0;
    }
    unsigned char *px = (unsigned char *)d->pixels;

    /* 1) 所有既有格子的最深底边之下才是安全空白：那里没有任何字形会采样 */
    int yfree = 0;
    for (int i = 0; i < d->count; i++) {
        int x0 = (int)(fa[i * 3 + 0] * (float)W);
        int x1 = (int)(fb[i * 3 + 0] * (float)W);
        int y1 = (int)(fb[i * 3 + 1] * (float)H);
        if (x1 > x0 && y1 > yfree) yfree = y1;
    }
    yfree += 4;

    int cx = 0, cy = yfree, moved = 0, refills = 0;
    long long moved_px = 0;
    /* 只搬标点/符号区（U+3000 及之后，起始槽 21248）。ASCII 与汉字格一律不动：
     * 带声调的拉丁大写（È/É/Ý）和某些汉字也会满足「顶行连通 + 下方有墨」的判据，
     * 搬走后会丢掉字母上方的重音符（实测 idx 200/201/221 的墨从 115/133/116 掉到 7/8/10）。 */
    for (int i = 0; i < d->count; i++) {
        /* 这不是「只搬标点区」：ASCII 的矮标点（- : % , . ! ; = _ ' "）与空白字符
         * 也被上一行残影污染了，而擦像素会连带伤害共享同一像素的邻居（实测 'J' 75->11、
         * 'd' 107->76），所以它们一律走「搬墨」这条路，原槽位一个像素都不动。
         * 字母/数字完全不碰。 */
        int is_ws = (i == 0x20 || i == 0xA0 || i == 0x09 || i == 0x0A || i == 0x0D);
        /* 0x21..0x7E 全部可打印 ASCII 都纳入：字母/数字/符号的格子顶部同样装着
         * 上一行字形的墨（角色名 D/d/m/v 头顶的小墨点就是这么来的）。
         * 搬墨只复制 [b,bot] 这一段，原格一个像素不动，所以字形本身逐字节不变。 */
        int is_ascii = 0, is_letter = 0;
        if (i >= 0x21 && i < 0x7F) {
            is_ascii = 1;
            is_letter = (i >= 0x30 && i <= 0x39) || (i >= 0x41 && i <= 0x5A) ||
                        (i >= 0x61 && i <= 0x7A);
        }
        int is_punct = is_ascii;
        if (!(i >= 21248 || is_ws || is_punct)) continue;
        int x0 = (int)(fa[i * 3 + 0] * (float)W);
        int y0 = (int)(fa[i * 3 + 1] * (float)H);
        int x1 = (int)(fb[i * 3 + 0] * (float)W);
        int y1 = (int)(fb[i * 3 + 1] * (float)H);
        int w = x1 - x0, h = y1 - y0;
        if (w <= 0 || h <= 0 || w > 256 || h > 256) continue;
        if (x0 < 0 || y0 < 0 || x1 > W || y1 > H) continue;
        /* 保留原始的小数宽度：u1-u0 不变 -> advance 不变、字宽不变 */
        const float wf = (fb[i * 3 + 0] - fa[i * 3 + 0]) * (float)W;

        /* 顶行有墨、往下第一次出现整行空白的位置 b：0..b-1 是上一行字形的残影 */
        int b = -1;
        for (int y = 0; y < h && b < 0; y++) {
            int any = 0;
            for (int x = x0; x < x1; x++)
                if (px[((size_t)(y0 + y) * W + x) * 4 + 3] > 8) { any = 1; break; }
            if (!any) b = y;
        }
        if (is_ws) b = h;                          /* 空白字符：新格整格留白 */
        int split = 0;
        if (b < 1 && i == 21250) {
            /* U+3002 在 fnt_maintext 17 里是「行 0 空、行 1-7 残墨、行 10-14 圆圈」，
             * b 会取到 0 而被跳过。这里改成取最后一段连续墨迹的起点（圆圈），
             * 只对这一个码点生效，避免把 ：「」 这类双段字形切坏。 */
            int last = -1, prev_blank = 1;
            for (int y = 0; y < h; y++) {
                int any = 0;
                for (int x = x0; x < x1; x++)
                    if (px[((size_t)(y0 + y) * W + x) * 4 + 3] > 8) { any = 1; break; }
                if (any && prev_blank) last = y;
                prev_blank = !any;
            }
            if (last >= 2 && h - last <= 10) { b = last; split = 1; }
        }
        if (b < 0) continue;
        if (!is_ws && b < 1) continue;             /* 顶行就空：没有残影，不用搬 */
        /* 字母/数字的上伸笔画一旦和残影连成一片，b 会切进字形里——那种情况宁可不搬。 */
        if (is_letter) {
            /* 字母/数字有上伸笔画：残影一旦和笔画连成一片就没法安全切，宁可不搬。 */
            int top_ink = 0;
            for (int y = 0; y < b; y++)
                for (int x = x0; x < x1; x++)
                    if (px[((size_t)(y0 + y) * W + x) * 4 + 3] > 8) top_ink++;
            if (b > 4 || top_ink > w * 2) continue;
        } else if (is_ascii) {
            /* 标点/符号没有上伸笔画，残影可以更高，只要下面剩下的墨是短短一段。 */
            if (b > 10 || h - b > 10) continue;
        }
        if (i >= 21248 && !is_ws && h - b > 12) continue;  /* 汉字区：下半格还有大块墨就不搬 */
        int low = 0;
        for (int y = b; y < h && !low && !is_ws; y++)
            for (int x = x0; x < x1; x++)
                if (px[((size_t)(y0 + y) * W + x) * 4 + 3] > 8) { low = 1; break; }
        if (!low && !is_ws) continue;              /* 整格只有顶部残墨又不是空白字符，不搬 */
        int conn = 1;                              /* 残影确实与格子上方连通吗 */
        if (!is_ws && !is_punct && !split) conn = 0;  /* ASCII 标点/分段：格顶有墨就当残影 */
        if (y0 > 0)
            for (int x = x0; x < x1; x++)
                if (px[((size_t)y0 * W + x) * 4 + 3] > 8 &&
                    px[((size_t)(y0 - 1) * W + x) * 4 + 3] > 8) { conn = 1; break; }
        if (!conn) continue;

        /* 下半格到底有多少墨？够多就是正常字形（顺便缓存成参考），
         * 少于 6 像素说明这个字号根本没烘出字形，只能借别的字号的墨。 */
        long ink = 0;
        for (int y = b; y < h; y++)
            for (int x = x0; x < x1; x++)
                if (px[((size_t)(y0 + y) * W + x) * 4 + 3] > 8) ink++;
        if (ink >= 12) nwzh_ref_store(i, px, W, x0, y0, w, h, b);
        struct nwzh_ref *rf = (ink < 6) ? nwzh_ref_find(i) : NULL;
        if (!is_ws && ink < 6 && rf == NULL) continue;  /* 又没有参考墨，跳过 */

        if (cx + w + 2 > W) { cx = 0; cy += h + 2; }
        if (cy + h > H) { sayf("[nwzh] relocate: atlas full at slot %d\n", i); break; }

        /* 格子底部也常常带着「下一行字形的顶」，一起搬过去就是新的残影。
         * 这里把实际墨迹范围算成 [b, bot]，范围外一律留白。 */
        int bot = b - 1;
        if (!is_ws) {
            int rs[64], re[64], nr = 0, prev = 1;
            for (int y = 0; y < h; y++) {
                int any = 0;
                for (int x = x0; x < x1; x++)
                    if (px[((size_t)(y0 + y) * W + x) * 4 + 3] > 8) { any = 1; break; }
                if (any && prev) { if (nr < 64) { rs[nr] = y; re[nr] = y; nr++; } }
                else if (any && nr > 0) { if (nr <= 64) re[nr - 1] = y; }
                prev = !any;
            }
            /* 只看 b 之后的墨迹段：b 之前是上一行的残影，本来就不搬。
             * 若 b 之后还有第二段、且它贴着格底、又很薄（<=5 行），那是下一个打包行的顶，丢掉。 */
            int first = -1;
            for (int k = 0; k < nr; k++) if (rs[k] >= b) { first = k; break; }
            if (first < 0) continue;                     /* b 之后没墨了，不该搬 */
            int keep = nr - 1;
            while (keep > first && re[keep] >= h - 2 && (re[keep] - rs[keep]) <= 5) keep--;
            bot = re[keep];
        }
        int nx = cx, ny = cy;
        int top = b;                               /* 墨迹在格内的起始行，原样保留 */
        for (int y = 0; y < h; y++) {
            unsigned char *dst = &px[((size_t)(ny + y) * W + nx) * 4];
            if (y >= top && y <= bot && rf == NULL)
                memcpy(dst, &px[((size_t)(y0 + y) * W + x0) * 4], (size_t)w * 4);
            else
                memset(dst, 0, (size_t)w * 4);
        }
        if (rf != NULL) {                          /* 最近邻缩放到本格 [b,bot]，底部对齐 */
            int hb = bot - b + 1;
            if (hb < 1) hb = 1;
            for (int y = 0; y < hb; y++) {
                int sy = (int)((long)y * rf->h / (hb > 0 ? hb : 1));
                if (sy >= rf->h) sy = rf->h - 1;
                unsigned char *dst = &px[((size_t)(ny + b + y) * W + nx) * 4];
                for (int x = 0; x < w; x++) {
                    int sx = (int)((long)x * rf->w / (w > 0 ? w : 1));
                    if (sx >= rf->w) sx = rf->w - 1;
                    memcpy(dst + (size_t)x * 4, rf->px + ((size_t)sy * rf->w + sx) * 4, 4);
                }
            }
            refills++;
        }
        fa[i * 3 + 0] = (float)nx / (float)W;
        fb[i * 3 + 0] = fa[i * 3 + 0] + wf / (float)W;
        fa[i * 3 + 1] = (float)ny / (float)H;
        fb[i * 3 + 1] = (float)(ny + h) / (float)H;
        cx += w + 2;
        moved++;
        moved_px += (long long)w * h;
    }
    if (moved > 0)
        sayf("[nwzh] relocate short glyphs: moved=%d refill=%d px=%lld free_from_y=%d cursor_y=%d atlas=%dx%d\n",
             moved, refills, moved_px, yfree, cy, W, H);
    return moved;
}

static void nwzh_getttf(const char *name, float fontsize, int *cps, int n,
                        float a, float b, float c, void *out)
{
    if (g_real_getttf == NULL) {
        sayf("[nwzh] getttf called with no real target, ignored\n");
        return;
    }
    if (t_in_shim) {                       /* 防重入：原样转发 */
        g_real_getttf(name, fontsize, cps, n, a, b, c, out);
        return;
    }
    t_in_shim = 1;
    pthread_mutex_lock(&g_lock);           /* B1/B3 的 imm32 是进程级全局指令，必须串行 */

    if (!g_cps_ready) build_codepoints(cps, n);

    double area = 0.0, ratio = 0.0;
    unsigned side = pick_atlas_side(fontsize, NWZH_N_TOTAL, &area, &ratio);

    int set_ok = 1;
    if ((int)side != g_atlas_side_now) {
        set_ok = write_imm32(VA_B1_IMM, side) && write_imm32(VA_B3_IMM, side);
        if (set_ok) {
            g_atlas_side_now = (int)side;
        } else {
            sayf("[nwzh] atlas side -> %u write FAILED, keeping %d\n", side, g_atlas_side_now);
        }
    }

    long rss_before = rss_kb();
    g_real_getttf(name, fontsize, g_codepoints, NWZH_N_TOTAL, a, b, c, out);
    long rss_after = rss_kb();

    if (g_ascii_clean && out != NULL) nwzh_ascii_clean_atlas(out, 256);
    if (g_relocate && out != NULL) nwzh_relocate_short_glyphs(out);

    const struct aur_ttf_data *d = (const struct aur_ttf_data *)out;
    if (d != NULL) {
        int nz = count_nonzero_coords(d->coords_a, d->count);
        int zero_z = -1, flat_x = -1;
        stat_degenerate(d->coords_a, d->coords_b, d->count, &zero_z, &flat_x);
        long long px = (long long)d->width * (long long)d->height;
        sayf("[nwzh] bake tid=%d name=%.48s fontsize=%.2f slots=%d side=%u area=%.2fM fill=%.1f%% "
             "| got count=%d size=%dx%d px=%lld coords_nonzero=%d/%d missing=%d rss=%ld->%ldKB\n",
             tid_now(), name != NULL ? name : "?", (double)fontsize, NWZH_N_TOTAL, side,
             area / 1e6, ratio * 100.0,
             d->count, d->width, d->height, px, nz, d->count,
             (nz >= 0 && d->count > 0) ? (d->count - nz) : -1,
             rss_before, rss_after);
        sayf("[nwzh] coords health: zero_z=%d flat_x=%d of %d (%.1f%% degenerate)\n",
             zero_z, flat_x, d->count,
             d->count > 0 ? 100.0 * (flat_x < 0 ? 0 : flat_x) / d->count : 0.0);

        if (nz >= 0 && d->count > 0 && nz < d->count) {
            sayf("[nwzh] atlas insufficient: need=%d got=%d missing=%d fontsize=%.2f size=%d\n",
                 d->count, nz, d->count - nz, (double)fontsize, d->width);
        }
        if (g_dump_path != NULL) {
            char fn[512];
            static int dump_n;
            snprintf(fn, sizeof(fn), "%s-%d.raw", g_dump_path, dump_n++);
            int fd = open(fn, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0) {
                /* AurTTFData::pixels 是 32 位 RGBA：每个像素 4 字节。之前只 dump 了
                   1/4（把前 1024 行当成了整张图），这里按 4 字节/像素写全。 */
                size_t bytes = (size_t)d->width * (size_t)d->height * 4u;
                size_t off = 0;
                const char *pp = (const char *)d->pixels;
                while (off < bytes) {
                    ssize_t w = write(fd, pp + off, bytes - off);
                    if (w <= 0) break;
                    off += (size_t)w;
                }
                close(fd);
                sayf("[nwzh] atlas dumped to %s bytes=%zu %dx%d count=%d fontsize=%.2f\n",
                     fn, bytes, d->width, d->height, d->count, (double)fontsize);
            }
        }
        if (g_coords_path != NULL && d->coords_a != NULL && d->count > 0) {
            char fn[512];
            static int cn;
            snprintf(fn, sizeof(fn), "%s-%d.txt", g_coords_path, cn++);
            FILE *fp = fopen(fn, "w");
            if (fp != NULL) {
                const float *ca = (const float *)d->coords_a;
                const float *cb = (const float *)d->coords_b;
                fprintf(fp, "# name=%.48s fontsize=%.2f count=%d size=%dx%d\n",
                        name != NULL ? name : "?", (double)fontsize,
                        d->count, d->width, d->height);
                for (int i = 0; i < d->count; i++) {
                    fprintf(fp, "%d %.6f %.6f %.6f %.6f %.6f %.6f\n", i,
                            (double)ca[i * 3 + 0], (double)ca[i * 3 + 1], (double)ca[i * 3 + 2],
                            cb != NULL ? (double)cb[i * 3 + 0] : 0.0,
                            cb != NULL ? (double)cb[i * 3 + 1] : 0.0,
                            cb != NULL ? (double)cb[i * 3 + 2] : 0.0);
                }
                fclose(fp);
                sayf("[nwzh] coords dumped to %s count=%d fontsize=%.2f\n",
                     fn, d->count, (double)fontsize);
            }
        }
        if (g_atlas_dbg) {
            const float *ca = (const float *)d->coords_a;
            const float *cb = (const float *)d->coords_b;
            static const int probe[] = { 0x41, 0x100, 0x51B, 0x11FA, 0x18B0, 0x1B63 };
            for (unsigned k = 0; k < sizeof(probe) / sizeof(probe[0]); k++) {
                int i = probe[k];
                if (ca == NULL || i >= d->count) continue;
                sayf("[nwzh] glyph[%#x] A=(%.5f,%.5f,%.5f)%s", i,
                     (double)ca[i * 3 + 0], (double)ca[i * 3 + 1], (double)ca[i * 3 + 2],
                     cb ? " B=(" : " B=none\n");
                if (cb)
                    sayf("(%.5f,%.5f,%.5f)\n", (double)cb[i * 3 + 0], (double)cb[i * 3 + 1],
                         (double)cb[i * 3 + 2]);
            }
        }
        if (d->count != NWZH_N_TOTAL) {
            sayf("[nwzh] warning: Load returned count=%d, expected %d\n", d->count, NWZH_N_TOTAL);
        }
    } else {
        sayf("[nwzh] bake tid=%d name=%.48s fontsize=%.2f slots=%d side=%u | out=NULL rss=%ld->%ldKB\n",
             tid_now(), name != NULL ? name : "?", (double)fontsize, NWZH_N_TOTAL, side,
             rss_before, rss_after);
    }

    pthread_mutex_unlock(&g_lock);
    t_in_shim = 0;
}

static void install_ttf_hook(void)
{
    void *slot = (void *)(g_main.base + VA_A2_PTR);
    if (!in_main(slot, sizeof(uintptr_t))) {
        sayf("[nwzh] patch aurora-gettff-hook @0x%lx skipped: outside main image\n",
             (unsigned long)VA_A2_PTR);
        return;
    }

    uintptr_t old = *(volatile uintptr_t *)slot;
    g_real_getttf = (getttf_fn)(g_main.base + VA_GETTTF_REAL);

    long ps = page_size();
    uintptr_t page = (uintptr_t)slot & ~((uintptr_t)ps - 1);
    size_t span = (size_t)((uintptr_t)slot + sizeof(uintptr_t) - page);
    if (mprotect((void *)page, span, PROT_READ | PROT_WRITE) != 0) {
        sayf("[nwzh] patch aurora-gettff-hook @0x%lx skipped: mprotect failed\n",
             (unsigned long)VA_A2_PTR);
        return;
    }

    *(volatile uintptr_t *)slot = (uintptr_t)&nwzh_getttf;
    __builtin___clear_cache((char *)slot, (char *)slot + sizeof(uintptr_t));

    int ok = *(volatile uintptr_t *)slot == (uintptr_t)&nwzh_getttf;
    sayf("[nwzh] patch aurora-gettff-hook @0x%lx %s (was 0x%lx, real 0x%lx, shim %p)\n",
         (unsigned long)VA_A2_PTR, ok ? "ok" : "FAILED",
         (unsigned long)old, (unsigned long)(g_main.base + VA_GETTTF_REAL),
         (void *)&nwzh_getttf);
}

/* ══════════════════════════════════════════════════════════════════════════
 * 第三阶段：TextOut / CalculateVisibleStringLengthAndWidth / UpdateCaret
 *           三处热路径的 UTF-8 解码
 *
 * 三处原始代码都是「取一个字节 → 当作 0..255 的字形索引」，我们把它换成
 * 「按 UTF-8 解出码点 → 映射成 index = 256 + (cp - 0x4E00)」，
 * 渲染、宽度、光标共用同一个解码函数，保证三者完全一致。
 * ════════════════════════════════════════════════════════════════════════ */
#define VA_TEXTOUT_DBC    0x49625cUL   /* jne TextOutDoubleByte（本版是空桩，什么都不画） */
#define VA_TEXTOUT_LOAD   0x496512UL
#define VA_CALC_LOAD      0x496888UL
#define VA_CALC_ESI       0x4968b7UL
#define VA_CARET_LOAD     0x496bedUL
#define VA_CARET_MOVR13D  0x496c01UL

#define VA_IMG_CALL1      0x487b27UL
#define VA_IMG_CALL2      0x487b93UL
#define VA_IMG_CALL3      0x487bfdUL
#define VA_IMG_REAL       0x484cc0UL
#define VA_GETREF_CALL    0x496cc4UL   /* CAurFont::CAurFont(char const*) 里的 call AurTextureGetReference */
#define VA_GETREF_REAL    0x48e480UL

/* Nuklear 字形查询：确认 NUI 那条路拿到的是码点还是单字节 */
#define VA_TOUTF8_STR_OVERLOAD 0x5fd6c0UL
#define VA_TOUTF8_NUI_CALL    0x865f82UL
#define VA_TOUTF8_STR   0x5fd260u   /* Encoding::ToUTF8(char const*, Locale) */

/* Encoding::FromUTF8(char const*, Locale) —— 反方向（UTF-8 -> 本地码页）。
 * 无法映射的字符在 0x5fd8a6 被写成 '?'：mov BYTE PTR [rdx+rax*1],0x3f，
 * 所以 repository.json 里的中文进到这一步就变成等长的 '?'。
 * ContentIndex::GetRepositories 有 3 处调用它。 */
/* calc 桩必须保住 %rsi：0x4969e8 的 markup 分支把 rsi 当 text+ebx 传给
 * ProcessTextMarkup（0x496077 mov [rdx],rdi / 0x496081 mov rbx,rsi），
 * 而 nwzh_decode 是 C 函数，rsi 是 caller-saved 会被踩掉。
 * 打开后 calc 桩是 5 个 push（40B），入口 rsp%16=8 -> 0，正好满足 call 对齐。 */
#ifndef STUB_D2
#define STUB_D2 1
#endif

#define VA_FROMUTF8_STR    0x5fd710UL
#define VA_FROMUTF8_BODY   0x5fd716UL   /* 跳过 push rbp / mov rbp,rsp / push r15 */
#define VA_STR_CTOR     0x513590u   /* basic_string(char const*, allocator const&) */
#define VA_NK_FG_JE     0xde3e53u   /* 0f 84 a6 00 00   je 0xde3eff */
#define VA_NK_FG_BODY   0xde3e59u

/* 统计（只用于日志，允许微小竞态） */
static unsigned long g_cjk, g_ascii, g_invalid, g_oor, g_punct;
static unsigned long g_decoded;
static int g_text_armed;
__attribute__((visibility("hidden")))
int g_text_dbg;

/* 回头跳进引擎的绝对地址 */
__attribute__((visibility("hidden")))
uintptr_t g_resume_textout, g_calc_resume, g_calc_markup, g_caret_resume;
__attribute__((visibility("hidden")))
uintptr_t g_real_img;

/* 解码：返回字形索引，*adv = 本字符占用的字节数（1..4）。
 * 规则与 README 里那张表一致；任何越界都退化成空格，绝不读越界。 */
static int g_dbg_seq;
static int g_dec_max = 48;

/* 所有 return 都经过这里，便于打点 */
/* 字体名重定向：把经典 GUI 位图字体 fnt_galahad14 换成引擎自己 remap 表里的
   "ttf fnt_maintext 17"（有 CJK 字形）。取不到就退回原名，绝不让上层拿到 NULL。 */
typedef void *(*getref_fn)(const char *, void *, long);
static getref_fn g_real_getref;
static int       g_font_remap = 1;
static int       g_remap_hits;
static int       g_names_dbg;
static int       g_names_n;

__attribute__((visibility("hidden")))
void *nwzh_shim_getref(const char *name, void *base, long dyn)
{
    static const char ttf_name[] = "ttf fnt_maintext 17";

    if (g_font_remap && name != NULL &&
        (strncasecmp(name, "fnt_galahad", 11) == 0)) {
        void *r = g_real_getref(ttf_name, base, dyn);
        if (r != NULL) {
            if (g_remap_hits < 4)
                sayf("[nwzh] font remap: \"%s\" -> \"%s\" (ok)\n", name, ttf_name);
            g_remap_hits++;
            return r;
        }
        if (g_remap_hits < 4)
            sayf("[nwzh] font remap: \"%s\" -> ttf lookup failed, keep original\n", name);
    }
    {
        void *r = g_real_getref(name, base, dyn);
        if (g_names_dbg && g_names_n < 60) {
            g_names_n++;
            sayf("[nwzh] getref #%d \"%s\" -> %p\n", g_names_n,
                 name != NULL ? name : "(null)", r);
        }
        return r;
    }
}

static int g_font_dbg;
static void *g_font_seen[12];

/* fontinfo → 字体名（由 nwzh_font_probe 填，GetSymbolCoords 探针按 fontinfo 反查）。
   同一个 fontinfo 可能被多个 CAurFont 共享，名字取第一个见到的即可。 */
static struct { void *fi; char name[24]; } g_fi_names[32];
static int g_fi_names_n;

static void nwzh_fi_remember(void *fi, const char *name)
{
    int i;
    if (fi == NULL || name == NULL) return;
    for (i = 0; i < g_fi_names_n && i < 32; i++)
        if (g_fi_names[i].fi == fi) return;
    if (g_fi_names_n >= 32) return;
    g_fi_names[g_fi_names_n].fi = fi;
    memcpy(g_fi_names[g_fi_names_n].name, name, 23);
    g_fi_names[g_fi_names_n].name[23] = 0;
    g_fi_names_n++;
}

static const char *nwzh_fi_name(void *fi)
{
    int i;
    for (i = 0; i < g_fi_names_n && i < 32; i++)
        if (g_fi_names[i].fi == fi) return g_fi_names[i].name;
    return "?";
}

/* 诊断：打印某个 CAurFont 背后 CAurFontInfo 的实际容量，以及高槽位是否越界。
   CAurFont+0x8 是 ctor 里 strncpy 进去的字体名，+0x28 -> CAurFontInfo；
   +0x00 numchars，+0x2c/+0x3c 两个坐标表长度。
   每个**不同的 CAurFont 指针**打一行——按 fontinfo 去重会把共享同一份
   fontinfo 的两个字体（不同字号）漏掉，正是需要分清的那一档。 */
__attribute__((visibility("hidden")))
void nwzh_font_probe(void *font, int idx)
{
    unsigned char *fi;
    int numchars, sizeA, sizeB;
    int i, dup = 0;

    if (!g_font_dbg || font == NULL) return;
    fi = *(unsigned char **)((unsigned char *)font + 0x28);
    if (fi == NULL) return;
    numchars = *(int *)(fi + 0x00);
    sizeA    = *(int *)(fi + 0x2c);
    sizeB    = *(int *)(fi + 0x3c);

    for (i = 0; i < (int)(sizeof(g_font_seen) / sizeof(g_font_seen[0])); i++) {
        if (g_font_seen[i] == font) { dup = 1; break; }
        if (g_font_seen[i] == NULL) { dup = 0; break; }
    }
    if (!dup && i < (int)(sizeof(g_font_seen) / sizeof(g_font_seen[0]))) {
        char name[24];
        g_font_seen[i] = font;
        memcpy(name, (const char *)font + 0x8, 23);
        name[23] = 0;
        for (int k = 0; k < 23; k++) {
            unsigned char c = (unsigned char)name[k];
            if (c < 0x20 || c > 0x7e) { name[k] = 0; break; }
        }
        nwzh_fi_remember(fi, name);
        sayf("[nwzh] font=%p name=\"%s\" fi=%p numchars=%d sizeA=%d sizeB=%d ptrA=%p ptrB=%p\n",
             font, name, (void *)fi, numchars, sizeA, sizeB,
             *(void **)(fi + 0x20), *(void **)(fi + 0x30));
    }
    if (g_dbg_seq < 60 && idx >= 0x100) {
        g_dbg_seq++;
        if (idx >= sizeA || idx >= sizeB) {
            float *a = *(float **)(fi + 0x20);
            float *b = *(float **)(fi + 0x30);
            sayf("[nwzh] glyph OOB idx=%d sizeA=%d sizeB=%d a=(%.5f,%.5f,%.5f) b=(%.5f,%.5f,%.5f)\n",
                 idx, sizeA, sizeB, a[0], a[1], a[2], b[0], b[1], b[2]);
        }
    }
}

/* ── GetSymbolCoords 入口探针（NWNZH_COORDS_DEBUG=1 才安装）───────────────
 * 目的：属性值/伤害那几格到底有没有走「按索引取字形坐标」这条路，是谁在问、
 * 问的是哪些索引、问的是哪份 fontinfo（+0x68/+0x69 这两个「待刷新」标志一置上，
 * 该函数就整体返回零坐标 = 什么都画不出来）。
 * 0x4856d0 开头 5 字节 = 80 7f 68 00 55（cmp [rdi+0x68],0 ; push rbp），
 * 桩回来后原样重放这两句（保持 flags），跳回 0x4856d5。 */
#define VA_COORDS_ENTRY   0x4856d0UL
#define VA_COORDS_RESUME  0x4856d5UL

__attribute__((visibility("hidden")))
uintptr_t g_coords_resume;

static int g_coords_dbg;
static int g_coords_cnt;
static unsigned long g_cd_seen[2048];
static void *g_cd_fi[16];
static int g_cd_fi_n;

__attribute__((visibility("hidden")))
void nwzh_coords_probe(void *fi, long idx, void *ret)
{
    unsigned char *p = (unsigned char *)fi;
    unsigned long h;
    int i, dup = 0, slot = -1;

    if (!g_coords_dbg || fi == NULL || g_coords_cnt >= 400) return;

    for (i = 0; i < g_cd_fi_n && i < 16; i++)
        if (g_cd_fi[i] == fi) { dup = 1; break; }
    if (!dup && g_cd_fi_n < 16) {
        g_cd_fi[g_cd_fi_n++] = fi;
        sayf("[nwzh] coords fi=%p name=\"%s\" numchars=%d sizeA=%d sizeB=%d f68=%d f69=%d\n",
             fi, nwzh_fi_name(fi), *(int *)(p + 0x00), *(int *)(p + 0x2c), *(int *)(p + 0x3c),
             (int)p[0x68], (int)p[0x69]);
    }

    /* 去重键 = (调用者返回地址, index, fontinfo) */
    h = (((unsigned long)ret >> 3) * 1099511628211UL) ^
        ((unsigned long)idx * 2654435761UL) ^ ((unsigned long)fi >> 5);
    for (i = 0; i < 8; i++) {
        unsigned long s = g_cd_seen[(h + (unsigned long)i * 131UL) & 2047UL];
        if (s == 0) { slot = (int)((h + (unsigned long)i * 131UL) & 2047UL); break; }
        if (s == (h | 1UL)) return;
    }
    if (slot < 0) slot = (int)(h & 2047UL);
    g_cd_seen[slot] = h | 1UL;

    g_coords_cnt++;
    sayf("[nwzh] coords#%d ret=0x%lx fi=%p idx=%ld low=0x%02lx\n",
         g_coords_cnt, (unsigned long)ret, fi, idx, (unsigned long)(idx & 0xffL));
}

__asm__(
".text\n"
".p2align 4\n"
".globl nwzh_stub_coords\n"
".type nwzh_stub_coords,@function\n"
"nwzh_stub_coords:\n"
"    cmpl $0, g_coords_dbg(%rip)\n"
"    je   .Lnwzh_coords_cold\n"
"    push %rdi\n"
"    push %rsi\n"
"    push %rdx\n"
"    push %rcx\n"
"    sub  $8, %rsp\n"
"    movslq %esi, %rsi\n"
"    mov  40(%rsp), %rdx\n"
"    call nwzh_coords_probe\n"
"    add  $8, %rsp\n"
"    pop  %rcx\n"
"    pop  %rdx\n"
"    pop  %rsi\n"
"    pop  %rdi\n"
".Lnwzh_coords_cold:\n"
"    cmpb $0, 0x68(%rdi)\n"
"    push %rbp\n"
"    jmp  *g_coords_resume(%rip)\n"
".size nwzh_stub_coords,.-nwzh_stub_coords\n"
);


/* 低字节冲突探针：引擎热路径里有 cmp r13b/r15b,0x3c / 0x20 / 0xa,
 * 用来识别 markup / 空格 / 换行。我们的槽位号只要低字节撞上这三个值，
 * 就值得记一笔——虽然桩里 r13b/r15b 拿的是**原始字节**（CJK 时 r13=0），
 * 但这张表能证明「映射里到底有多少个这样的槽位」。 */
static unsigned long g_lowbyte_hits, g_lowbyte_shown;

static void nwzh_lowbyte_probe(unsigned char b, int idx, int adv)
{
    unsigned lb = (unsigned)idx & 0xffu;
    if (lb != 0x3c && lb != 0x20 && lb != 0x0a) return;
    g_lowbyte_hits++;
    if (g_lowbyte_shown < 8) {
        g_lowbyte_shown++;
        sayf("[nwzh] lowbyte hit: b=%02x idx=%d(0x%x) low=0x%02x adv=%d\n",
             b, idx, idx, lb, adv);
    }
}

static int nwzh_decode_trace(const unsigned char *p, int off, int idx, int adv)
{
    if (g_text_dbg && g_dbg_seq < g_dec_max) {
        g_dbg_seq++;
        sayf("[nwzh] dec off=%d b=%02x -> idx=%#x adv=%d\n", off, p[off], idx, adv);
    }
    if (idx >= 0x100) nwzh_lowbyte_probe(p[off], idx, adv);
    return idx;
}

/* 诊断：NWNZH_DIGIT_PROBE=1 时把 ASCII '0'..'9' 映射到「零一二三四五六七八九」。
 * 用来回答「属性值那几格到底是不是走我们这层 decode 画的」。 */
static int g_digit_probe;
/* 诊断：NWNZH_SYM_PROBE=1 时把几个「渲染成残片」的 ASCII 换成汉字，
 * 用来判断那个位置到底是不是我们 decode 给的索引在画。 */
static int g_sym_probe;
static const unsigned g_digit_cp[10] = {
    0x96F6, 0x4E00, 0x4E8C, 0x4E09, 0x56DB, 0x4E94, 0x516D, 0x4E03, 0x516B, 0x4E5D
};

int nwzh_decode(const unsigned char *p, int len, int off, int *adv)
{
    unsigned b0 = p[off];
    int idx;

    if (!g_text_armed) {
        g_text_armed = 1;
        sayf("[nwzh] text decoder armed: textout@0x%lx calc@0x%lx caret@0x%lx\n",
             (unsigned long)VA_TEXTOUT_LOAD, (unsigned long)VA_CALC_LOAD,
             (unsigned long)VA_CARET_LOAD);
    }

    if (b0 < 0x80) {                       /* ASCII */
        *adv = 1;
        g_ascii++;
        idx = (int)b0;
        goto done_ascii;
    }

    if (b0 >= 0xE0 && b0 <= 0xEF && off + 2 < len) {
        unsigned b1 = p[off + 1], b2 = p[off + 2];
        if ((b1 & 0xC0) == 0x80 && (b2 & 0xC0) == 0x80) {
            unsigned cp = ((b0 & 0x0Fu) << 12) | ((b1 & 0x3Fu) << 6) | (b2 & 0x3Fu);
            *adv = 3;
            int slot = nwzh_cp_to_index(cp);
            if (slot >= 0) {
                if (cp >= NWZH_CJK_FIRST && cp <= NWZH_CJK_LAST) g_cjk++;
                else g_punct++;
                return nwzh_decode_trace(p, off, slot, *adv);
            }
            g_oor++;
            return nwzh_decode_trace(p, off, ' ', *adv);
        }
        *adv = 1; g_invalid++; return nwzh_decode_trace(p, off, ' ', 1);
    }

    if (b0 >= 0xC2 && b0 <= 0xDF && off + 1 < len) {
        unsigned b1 = p[off + 1];
        if ((b1 & 0xC0) == 0x80) {
            *adv = 2;                       /* cp < 0x800，不落在任何目标区段 */
            g_oor++;
            return nwzh_decode_trace(p, off, ' ', *adv);
        }
        *adv = 1; g_invalid++; return nwzh_decode_trace(p, off, ' ', 1);
    }

    if (b0 >= 0xF0 && b0 <= 0xF4 && off + 3 < len) {
        unsigned b1 = p[off + 1], b2 = p[off + 2], b3 = p[off + 3];
        if ((b1 & 0xC0) == 0x80 && (b2 & 0xC0) == 0x80 && (b3 & 0xC0) == 0x80) {
            *adv = 4; g_invalid++; return nwzh_decode_trace(p, off, ' ', 4);
        }
    }

    *adv = 1; g_invalid++;
    return nwzh_decode_trace(p, off, ' ', 1);

done_ascii:
    idx = nwzh_decode_trace(p, off, (int)b0, 1);
    if (g_digit_probe && b0 >= '0' && b0 <= '9') {
        int slot = nwzh_cp_to_index(g_digit_cp[b0 - '0']);
        if (slot >= 0) idx = slot;
    }
    if (g_sym_probe) {
        unsigned cp = 0;
        switch (b0) {
        case '-': cp = 0x4E00; break;   /* 一 */
        case ':': cp = 0x4E8C; break;   /* 二 */
        case '+': cp = 0x4E09; break;   /* 三 */
        case 'd': cp = 0x56DB; break;   /* 四 */
        case 'a': cp = 0x4E94; break;   /* 五 */
        case '%': cp = 0x516D; break;   /* 六 */
        default: break;
        }
        if (cp) { int slot = nwzh_cp_to_index(cp); if (slot >= 0) idx = slot; }
    }
    if (++g_decoded >= 50000 && (g_decoded % 50000) == 0) {
        sayf("[nwzh] text stats: decoded=%lu cjk=%lu punct=%lu ascii=%lu invalid=%lu oor=%lu\n",
             g_decoded, g_cjk, g_punct, g_ascii, g_invalid, g_oor);
    }
    return idx;
}

/* 诊断：打印 TextOut/calc 每次**字符串开头**拿到的 len、字体名和原始字节。
 * NWNZH_TEXT_DEBUG=1 只看含非 ASCII 的串（旧行为）；
 * NWNZH_TEXT_DEBUG=2 (NWNZH_STR_ALL=1) 连纯 ASCII 串一起打，用来追
 * 「某段文本到底有没有走我们这两个桩」。 */
static int g_str_all;
static int g_str_seq;
#define NWZH_STR_SEEN_N 4096
static unsigned g_str_seen[NWZH_STR_SEEN_N];
/* 相同 (字体, 内容) 的串只打第一次：主菜单每帧重画会把日志上限吃光。 */
static unsigned nwzh_str_h(const char *s, int len, const void *font, int src)
{
    /* src 也进哈希：同一个串先被 calc 量宽、再被 textout 画，两边都要留痕，
       否则 dedupe 会把 textout 那条吃掉，看不出「量过但没画」。 */
    unsigned h = 2166136261u ^ (unsigned)((unsigned long)font >> 4) ^ (unsigned)(src * 2654435761u);
    if (len > 32) len = 32;
    for (int i = 0; i < len; i++) h = (h ^ (unsigned char)s[i]) * 16777619u;
    return h ? h : 1u;
}
__attribute__((noinline, visibility("hidden")))
void nwzh_text_log(const char *str, int len, const void *font, int src)
{
    int hi = 0;
    char name[20];
    if (!g_text_dbg) return;
    if (len <= 0 || str == NULL) return;
    if (g_str_seq >= (g_str_all ? 600 : 50)) return;
    for (int i = 0; i < len; i++) {
        if ((unsigned char)str[i] >= 0x80) { hi = 1; break; }
    }
    if (!hi && !g_str_all) return;      /* 默认只看含非 ASCII 的串，省日志 */
    {
        unsigned h = nwzh_str_h(str, len, font, src);
        unsigned i = h & (NWZH_STR_SEEN_N - 1);
        for (int probe = 0; probe < 8; probe++, i = (i + 1) & (NWZH_STR_SEEN_N - 1)) {
            if (g_str_seen[i] == h) return;             /* 打过，跳过 */
            if (g_str_seen[i] == 0) { g_str_seen[i] = h; break; }
        }
    }
    g_str_seq++;
    name[0] = 0;
    if (font != NULL) {
        memcpy(name, (const char *)font + 0x8, 19);
        name[19] = 0;
        for (int i = 0; i < 19; i++) {
            unsigned char c = (unsigned char)name[i];
            if (c < 0x20 || c > 0x7e) { name[i] = 0; break; }
        }
    }
    char hex[200];
    char buf[100];
    int n = 0, h = 0;
    for (int i = 0; i < len && i < 48; i++) {
        unsigned char c = (unsigned char)str[i];
        buf[n++] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
        h += snprintf(hex + h, sizeof(hex) - h, "%02x ", c);
        if (h > (int)sizeof(hex) - 4) break;
    }
    buf[n] = 0; hex[h] = 0;
    sayf("[nwzh] str#%d %s font=\"%s\" len=%d ascii=\"%s\" hex=%s\n",
         g_str_seq, src == 1 ? "textout" : "calc", name, len, buf, hex);
}

/* 这三个桩用「跳到引擎地址」的方式回去，不用 call/ret。 */
__asm__(
".text\n"
".globl nwzh_stub_textout\n"
".type nwzh_stub_textout,@function\n"
"nwzh_stub_textout:\n"
"    cmpl $0, g_text_dbg(%rip)\n"
"    je   .Lnwzh_to_nd\n"
"    test %r12d, %r12d\n"
"    jne  .Lnwzh_to_nd\n"
"    push %rdi\n"
"    push %rsi\n"
"    push %rdx\n"
"    push %rcx\n"
"    sub  $16, %rsp\n"
"    mov  %r13, %rdi\n"
"    mov  -0x84(%rbp), %esi\n"
"    mov  %r15, %rdx\n"
"    mov  $1, %ecx\n"
"    call nwzh_text_log\n"
"    add  $16, %rsp\n"
"    pop  %rcx\n"
"    pop  %rdx\n"
"    pop  %rsi\n"
"    pop  %rdi\n"
".Lnwzh_to_nd:\n"
"    push %rdi\n"
"    push %rdx\n"
"    push %rcx\n"
"    push %r11\n"
"    sub  $16, %rsp\n"
"    mov  %r13, %rdi\n"
"    mov  -0x84(%rbp), %esi\n"
"    sub  %r12d, %esi\n"
"    xor  %edx, %edx\n"
"    mov  %rsp, %rcx\n"
"    call nwzh_decode\n"
"    mov  %r13, -0x98(%rbp)\n"
"    mov  (%rsp), %edx\n"
"    add  %edx, %r12d\n"
"    sub  $1, %r12d\n"
"    mov  %eax, %esi\n"
"    xor  %r13d, %r13d\n"
"    cmp  $0x100, %eax\n"
"    jae  .Lnwzh_to_norm\n"
"    mov  %eax, %r13d\n"
".Lnwzh_to_norm:\n"
"    add  $16, %rsp\n"
"    pop  %r11\n"
"    pop  %rcx\n"
"    pop  %rdx\n"
"    pop  %rdi\n"
"    jmp  *g_resume_textout(%rip)\n"
".size nwzh_stub_textout,.-nwzh_stub_textout\n"
".globl nwzh_stub_calc\n"
".type nwzh_stub_calc,@function\n"
"nwzh_stub_calc:\n"
"    push %rdi\n"
#if STUB_D2
"    push %rsi\n"
#endif
"    push %rdx\n"
"    push %rcx\n"
"    push %r11\n"
"    sub  $24, %rsp\n"
"    test %ebx, %ebx\n"
"    jne  .Lnwzh_calc_nostr\n"
"    mov  %r14, %rdi\n"
"    mov  -0x88(%rbp), %esi\n"
"    mov  %r13, %rdx\n"
"    mov  $2, %ecx\n"
"    call nwzh_text_log\n"
"    mov  48(%rsp), %rsi\n"          /* 补回被 C 调用踩掉的 text+ebx（save 槽） */
".Lnwzh_calc_nostr:\n"
"    movzbl (%rsi), %r15d\n"
"    mov  %rsi, %rdi\n"
"    mov  -0x88(%rbp), %esi\n"
"    sub  %ebx, %esi\n"
"    xor  %edx, %edx\n"
"    mov  %rsp, %rcx\n"
"    call nwzh_decode\n"
"    cmpl $0, g_font_dbg(%rip)\n"
"    je   .Lnwzh_calc_noprobe\n"
"    mov  %eax, 8(%rsp)\n"
"    mov  %r13, %rdi\n"
"    mov  %eax, %esi\n"
"    call nwzh_font_probe\n"
"    mov  8(%rsp), %eax\n"
".Lnwzh_calc_noprobe:\n"
"    mov  %eax, -0x70(%rbp)\n"
"    mov  (%rsp), %eax\n"
"    add  %eax, %ebx\n"
"    sub  $1, %ebx\n"
"    add  $24, %rsp\n"
"    pop  %r11\n"
"    pop  %rcx\n"
"    pop  %rdx\n"
#if STUB_D2
"    pop  %rsi\n"
#endif
"    pop  %rdi\n"
"    cmp  $0x3c, %r15b\n"
"    je   .Lnwzh_calc_markup\n"
"    jmp  *g_calc_resume(%rip)\n"
".Lnwzh_calc_markup:\n"
"    jmp  *g_calc_markup(%rip)\n"
".size nwzh_stub_calc,.-nwzh_stub_calc\n"
".globl nwzh_stub_calc_esi\n"
".type nwzh_stub_calc_esi,@function\n"
"nwzh_stub_calc_esi:\n"
"    mov  -0x70(%rbp), %esi\n"
"    mov  -0x78(%rbp), %rdx\n"
"    jmp  *g_calc_esi_resume(%rip)\n"
".size nwzh_stub_calc_esi,.-nwzh_stub_calc_esi\n"
".globl nwzh_stub_caret\n"
".type nwzh_stub_caret,@function\n"
"nwzh_stub_caret:\n"
"    push %rdi\n"
"    push %rdx\n"
"    push %rcx\n"
"    push %r11\n"
"    push %rax\n"
"    sub  $24, %rsp\n"
"    movzbl (%rax,%r14,1), %r13d\n"
"    mov  %rax, %rdi\n"
"    mov  -0x88(%rbp), %esi\n"
"    mov  %r14d, %edx\n"
"    mov  %rsp, %rcx\n"
"    call nwzh_decode\n"
"    mov  %eax, 8(%rsp)\n"
"    mov  (%rsp), %eax\n"
"    add  %eax, %r14d\n"
"    sub  $1, %r14d\n"
"    mov  8(%rsp), %esi\n"
"    add  $24, %rsp\n"
"    pop  %rax\n"
"    pop  %r11\n"
"    pop  %rcx\n"
"    pop  %rdx\n"
"    pop  %rdi\n"
"    jmp  *g_caret_resume(%rip)\n"
".size nwzh_stub_caret,.-nwzh_stub_caret\n"
".globl nwzh_shim_img\n"
".type nwzh_shim_img,@function\n"
"nwzh_shim_img:\n"
"    push %rdi\n"
"    push %rsi\n"
"    push %rdx\n"
"    push %rcx\n"
"    push %r8\n"
"    push %r9\n"
"    sub  $8, %rsp\n"
"    call nwzh_is_galahad14\n"
"    add  $8, %rsp\n"
"    pop  %r9\n"
"    pop  %r8\n"
"    pop  %rcx\n"
"    pop  %rdx\n"
"    pop  %rsi\n"
"    pop  %rdi\n"
"    test %eax, %eax\n"
"    jne  .Lnwzh_img_block\n"
"    jmp  *g_real_img(%rip)\n"
".Lnwzh_img_block:\n"
"    xor  %eax, %eax\n"
"    ret\n"
".size nwzh_shim_img,.-nwzh_shim_img\n"
);

__asm__(
".globl nwzh_shim_toutf8\n"
".type nwzh_shim_toutf8,@function\n"
"nwzh_shim_toutf8:\n"
"    push %rdi\n"
"    push %rsi\n"
"    push %rdx\n"
"    mov  16(%rsp), %rdi\n"
"    mov  8(%rsp), %rsi\n"
"    mov  (%rsp), %rdx\n"
"    mov  24(%rsp), %rcx\n"
"    call nwzh_toutf8_probe\n"
"    test %eax, %eax\n"
"    je   .Lnwzh_tu_real_pop\n"
"    mov  16(%rsp), %rdi\n"
"    mov  8(%rsp), %rsi\n"
"    lea  g_dummy_alloc(%rip), %rdx\n"
"    call *g_str_ctor(%rip)\n"
"    mov  16(%rsp), %rdi\n"
"    mov  8(%rsp), %rsi\n"
"    call nwzh_verify_str\n"
"    mov  16(%rsp), %rax\n"
"    add  $24, %rsp\n"
"    ret\n"
".Lnwzh_tu_real_pop:\n"
"    pop  %rdx\n"
"    pop  %rsi\n"
"    pop  %rdi\n"
".Lnwzh_tu_real:\n"
"    push %rbp\n"
"    mov  %rsp, %rbp\n"
"    push %r15\n"
"    jmp *g_real_toutf8(%rip)\n"
".size nwzh_shim_toutf8,.-nwzh_shim_toutf8\n"
);

__asm__(
".globl nwzh_shim_toutf8_nui\n"
".type nwzh_shim_toutf8_nui,@function\n"
"nwzh_shim_toutf8_nui:\n"
"    push %rdi\n"
"    push %rsi\n"
"    push %rdx\n"
"    mov  16(%rsp), %rdi\n"
"    mov  8(%rsp), %rsi\n"
"    mov  (%rsp), %rdx\n"
"    mov  24(%rsp), %rcx\n"
"    call nwzh_toutf8_nui_probe\n"
"    test %eax, %eax\n"
"    je   .Lnwzh_tun_real\n"
"    mov  8(%rsp), %rsi\n"
"    mov  (%rsi), %rsi\n"
"    mov  16(%rsp), %rdi\n"
"    lea  g_dummy_alloc(%rip), %rdx\n"
"    call *g_str_ctor(%rip)\n"
"    mov  16(%rsp), %rdi\n"
"    mov  8(%rsp), %rsi\n"
"    mov  (%rsi), %rsi\n"
"    call nwzh_verify_str\n"
"    mov  16(%rsp), %rax\n"
"    add  $24, %rsp\n"
"    ret\n"
".Lnwzh_tun_real:\n"
"    pop  %rdx\n"
"    pop  %rsi\n"
"    pop  %rdi\n"
"    jmp  *g_real_toutf8_str(%rip)\n"
".size nwzh_shim_toutf8_nui,.-nwzh_shim_toutf8_nui\n"
);

/* 纯跟踪模式：只记录，(不改行为)，然后复原序言进原函数 */
__asm__(
".globl nwzh_shim_toutf8_trace\n"
".type nwzh_shim_toutf8_trace,@function\n"
"nwzh_shim_toutf8_trace:\n"
"    push %rdi\n"
"    push %rsi\n"
"    push %rdx\n"
"    mov  16(%rsp), %rdi\n"
"    mov  8(%rsp), %rsi\n"
"    mov  (%rsp), %rdx\n"
"    mov  24(%rsp), %rcx\n"
"    call nwzh_toutf8_trace_probe\n"
"    pop  %rdx\n"
"    pop  %rsi\n"
"    pop  %rdi\n"
"    push %rbp\n"
"    mov  %rsp, %rbp\n"
"    push %r15\n"
"    jmp *g_real_toutf8(%rip)\n"
".size nwzh_shim_toutf8_trace,.-nwzh_shim_toutf8_trace\n"
);

__asm__(
".globl nwzh_shim_findglyph\n"
".type nwzh_shim_findglyph,@function\n"
"nwzh_shim_findglyph:\n"
"    test %rdi, %rdi\n"
"    je   .Lnwzh_fg_zero\n"
"    cmp  $0x80, %esi\n"
"    jb   .Lnwzh_fg_fast\n"
"    push %rdi\n"
"    push %rsi\n"
"    push %rdx\n"
"    push %rcx\n"
"    push %r8\n"
"    push %r9\n"
"    sub  $8, %rsp\n"
"    call *g_findglyph_body(%rip)\n"
"    mov  %rax, (%rsp)\n"
"    mov  40(%rsp), %edi\n"
"    mov  %rax, %rsi\n"
"    call nwzh_nk_glyph_log\n"
"    mov  (%rsp), %rax\n"
"    add  $8, %rsp\n"
"    pop  %r9\n"
"    pop  %r8\n"
"    pop  %rcx\n"
"    pop  %rdx\n"
"    pop  %rsi\n"
"    pop  %rdi\n"
"    ret\n"
".Lnwzh_fg_fast:\n"
"    jmp *g_findglyph_body(%rip)\n"
".Lnwzh_fg_zero:\n"
"    xor  %eax, %eax\n"
"    ret\n"
".size nwzh_shim_findglyph,.-nwzh_shim_findglyph\n"
);

extern char nwzh_stub_textout[], nwzh_stub_calc[], nwzh_stub_calc_esi[];
extern char nwzh_stub_caret[], nwzh_stub_coords[], nwzh_shim_img[], nwzh_shim_findglyph[];
extern char nwzh_shim_toutf8[], nwzh_shim_toutf8_nui[], nwzh_shim_toutf8_trace[];

__attribute__((visibility("hidden")))
uintptr_t g_findglyph_body;
static int g_nk_trace;
static unsigned long g_nk_hits;

__attribute__((visibility("hidden")))
uintptr_t g_calc_esi_resume;
__attribute__((visibility("hidden")))
uintptr_t g_real_toutf8, g_str_ctor, g_real_toutf8_str;
__attribute__((visibility("hidden")))
uintptr_t g_real_fromutf8;
__attribute__((visibility("hidden")))
char g_dummy_alloc[64];

/* 「是不是 fnt_galahad14」——允许 fnt_galahad014 这类零填充写法 */
static int g_img_debug;
static int g_dbg_n;

__attribute__((visibility("hidden")))
void nwzh_nk_glyph_log(unsigned cp, void *glyph)
{
    if (!g_nk_trace || g_nk_hits >= 200) return;
    g_nk_hits++;
    sayf("[nwzh] nk glyph U+%04X -> %s\n", cp, glyph ? "found" : "MISSING");
}

__attribute__((visibility("hidden")))
int nwzh_is_galahad14(const char *name)
{
    static const char pfx[] = "fnt_galahad";
    if (g_img_debug && g_dbg_n < 40) {
        g_dbg_n++;
        sayf("[nwzh] img probe #%d: \"%s\"\n", g_dbg_n, name ? name : "(null)");
    }
    if (name == NULL) return 0;
    if (strncmp(name, pfx, sizeof(pfx) - 1) != 0 &&
        strncmp(name, "Fnt_galahad", sizeof(pfx) - 1) != 0) return 0;
    const char *d = name + (sizeof(pfx) - 1);
    while (*d == '0') d++;
    return d[0] == '1' && d[1] == '4' && d[2] == '\0';
}

/* 主程序 ±2GB 内找一页 RWX 做 veneer：E9 rel32 到不了 .so（0x7f.. vs 0x55..） */
static unsigned char *g_veneer;
static size_t g_veneer_used;

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

static void *alloc_veneer_page(void)
{
    static const intptr_t offs[] = {
        0x08000000, 0x10000000, 0x20000000, 0x30000000, 0x40000000,
        -0x08000000, -0x10000000, -0x20000000, -0x30000000
    };
    for (size_t i = 0; i < sizeof(offs) / sizeof(offs[0]); i++) {
        uintptr_t want = g_main.base + (uintptr_t)offs[i];
        void *p = mmap((void *)want, 0x1000, PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (p != MAP_FAILED && (uintptr_t)p == want) return p;
        if (p != MAP_FAILED) munmap(p, 0x1000);
    }
    return NULL;
}

/* 造一个 veneer：movabs $target,%rax ; jmp *%rax */
static void *make_veneer(void *target)
{
    if (g_veneer == NULL) return NULL;
    unsigned char *p = g_veneer + g_veneer_used;
    if (g_veneer_used + 12 > 0x1000) return NULL;
    uintptr_t t = (uintptr_t)target;
    p[0] = 0x48; p[1] = 0xB8;
    memcpy(p + 2, &t, 8);
    p[10] = 0xFF; p[11] = 0xE0;
    g_veneer_used += 12;
    __builtin___clear_cache((char *)p, (char *)p + 12);
    return p;
}

/* 把一处 5 字节 call/jmp 改成 E9 rel32 → veneer */
static int patch_veneer(const char *name, uintptr_t va,
                        const unsigned char orig[5], void *target)
{
    unsigned char repl[5];
    void *v = make_veneer(target);
    if (v == NULL) {
        sayf("[nwzh] patch %s @0x%lx skipped: no veneer page\n", name, (unsigned long)va);
        return 0;
    }
    int64_t rel = (int64_t)((uintptr_t)v - (g_main.base + va + 5));
    if (rel > 0x7fffffffLL || rel < -0x80000000LL) {
        sayf("[nwzh] patch %s @0x%lx skipped: veneer out of rel32 range\n", name, (unsigned long)va);
        return 0;
    }
    repl[0] = 0xE9;
    memcpy(repl + 1, &rel, 4);
    return patch_code(name, va, orig, repl, 5);
}

/* 把一处 5 字节 call 改成 E8 rel32 → veneer。
 * 必须是 E8 而不是 E9：callee 的第 7/8/9 个参数在 [rsp+8]/[rsp+16]/[rsp+24]，
 * 这些偏移是按「返回地址已压在栈上」算的；用 jmp 会让三个栈参数整体错位，
 * 并且 ret 会跳回 LoadImage 的调用者而不是下一条指令。 */
static int patch_veneer_call(const char *name, uintptr_t va,
                             const unsigned char orig[5], void *target)
{
    unsigned char repl[5];
    void *v = make_veneer(target);
    if (v == NULL) {
        sayf("[nwzh] patch %s @0x%lx skipped: no veneer page\n", name, (unsigned long)va);
        return 0;
    }
    int64_t rel = (int64_t)((uintptr_t)v - (g_main.base + va + 5));
    if (rel > 0x7fffffffLL || rel < -0x80000000LL) {
        sayf("[nwzh] patch %s @0x%lx skipped: veneer out of rel32 range\n", name, (unsigned long)va);
        return 0;
    }
    repl[0] = 0xE8;
    memcpy(repl + 1, &rel, 4);
    return patch_code(name, va, orig, repl, 5);
}

/* 判断是不是「含多字节序列且整体合法的 UTF-8」——是则不需要 cp1252 转换 */
__attribute__((visibility("hidden")))
int nwzh_is_utf8_text(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    int mb = 0;
    while (*p) {
        int n;
        if (*p < 0x80) { p++; continue; }
        if ((*p & 0xE0) == 0xC0) n = 2;
        else if ((*p & 0xF0) == 0xE0) n = 3;
        else if ((*p & 0xF8) == 0xF0) n = 4;
        else return 0;
        for (int i = 1; i < n; i++) {
            if (p[i] == 0 || (p[i] & 0xC0) != 0x80) return 0;
        }
        mb = 1;
        p += n;
    }
    return mb;
}

/* Encoding::ToUTF8 已经是 UTF-8 时直通（否则引擎会按 cp1252 把每个字节重编码） */
static void install_toutf8_fix(void)
{
    g_str_ctor = g_main.base + VA_STR_CTOR;
    const char *mode = getenv("NWNZH_TOUTF8");
    if (mode != NULL && mode[0] == '0') {
        sayf("[nwzh] ToUTF8 patch disabled by NWNZH_TOUTF8=0\n");
        return;
    }
    if (getenv("NWNZH_TOUTF8_TRACE") != NULL && getenv("NWNZH_TOUTF8_TRACE")[0] == '1') {
        /* 纯跟踪：记录全部调用者，不改行为 */
        g_real_toutf8 = g_main.base + VA_TOUTF8_STR + 6;
        static const unsigned char ot[5] = {0x55, 0x48, 0x89, 0xe5, 0x41};
        patch_veneer("toutf8-trace", VA_TOUTF8_STR, ot, nwzh_shim_toutf8_trace);
        return;
    }
    if (mode != NULL && mode[0] == '1') {
        /* 全局模式（实验用）：整个 Encoding::ToUTF8(char const*, Locale) 透传 */
        g_real_toutf8 = g_main.base + VA_TOUTF8_STR + 6;  /* 跳过 push rbp/mov rbp,rsp/push r15 */
        static const unsigned char o[5] = {0x55, 0x48, 0x89, 0xe5, 0x41};
        patch_veneer("toutf8-global", VA_TOUTF8_STR, o, nwzh_shim_toutf8);
        return;
    }
    /* 默认（方案 2）：只改 NUI 的那一个调用点，经典 GUI 完全不动 */
    g_real_toutf8_str = g_main.base + VA_TOUTF8_STR_OVERLOAD;
    static const unsigned char oc[5] = {0xe8, 0x39, 0x77, 0xd9, 0xff};
    patch_veneer_call("toutf8-nui", VA_TOUTF8_NUI_CALL, oc, nwzh_shim_toutf8_nui);
}

__attribute__((visibility("hidden")))
unsigned long g_toutf8_hits, g_toutf8_calls;

/* 长度受限的「含合法多字节 UTF-8」判断 */
__attribute__((visibility("hidden")))
int nwzh_is_utf8_n(const char *p, int len)
{
    int i = 0, mb = 0;
    while (i < len) {
        unsigned char c = (unsigned char)p[i];
        if (c < 0x80) { i++; continue; }
        int n;
        if ((c & 0xE0) == 0xC0) n = 2;
        else if ((c & 0xF0) == 0xE0) n = 3;
        else if ((c & 0xF8) == 0xF0) n = 4;
        else return 0;
        if (i + n > len) return 0;
        for (int k = 1; k < n; k++)
            if (((unsigned char)p[i + k] & 0xC0) != 0x80) return 0;
        mb = 1;
        i += n;
    }
    return mb;
}

/* 跟踪探针：只记录，不改行为 */
__attribute__((visibility("hidden")))
void nwzh_toutf8_trace_probe(void *sret, const char *src, long locale, unsigned long ret)
{
    static int n;
    g_toutf8_calls++;
    int n2 = (src != NULL) ? (int)strlen(src) : -1;
    int utf8 = (src && n2 > 0) ? nwzh_is_utf8_text(src) : 0;
    if (n2 > 0 && utf8) g_toutf8_hits++;
    if (n++ < 4000) {
        char buf[17];
        for (int i = 0; i < 16; i++) {
            unsigned char c = (src && i < n2) ? (unsigned char)src[i] : 0;
            buf[i] = c ? ((c >= 0x20 && c < 0x7f) ? (char)c : '.') : ' ';
        }
        buf[16] = 0;
        sayf("[nwzh] t8 #%d ret=0x%lx loc=%ld len=%d utf8=%d \"%s\"\n",
             g_toutf8_calls, ret, locale, n2, utf8, buf);
    }
}

/* NUI 调用点：rdi=sret, rsi=std::string const*, rdx=Locale。返回 1 = 原样透传 */
__attribute__((visibility("hidden")))
int nwzh_toutf8_nui_probe(void *sret, const void *st, long locale, unsigned long ret)
{
    static int n;
    g_toutf8_calls++;
    const char *src = st ? *(const char * const *)st : NULL;
    size_t sz = st ? *(const size_t *)((const char *)st + 8) : 0;
    int utf8 = (src && sz > 0 && sz < 4096) ? nwzh_is_utf8_n(src, (int)sz) : 0;
    if (utf8) g_toutf8_hits++;
    if (getenv("NWNZH_TOUTF8_DEBUG") && n++ < 40) {
        char buf[9];
        for (int i = 0; i < 8; i++) {
            unsigned char c = (src && (size_t)i < sz) ? (unsigned char)src[i] : 0;
            buf[i] = c ? ((c >= 0x20 && c < 0x7f) ? (char)c : '.') : ' ';
        }
        buf[8] = 0;
        sayf("[nwzh] toutf8-nui #%d ret=0x%lx sret=%p src=%p loc=%ld len=%zu utf8=%d -> %s \"%s\"\n",
             g_toutf8_calls, ret, sret, (void *)src, locale, sz, utf8,
             utf8 ? "PASSTHROUGH" : "real", buf);
    }
    return utf8;
}

/* 入参：rdi=sret(std::string*), rsi=char const*, rdx=Locale。返回 1 = 原样透传 */
__attribute__((visibility("hidden")))
int nwzh_toutf8_probe(void *sret, const char *src, long locale, unsigned long ret)
{
    static int n;
    g_toutf8_calls++;
    int utf8 = (src != NULL && src[0] != 0) ? nwzh_is_utf8_text(src) : 0;
    if (getenv("NWNZH_TOUTF8_DEBUG") && n++ < 30) {
        char buf[33];
        int i = 0, n2 = 0;
        if (src) {
            for (; i < 32 && src[i]; i++) {
                unsigned char c = (unsigned char)src[i];
                buf[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
                n2++;
            }
        }
        buf[i] = 0;
        sayf("[nwzh] toutf8 #%d ret=0x%lx src=%p loc=%ld len=%d utf8=%d -> %s \"%s\"\n",
             g_toutf8_calls, ret, (void *)src, locale, n2, utf8,
             utf8 ? "PASSTHROUGH" : "real", buf);
    }
    if (utf8) g_toutf8_hits++;
    return utf8;
}

/* 构造结果自检：确认返回的 std::string 内容与源串一致 */
__attribute__((visibility("hidden")))
void nwzh_verify_str(const void *st, const char *src)
{
    static int n;
    if (!getenv("NWNZH_TOUTF8_DEBUG") || n++ >= 10) return;
    const char *p = *(const char * const *)st;
    size_t sz = *(const size_t *)((const char *)st + 8);
    size_t srclen = src ? strlen(src) : 0;
    int match = (p != NULL && srclen == sz && memcmp(p, src, sz) == 0);
    sayf("[nwzh] toutf8 verify: data=%p size=%zu srclen=%zu match=%d\n",
         (void *)p, sz, srclen, match);
}

/* 诊断用：把 Nuklear 的字形查询改成「先调原函数、再记录码点与结果」 */
/* ── Encoding::FromUTF8 直通 ─────────────────────────────────────────
 * 只要输入是「含多字节且整体合法 UTF-8」，就原样构造 std::string 返回，
 * 不再做 UTF-8 -> cp1252 的转换；否则复原序言进原函数。
 * 开关：NWNZH_FROMUTF8=0 关闭。
 */
void nwzh_shim_fromutf8(void);

static int g_fu8_dbg = -1;
static int g_fu8_n;

int nwzh_fromutf8_decide(const char *src)
{
    int hit = (src != NULL && src[0] != 0) ? nwzh_is_utf8_text(src) : 0;
    if (g_fu8_dbg < 0) {
        const char *e = getenv("NWNZH_FROMUTF8_DEBUG");
        g_fu8_dbg = (e != NULL && e[0] == '1');
    }
    if (g_fu8_dbg && g_fu8_n < 30) {
        char hx[3 * 25 + 1];
        int hn = 0;
        for (int i = 0; i < 24 && src != NULL && src[i] != 0; i++) {
            static const char hexd[] = "0123456789abcdef";
            unsigned char c = (unsigned char)src[i];
            hx[hn++] = hexd[c >> 4];
            hx[hn++] = hexd[c & 15];
            hx[hn++] = ' ';
        }
        hx[hn] = 0;
        sayf("[nwzh] fu8 #%d passthru=%d hex=%s\n", g_fu8_n, hit, hx);
        g_fu8_n++;
    }
    return hit;
}

__asm__(
".globl nwzh_shim_fromutf8\n"
".type nwzh_shim_fromutf8,@function\n"
"nwzh_shim_fromutf8:\n"
"    push %rdi\n"
"    push %rsi\n"
"    push %rdx\n"
"    mov  8(%rsp), %rdi\n"
"    call nwzh_fromutf8_decide\n"
"    test %eax, %eax\n"
"    je   .Lnwzh_fu_real_pop\n"
"    mov  16(%rsp), %rdi\n"
"    mov  8(%rsp), %rsi\n"
"    lea  g_dummy_alloc(%rip), %rdx\n"
"    call *g_str_ctor(%rip)\n"
"    mov  16(%rsp), %rax\n"
"    add  $24, %rsp\n"
"    ret\n"
".Lnwzh_fu_real_pop:\n"
"    pop  %rdx\n"
"    pop  %rsi\n"
"    pop  %rdi\n"
"    push %rbp\n"
"    mov  %rsp, %rbp\n"
"    push %r15\n"
"    jmp *g_real_fromutf8(%rip)\n"
".size nwzh_shim_fromutf8,.-nwzh_shim_fromutf8\n"
);

static void install_fromutf8_fix(void)
{
    const char *mode = getenv("NWNZH_FROMUTF8");
    if (mode != NULL && mode[0] == '0') {
        sayf("[nwzh] FromUTF8 passthrough disabled by NWNZH_FROMUTF8=0\n");
        return;
    }
    if (g_str_ctor == 0) g_str_ctor = g_main.base + VA_STR_CTOR;
    g_real_fromutf8 = g_main.base + VA_FROMUTF8_BODY;
    static const unsigned char o[5] = {0x55, 0x48, 0x89, 0xe5, 0x41};
    patch_veneer("fromutf8-passthrough", VA_FROMUTF8_STR, o, nwzh_shim_fromutf8);
}

static void install_nk_trace(void)
{
    g_nk_trace = getenv("NWNZH_NK_TRACE") != NULL;
    if (!g_nk_trace) return;
    if (g_veneer == NULL) {
        sayf("[nwzh] nk trace skipped: no veneer page\n");
        return;
    }
    g_findglyph_body = g_main.base + VA_NK_FG_BODY;
    static const unsigned char o[5] = {0x0f, 0x84, 0xa6, 0x00, 0x00};
    if (patch_veneer("nk-find-glyph", VA_NK_FG_JE, o, nwzh_shim_findglyph))
        sayf("[nwzh] nk trace armed (log first 200 non-ASCII glyph lookups)\n");
}

/* 有些 fontinfo 的 +0x40 非零时，TextOut 会在 0x49625c 直接跳去
 * CAurFont::TextOutDoubleByte（0x496210）。这个函数在本版是个 16 字节空桩
 * （55 31 c0 48 89 e5 5d c3），于是**整段文字一个像素都不画**——表现就是
 * 「宽度量了（calc 有日志）、屏幕上是空的」。把这条分支 NOP 掉，让它走
 * 正常的单字节路径，由我们的 UTF-8 解码桩给出索引。
 * NWNZH_TEXTOUT_NODBC=0 可关掉。 */
static void install_textout_nodbc(void)
{
    static const unsigned char o[6] = {0x0f,0x85,0x0e,0x04,0x00,0x00};
    static const unsigned char r[6] = {0x90,0x90,0x90,0x90,0x90,0x90};
    if (patch_code("textout-nodbc", VA_TEXTOUT_DBC, o, r, 6))
        sayf("[nwzh] textout: double-byte dead-end bypassed (fi+0x40 flag ignored)\n");
}

static void install_text_decoder(void)
{
    g_resume_textout = g_main.base + 0x496521UL;
    g_calc_resume    = g_main.base + 0x496896UL;
    g_calc_markup    = g_main.base + 0x4969e8UL;
    g_calc_esi_resume= g_main.base + 0x4968bfUL;
    g_caret_resume   = g_main.base + 0x496bf2UL;

    g_veneer = (unsigned char *)alloc_veneer_page();
    if (g_veneer == NULL) {
        sayf("[nwzh] text decoder skipped: cannot map a page within +/-2GB of the exe\n");
        return;
    }
    sayf("[nwzh] veneer page at %p (exe base 0x%lx)\n",
         (void *)g_veneer, (unsigned long)g_main.base);

    /* 1) TextOut 取字节：41 0f b6 75 00 (movzx esi,[r13]) 正好 5 字节 */
    {
        static const unsigned char o[5] = {0x41, 0x0f, 0xb6, 0x75, 0x00};
        patch_veneer("textout-decode", VA_TEXTOUT_LOAD, o, nwzh_stub_textout);
    }
    /* 2) CalcVisible 取字节：44 0f b6 3e + 下一指令首字节 41 */
    {
        static const unsigned char o[5] = {0x44, 0x0f, 0xb6, 0x3e, 0x41};
        patch_veneer("calcwidth-decode", VA_CALC_LOAD, o, nwzh_stub_calc);
    }
    /* 3) CalcVisible 装索引：movzx esi,r15b / mov rdx,[rbp-0x78] 共 8 字节 */
    {
        static const unsigned char o[8] = {0x41, 0x0f, 0xb6, 0xf7, 0x48, 0x8b, 0x55, 0x88};
        static const unsigned char n[8] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90};
        unsigned char repl[8];
        memcpy(repl, n, 8);
        void *v = make_veneer(nwzh_stub_calc_esi);
        if (v == NULL) {
            sayf("[nwzh] patch calcwidth-esi @0x%lx skipped: no veneer\n",
                 (unsigned long)VA_CALC_ESI);
        } else {
            int64_t rel = (int64_t)((uintptr_t)v - (g_main.base + VA_CALC_ESI + 5));
            if (rel > 0x7fffffffLL || rel < -0x80000000LL) {
                sayf("[nwzh] patch calcwidth-esi @0x%lx skipped: out of range\n",
                     (unsigned long)VA_CALC_ESI);
            } else {
                memcpy(repl + 1, &rel, 4);
                patch_code("calcwidth-esi", VA_CALC_ESI, o, repl, 8);
            }
        }
    }
    /* 4) UpdateCaret 取字节：42 0f b6 34 30 正好 5 字节 */
    {
        static const unsigned char o[5] = {0x42, 0x0f, 0xb6, 0x34, 0x30};
        patch_veneer("caret-decode", VA_CARET_LOAD, o, nwzh_stub_caret);
    }
    /* 5) UpdateCaret 的 mov r13d,esi 要空掉，否则覆盖掉我们设的原字节 */
    {
        static const unsigned char o[3] = {0x41, 0x89, 0xf5};
        static const unsigned char n[3] = {0x90, 0x90, 0x90};
        patch_code("caret-keep-r13d", VA_CARET_MOVR13D, o, n, 3);
    }
}

static void install_coords_probe(void)
{
    static const unsigned char o[5] = {0x80, 0x7f, 0x68, 0x00, 0x55};
    g_coords_resume = g_main.base + VA_COORDS_RESUME;
    if (!g_coords_dbg) return;
    if (g_veneer == NULL) {
        sayf("[nwzh] coords probe skipped: no veneer page\n");
        return;
    }
    patch_veneer("coords-probe", VA_COORDS_ENTRY, o, nwzh_stub_coords);
}

/* 让 fnt_galahad14 找不到位图，从而掉进 CAurTextureBasic::LoadFromTTF */
static void install_image_shim(void)
{
    g_img_debug = getenv("NWNZH_IMG_DEBUG") != NULL;
    g_text_dbg  = getenv("NWNZH_TEXT_DEBUG") != NULL;
    {
        const char *td = getenv("NWNZH_TEXT_DEBUG");
        g_str_all = (getenv("NWNZH_STR_ALL") != NULL) ||
                    (td != NULL && td[0] >= '2');
    }
    {
        const char *dm = getenv("NWNZH_DEC_MAX");
        if (dm != NULL && dm[0] != '\0') {
            long v = strtol(dm, NULL, 10);
            if (v > 0 && v < 100000) g_dec_max = (int)v;
        }
    }
    {
        const char *av = getenv("NWNZH_ASCII_CLEAN");
        g_ascii_clean = (av != NULL && av[0] != '0');
    }
    {
        const char *rv = getenv("NWNZH_RELOCATE");
        if (rv == NULL) g_relocate = 1;
        else if (rv[0] == '0') g_relocate = 0;
        else if (rv[0] == '2') g_relocate = 2;
        else g_relocate = 1;
    }
    g_sym_probe = (getenv("NWNZH_SYM_PROBE") != NULL &&
                   getenv("NWNZH_SYM_PROBE")[0] == '1');
    g_digit_probe = (getenv("NWNZH_DIGIT_PROBE") != NULL &&
                     getenv("NWNZH_DIGIT_PROBE")[0] == '1');
    g_atlas_dbg = getenv("NWNZH_ATLAS_DEBUG") != NULL;
    g_dump_path = getenv("NWNZH_DUMP_ATLAS");
    g_coords_path = getenv("NWNZH_DUMP_COORDS");
    const char *sw = getenv("NWNZH_IMAGE_SHIM");
    if (sw != NULL && sw[0] == '0') {
        sayf("[nwzh] image shim disabled by NWNZH_IMAGE_SHIM=0\n");
        return;
    }
    g_real_img = g_main.base + VA_IMG_REAL;
    if (g_veneer == NULL) {
        sayf("[nwzh] image shim skipped: no veneer page\n");
        return;
    }
    static const unsigned char o1[5] = {0xe8, 0x94, 0xd1, 0xff, 0xff};
    static const unsigned char o2[5] = {0xe8, 0x28, 0xd1, 0xff, 0xff};
    static const unsigned char o3[5] = {0xe8, 0xbe, 0xd0, 0xff, 0xff};
    int a = patch_veneer_call("img-wrapper-1", VA_IMG_CALL1, o1, nwzh_shim_img);
    int b = patch_veneer_call("img-wrapper-2", VA_IMG_CALL2, o2, nwzh_shim_img);
    int c = patch_veneer_call("img-wrapper-3", VA_IMG_CALL3, o3, nwzh_shim_img);
    if (a && b && c)
        sayf("[nwzh] image shim armed: fnt_galahad14 -> TTF path (real=0x%lx)\n",
             (unsigned long)g_real_img);
}

static void nwzh_atexit(void)
{
    if (g_cjk || g_punct || g_ascii || g_invalid || g_oor)
        sayf("[nwzh] text stats(final): cjk=%lu punct=%lu ascii=%lu invalid=%lu oor=%lu\n",
             g_cjk, g_punct, g_ascii, g_invalid, g_oor);
}

/* ── 构造函数 ─────────────────────────────────────────────────────────── */
__attribute__((constructor))
static void nwzh_init(void)
{
    memset(&g_main, 0, sizeof(g_main));
    dl_iterate_phdr(find_main, &g_main);

    if (g_main.base == 0) {
        sayf("[nwzh] main executable not found, skipped\n");
        return;
    }

    const char *env = getenv("NWNZH_ATLAS");
    if (env != NULL && env[0] != '\0') {
        int v = atoi(env);
        if (v >= 1024 && v <= 16384 && (v & (v - 1)) == 0) {
            g_atlas_side_now = v;
            g_atlas_forced = 1;
            sayf("[nwzh] NWNZH_ATLAS=%d (forced)\n", v);
        } else {
            sayf("[nwzh] NWNZH_ATLAS=%s ignored (need power of 2 in 1024..16384)\n", env);
        }
    }

    /* ---------- G1：Nuklear 字形范围 ---------- */
    {
        static const unsigned char o[5] = {0xe8, 0xaa, 0xd4, 0xfd, 0xff};
        static const unsigned char n[5] = {0xe8, 0xba, 0xd4, 0xfd, 0xff};
        patch_code("nuklear-glyph-ranges", VA_G1, o, n, sizeof(o));
    }

    /* ---------- G1-B：默认 ranges 访问器改成返回 chinese ranges ---------- */
    {
        static const unsigned char o[1] = {0x78};
        static const unsigned char n[1] = {0x48};
        unsigned int *rx = (unsigned int *)(g_main.base + 0x136e060UL);
        unsigned int *rd = (unsigned int *)(g_main.base + 0x136e090UL);
        if (patch_code("nk-default-glyph-ranges", VA_G1B, o, n, sizeof(o))) {
            sayf("[nwzh] glyph ranges: default accessor -> chinese "
                 "U+%04X-U+%04X U+%04X-U+%04X U+%04X-U+%04X U+%04X-U+%04X U+%04X-U+%04X\n",
                 rx[0], rx[1], rx[2], rx[3], rx[4], rx[5], rx[6], rx[7], rx[8], rx[9]);
            sayf("[nwzh] glyph ranges: engine default was U+%04X-U+%04X\n", rd[0], rd[1]);
        }
    }

    /* ---------- 打包 padding（诊断/修复用，默认不动） ---------- */
    {
        const char *pv = getenv("NWNZH_PACK_PAD");
        if (pv != NULL && pv[0] != '\0') {
            int want = atoi(pv);
            if (want >= 0 && want <= 16) {
                unsigned char o[4] = {0x03, 0x00, 0x00, 0x00};
                unsigned char n[4];
                n[0] = (unsigned char)want; n[1] = 0; n[2] = 0; n[3] = 0;
                if (patch_code("aurora-pack-padding", VA_PACK_PAD, o, n, sizeof(o)))
                    sayf("[nwzh] pack padding -> %d\n", want);
            }
        }
    }

    /* ---------- G2-B：图集尺寸改成每轮现写 ---------- */
    {
        unsigned char b1o[5] = {0xbb, 0x01, 0x00, 0x00, 0x00};
        unsigned char b1n[5];
        unsigned char b3o[6] = {0x41, 0xbc, 0x01, 0x00, 0x00, 0x00};
        unsigned char b3n[6];
        static const unsigned char b2o[2] = {0x01, 0xdb};
        static const unsigned char b2n[2] = {0xeb, 0x04};
        static const unsigned char b4o[3] = {0x45, 0x01, 0xe4};
        static const unsigned char b4n[3] = {0xeb, 0x06, 0x90};

        unsigned side = (unsigned)g_atlas_side_now;
        b1n[0] = 0xbb;
        b1n[1] = (unsigned char)(side & 0xff);
        b1n[2] = (unsigned char)((side >> 8) & 0xff);
        b1n[3] = (unsigned char)((side >> 16) & 0xff);
        b1n[4] = (unsigned char)((side >> 24) & 0xff);
        b3n[0] = 0x41; b3n[1] = 0xbc;
        b3n[2] = (unsigned char)(side & 0xff);
        b3n[3] = (unsigned char)((side >> 8) & 0xff);
        b3n[4] = (unsigned char)((side >> 16) & 0xff);
        b3n[5] = (unsigned char)((side >> 24) & 0xff);

        int ok = 1;
        ok = patch_code("atlas-size-height", VA_B1, b1o, b1n, sizeof(b1o)) && ok;
        ok = patch_code("atlas-size-height-skip-pow2", VA_B2, b2o, b2n, sizeof(b2o)) && ok;
        ok = patch_code("atlas-size-width", VA_B3, b3o, b3n, sizeof(b3o)) && ok;
        ok = patch_code("atlas-size-width-skip-pow2", VA_B4, b4o, b4n, sizeof(b4o)) && ok;

        if (!ok) {
            sayf("[nwzh] atlas size NOT dynamic -> glyph hook skipped "
                 "(%d glyphs with the original formula would allocate %dx...)\n", NWZH_N_TOTAL, NWZH_N_TOTAL);
        } else {
            sayf("[nwzh] atlas size dynamic: initial %u, imm32 @0x%lx/0x%lx\n",
                 side, (unsigned long)VA_B1_IMM, (unsigned long)VA_B3_IMM);
            /* ---------- G2-A：钩子 ---------- */
            static const unsigned char a1o[3] = {0x48, 0x89, 0x10};
            static const unsigned char a1n[3] = {0x90, 0x90, 0x90};
            if (patch_code("aurora-ctor-keep-hook", VA_A1, a1o, a1n, sizeof(a1o)))
                install_ttf_hook();
            else
                sayf("[nwzh] glyph hook skipped: constructor would overwrite it\n");
        }
    }

    /* ---------- G2-C：符号坐标上界 0xff → 21247 ---------- */
    {
        static const unsigned char o[6] = {0x81, 0xfe, 0xff, 0x00, 0x00, 0x00};
        unsigned char n[6];
        n[0] = 0x81; n[1] = 0xfe;
        n[2] = (unsigned char)(NWZH_MAX_INDEX & 0xff);
        n[3] = (unsigned char)((NWZH_MAX_INDEX >> 8) & 0xff);
        n[4] = 0; n[5] = 0;
        patch_code("symbol-coords-hi", VA_C1, o, n, sizeof(o));
        patch_code("set-symbol-coords-hi", VA_C2, o, n, sizeof(o));
    }

    /* ---------- 第三阶段：TextOut 系列 UTF-8 解码 ---------- */
    g_font_dbg = (getenv("NWNZH_FONT_DEBUG") != NULL && getenv("NWNZH_FONT_DEBUG")[0] == '1');
    if (g_font_dbg) sayf("[nwzh] font probe armed\n");
    g_names_dbg = (getenv("NWNZH_FONT_NAMES") != NULL);
    if (g_names_dbg) sayf("[nwzh] font name trace armed\n");

    g_coords_dbg = (getenv("NWNZH_COORDS_DEBUG") != NULL && getenv("NWNZH_COORDS_DEBUG")[0] == '1');
    if (g_coords_dbg) sayf("[nwzh] coords probe armed (GetSymbolCoords entry)\n");

    install_text_decoder();
    if (!(getenv("NWNZH_TEXTOUT_NODBC") && getenv("NWNZH_TEXTOUT_NODBC")[0] == '0'))
        install_textout_nodbc();
    install_coords_probe();
    install_image_shim();
    install_fromutf8_fix();

    /* 字体名重定向（默认开，NWNZH_FONT_REMAP=0 关闭） */
    {
        const char *e = getenv("NWNZH_FONT_REMAP");
        if (e != NULL && e[0] == '0') g_font_remap = 0;
        g_real_getref = (getref_fn)(g_main.base + VA_GETREF_REAL);
        if (g_font_remap && in_main((void *)(g_main.base + VA_GETREF_CALL), 5)) {
            void *v = make_veneer((void *)nwzh_shim_getref);
            if (v != NULL) {
                unsigned char o[5], n[5];
                memcpy(o, (const void *)(g_main.base + VA_GETREF_CALL), 5);
                if (o[0] == 0xE8) {
                    n[0] = 0xE8;
                    *(int *)(n + 1) = (int)((long)v - (long)(g_main.base + VA_GETREF_CALL + 5));
                    patch_code("font-remap-gettref", VA_GETREF_CALL, o, n, 5);
                }
            }
        }
        sayf("[nwzh] font remap %s (galahad14 -> ttf fnt_maintext 17)\n",
             g_font_remap ? "armed" : "disabled");
    }
    install_toutf8_fix();
    install_nk_trace();
    atexit(nwzh_atexit);
}
