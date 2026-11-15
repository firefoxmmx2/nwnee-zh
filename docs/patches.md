# 补丁点清单

共 **22 处**，全部是 `nwmain-linux`（build **8193.37-17**，ELF64 PIE，not stripped）的 vaddr，
运行时加上主程序基址。每组补丁改之前都 `memcmp` 原字节；不符（游戏更新）就打
`[nwzh] patch <名字> @0x... skipped: unexpected bytes` 并跳过该处，不会写坏二进制。

除这 22 处之外，还有一件运行期的事：**短字形搬墨**（第六节），它不写任何指令字节，
只改烘焙好的图集像素和坐标数组。

## 一、字形范围（2 处）

让 Nuklear UI 和 Aurora 两边的字体烘焙都去要 CJK 字形，而不是只要 Latin-1。

| # | 地址 | 原字节 | 新字节 | 作用 |
|---|---|---|---|---|
| G1 | `0xe06961` | `e8 aa d4 fd ff` | `e8 ba d4 fd ff` | `nk_sdl_refresh_config` 里把 `nk_font_default_glyph_ranges` 的调用改成 `nk_font_chinese_glyph_ranges`；只动第 2 个字节 |
| G1-B | `0xde3e14` | `78` | `48` | `nk_font_default_glyph_ranges` 函数体内那条 `lea` 的 disp32，改成指向 chinese ranges 表 —— 一处改动覆盖 `nk_font_config` 和 `nk_font_atlas_bake` 两个兜底调用者 |

`nk_font_default_glyph_ranges` 前后是这样：

```
0xde3e10:  push rbp
0xde3e11:  lea  rax,[rip+0x58a278]   ; disp32 @0xde3e14 = 78 a2 58 00 -> 0x136e090 (default)
0xde3e18:  mov  rbp,rsp
0xde3e1b:  pop  rbp
0xde3e1c:  ret
```

`next-ip = 0xde3e18`，要指向 `0x136e060`（chinese ranges）只需 `disp32 = 0x58a248`，
即把 `0xde3e14` 的 `78` 改成 `48`。改动范围 `0xde3e14..0xde3e17`，离下一个函数
`nk_font_chinese_glyph_ranges`（`0xde3e20`）还有 8 字节，不会踩到 ranges 表本身。

## 二、Aurora 图集扩容（8 处）

Aurora（经典 GUI）的 TTF 图集原本只排 256 个字形。这一组把容量推到 **21880**，
并让图集边长按每轮实际字号现算。

| # | 地址 | 原字节 | 新字节 | 作用 |
|---|---|---|---|---|
| A1 | `0x1124b96` | `48 89 10` | `90 90 90` | `CAuroraInterface` 构造函数不再把真函数指针写回，保住 A2 的钩子 |
| A2 | `0x19abdd0` | `0x1126000` | `&nwzh_getttf` | `.bss` 里 8 字节的 `AurGetTTFTexture` 函数指针；全二进制只有 1 处间接调用（`0x4876f2`）、1 处赋值（`0x1124b96`，已被 A1 废掉） |
| B1 | `0x1132a6d` | `bb 01 00 00 00` | `bb <side> 00 00 00` | 图集高：`mov ebx,1` → `mov ebx,side`，跳过 `next_pow2` |
| B2 | `0x1132a98` | `01 db` | `eb 04` | 跳过高度方向的 `add ebx,ebx` 循环 |
| B3 | `0x1132aad` | `41 bc 01 00 00 00` | `41 bc <side> 00 00 00` | 图集宽：`mov r12d,1` → `mov r12d,side` |
| B4 | `0x1132ab8` | `45 01 e4` | `eb 06 90` | 跳过宽度方向的 pow2 循环 |
| C1 | `0x4856e0` | `81 fe ff 00 00 00` | `81 fe 77 55 00 00` | `GetSymbolCoords` 上界 `cmp esi,0xff` → `cmp esi,NWZH_MAX_INDEX`（21879 = `0x5577`） |
| C2 | `0x485750` | `81 fe ff 00 00 00` | 同上 | `SetSymbolCoords` 上界 |

