# emil (埃米尔)

[![OpenSSF Best Practices](https://www.bestpractices.dev/projects/11997/badge)](https://www.bestpractices.dev/projects/11997)

`emil` 是小型便携的 UTF-8 终端文本编辑器，提供 Emacs 核心命令子集。

以标准 C99 编写，运行于提供最小 POSIX.1-2001 接口（单进程子集）及 VT100 兼容终端的系统。不使用脚本、插件、配置文件，无后台网络活动或自动保存文件。


## 功能特性

- 可视文本选择
- 矩形区域编辑
- 剪切环（剪贴板历史）
- 文本片段（会话级寄存器）
- 增量正则搜索与替换
- 键盘宏
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
不含 Shell 集成。

```bash
make CFLAGS="-DEMIL_LANG_ZH" android
```

**Windows (MSYS2)**  
须在 **MSYS2** 终端中运行（非 mingw64，后者缺少 termios）。  

安装依赖：

```bash
pacman -S msys2-devel msys2-runtime-devel
```

构建并安装：

```bash
make CFLAGS="-DEMIL_LANG_ZH"
make install MAN_SOURCE=emil.zh.1 MAN_SUBDIR=zh
```

卸载：
```
sudo make uninstall MAN_SUBDIR=zh
```

## 快速上手

打开文件：

```
emil file.txt
```

### 基本命令

| 操作               | 命令               |
| ------------------ | ------------------ |
| 打开文件           | `Ctrl-x Ctrl-f`   |
| 保存文件           | `Ctrl-x Ctrl-s`   |
| 退出 emil          | `Ctrl-x Ctrl-c`   |
| 设置标记（选择）   | `Ctrl-SPACE`       |
| 剪切               | `Ctrl-w`           |
| 复制               | `Alt-w` 或 `Ctrl-c` |
| 粘贴               | `Ctrl-y`           |
| 撤销               | `Ctrl-_`           |
| 搜索               | `Ctrl-s`           |
| 取消               | `Ctrl-g`           |

完整命令参考见 man 手册：

```
man emil
```

## 面向 Shell 的编辑

`emil` 设计为配合 *emacs-mode* 的 Shell 使用 [^1]。  
Bash 中于 `~/.bashrc` 设置：

```bash
set -o emacs
```

`~/.inputrc` 通常还需以下条目以支持复制/剪切绑定：

```inputrc
$include /etc/inputrc          # 保留系统默认配置
set bind-tty-special-chars off

"\C-w": kill-region
"\ew": copy-region-as-kill 
```


### Shell 集成

Shell 集成为编译时选项（默认启用），可在缓冲区上执行 shell 命令：

- **`Alt-|`**  
  将当前区域传入 shell 命令，**输出**显示于 `*Shell Output*` 缓冲区。

- **`Ctrl-u Alt-|`**    
  将当前区域传入 shell 命令，以命令**输出**替换区域内容。

- **`Alt-!`**   
  在 minibuffer 中输入 shell 命令，输出显示于 `*Shell Output*`。

- **`Alt-x diff-buffer-with-file`**    
  显示未保存的更改。

编译时可用 `-DEMIL_DISABLE_SHELL` 禁用 Shell 集成。

#### Shell 集成用例

以下为使用标准 Unix 工具的常见用法：

| 任务 | 命令 | 按键 |
| :--- | :--- | :--- |
| **填充区域** | `fmt` | `Ctrl-u Alt-\|` |
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
| **去重** | `awk '!seen[$0]++'` | `Ctrl-u Alt-\|` |


### Shell 抽屉

`Ctrl-x Ctrl-z` 挂起 `emil`，保留编辑器屏幕。可在编辑器画面下方执行 shell 命令，完成后 `fg` 恢复编辑。

注意：
- `less` 退出时清屏；`less -X` 和 `more` 不会。
- `emil` 中的 `cd` 命令仅更改编辑器工作目录，不影响外部 shell。

## 系统剪贴板集成

使用支持 OSC 52 的终端时，`Ctrl-c` 同时复制到剪切环与系统剪贴板。

OSC 52 协议上限为 74,993 字节。超出上限的选区不发送到剪贴板，并显示状态消息。部分终端模拟器上限更低，仅写入前部文本后静默失败。

## 编辑大文件

`emil` 并非为编辑超大文件设计。无法打开超过 1 GB 的文件。

## 原生控制台

Linux 原生虚拟控制台（Ctrl+Alt+F3 等）的内核控制台无法显示中文。替代方案：**kmscon** 或 **fbterm**。

## 内部实现

每个缓冲区为逻辑行数组（`erow`），各行存储原始 UTF-8 字节。所有缓冲区仅含有效 UTF-8；加载时拒绝验证失败的文件。渲染和文本布局不修改缓冲区。

显示宽度按行缓存，仅在该行被编辑时重新计算。**累积屏幕行缓存将逻辑行映射至屏幕位置，使自动换行时滚动高效。**

渲染器每帧直接从缓冲区读取原始字节，将终端控制序列输出至临时追加缓冲区，无中间渲染缓冲。**追加缓冲区经单次 `write()` 写入终端后清空。**

渲染系统仅使用：光标定位（CSI H）、清除至行尾（CSI K）、反显（CSI 7m / CSI 0m）和清除下方（CSI J）。

所有输入在单一循环中处理：

1. 读取按键
2. 执行命令（可能修改缓冲区）
3. 刷新屏幕：校正窗口偏移、按需重建缓存、滚动、重绘、刷新

## 贡献

欢迎提交缺陷修复、可移植性改进、性能优化及代码质量提升的 PR。请勿提议新功能。


## 致谢与许可

emil 是 [`japanoise/emsys`](https://github.com/japanoise/emsys) 的衍生项目，与 Free Software Foundation 或 GNU Project 无关。  
采用 MIT 许可协议。

---

[^1]: POSIX.1 中已省略，参见 [Rationale](https://pubs.opengroup.org/onlinepubs/007904975/utilities/sh.html)。


