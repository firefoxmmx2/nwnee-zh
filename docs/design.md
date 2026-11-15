# 原理

## 问题

NWN:EE 的 Linux 版（`nwmain-linux`）在中文环境下面临三个独立的问题：

1. **Nuklear UI（NUI）** 只请求 `0x20–0xFF` 的字形范围，中文字形根本没进图集；
2. **Aurora 经典 GUI** 的 TTF 图集只留了 256 个字形槽位（Latin-1），同时它取字节当索引，
   一个 UTF-8 三字节汉字会被当成三个单字节字符；
3. 引擎在字符串进出 UTF-8 边界时按 cp1252 转码，中文在到达渲染前就已经变成 `?`。

这三条是两条**不同的渲染路径**，需要分别处理。

| 组 | 目标 | 状态 |
|---|---|---|
| 字形范围 | Nuklear UI 的字形范围（`nk_sdl_refresh_config` 的调用点 + 默认 ranges 访问器） | 已实测 |
| 图集扩容 | Aurora TTF 图集的字形容量（256 → 21880） | 已实测 |
| UTF-8 解码 | `TextOut` / 宽度计算 / 光标三处一致地按 UTF-8 步进 | 已实测 |
| 字体 remap | 让 `fnt_galahad14` 走 TTF，并映射到含 CJK 的字体 | 已实测 |

全部只改进程内存，不写磁盘上任何游戏文件；不设 `LD_PRELOAD` 即完全恢复。

补丁点的地址与原字节见 [patches.md](patches.md)。

---

## 一、Nuklear 字形范围

引擎在 `0xe06961` 调 `nk_font_default_glyph_ranges`（只含 `0x20–0xFF`）。
把它改成 `nk_font_chinese_glyph_ranges` 即可：

```
0xe06961:  e8 aa d4 fd ff   call 0xde3e10  nk_font_default_glyph_ranges
       ↓ 只改 [1]: aa → ba
0xe06961:  e8 ba d4 fd ff   call 0xde3e20  nk_font_chinese_glyph_ranges
                            (+0x3000-0x30FF, 0x31F0-0x31FF, 0xFF00-0xFFEF, 0x4E00-0x9FAF)
```

实测（同一 build，只换 `lang` 字体来源，看 `nuklear.cpp:270` 的
`NUI: Font bake complete; texture is NxNpx`）：

| 字体（resref `fnt_maintext`） | 打补丁 | 图集大小 |
|---|---|---|
| 原版（`lang/en`） | 打 | `1024x64px` |
| 中文 Noto Sans SC | **打** | **`1024x8192px`** |
| 中文 Noto Sans SC | 不打 | `512x128px` |

1 字节补丁确实生效并流进 `nk_font_atlas_bake`（面积差 128 倍），GL 纹理上限不是问题；
但只改 range 不够 —— **字体本身必须含 CJK 字形**，否则 stb 跳过找不到的码点，图集尺寸不变。

只看单个调用点不够：`nk_font_default_glyph_ranges` 还有另外两个调用者 —— `nk_font_config`
（`0xde4155`）和 `nk_font_atlas_bake`（`0xde9280`，字体没给 ranges 时的兜底）。
走兜底的那些字体（例如 NUI 的等宽字体 `jbmonoreg`）只拿到 `0x20–0xFF`，中文全缺 → 显示成 `?`。
与其逐个改调用点，不如把**函数体本身**改掉（G1-B），一处覆盖三处。

---

## 二、Aurora 图集扩容

Aurora 的 TTF 图集由 `CAurFont::LoadFromTTF` 烘焙，原来：

```
图集宽 = next_pow2(count*3+3 + (int)(count*fontsize))
图集高 = next_pow2(fontsize+6)
```

`count = 256` 时这些式子还算得动；`count = 21880` 时宽度会算成 42 万，必然分配失败。
所以改成**按每轮实际字号现算边长**（B1–B4），而 `count` 通过 A2 的函数指针钩子喂进去。

函数指针钩子这条路比「改 `0x487636` 的 `0x100`」安全：后者会让那个循环往上写 85KB 栈内存，
第一条就越过 canary。由 `nwzh_getttf` 用静态数组喂 21880 个码点给真 `GetTTFTexture`，
既不碰栈布局，也不改 `fontinfo->numchars`（保持 256，`LoadFromTTF` 尾部按 `Load` 返回的
count 扩容坐标数组，所以数组大小不受影响）。

