# nwzh

给 **Neverwinter Nights: Enhanced Edition** 的 Linux 版补上中文渲染。

一个 `LD_PRELOAD` 的 `.so`，在进程内存里改二十几个字节，让 Nuklear UI 和 Aurora 经典 GUI
两条渲染路径都能正确显示中文。不写磁盘、不改游戏文件、不改存档，删掉 `LD_PRELOAD` 即恢复原样。

## 原理

游戏在 Linux 上不带 CJK 支持，问题分三处：

1. **Nuklear UI** 只向字体图集要 `0x20–0xFF` 的字形，中文根本没进图集；
2. **Aurora 经典 GUI** 的 TTF 图集只有 256 个槽位，而且把 UTF-8 的每个字节当成一个字符；
3. 引擎在字符串**进出** UTF-8 边界时都按 cp1252 转码，中文在渲染前就变成了等长 `?`。

对应 **22 个补丁点**（地址、原字节、新字节见 [docs/patches.md](docs/patches.md)），
按要解决的问题分成六块：

| 环节 | 做法 | 点数 |
|---|---|---|
| 字形范围扩展 | 把一个调用点换成 `nk_font_chinese_glyph_ranges`，再把默认 ranges 访问器整条指过去 | 2 |
| 图集扩容 | 钩住 `AurGetTTFTexture` 喂 21880 个码点；图集边长按每轮字号现算；坐标数组上界同步放大 | 8 |
| UTF-8 解码 | `TextOut` / 宽度计算 / 光标三处换成同一个解码函数，按 UTF-8 步进并映射成槽位号；另外封掉 `TextOutDoubleByte` 那条画不出东西的死路 | 6 |
| ToUTF8 透传 | `Encoding::ToUTF8` 已经是 UTF-8 就直通，不再按 cp1252 逐字节重编码 | 1 |
| FromUTF8 透传 | `Encoding::FromUTF8` 遇到合法 UTF-8 直接透传，不再把字符写成 `?` | 1 |
| 字体 remap | 让 `fnt_galahad14` 找不到位图、落到 TTF 路径，并换成引擎表里的 CJK 字体 | 4 |
| 短字形搬墨 | 运行时把「格子上半是上一行残影」的矮字形搬到图集空白处（不改任何指令字节） | — |

最后一项不是指令补丁，而是运行期改写图集的像素与坐标数组：引擎给的 UV 格子高度恒等于字号
（行框，不是墨迹盒），矮字形（`，。、；：` 空格、连字符）的格子上部装的是上一行字形的像素，
画出来会叠成黑块。**不能直接擦掉那些像素** —— 同一块像素会被多个 quad 采样，擦一处会连带
毁掉邻居（实测只清 14 格，35 格跟着坏），所以改成把只含自己墨迹的整高格子复制到下方的空白区、
再把该槽位的 `u` 指过去。原槽位一个像素都不动。

因为主程序（PIE，`0x55..`）和 `.so`（`0x7f..`）相距超过 ±2GB，`E9 rel32` 跳不到，
启动时在主程序附近 mmap 一页 RWX 当 veneer，每个调用点跳 `movabs $handler,%rax; jmp *%rax`。

更细的推导、踩过的坑（寄存器契约、栈对齐、指针语义、搬墨判据）在 [docs/design.md](docs/design.md)。

## 依赖

- `nwmain-linux` **8193.37-17**（Steam build `20277208`，ELF64 PIE，not stripped）
- gcc（或任何支持 x86-64 inline asm 的 C 编译器）、glibc、`-ldl -lpthread`
- 一份**含 CJK 字形的静态 TTF**，例如汉化包里的 `NotoSansSC-Regular-static.ttf`
- 正版游戏本体

本仓库**不含**任何字体、`.tlk`、GUI 资源或游戏文件。

## 编译

```sh
make            # → libnwzh.so（= gcc -shared -fPIC -O2 -Wall -Wextra src/nwzh.c -ldl -lpthread）
make test       # 编译并跑解码器单元测试（11 例）
make install    # → $(HOME)/.local/lib/nwn-zh/libnwzh.so
```

## 用法

把字放进游戏的用户目录 `override/`，四份正文字体加一份 NUI 等宽字体，缺任何一份都会有控件缺字：

```
override/fnt_default.ttf
override/fnt_default_hr.ttf
override/fnt_maintext.ttf
override/fnt_galahad14.ttf     ← 位图优先，靠 remap 才落到 TTF
override/jbmonoreg.ttf         ← NUI 等宽字体
```

这四份**内容可以完全相同** —— 就是把同一份 `NotoSansSC-Regular-static.ttf` 复制改名，
运行时按档位各自烘焙 14 / 16 / 17 三个字号。字体必须是**静态 TTF（`glyf` 轮廓）**；
**TTC 和 CFF（OpenType、`.otf`）都不行**，引擎用的是 stb_truetype，只认 `glyf`。

### Steam

库 → 右键游戏 → 属性 → 启动选项：

```
env NWNZH_TOUTF8=1 LD_PRELOAD=$HOME/.local/lib/nwn-zh/libnwzh.so %command%
```

### 命令行

必须从游戏的 `bin/linux-x86` 目录启动，否则会报 `Could not divine nwn base data directory`：