B1/B3 的 imm32 在 `0x1132a6e` / `0x1132aaf`，**每轮烘焙前由 `nwzh_getttf` 现写**，
所以图集边长可以随字号变；「写 imm32 → 调真 Load → 读回结果」整段用 `pthread_mutex`
锁住，因为它是进程级全局指令。

### 为什么不改 `0x487636` 的 `0x100`

```
0x4875f2  lea r14,[rbp-0x440]     ; 码点数组在栈上
0x487636  mov DWORD PTR [rax],0x100
```

`rbp-0x440` 到 canary 正好 1024 字节 = 256 个 `int`。把 `0x100` 改成 21880，那个循环会
往上写 85KB，第一次就越过 canary → `__stack_chk_fail`。所以改走 A2 函数指针钩子：
由 `nwzh_getttf` 用静态数组喂 21880 个码点给真 `GetTTFTexture`。

### 槽位表

| 段 | 槽位起点 | 码点范围 | 个数 |
|---|---|---|---|
| latin | 0 | U+0000–U+00FF | 256 |
| cjk | 256 | U+4E00–U+9FFF | 20992 |
| punct | 21248 | U+3000–U+303F | 64 |
| fw | 21312 | U+FF01–U+FF5E | 94 |
| quote | 21406 | U+2018 / 2019 / 201C / 201D | 4 |
| dash | 21410 | U+2010–U+2015 | 6 |
| arrow | 21416 | U+2190–U+21FF | 112 |
| geo | 21528 | U+25A0–U+25FF | 96 |
| sym | 21624 | U+2600–U+26FF | 256 |

`NWZH_N_TOTAL = 21880`，`NWZH_MAX_INDEX = 21879 = 0x5577`，C1/C2 的 imm32 由它现算。

## 三、UTF-8 解码（6 处）

引擎三段热路径原本都是「取一个字节当字形索引」。这里换成「按 UTF-8 解出码点 → 算出槽位号」，
三处共用同一个 `nwzh_decode()`，且共用同一套步进，保证渲染、宽度、光标完全一致。

| # | 地址 | 原字节 | 新字节 | 作用 |
|---|---|---|---|---|
| N1 | `0x49625c` | `0f 85 0e 04 00 00` | `90 90 90 90 90 90` | 封掉 `TextOut` 里「`fontinfo+0x40` 非零就跳 `TextOutDoubleByte`」的分支。那个函数在本版是 16 字节空桩（`55 31 c0 48 89 e5 5d c3`），跳过去整段文字一个像素都不画 —— 表现是「宽度量出来了、屏幕上是空的」 |
| D1 | `0x496512` | `41 0f b6 75 00` | `E9 rel32` | `TextOut` 取字节处 → 解码桩（桩把 `r12d += adv-1`，配合引擎原有的 `add r12d,1` 变成 `+adv`） |
| D2 | `0x496888` | `44 0f b6 3e 41` | `E9 rel32` | `CalculateVisible…` 取字节处 → 解码桩；桩自己做 `cmp r15b,0x3c` 分流 |
| D3 | `0x4968b7` | `41 0f b6 f7 48 8b 55 88` | `E9 rel32` + `90 90 90` | 同一函数装索引处；桩从 `[rbp-0x70]` 读回 |
| D4 | `0x496bed` | `42 0f b6 34 30` | `E9 rel32` | `UpdateCaret` 取字节处 → 解码桩 |
| D5 | `0x496c01` | `41 89 f5` | `90 90 90` | 空掉 `mov r13d,esi`，保住原字节给 `cmp r13b,0xa` 换行判断 |

N1 可以用 `NWNZH_TEXTOUT_NODBC=0` 关掉（默认开）。关掉后遇到打了 `fontinfo+0x40` 标记的字体，
文字依然会整段消失 —— 那条路径里没有任何绘制代码。

解码规则（越界一律退化成空格，绝不读越界）：

