# mg 语法高亮设计文档

## 1. 目标

为 mg 编辑器添加轻量级语法高亮，满足个人使用场景。

### 设计约束

- 不引入正则引擎（mg 的 `re_search.c` 是 POSIX regex 封装，不适合高频语法扫描）
- 不为每种语言手写 C tokenizer（扩展语言不需要重新编译）
- 不破坏现有 UTF-8/CJK 改动
- 只高亮可见行，不做全文件预处理
- 总代码量控制在 ~800 行 C + 每种语言 ~20 行数据

### 覆盖范围

- 关键字着色
- 字符串着色（单行和跨行）
- 注释着色（行注释和块注释，块注释可跨行）
- 数字着色
- 类型名着色（可选，与关键字区分）

### 不覆盖

- 嵌套 region（如注释内的 URL 高亮）
- nextgroup / contains 等嵌套规则
- 正则模式匹配
- treesitter 级别的语义分析

---

## 2. 整体架构

```
.syntax 文件 (数据)              mg 内核 (C 代码)
┌─────────────────────┐         ┌──────────────────────────┐
│ ~/.mg/syntax/       │         │ syntax.c                 │
│   c.syntax          │  加载   │  ├─ syntax_load()        │
│   python.syntax     │◄───────│  ├─ syntax_match_line()  │
│   sh.syntax         │         │  └─ syntax_free()        │
│                     │         │                          │
│ 格式：              │         │ syntax.h                 │
│   keywords: ...     │         │  ├─ struct syntax_rule   │
│   strings: " "      │         │  ├─ struct syntax_table  │
│   comments: // /* */│         │  └─ 颜色码定义            │
│   numbers: \d       │         │                          │
└─────────────────────┘         │ display.c                │
                                │  struct video + v_hue    │
                                │  uline() 按列切色         │
                                │                          │
                                │ tty.c                    │
                                │  ttcolor() 扩展 256 色    │
                                └──────────────────────────┘
```

### 数据流

1. 启动时或 `M-x syntax-on` 时，从 `~/.mg/syntax/<lang>.syntax` 加载语法表
2. 渲染某行时，`syntax_match_line()` 从上一行结束状态恢复，扫描该行，为每个字节生成颜色码
3. `vtputchar()` 渲染字符时，将颜色码写入 `v_hue[col]`
4. `uline()` 输出到终端时，颜色码变化处调 `ttcolor()` 切色

---

## 3. 数据结构

### 3.1 语法规则

```c
/* syntax.h */

/* 规则类型 */
enum {
    SYNTAX_KEYWORD,     /* 整词匹配（int, if, return） */
    SYNTAX_TYPE,        /* 整词匹配，但用不同颜色（size_t, FILE） */
    SYNTAX_STRING,      /* 定界符包裹（"..." '...'） */
    SYNTAX_LINE_COMMENT,/* 前缀到行尾（// ...） */
    SYNTAX_BLOCK_COMMENT,/* 定界符包裹，可跨行（/* ... *\/） */
    SYNTAX_NUMBER,      /* 数字起始字符 */
};

/* 单条规则 */
struct syntax_rule {
    int              type;        /* SYNTAX_* */
    char            *pattern;     /* 关键字或起始定界符 */
    char            *end_pattern; /* 终止定界符（NULL = 行尾） */
    int              color;       /* 颜色码 COLOR_* */
    struct syntax_rule *next;     /* 链表 */
};

/* 语法表（一种语言一张） */
struct syntax_table {
    char                *lang;    /* "c", "python", "sh" */
    struct syntax_rule  *rules;   /* 规则链表 */
    /* 关键字哈希表（快速查找） */
    struct keyword_entry *keywords[HASHSIZE];
    struct keyword_entry *types[HASHSIZE];
};

/* 关键字哈希条目 */
struct keyword_entry {
    char                *word;
    int                 color;
    struct keyword_entry *next;
};
```

### 3.2 颜色码