```sh
cd "$HOME/.steam/steamapps/common/Neverwinter Nights/bin/linux-x86"
LD_PRELOAD="$HOME/.local/lib/nwn-zh/libnwzh.so" ./nwmain-linux
```

日志在 stderr 上，每组补丁一行 `[nwzh] patch <名字> @0x... ok`；
出现 `skipped: unexpected bytes` 说明游戏版本不是 8193.37-17。

## 环境变量

| 变量 | 默认 | 作用 |
|---|---|---|
| `NWNZH_TOUTF8=1` | — | **建议开启**。主菜单阶段需要它，否则可能黑屏 |
| `NWNZH_TOUTF8=0` | — | 关闭 ToUTF8 补丁 |
| `NWNZH_FROMUTF8=0` | — | 关闭 FromUTF8 直通（`repository.json` 里的中文会变 `?`） |
| `NWNZH_RELOCATE=0` | 开 | 关闭短字形搬墨（矮字形会重新出现上一行残影） |
| `NWNZH_RELOCATE=2` | — | 判别实验：让 U+FF0C 借用 U+3002 的格子，验证坐标数组是否真的被引擎采用 |
| `NWNZH_FONT_REMAP=0` | — | 关闭字体名重定向（`fnt_galahad14` → `ttf fnt_maintext 17`） |
| `NWNZH_IMAGE_SHIM=0` | — | 不 patch 那三处位图查找（E1–E3） |
| `NWNZH_TEXTOUT_NODBC=0` | 开 | 保留 `TextOutDoubleByte` 那条死路（文字可能整段不画） |
| `NWNZH_ATLAS=<n>` | 4096 | 手工指定图集边长，2 的幂，1024–16384 |
| `NWNZH_TEXT_DEBUG=1` | — | 打前 8 次 `TextOut` 的长度与字符串；`=2` 记录全部 |
| `NWNZH_ATLAS_DEBUG=1` | — | 打若干探针槽位的实际坐标 |
| `NWNZH_IMG_DEBUG=1` | — | 打前 40 个经过 image shim 的 resref |
| `NWNZH_FONT_DEBUG=1` | — | 字体探针 |
| `NWNZH_FONT_NAMES=1` | — | 打前 60 次 getref 的字体名 |
| `NWNZH_NK_TRACE=1` | — | 额外 patch 一处 `je`，记录前 200 次非 ASCII 字形查询 |
| `NWNZH_COORDS_DEBUG=1` | — | 额外 patch `GetSymbolCoords` 入口，跟踪坐标查询 |
| `NWNZH_ASCII_CLEAN=1` | 关 | **实验项**：擦掉 ASCII 格子上部的残影像素。会连带伤到共享同一像素的邻居，仅用于对比 |
| `NWNZH_PACK_PAD=<n>` | 3（不动） | **实验项**：改 `stbtt_PackBegin` 的字形 padding（0–16），用来观察行框重叠 |
| `NWNZH_DUMP_ATLAS=<path>` | — | 把烘焙好的图集按 RGBA 原样落盘（调试用，`.raw`，可用 ImageMagick 转 PNG） |
| `NWNZH_DUMP_COORDS=<path>` | — | 把字形坐标表落盘 |

调试项默认全部关闭；只有显式设成 `1`（或给了路径）才会 patch 或写文件。
`NWNZH_PACK_PAD` 不设置时一个字节都不动。

## 已知限制

早期版本里的「`?` 与属性值符号显示异常」**已经解决**：`?` 是 `Encoding::FromUTF8` 的 cp1252
回退写出来的（现由 FromUTF8 透传修掉），属性值里的方块则是矮字形格子顶部混入了上一行字形的
像素（现由搬墨修掉）。剩下的限制是：

1. **图集默认 4096²**，字号很大时可能装不下；占用超过 70% 且字号 > 18 才会自动升到 8192²。
2. **只支持 8193.37-17** 这一份二进制。游戏一更新，所有补丁点都会因为原字节不符而跳过。
3. 只有 BMP 内的码点；`0xF0` 打头的四字节 UTF-8 一律退化成空格。
4. 搬墨只在「下半格墨迹够短」时触发；带声调的拉丁字母和上伸笔画一旦与残影连成一片就不搬
   （宁可不搬，也不切坏字形）。
5. 图集空白区搬满后（`relocate: atlas full`）后面的矮字形不再搬，会退回残影状态。
6. 纯软件渲染（无 `/dev/dri`）下主菜单可能整屏黑，这属于渲染后端问题，不是本补丁的 bug。

## 致谢

- [nwn_ee_korean](https://github.com/wonilclone/nwn_ee_korean) —— 韩国社区把同一类思路做在了 Windows 版上，
  这份实现的字形范围与图集扩容方案受其启发。
- 中文汉化包的作者们 —— 字体选型、`.tlk` 与 UI 资源的中文化是这一切的前提。

## 免责声明

本工具**只做运行期内存补丁**，不修改任何游戏文件、不包含任何游戏资源，仅供个人学习与研究使用。
使用前请自备正版游戏与合法的汉化资源。作者不对因使用本工具产生的任何后果负责。

## 许可

MIT，见 [LICENSE](LICENSE)。
