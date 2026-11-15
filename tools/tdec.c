/* tdec — 解码轨迹工具：把几个典型码点喂给 nwzh_decode，打印它算出的字形索引与
 * 字节步进，用来核对「渲染 / 宽度 / 光标」三处用的是同一套映射。
 *
 * 用法：./tdec [libnwzh.so 的路径]      默认 ./libnwzh.so
 */
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

typedef int (*dec_t)(const unsigned char *, int, int, int *);

int main(int argc, char **argv)
{
    const char *so = argc > 1 ? argv[1] : "./libnwzh.so";
    void *h = dlopen(so, RTLD_NOW);
    if (!h) { printf("dlopen: %s\n", dlerror()); return 1; }
    dec_t d = (dec_t)dlsym(h, "nwzh_decode");
    if (!d) { printf("dlsym: %s\n", dlerror()); return 1; }

    const char *cases[] = {"A","\xe4\xb8\x80","\xe3\x80\x82","\xef\xbc\x8c","\xef\xbc\x81",
        "\xe2\x80\x9c","\xef\xbc\x88","\xe3\x80\x81","\xe6\xb1\x89","\xe2\x82\xac","\x80","\xf0\x9f\x98\x80"};
    const char *names[] = {"ASCII A","U+4E00","U+3002 。","U+FF0C ，","U+FF01 ！",
        "U+201C “","U+FF08 （","U+3001 、","U+6C49 汉","U+20AC",  "孤立80","U+1F600"};

    for (unsigned i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
        int adv = 0;
        int idx = d((const unsigned char *)cases[i], (int)strlen(cases[i]), 0, &adv);
        printf("%-12s -> idx=%-6d (0x%04x) adv=%d\n", names[i], idx, idx, adv);
    }
    return 0;
}