```c
/* syntax.h */

/* 颜色码 — 直接对应终端颜色 */
enum {
    COLOR_DEFAULT    = 0,   /* 正常文本 */
    COLOR_KEYWORD    = 1,   /* 关键字 */
    COLOR_TYPE       = 2,   /* 类型名 */
    COLOR_STRING     = 3,   /* 字符串 */
    COLOR_COMMENT    = 4,   /* 注释 */
    COLOR_NUMBER     = 5,   /* 数字 */
    COLOR_PREPROC    = 6,   /* 预处理指令（可选） */
    COLOR_MAX        = 7,
};
```

### 3.3 行级状态

```c
/* def.h — struct line 新增字段 */

struct line {
    /* ... 既有字段 ... */
    short  l_synstate;  /* 该行结束时的语法状态 */
};
```

状态值定义：

```c
/* syntax.h */
enum {
    SYNSTATE_DEFAULT     = 0,  /* 正常状态 */
    SYNSTATE_IN_BLOCK_COMMENT = 1,  /* 在块注释内 */
};
```

当前只需要一个状态值：是否在块注释内。如果以后需要更多状态（如跨行字符串），可以扩展为位掩码。

---

## 4. 显示层改动

### 4.1 struct video 增加 v_hue

```c
/* display.c — struct video */
struct video {
    /* ... 既有字段 ... */
    char    *v_text;    /* UTF-8 bytes, ncol*UTF8_MAX_BYTES */
    char    *v_wid;     /* 显示宽度 per column */
    char    *v_hue;     /* 颜色码 per column — 新增 */
};
```

改动点与之前加 `v_wid` 完全同模式：

| 函数 | 改动 |
|------|------|
| `vtresize()` | 分配 `ncol` 给 `v_hue`；缩行时 free |
| `vtinit()` | 空白屏 `v_hue[i] = COLOR_DEFAULT` |
| `vteeol()` | 清空区域 `v_hue[i] = COLOR_DEFAULT` |
| `vtputchar()` / `vtputechar()` | 写槽位时同步写 `v_hue[col]` |
| `ucopy()` | 复制 `v_hid`（`ncol` 字节） |
| `hash()` | hash 计算包含 `v_hue` |
| `uline()` | 输出时按列检查 `v_hue` 变化，调 `ttcolor()` |

### 4.2 uline() 输出逻辑

`uline()` 当前按列输出 UTF-8 字节。改动后，在输出每个槽位前检查颜色：

```c
/* uline() 中的输出循环 */
for (col = first; col < erase_from; ) {
    if (vvp->v_hue[col] != current_hue) {
        ttcolor(vvp->v_hue[col]);
        current_hue = vvp->v_hue[col];
    }
    w = vvp->v_wid[col];
    if (w == 0) { col++; continue; }
    vslot = VSLOT(vvp, col);
    for (k = 0; k < UTF8_MAX_BYTES && vslot[k]; k++)
        ttputc(vslot[k]);
    ttcol += w;
    col += w;
}
ttcolor(COLOR_DEFAULT);  /* 恢复 */
```

### 4.3 渲染时调用 tokenizer

`update()` 中渲染每行时，在 `vtputchar()` 循环之前调用 tokenizer 生成该行的颜色映射：

```c
/* update() WFEDIT/WFFULL 渲染路径 */
char huebuf[MAXCOL];  /* 每列颜色码 */
syntax_match_line(lp, lback(lp), huebuf, ncol);
for (j = 0; j < llength(lp); ) {
    vtputchar_hue(lp, &j, wp, huebuf);
}
```

`vtputchar_hue()` 是 `vtputchar()` 的扩展版，从 `huebuf` 读取颜色写入 `v_hue`。

---

## 5. tokenizer 引擎

### 5.1 核心函数

