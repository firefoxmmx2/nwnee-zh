/* nwzh 解码器单元测试：dlopen libnwzh.so，直接调 nwzh_decode 验索引与步进。
 * 用法：./test_decode libnwzh.so */
#include <stdio.h>
#include <dlfcn.h>

int main(int argc, char **argv)
{
    const char *so = argc > 1 ? argv[1] : "./libnwzh.so";
    void *h = dlopen(so, RTLD_LAZY);
    if (!h) { printf("dlopen: %s\n", dlerror()); return 1; }
    int (*dec)(const unsigned char *, int, int, int *) = dlsym(h, "nwzh_decode");
    if (!dec) { printf("no sym nwzh_decode\n"); return 1; }

    struct { const char *name; unsigned char b[8]; int len, off, want_idx, want_adv; } t[] = {
        {"ASCII 'A'",          {0x41},                    1, 0, 0x41, 1},
        {"U+4E00 一",          {0xE4,0xB8,0x80},          3, 0, 0x100, 3},
        {"U+9FFF",             {0xE9,0xBF,0xBF},          3, 0, 0x100 + 0x51FF, 3},
        {"U+6C49 汉",          {0xE6,0xB1,0x89},          3, 0, 0x100 + 0x1E49, 3},
        {"串内第2字 U+5B57",   {0xE4,0xB8,0x80,0xE5,0xAD,0x97}, 6, 3, 0x100 + 0x5B57 - 0x4E00, 3},
        {"C3 A9 (U+00E9)",     {0xC3,0xA9},                2, 0, ' ', 2},
        {"孤立 E4 结尾",       {0xE4},                    1, 0, ' ', 1},
        {"坏续字节 E4 41 41",  {0xE4,0x41,0x41},           3, 0, ' ', 1},
        {"非法 0x80",          {0x80},                    1, 0, ' ', 1},
        {"4字节 F0 9F 98 80",  {0xF0,0x9F,0x98,0x80},     4, 0, ' ', 4},
        {"越界 E4 B8 (len2)",  {0xE4,0xB8},                2, 0, ' ', 1},
    };
    int fail = 0, n = (int)(sizeof(t) / sizeof(t[0]));
    for (int i = 0; i < n; i++) {
        int adv = -1;
        int got = dec(t[i].b, t[i].len, t[i].off, &adv);
        int ok = (got == t[i].want_idx && adv == t[i].want_adv);
        if (!ok) fail++;
        printf("%-22s idx=%#06x adv=%d  expect=%#06x/%d  %s\n",
               t[i].name, got, adv, t[i].want_idx, t[i].want_adv, ok ? "OK" : "**FAIL**");
    }
    printf(fail ? "UNIT: %d FAILED\n" : "UNIT: all %d pass\n", fail ? fail : n);
    return fail ? 1 : 0;
}