### 索引方案：纯算术，无码点表

```
index = 256    + (cp - 0x4E00)    cp ∈ U+4E00..U+9FFF   汉字
index = 21248  + (cp - 0x3000)    cp ∈ U+3000..U+303F   CJK 标点
index = 21312  + (cp - 0xFF01)    cp ∈ U+FF01..U+FF5E   全角 ASCII
index = 21406  + i                cp ∈ {U+2018,2019,201C,201D}   弯引号
index = 21410  + (cp - 0x2010)    cp ∈ U+2010..U+2015   连字符/破折号
index = 21416  + (cp - 0x2190)    cp ∈ U+2190..U+21FF   箭头
index = 21528  + (cp - 0x25A0)    cp ∈ U+25A0..U+25FF   几何符号（▪ 等）
index = 21624  + (cp - 0x2600)    cp ∈ U+2600..U+26FF   杂项符号（♀ ♂ 等）
```

槽位 `0..255` 保留原来那 256 个 Latin-1 码点（调用方传入的 `codepoints` 数组原样保留）。
解码头只要算出同一个 index 就能命中，**不需要维护任何码点表**。

符号那三段是后补的：角色卡/对话里出现过 `▪`（行首方块）、`♦`（应是 ♀）、`?`（应是 —、-、%），
它们原本落在范围外 → 返回空格 → 在等宽字体里表现为方块。

### 图集边长策略

stb 每格约 `(fontsize + 6)²`（字形框 + 两侧各 3px padding），面积 = `slots × (fs+6)²`：

| 字号 | 面积 | 4096²(16.78M) 占用 |
|---|---|---|
| 14（实测 fnt_default） | 8.50M | 50.7% |
| 17（实测 fnt_maintext） | 11.24M | 67.0% |
| 18 | 12.24M | 73% |
| 20 | 14.36M | 86% |

- 默认 **4096²**（RGBA 64MB）
- 仅当占用 > 70% **且** 字号 > 18 时升 **8192²**（RGBA 256MB），升之前打
  `[nwzh] atlas escalate 4096->8192 area=… rss=…`
- `NWNZH_ATLAS=8192` 手工强制（必须是 2 的幂，1024..16384）

### 短字形搬墨（行框 vs 墨迹盒）

扩容解决的是「装不装得下」，搬墨解决的是「装进去了但格子里混进了别人的像素」。

引擎给每个字形分配的 UV 格子高度**恒等于字号**。这不是墨迹盒，是从基线往上开的**行框带**：
字号 17 就永远给 17 行，不管这个字实际只有 7 行墨。矮字形（`，。、；：！？`、空格、`-`、`:`）
的行框上部就落在图集里**上一行字形**的像素上，画出来和真字形叠成黑块；
角色名里 `D/d/m/v` 头顶的小墨点也是同一来源。

第一版的做法是「擦掉格子顶部那几行」，结果是：

> 擦 14 个格子的上部，**35 个格子的字形跟着被削**。`J` 的墨点从 75 掉到 11，`d` 从 107 掉到 76。

原因是图集按行紧密打包（padding 只有 3px），**同一块像素会被相邻格子的 quad 一起采样**。
你以为在擦「不属于这个字的残影」，实际擦的是下面那个字的真实笔画。

所以改成搬：在既有格子的最深底边之下 4px 处找一块安全空白（那里没有任何 quad 会采样），
把该格子整格复制过去，但只复制 `[b, bot]` 这一段墨迹行 ——

- `b`：从格子顶部往下扫，第一次出现整行空白的位置。`0..b-1` 是上一行的残影，不搬；
- `bot`：`b` 之后最后一段墨迹的底边。格子底部往往也带着「下一行字形的顶」，一起搬就是新的残影，
  所以尾部那些贴着格底、又薄（≤5 行）的段直接丢掉；
- 墨迹在格内的行号不变，所以基线对齐、字在行里的位置一点没动；
- 只把该槽位的 `u0/u1` 改到新位置，`u1-u0` 保留原始小数宽度 —— advance 和字宽不变。

**原槽位一个像素都不动**，所以哪怕判据判错了，坏的也只是那个字（多一条残影），不会伤到邻居。