```c
/*
 * 扫描一行文本，为每个字节生成颜色码。
 * prev_lp: 上一行（用于恢复块注释状态）
 * lp:      当前行
 * hue:     输出缓冲区，长度 >= llength(lp)
 * ncol:    屏幕列数（用于截断）
 */
void
syntax_match_line(struct line *prev_lp, struct line *lp,
                  char *hue, int ncol)
{
    int state = prev_lp ? prev_lp->l_synstate : SYNSTATE_DEFAULT;
    int i, len;
    const char *text = lp->l_text;
    int textlen = llength(lp);

    memset(hue, COLOR_DEFAULT, textlen);

    for (i = 0; i < textlen; ) {
        if (state == SYNSTATE_IN_BLOCK_COMMENT) {
            /* 在块注释内：找终止符 */
            int end = find_end(text, i, textlen,
                              table->block_comment_end);
            if (end < 0) {
                /* 到行尾仍在注释内 */
                memset(hue + i, COLOR_COMMENT, textlen - i);
                state = SYNSTATE_IN_BLOCK_COMMENT;
                break;
            }
            memset(hue + i, COLOR_COMMENT, end - i);
            i = end;
            state = SYNSTATE_DEFAULT;
            continue;
        }

        /* 正常状态：逐字符匹配 */
        unsigned char c = text[i];

        /* 空白 */
        if (c == ' ' || c == '\t') {
            hue[i] = COLOR_DEFAULT;
            i++;
            continue;
        }

        /* 尝试匹配注释前缀 */
        if (match_prefix(text, i, textlen,
                        table->line_comment_prefix)) {
            /* 行注释：到行尾 */
            memset(hue + i, COLOR_COMMENT, textlen - i);
            break;
        }
        if (match_prefix(text, i, textlen,
                        table->block_comment_start)) {
            /* 块注释：找终止符 */
            int start = i;
            int end = find_end(text, i + strlen(table->block_comment_start),
                              textlen, table->block_comment_end);
            if (end < 0) {
                memset(hue + start, COLOR_COMMENT, textlen - start);
                state = SYNSTATE_IN_BLOCK_COMMENT;
                break;
            }
            memset(hue + start, COLOR_COMMENT, end - start);
            i = end;
            continue;
        }

        /* 尝试匹配字符串 */
        if (c == '"' || c == '\'') {
            int end = find_string_end(text, i, textlen);
            int clen = (end < 0) ? textlen - i : end - i;
            memset(hue + i, COLOR_STRING, clen);
            i += clen;
            continue;
        }

        /* 尝试匹配数字 */
        if (c >= '0' && c <= '9') {
            int end = i;
            while (end < textlen && is_word_char(text[end]))
                end++;
            memset(hue + i, COLOR_NUMBER, end - i);
            i = end;
            continue;
        }

        /* 尝试匹配关键字/类型 */
        if (is_word_start(c)) {
            int end = i;
            while (end < textlen && is_word_char(text[end]))
                end++;
            int color = keyword_lookup(table, text + i, end - i);
            if (color != COLOR_DEFAULT)
                memset(hue + i, color, end - i);
            i = end;
            continue;
        }

        /* 其他字符 */
        hue[i] = COLOR_DEFAULT;
        i++;
    }

    lp->l_synstate = state;
}
```

### 5.2 辅助函数

```c
/* 判断字符是否可作为标识符起始 */
static int
is_word_start(unsigned char c)
{
    return (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') || c == '_';
}

/* 判断字符是否可在标识符中出现 */
static int
is_word_char(unsigned char c)
{
    return is_word_start(c) || (c >= '0' && c <= '9');
}

/* 检查 text+offset 处是否以 prefix 开头 */
static int
match_prefix(const char *text, int offset, int textlen,
             const char *prefix)
{
    if (prefix == NULL)
        return 0;
    int plen = strlen(prefix);
    if (offset + plen > textlen)
        return 0;
    return memcmp(text + offset, prefix, plen) == 0;
}

/* 从 text+offset 开始找 end 定界符，返回其结束位置 */
static int
find_end(const char *text, int offset, int textlen,
         const char *end)
{
    if (end == NULL)
        return -1;
    int elen = strlen(end);
    for (int i = offset; i + elen <= textlen; i++) {
        if (memcmp(text + i, end, elen) == 0)
            return i + elen;
    }
    return -1;
}

/* 从 text+offset 处的引号开始，找配对的结束引号 */
static int
find_string_end(const char *text, int offset, int textlen)
{
    char quote = text[offset];
    int i = offset + 1;
    while (i < textlen) {
        if (text[i] == '\\') { i += 2; continue; }
        if (text[i] == quote) return i + 1;
        i++;
    }
    return -1;  /* 未闭合，染到行尾 */
}

/* 在关键字哈希表中查找，返回颜色码 */
static int
keyword_lookup(struct syntax_table *table,
               const char *word, int len)
{
    /* 哈希查找，返回 COLOR_KEYWORD / COLOR_TYPE / COLOR_DEFAULT */
}
```

