# emil ([埃米尔](./README.zh-CN.md))

[![OpenSSF Best Practices](https://www.bestpractices.dev/projects/11997/badge)](https://www.bestpractices.dev/projects/11997)

`emil` 是一款专为 UTF-8 文件设计的小型、便携式终端文本编辑器，提供 Emacs 命令的核心子集。

使用标准 C99 编写，运行于提供最小 POSIX.1-2001 接口（单进程子集）及 VT100 兼容终端的系统。刻意规避常见复杂性来源：脚本、插件、配置文件、后台网络活动或自动保存文件。

## 项目状态

功能完备，但 **不稳定**。

当前工作重点：清理架构、修复缺陷、提升可维护性。欢迎简化内部结构、提高可移植性或修复正确性问题的贡献。

## 功能特性

- 视觉文本选区
- 矩形文本编辑
- 删除环（剪贴板历史）
- 片段（会话级寄存器）
- 增量正则搜索与替换
- 击键宏
- Shell 集成
- 自动换行
- 窗口分割
- 标记环
- 书签（会话级寄存器）
- 跳转至符号定义（Ctags）

## 安装

**Unix / Linux / macOS**

```bash
make CFLAGS="-DEMIL_LANG_ZH"
sudo make install MAN_SOURCE=emil.zh.1 MAN_SUBDIR=zh
```

**Android (Termux)**  
（不包含 Shell 集成）

```bash
make CFLAGS="-DEMIL_LANG_ZH" android
```

**Windows (MSYS2)**  
请在 **MSYS2** 终端（而非 mingw64，后者缺少 termios）中运行。  

安装所需包：

```bash
pacman -S msys2-devel msys2-runtime-devel
```

构建并安装：

```bash
make CFLAGS="-DEMIL_LANG_ZH"
make install MAN_SOURCE=emil.zh.1 MAN_SUBDIR=zh
```

卸载:
```
sudo make uninstall MAN_SUBDIR=zh
```

## 快速上手

打开文件：

```
emil file.txt
```

### 基本命令

| 操作                 | 命令              |
| -------------------- | ----------------- |
| 打开文件             | `Ctrl-x Ctrl-f`   |
| 保存文件             | `Ctrl-x Ctrl-s`   |
| 退出 emil            | `Ctrl-x Ctrl-c`   |
| 标记（选择文本）     | `Ctrl-SPACE`      |
| 剪切                 | `Ctrl-w`          |
| 复制                 | `Alt-w` 或 `Ctrl-c` |
| 粘贴                 | `Ctrl-y`          |
| 撤销                 | `Ctrl-_`          |
| 搜索                 | `Ctrl-s`          |
| 取消                 | `Ctrl-g`          |

完整命令参考请见 man 手册：

```
man emil
```

## 面向 Shell 的编辑

`emil` 设计配合设置为 *emacs-mode* 的 Shell 使用 [^1]。  
Bash 中在用户 `~/.bashrc` 设置：

```bash
set -o emacs
```

`~/.inputrc` 通常也需添加以下条目以支持复制/剪切绑定：

```inputrc
$include /etc/inputrc          # 保留系统默认配置
set bind-tty-special-chars off

"\C-w": kill-region
"\ew": copy-region-as-kill 
```

在 `emil` 与 Readline 中，`Ctrl-h` 删除前一字符；而在 `emacs` 中它是帮助前缀键。


### Shell 集成

Shell 集成为编译时选项（默认启用），允许在缓冲区上使用 shell 命令：

- **`Alt-|`**  
  将当前选区馈送至输入的 shell 命令，**输出**显示于 `*Shell Output*` 缓冲区。

- **`Ctrl-u Alt-|`**    
  将当前选区馈送至 shell 命令，并用命令**输出**替换原选区。

- **`Alt-!`**   
  在 minibuffer 中输入 shell 命令，输出显示于 `*Shell Output*`。

- **`Alt-x diff-buffer-with-file`**    
  显示未保存的更改。

构建时可通过编译器标志 `-DEMIL_DISABLE_SHELL` 禁用 Shell 集成。

#### Shell 集成实用示例

以下为使用标准 Unix 工具的常见"配方"：

| 任务 | 命令 | 按键 |
| :--- | :--- | :--- |
| **排版选区** | `fmt` | `Ctrl-u Alt-\|` |
| **排序行** | `sort` | `Ctrl-u Alt-\|` |
| **对齐列** | `column -t` | `Ctrl-u Alt-\|` |
| **对齐文本表格** | `column -t -s '\|'` -o '\|' | `Ctrl-u Alt-\|` |
| **添加行号** | `cat -n` | `Ctrl-u Alt-\|` |
| **统计字数** | `wc` | `Alt-\|` |
| **计算表达式** | `bc` | `Alt-\|` 或 `Ctrl-u Alt-\|` |
| **格式化 JSON** | `jq .` | `Alt-\|` 或 `Ctrl-u Alt-\|` |
| **查找拼写错误** | `aspell list` | `Alt-\|` |
| **格式化 C 代码** | `make format` | `Ctrl-u Alt-\|` |
| **检查 Shell 脚本** | `shellcheck` | `Ctrl-u Alt-\|` |
| **删除行尾空白** | `sed 's/[[:space:]]\+$//'` | `Ctrl-u Alt-\|` |
| **删除重复行** | `awk '!seen[$0]++'` | `Ctrl-u Alt-\|` |

更复杂的命令可转为 shell 脚本。例如：添加词典查询功能，在 `$PATH` 中创建名为 `edict` 的文件：

```bash
#!/bin/sh
# edict: 通过 stdin 查询单词释义
word=$(cat)
curl -s "dict://dict.org/d:${word}"
```

现在只需在 emil 中选中文本并输入 `Alt-| edict` 即可查看释义。


### Shell 抽屉

`Ctrl-x Ctrl-z` 在保留当前编辑器屏幕的前提下挂起 `emil`。允许在编辑器内容下方的终端执行 shell 命令，之后可用 `fg` 恢复编辑。

注意：
- `less` 退出时会清屏；`less -X` 与 `more` 不会。
- `emil` 中的命名命令 `cd` 仅更改编辑器内工作目录，不影响外部 shell。

### 系统剪贴板集成

使用支持 OSC 52 的终端客户端时，`Ctrl-c` 会将选中文本同时复制到删除环与系统剪贴板。

---


## 编辑大文件

emil 会跟踪所有已打开文件和剪切环内容的总大小，并与可配置的上限（默认
1 GB）进行比较。未保存的缓冲区增长、撤销数据以及命令历史等次要分配不计入上限。

编译时可调整上限：

```
    make CFLAGS="-DEMIL_MAX_OPEN_BYTES=8388608"
```

## 路线图

1. **版本 0.1.0** [已完成] ✅
   - 从此版本起，使用 `emil` 编写 `emil`

2. **版本 0.1.1 功能完备** [已完成] ✅

3. **版本 0.2.1 首次预发布** [已完成] ✅

4. **渲染优化**
   通过可选的渲染加速层减少网络传输字节
   - 运行时开关启用
   - 编辑、移动、滚动操作发送渲染提示

5. **移除对 `subprocess.h` 的依赖**
   将 pipe/exec/fork 相关代码内部化。

6. **版本 1.0.0 无缺陷且令人喜爱**
   - 测试平台：Solaris、AIX、Linux、BSD、MSYS2、macOS、Android
   - 测试环境：原生控制台及各终端模拟器
   - 测试系统：MINIX、RTEMS 与 NuttX
   - 测试输入：IME 与国际键盘
   - 收录于 Linux 发行版仓库

---

## 原生控制台

在 Linux 原生虚拟控制台（Ctrl+Alt+F3 等）上，内核控制台无法显示中文。可选方案：**kmscon** 或 **fbterm**。

## 内部实现

每个缓冲区为逻辑行数组（`erow`），每行存储原始 UTF-8 字节。所有缓冲区均含有效 UTF-8；加载时拒绝验证失败的文件。缓冲区从不因渲染或文本布局而修改。

显示宽度按行缓存，仅在该行编辑时重算。累积屏幕行缓存将逻辑行映射至屏幕位置，启用自动换行时实现高效滚动。

每帧渲染时，渲染器直接从缓冲区读取原始字节，将终端就绪序列直接输出至临时追加缓冲区。无中间渲染缓冲。追加缓冲区通过单次 `write()` 调用写入终端。

渲染系统仅使用：光标定位（CSI H）、清除至行尾（CSI K）、反显（CSI 7m / CSI 0m）、清除下方（CSI J）。核心渲染器不使用滚动区域操作及行插入/删除；这些功能计划作为可选渲染加速层，通过运行时开关启用。

所有输入在单一循环中处理：

1. 读取按键
2. 执行命令（可能修改缓冲区）
3. 刷新屏幕：钳制窗口偏移、缓存过期则重建、滚动、重绘、刷新

## 贡献

欢迎提交缺陷修复、可移植性改进、性能优化及代码质量提升的 PR。
若提议新功能，请先开 issue —— `emil` 刻意保持小型。

## 致谢与许可

emil 是 [`japanoise/emsys`](https://github.com/japanoise/emsys) 的衍生项目，与 Free Software Foundation 或 GNU Project 无关。  
采用 MIT 许可协议。

---

[^1]: POSIX.1 已省略此项，参见 [Rationale](https://pubs.opengroup.org/onlinepubs/007904975/utilities/sh.html)。
