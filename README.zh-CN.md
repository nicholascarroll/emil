
# [emil](./README.md) (埃米尔)

[![OpenSSF Best Practices](https://www.bestpractices.dev/projects/11997/badge)](https://www.bestpractices.dev/projects/11997)


`emil` 是一款专为 UTF-8 文件设计的小型、便携式终端文本编辑器。它在 VT100 兼容终端上提供了一套精简的 Emacs 命令子集。

`emil` 使用标准 C99 编写，仅依赖符合 POSIX.1-2001 标准的运行环境。它刻意避开了常见的复杂性来源：脚本支持、插件系统、配置文件、后台网络活动或自动保存文件。


## 项目状态

功能已完备，但 **仍处于不稳定阶段**。

当前工作重点是清理架构、修复 bug 并提升可维护性。特别欢迎简化内部结构、提高可移植性或修复正确性问题的贡献。

## 功能特性

- 剪切环（kill ring）
- 视觉文本选择
- 增量正则搜索与替换
- 自动换行
- 多窗口
- 矩形编辑
- 击键宏
- 寄存器
- Shell 集成


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

```bash
emil file.txt
```

### 基本命令

| 操作               | 命令              |
|--------------------|-------------------|
| 打开文件           | `Ctrl-x Ctrl-f`   |
| 保存文件           | `Ctrl-x Ctrl-s`   |
| 退出 emil          | `Ctrl-x Ctrl-c`   |
| 标记（选择文本）   | `Ctrl-SPACE`      |
| 剪切               | `Ctrl-w`          |
| 复制               | `Alt-w`           |
| 粘贴               | `Ctrl-y`          |
| 搜索               | `Ctrl-s`          |
| 取消               | `Ctrl-g`          |

完整命令参考请见 man 手册：

```bash
man emil
```

## 面向 Shell 的编辑

虽然 POSIX.1 只标准化了 `vi-mode` 命令行编辑接口，但许多 shell（包括 Bash）也提供了 `emacs-mode`（通过 `set -o emacs` 激活）。  
该模式曾被考虑纳入标准，但最终被省略。  
官方 [rationale](https://pubs.opengroup.org/onlinepubs/007904975/utilities/sh.html) 解释道：

> "在早期的提案中，基于 KornShell 的命令行编辑 emacs 模式被包含在内，尽管 emacs 编辑器本身并未被包含。Emacs 支持者社区坚决反对将完整的 emacs 编辑器纳入标准，因为他们担心标准化这个功能极其强大的环境会促使供应商只提供严格符合标准的版本，而这些版本缺少社区所必需的可扩展性。原 emacs 程序的作者也明确表示希望省略该程序。此外，许多历史系统要么不包含 emacs，要么包含后也不提供支持，但几乎没有系统不包含且不支持 vi。最终，shell 的 emacs 命令行编辑模式被删除，因为 KornShell 版本与 GNU 系统所分发的编辑器在若干方面已出现明显分歧。Emacs 作者要求 POSIX 的 emacs 模式要么完全删除，要么增加大量未指定的条件。虽然 KornShell 作者同意考虑修改以实现一致，但标准制定者当时决定暂缓规范。当时大家以为后续草案会达成可接受的统一定义，但此事并未发生，而且目前看来也没有推动的动力。无论如何，实现方可自由提供额外的命令行编辑模式，基于用户最熟悉的编辑器的精确模型。"

这些不兼容性是次要的；tty 驱动程序自 4BSD 以来一直将 Ctrl-w 视为 WERASE，而此行为可由 ~/.inputrc 中的以下条目覆盖：

```inputrc
$include /etc/inputrc          # retain system-wide defaults
set bind-tty-special-chars off

"\C-w": kill-region
"\M-w": copy-region-as-kill
```
如果没有标记任何区域，它会杀死光标前面的一个词（相当于向后杀词，backward-kill-word）。

在 Readline 和 `emil` 中，`Ctrl-h` 会删除前一个字符，但在 `emacs` 中它是帮助前缀键。

Readline 支持两种删除到行首的方式：`Ctrl-x BACKSPACE`（在 *emacs* 中支持）以及更符合人体工学的 `Ctrl-u`，但后者与 *emacs* 的通用参数（universal argument）冲突。`emil` 通过将 `Ctrl-u Ctrl-a` 绑定为删除到行首来解决这一冲突。

### Shell 集成

Shell 集成是一个编译时选项（默认启用）。它允许在缓冲区上使用 shell 命令：   

- **`Alt-|`** (shell-command-on-region)  
  将当前区域馈送到您输入的 shell 命令，并将**输出**显示在临时 `*Shell Command Output*` 缓冲区中（如果输出较小，则显示在回显区）。

- **`Ctrl-u Alt-|`**  
  将当前区域馈送到您输入的 shell 命令，并用命令的**输出**替换该区域。

- **`Alt-x diff-buffer-with-file`**  
  显示尚未保存的更改。


Shell 集成可以在构建时通过编译器标志 `-DEMIL_DISABLE_SHELL` 禁用。
### Shell 抽屉

`Ctrl-x Ctrl-z` 会在保留当前编辑器屏幕的情况下挂起 `emil`。这允许在编辑器内容下方的终端中执行 shell 命令，之后可使用 `fg` 恢复编辑。

### 系统剪贴板集成

在使用启用 OSC 52 的终端客户端时，`Ctrl-c` 会将选中的文本同时复制到 kill ring 和用户的系统剪贴板。


## 路线图

Shell 集成是一个编译时选项（默认启用）。它允许在缓冲区上使用 shell 命令：  

1. **版本 0.1.0 [已完成]**  ✅    
   - 从此版本开始，我们使用 `emil` 来编写 `emil`

2. **版本 0.1.1 功能完成版**  ✅    

3. **版本 0.2.0 稳定预览版** 🚧🔨👷  
   - 修复大部分已知 bug  
   - 首次在 GitHub 发布（使用预发布标签 prerelease）  
   - 在论坛上宣布（HN、Reddit 等）

如果需要调整语气（更正式/更口语化）或补充其他内容，随时告诉我！

4. **渲染系统升级**  
   测试 SSH 下的性能和渲染系统。

5. **移除对 `subprocess.h` 的依赖**  
   将用于 pipe/exec/fork 的代码内部化。

6. **版本 1.0.0 无 bug，爱上它**  
   - 在 Solaris、AIX、Linux、BSD、MSYS2、OSX、Android 上测试。  
   - 使用 IME 和国际键盘测试  
   - 包含在 Linux 发行版仓库中

### 字符显示注意事项

在原始的 Linux 虚拟控制台（Ctrl+Alt+F3 等）上，内核控制台无法显示中文。  
请改用 **kmscon** 或 **fbterm**。


## 贡献

欢迎提交 bug 修复、可移植性改进、性能优化以及代码质量提升相关的 PR。
若要提出新功能，请先开 issue —— emil 刻意保持小型。
## 致谢与许可

emil 是 [`japanoise/emsys`](https://github.com/japanoise/emsys) 的衍生项目，与 Free Software Foundation 或 GNU Project 无关。  
采用 MIT 许可协议。