---

## 6. 语法文件格式

### 6.1 文件位置

```
~/.mg/syntax/<lang>.syntax
```

按文件扩展名自动选择：
- `.c` `.h` → `c.syntax`
- `.py` → `python.syntax`
- `.sh` `.bash` → `sh.syntax`
- 无扩展名但有 shebang `#!/bin/sh` → `sh.syntax`

### 6.2 格式定义

纯文本，每行一条规则，`#` 开头为注释：

```
# C 语法定义
# 关键字（COLOR_KEYWORD）
keyword: int
keyword: long
keyword: short
keyword: char
keyword: if
keyword: else
keyword: while
keyword: for
keyword: return
keyword: struct
keyword: typedef
# ... 更多关键字

# 类型名（COLOR_TYPE）
type: size_t
type: ssize_t
type: FILE
type: NULL
type: size_t

# 字符串定界符
string: " "
string: ' '

# 行注释前缀
line_comment: //

# 块注释定界符
block_comment: /* */

# 数字起始字符（可选，默认 0-9）
number: 0123456789
```

### 6.3 多定界符支持

同一种类型可以有多条规则。例如 C 的字符串和字符字面量：

```
string: " "
string: ' '
```

Python 的两种字符串风格：

```
string: " "
string: ' '
block_comment: """ """
block_comment: ''' '''
```

Shell 的两种注释（实际上只有 `#` 行注释）：

```
line_comment: #
```

### 6.4 解析器

```c
/*
 * 从文件加载语法表。
 * path: .syntax 文件路径
 * 返回: 语法表指针，失败返回 NULL
 */
struct syntax_table *
syntax_load(const char *path)
{
    FILE *fp;
    char line[NFILEN];
    struct syntax_table *table;

    if ((fp = fopen(path, "r")) == NULL)
        return NULL;

    table = calloc(1, sizeof(struct syntax_table));

    while (fgets(line, sizeof(line), fp) != NULL) {
        /* 去除空白和注释 */
        char *p = skip_ws(line);
        if (*p == '#' || *p == '\0' || *p == '\n')
            continue;

        if (strncmp(p, "keyword:", 8) == 0)
            add_keyword(table, skip_ws(p + 8), COLOR_KEYWORD);
        else if (strncmp(p, "type:", 5) == 0)
            add_keyword(table, skip_ws(p + 5), COLOR_TYPE);
        else if (strncmp(p, "string:", 7) == 0)
            add_string_rule(table, skip_ws(p + 7));
        else if (strncmp(p, "line_comment:", 13) == 0)
            add_prefix_rule(table, skip_ws(p + 13),
                           COLOR_COMMENT, SYNTAX_LINE_COMMENT);
        else if (strncmp(p, "block_comment:", 14) == 0)
            add_block_comment_rule(table, skip_ws(p + 14));
        else if (strncmp(p, "number:", 7) == 0)
            table->number_chars = strdup(skip_ws(p + 7));
    }
    fclose(fp);
    return table;
}
```

---

## 7. 终端颜色层

### 7.1 ttcolor() 扩展

当前 `ttcolor()` 只支持 `CTEXT`（正常）和 `CMODE`（反白）。扩展为支持 256 色：