判据全是保守的，宁可少搬：

| 类别 | 条件 |
|---|---|
| 适用范围 | `U+3000` 之后的槽位（21248 起）、空白字符、可打印 ASCII `0x21..0x7E` |
| 字母/数字 | 残影 > 4 行，或连通墨点 > `2×宽` 就不搬（上伸笔画和残影连成一片，切了就掉重音符） |
| ASCII 标点 | 残影 > 10 行，或下半格墨迹 > 10 行就不搬 |
| 汉字 | 下半格墨迹 > 12 行就不搬 |
| 连通性 | 格子顶行的墨必须与 `y0-1` 行的墨连通，否则那本来就是字形自己的一部分 |

实测数据说明了为什么不能只看「顶行有墨」：`È/É/Ý`（idx 200/201/221）和部分汉字也满足
「顶行连通 + 下方有墨」，早期版本搬完之后这些字母的重音符从 115/133/116 个墨点掉到 7/8/10。
现在字母/数字走单独的判据，`0x21..0x7E` 里只搬标点和符号。

还有一类是「格子本身就是空的」：`fnt_maintext` 17 的 `U+FF0C` 烘出来只有上一行残影，
下半格一个墨点都没有，搬过去是一片空白。这种情况按索引缓存「下半格确实有墨」的同类矮字形
（最多 96 个），用最近邻缩放补贴一份（日志里的 `refill=`）。

搬墨在 `nwzh_getttf` 里、调完真 `AurGetTTFTexture` 之后执行，日志形如：

```
[nwzh] relocate short glyphs: moved=37 refill=3 px=14450 free_from_y=1122 cursor_y=1140 atlas=4096x4096
```

图集空白区用尽时打 `[nwzh] relocate: atlas full at slot N` 并停止，剩下的矮字形退回残影状态。
`NWNZH_RELOCATE=0` 关闭。

`NWNZH_RELOCATE=2` 是判别实验，不是功能：它让 `U+FF0C` 的坐标条目直接改成 `U+3002` 的那一条。
如果屏幕上逗号的位置画出圆圈，说明引擎确实在用被改过的坐标数组；如果还是逗号，说明引擎在
`GetTTFTexture` 之后把坐标复制走了 —— 那样搬墨改 `u` 就没用，得换别的路子。

---

## 三、UTF-8 解码

三段热路径原来都是「取一个字节当字形索引」，现在换成「解出码点 → 算槽位号」，
三处共用同一个 C 函数 `nwzh_decode()`，保证渲染、宽度、光标完全一致。

| 位置 | 引擎原指令 | 作用 |
|---|---|---|
| `0x496512` | `movzx esi,[r13]` | `TextOut` 取字节 |
| `0x496888` | `movzx edi,[rsi]` 附近 | `CalculateVisibleStringLengthAndWidth` 取字节 |
| `0x4968b7` | `movzx esi,r15b` | 同一函数装索引 |
| `0x496bed` | `movzx esi,[rax+rsi]` | `UpdateCaret` 取字节 |

### 指针语义（踩过的坑，别改回去）

引擎在 `0x4964f6` 就已经把指针前进过了（`movsxd r13,r12d` + `add r13,[rbp-0x90]`），
`0x496512` 是 `movzx esi,[r13]`；`CalculateVisible` 那边同理。
**所以桩里必须传 `off = 0`、`len = 剩余字节数`。**

如果同时传指针和偏移，实际读到的是 `text + 2*offset` —— 表现是「每隔一个字符读一个」，
串尾再往后读 2–3 个字节的邻居内存（常见是 `0x00` 和 `0x20`），于是 12 字节的 `创建新档` 被解成
`创、新、00、00、20、00、00、00`，画面上就是「只有两个汉字 + 一串竖线」。

### 寄存器契约（踩过两次同一个坑）

`nwzh_decode` 是 C 函数，`%rax/%rcx/%rdx/%rsi/%rdi/%r8-%r11` 全是 caller-saved，
桩在 `call` 之前必须把自己「借走」的每个寄存器都存下来。

