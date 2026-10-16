/*
 * fuzz-harness 通用模板 —— 闭源 Windows DLL 的 WinAFL persistent harness
 * 参考案例：QQ libvips-42.dll（16.72% -> 98.72%，见 references/libvips-case.md）
 *
 * 四件套已内嵌（预热 / 关缓存 / 单线程 / 每轮回收），按目标库替换占位符。
 *
 * 构建（必须带 PDB，-target_method 依赖符号）：
 *   cl /nologo /O2 /MD /Zi /utf-8 <name>.c /Fe:<name>.exe /link /DEBUG:FULL
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

/* ===== 目标库入口（按目标修改） ===== */
typedef void *(*PFN_parse)(const void *buf, size_t len, const char *opt, ...);
typedef void (*PFN_init)(const char *);
typedef void (*PFN_unref)(void *);
typedef void (*PFN_cache_off)(int);      /* libvips: vips_cache_set_max   */
typedef void (*PFN_set_threads)(int);    /* libvips: vips_concurrency_set */
typedef void (*PFN_err_clear)(void);     /* libvips: vips_error_clear     */

static PFN_parse     p_parse = NULL;
static PFN_unref     p_unref = NULL;
static PFN_err_clear p_errclr = NULL;

/* target function：必须 noinline；WinAFL 用 -target_method 定位它 */
__declspec(noinline) void fuzz_one(char *filename)
{
    FILE *f = fopen(filename, "rb");
    if (!f) return;

    /* 动态缓冲：不要写死大小截断大种子 */
    fseek(f, 0, SEEK_END);
    long cap = ftell(f);
    if (cap < 0 || cap > 64 * 1024 * 1024) { fclose(f); return; }
    rewind(f);
    unsigned char *buf = (unsigned char *)malloc((size_t)cap);
    if (!buf) { fclose(f); return; }
    size_t n = fread(buf, 1, (size_t)cap, f);
    fclose(f);
    if (n == 0) { free(buf); return; }

    void *im = p_parse(buf, n, "", NULL);

    /* 每轮回收：unref 结果 + 清错误缓冲（persistent 无累积） */
    if (im && p_unref) p_unref(im);
    if (p_errclr) p_errclr();

    free(buf);
}

/* 读一个预热样本并解析一次；文件缺失时静默跳过 */
static void warm_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return;
    unsigned char tmp[4096];
    size_t n = fread(tmp, 1, sizeof(tmp), f);
    fclose(f);
    if (n && p_parse) p_parse(tmp, n, "", NULL);
}

int main(int argc, char **argv)
{
    HMODULE h = LoadLibraryA("<目标.dll>");
    if (!h) { fprintf(stderr, "LoadLibrary failed: %lu\n", GetLastError()); return 2; }

    p_parse = (PFN_parse)GetProcAddress(h, "<parse_entry>");
    if (!p_parse) { fprintf(stderr, "missing export\n"); return 3; }

    PFN_init p_init = (PFN_init)GetProcAddress(h, "<init_entry>");
    if (p_init) p_init("<progname>");

    /* ===== 四件套 #2 关缓存/池、#3 单线程（存在才调） ===== */
    PFN_cache_off   p_coff = (PFN_cache_off)GetProcAddress(h, "<cache_off_fn>");
    PFN_set_threads p_thr  = (PFN_set_threads)GetProcAddress(h, "<set_threads_fn>");
    if (p_coff) p_coff(0);
    if (p_thr)  p_thr(1);

    p_unref  = (PFN_unref)GetProcAddress(h, "<unref_fn>");
    if (!p_unref) p_unref = (PFN_unref)GetProcAddress(GetModuleHandleA("<gobject.dll>"), "g_object_unref");
    p_errclr = (PFN_err_clear)GetProcAddress(h, "<err_clear_fn>");

    if (argc < 2) { fprintf(stderr, "usage: %s <input>\n", argv[0]); return 1; }

    /* ===== 四件套 #1 预热：三类样本必须在 target function 首次进入之前 ===== */
    warm_file(argv[1]);               /* ① 有效输入 */
    {                                 /* ② 完全不匹配的垃圾 */
        static const unsigned char garbage[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
        if (p_parse) p_parse(garbage, sizeof(garbage), "", NULL);
    }
    warm_file("warm_bad.bin");        /* ③ magic 匹配但内容损坏（各放 CWD） */
    warm_file("warm_valid.bin");      /* 可选：与真实种子同类的有效样本 */

    fuzz_one(argv[1]);
    return 0;
}