```c
/* tty.c */

/* 颜色码到终端颜色号的映射 */
static int color_map[COLOR_MAX] = {
    [COLOR_DEFAULT] = -1,      /* 默认，不切色 */
    [COLOR_KEYWORD] = 3,       /* 黄色 */
    [COLOR_TYPE]    = 5,       /* 洋红 */
    [COLOR_STRING]  = 2,       /* 绿色 */
    [COLOR_COMMENT] = 4,       /* 蓝色 */
    [COLOR_NUMBER]  = 6,       /* 青色 */
    [COLOR_PREPROC] = 1,       /* 红色 */
};

void
ttcolor(int color)
{
    if (color == tthue)
        return;

    if (color == COLOR_DEFAULT || color == CTEXT) {
        putpad(exit_standout_mode, 1);
        /* 重置前景色 */
        putpad(tiparm(set_a_foreground, 7), 1);  /* 白色 */
    } else if (color == CMODE) {
        /* 模式行：反白 */
        putpad(enter_standout_mode, 1);
    } else if (color >= 0 && color < COLOR_MAX) {
        int fg = color_map[color];
        if (fg >= 0 && set_a_foreground != NULL)
            putpad(tiparm(set_a_foreground, fg), 1);
    }
    tthue = color;
}
```

### 7.2 颜色兼容性

- 现有 `CTEXT` / `CMODE` 值不变（1 / 2），与 `COLOR_*` 不冲突
- `COLOR_*` 从 0 开始，`CTEXT` = 1，`CMODE` = 2
- 需要调整枚举值避免重叠，或用不同范围：

```c
/* 最终方案：分离两套值 */
#define CTEXT   0       /* 正常文本（改自 1） */
#define CMODE   1       /* 模式行反白 */

/* 语法颜色从 16 开始，避免与 CTEXT/CMODE 冲突 */
enum {
    COLOR_DEFAULT    = 0,
    COLOR_KEYWORD    = 16,
    COLOR_TYPE       = 17,
    COLOR_STRING     = 18,
    COLOR_COMMENT    = 19,
    COLOR_NUMBER     = 20,
    COLOR_PREPROC    = 21,
};
```

### 7.3 用户自定义颜色

在 `.mg` 配置文件中：

```
(syntax-color keyword yellow)
(syntax-color string  green)
(syntax-color comment  blue)
(syntax-color number   cyan)
(syntax-color type     magenta)
```

对应一个 `M-x syntax-color` 命令，修改 `color_map[]`。

---

## 8. 配置和命令

### 8.1 mg 命令

| 命令 | 快捷键 | 说明 |
|------|--------|------|
| `syntax-on` | 无 | 开启语法高亮 |
| `syntax-off` | 无 | 关闭语法高亮 |
| `syntax-mode` | 无 | 手动切换语言（`M-x syntax-mode c`） |
| `syntax-color` | 无 | 设置颜色（`M-x syntax-color keyword yellow`） |
| `syntax-reload` | 无 | 重新加载语法文件 |

### 8.2 .mg 配置

```
(syntax-on)
(syntax-mode c)
(syntax-color keyword yellow)
(syntax-color string  green)
(syntax-color comment  blue)
```

### 8.3 自动检测

打开文件时按扩展名自动选择语法表：

```c
/* file.c — 文件打开后调用 */
void
syntax_detect(struct buffer *bp)
{
    char *name = bp->b_bname;
    char *ext = strrchr(name, '.');

    if (ext == NULL) {
        /* 检查 shebang */
        if (buffer_has_shebang(bp, "/bin/sh"))
            bp->b_syntax = syntax_load("sh");
        else if (buffer_has_shebang(bp, "python"))
            bp->b_syntax = syntax_load("python");
        return;
    }
    ext++;
    if (strcmp(ext, "c") == 0 || strcmp(ext, "h") == 0)
        bp->b_syntax = syntax_load("c");
    else if (strcmp(ext, "py") == 0)
        bp->b_syntax = syntax_load("python");
    else if (strcmp(ext, "sh") == 0 || strcmp(ext, "bash") == 0)
        bp->b_syntax = syntax_load("sh");
    /* ... */
}
```

---

## 9. buffer 和 line 改动

### 9.1 struct buffer

```c
/* def.h — struct buffer 新增 */
struct buffer {
    /* ... 既有字段 ... */
    struct syntax_table *b_syntax;  /* 当前语法表，NULL = 无高亮 */
    int    b_syntax_on;             /* 高亮开关 */
};
```