| 桩 | 必须保住 | 为什么 |
|---|---|---|
| `nwzh_stub_textout`（`0x496512`） | rdi/rdx/rcx/r11 | 跳回 `0x496521` 后引擎还要用 rdx/rcx |
| `nwzh_stub_calc`（`0x496888`） | **rsi** + rdi/rdx/rcx/r11 | `je 0x4969e8` 的 markup 分支用 `rsi = text+ebx` 当 `ProcessTextMarkup` 第 2 参 |
| `nwzh_stub_caret`（`0x496bed`） | **rax**（字符串基址）+ rdi/rdx/rcx/r11 | 跳回 `0x496bf2` 后引擎继续用 rax 取字符 |
| `nwzh_stub_calc_esi`（`0x4968b7`） | 无（只写 esi/rdx 后跳 `0x4968bf`） | 不在 markup 路径上 |

**栈对齐**：入口 `rsp%16 == 8`（call 压了返回地址）。让 `call nwzh_decode` 时 `rsp%16 == 0`：

- 4 个 push（32B）→ 仍 8 → 要 `sub $8`
- 5 个 push（40B）→ 0 → `sub $16` 仍为 0

少一个 `push %rsi` 的症状是「新建角色 / 预设角色必崩」，崩栈是
`ProcessTextMarkup+0x1f` ← `CalculateVisibleStringLengthAndWidth+0x30a`。

---

## 四、字体 remap

`CAurTextureBasic::LoadImage` 先找位图（BIF 里有 `fnt_galahad14.tga`），找到就不看 TTF，
所以 `override/fnt_galahad14.ttf` 白放。三处 `call AurGetImageWrapper` 换成 `call shim`，
shim 只对这一个 resref 返回 NULL → 三次都失败 → 自然落到 `0x487c57 LoadFromTTF`。
再叠加 S1，把字体名直接换成 `ttf fnt_maintext 17`，用引擎自己的 remap 表。

**必须用 `E8`（call）不能用 `E9`（jmp）**：`AurGetImageWrapper` 有 9 个参数，第 7/8/9 个在
`[rsp+8]` / `[rsp+16]` / `[rsp+24]`，这些偏移的前提是「返回地址已压栈」。用 `jmp` 会让三个
栈参数整体错位一格，`GetCompressedRaster` 拿到错的 this 直接段错误。

---

## 五、veneer 页

主程序 PIE 在 `0x55..`、`.so` 在 `0x7f..`，相距远超 ±2GB，`E9 rel32` 跳不到 `.so` 里的桩。
所以启动时在 exe 附近（±2GB 内，`MAP_FIXED_NOREPLACE` 找一个空页）mmap 一页 RWX，
每个调用点跳 12 字节的 `movabs $handler,%rax; jmp *%rax`。

---

## 六、并发

B1/B3 的 imm32 是**进程级全局指令**，所以「写 imm32 → 调真 Load → 读回结果」整段用
`pthread_mutex` 锁住。日志里带 `tid=`，实测两个字号都是同一个主线程、先后烘焙。

---

## 七、怎么确认生效

1. 每组补丁一行 `[nwzh] patch <名字> @0x... ok`；
2. `[nwzh] bake ...` 一行给出 `slots / side / area / fill / count / coords_nonzero / missing / rss`；
3. `coords_nonzero < count` 时会额外打
   `[nwzh] atlas insufficient: need=N got=M missing=K fontsize=F size=S`；
4. `[nwzh] relocate short glyphs: moved=N refill=M px=… free_from_y=… cursor_y=… atlas=WxH`
   —— 搬墨实际搬了多少格、补贴了多少格；
5. 引擎自己打 `NUI: Font bake complete; texture is NxNpx`（Nuklear 侧）。

## 八、怎么关闭

删掉 `LD_PRELOAD=...` 即可；本 `.so` 从不写磁盘。

单项关闭：

| 变量 | 关掉什么 |
|---|---|
| `NWNZH_RELOCATE=0` | 短字形搬墨（残影回来） |
| `NWNZH_TEXTOUT_NODBC=0` | `TextOut` 里那条双字节死路的 bypass |
| `NWNZH_FROMUTF8=0` / `NWNZH_TOUTF8=0` | 两个编码边界的透传（中文会变 `?` / 主菜单可能黑屏） |
| `NWNZH_FONT_REMAP=0` / `NWNZH_IMAGE_SHIM=0` | 字体名重定向 / 位图查找 shim |
| `NWNZH_ASCII_CLEAN=1` | 反例：打开「擦像素」的旧做法做对比 |