| 首字节 | 条件 | 步进 | 结果 |
|---|---|---|---|
| `< 0x80` | — | 1 | 原字节（0–255） |
| `0xE4–0xE9` | 后两字节 `80–BF` | 3 | 命中某段码点范围 → 槽位号；否则空格 |
| `0xC2–0xDF` | 后一字节 `80–BF` | 2 | 同上 |
| `0xF0–0xF4` | 后三字节 `80–BF` | 4 | 空格（BMP 外不处理） |
| 其它 | — | 1 | 空格 |

`r13d` / `r15d` 只对纯 ASCII 保留原字节 —— 否则 `cmp r13b,0x3c`（`<` 标记）会被槽位号的低字节误判。

## 四、编码边界（2 处）

引擎在字符串**进出** UTF-8 边界时都按 cp1252 转码，两个方向各一处。

| # | 地址 | 原字节 | 新字节 | 作用 |
|---|---|---|---|---|
| U1 | `0x865f82` | `e8 39 77 d9 ff` | `E8 rel32` | NUI 那个唯一的 `Encoding::ToUTF8(char const*, Locale)` 调用点 → 已是合法 UTF-8 就直通，不再按 cp1252 重编码。主菜单阶段需要它，否则可能黑屏 |
| U2 | `0x5fd710` | `55 48 89 e5 41` | `E9 rel32` | `Encoding::FromUTF8` 入口 → 合法 UTF-8 直通；否则复原 6 字节序言跳回 `0x5fd716` 走原函数 |

U2 修的是「`repository.json` 里的中文变成等长 `?`」：`0x5fd8a6` 的
`mov BYTE PTR [rdx+rax*1],0x3f` 把 cp1252 无法映射的字符直接写成 `?`，
`ContentIndex::GetRepositories` 有 3 处调用它。同理，属性值里那些 `?` 也走这条路径。

## 五、字体 remap（4 处）

经典 GUI 的 `fnt_galahad14` 优先取 BIF 里的 `fnt_galahad14.tga` 位图，`override/` 里放 ttf 也白放。
E1–E3 让位图三次查找都失败，自然落到 `LoadFromTTF`；S1 再把字体名本身换成引擎 remap 表里的
`ttf fnt_maintext 17`（有 CJK 字形）。

| # | 地址 | 原字节 | 新字节 | 作用 |
|---|---|---|---|---|
| E1 | `0x487b27` | `e8 94 d1 ff ff` | `E8 rel32` | `AurGetImageWrapper` 调用 → shim，只对 `fnt_galahad14` 返回 NULL |
| E2 | `0x487b93` | `e8 28 d1 ff ff` | `E8 rel32` | 同上 |
| E3 | `0x487bfd` | `e8 be d0 ff ff` | `E8 rel32` | 同上（精确 resref 那次） |
| S1 | `0x496cc4` | `e8` 开头的 `call AurTextureGetReference` | `E8 rel32` | `CAurFont::CAurFont(char const*)` 里把 `fnt_galahad*` 换成 `ttf fnt_maintext 17`，取不到就退回原名 |

E1–E3 **必须是 `E8`（call）不能是 `E9`（jmp）**：`AurGetImageWrapper` 有 9 个参数，第 7/8/9 个在
`[rsp+8]` / `[rsp+16]` / `[rsp+24]`，这些偏移的前提是「返回地址已压栈」。用 `jmp` 会让三个栈参数
整体错位一格，`GetCompressedRaster` 拿到错的 this 直接段错误。

## 六、短字形搬墨（运行期改写数据，不改指令）

不算在 22 处补丁里，因为它一个指令字节都不改。发生在 `nwzh_getttf` 调完真
`AurGetTTFTexture` 之后（`src/nwzh.c` 的 `nwzh_relocate_short_glyphs()`）。

**问题**：引擎给每个字形的 UV 格子高度恒等于字号 —— 那是从基线往上开的**行框**，不是墨迹盒。
矮字形（`，。、；：！？`、空格、`-`、`:`）的行框上部装的是图集里**上一行字形的像素**，
画出来就和真字形叠成黑块；角色名里 `D/d/m/v` 头顶的小墨点也是同一来源。