### 9.2 struct line

```c
/* def.h — struct line 新增 */
struct line {
    /* ... 既有字段 ... */
    short  l_synstate;  /* 该行结束时的语法状态 */
};
```

### 9.3 性能影响

- `l_synstate` 是 `short`（2 字节），每行增加 2 字节内存
- `b_syntax` 是指针，每个 buffer 增加 8 字节
- tokenizer 只在渲染可见行时运行，不影响非可见行
- 关键字查找用哈希表，O(1)

---

## 10. 文件清单

### 新增文件

| 文件 | 行数（估） | 内容 |
|------|-----------|------|
| `syntax.h` | ~60 | 数据结构声明、颜色码定义、函数原型 |
| `syntax.c` | ~450 | tokenizer 引擎、语法文件解析、关键字哈希、命令实现 |
| `syntax/c.syntax` | ~30 | C 语法定义 |
| `syntax/python.syntax` | ~25 | Python 语法定义 |
| `syntax/sh.syntax` | ~20 | Shell 语法定义 |

### 修改文件

| 文件 | 改动行数（估） | 内容 |
|------|---------------|------|
| `display.c` | ~150 | `struct video` 加 `v_hue`，`uline`/`ucopy`/`hash`/`vtresize`/`vtinit`/`vteeol` 适配，渲染路径调 tokenizer |
| `tty.c` | ~40 | `ttcolor()` 扩展 256 色 |
| `def.h` | ~10 | `struct buffer` 加 `b_syntax`/`b_syntax_on`，`struct line` 加 `l_synstate`，函数声明 |
| `funmap.c` | ~10 | 注册 `syntax-on`/`syntax-off`/`syntax-mode`/`syntax-color`/`syntax-reload` |
| `file.c` | ~20 | 文件打开时自动检测语言 |
| `line.c` | ~5 | `lalloc()` 初始化 `l_synstate = 0` |
| `configure` / `Makefile` | ~2 | 加入 `syntax.o` |

### 总量

| 类别 | 行数 |
|------|------|
| 新增 C 代码 | ~510 |
| 修改 C 代码 | ~235 |
| 语法数据文件 | ~75 |
| **合计** | **~820** |

---

## 11. 实现顺序

1. **终端颜色层**：`ttcolor()` 扩展，验证 256 色输出
2. **显示缓存层**：`struct video` 加 `v_hue`，`uline`/`ucopy`/`hash` 适配
3. **tokenizer 引擎**：`syntax.c` 核心扫描逻辑，先用硬编码的 C 关键字测试
4. **语法文件解析**：`.syntax` 文件加载器
5. **多行状态**：`l_synstate` + 块注释跨行
6. **配置集成**：`M-x syntax-on/off`，自动检测，`.mg` 配置
7. **语法定义文件**：c / python / sh
8. **编译测试**

---

## 12. 测试计划

### 基本功能

- [ ] C 文件关键字着色正确
- [ ] 字符串着色（单行）
- [ ] 行注释着色（`//`）
- [ ] 块注释着色（`/* */`）
- [ ] 块注释跨行（连续多行都是注释色）
- [ ] 数字着色
- [ ] 类型名着色（与关键字不同色）

### 边界情况

- [ ] 未闭合的字符串（染到行尾，不崩溃）
- [ ] 未闭合的块注释（染到文件尾，不崩溃）
- [ ] 嵌套引号 `"he said \"hi\""`
- [ ] 注释中的引号 `/* "string" */`
- [ ] 字符串中的注释标记 `"not a // comment"`
- [ ] 空行
- [ ] 只有注释的行
- [ ] 超长行

### 兼容性

- [ ] UTF-8/CJK 文本仍正确显示
- [ ] 无语法表的文件显示正常（全默认色）
- [ ] `syntax-off` 后恢复无色显示
- [ ] ASCII 文件回归正常
- [ ] 模式行仍为反白
- [ ] 搜索高亮不受影响

### 性能

- [ ] 10,000 行 C 文件滚动不卡顿
- [ ] 大块注释文件滚动不卡顿
- [ ] 频繁编辑时不卡顿