**为什么不能擦**：图集按行打包，同一块像素会被相邻格子的 quad 采样。擦掉「属于别人的」那些行
会连带毁掉邻居 —— 实测擦 14 格的上部，35 个格子的字形跟着被削（`J` 从 75 个墨点掉到 11、
`d` 从 107 掉到 76）。

**做法**：在既有格子的最深底边之下 4 像素处找一块安全空白，把该格子**整格复制**过去 ——
只复制 `[b, bot]` 这一段墨迹行（`b` 是顶行残影结束的位置，`bot` 是墨迹底边），
墨迹在格内的行号保持不变（基线对齐），其余留白；然后把该槽位的 `u0/u1` 改指新位置，
`u1-u0` 保留原始小数宽度，所以 advance 和字宽不变。**原槽位一个像素都不动**，
其它槽位的坐标条目一个都不动。

判据（保守优先，宁可不搬也不切坏字形）：

- 只处理 `U+3000` 之后（槽位 21248 起）、空白字符、可打印 ASCII `0x21..0x7E`；
- 字母/数字：顶行残影超过 4 行或连通墨点超过 `2×宽` 就不搬（上伸笔画会和残影连成一片）；
- ASCII 标点：残影超过 10 行、或下半格墨迹超过 10 行就不搬；
- 汉字区：下半格还有超过 12 行墨就不搬；
- 还要验证残影确实与格子上方（`y0-1` 行）连通，否则它是字形自己的一部分。

个别字烘出来整格是空的（`fnt_maintext` 17 的 `U+FF0C` 就是：格子里只有上一行残影）。
这种情况按索引缓存「下半格确实有墨」的同类矮字形，用最近邻缩放补贴一份（`refills` 计数）。

`NWNZH_RELOCATE=0` 关闭；`=2` 是判别实验（让 `U+FF0C` 借 `U+3002` 的格子，验证引擎是否真的
在用被改过的坐标数组）。

## veneer 页

主程序 PIE 在 `0x55..`、`.so` 在 `0x7f..`，相距远超 ±2GB，`E9 rel32` 跳不到 `.so` 里的桩。
启动时在 exe 附近（±2GB 内，`MAP_FIXED_NOREPLACE` 找空页）mmap 一页 RWX，
每个调用点跳 12 字节的 `movabs $handler,%rax; jmp *%rax`。

只有 `E9`/`E8` 那几处用 veneer；G1、G1-B、B2、B4、C1、C2、D5、N1 是就地改字节。

## 诊断探针（默认全部关闭）

这些不写进上面 22 处，不开时一个字节都不写：

| 探针 | 环境变量 | 额外改动 |
|---|---|---|
| ToUTF8 全局钩子 | `NWNZH_TOUTF8=1` | 把 `0x865f82` 那条从「NUI 单点」改成挂函数入口的全路径钩子（主菜单黑屏时用） |
| ToUTF8 跟踪 | `NWNZH_TOUTF8_TRACE=1` | 同上位置，加日志 |
| 字形查询跟踪 | `NWNZH_NK_TRACE=1` | patch `0xde3e53` 的 `je`，记录前 200 次非 ASCII 字形查询 |
| 坐标查询跟踪 | `NWNZH_COORDS_DEBUG=1` | patch `0x4856d0`（`GetSymbolCoords` 入口）跳 veneer |
| 打包 padding | `NWNZH_PACK_PAD=<0..16>` | patch `0x1132b25` 的 imm32（`mov r9d,0x3` → 新值），观察行框重叠 |
| 擦像对比 | `NWNZH_ASCII_CLEAN=1` | 不改指令；烘焙后擦 ASCII 格子上部（会伤邻居，仅作对照） |
| 图集/坐标落盘 | `NWNZH_DUMP_ATLAS` / `NWNZH_DUMP_COORDS` | 不改指令；把 `AurTTFData` 落成 `.raw`（RGBA，4 字节/像素）和坐标表 `.txt` |
