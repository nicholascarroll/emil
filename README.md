
# emil

[![OpenSSF Best Practices](https://www.bestpractices.dev/projects/11997/badge)](https://www.bestpractices.dev/projects/11997)

`emil` is a small, portable text editor for UTF-8 files, providing a core subset of emacs commands in the terminal. 

Written in standard C99, `emil` runs on any system providing a minimal POSIX.1-2001 interface (single-process subset) and a VT100-compatible terminal. It eschews common sources of complexity: scripting, plugins, configuration files, background network activity, or auto-save files. 


## Functional Capabilities

- Visual text selection
- Edit rectangular text regions
- Kill ring ('clipboard history')
- Snippets (as session local registers)
- Incremental regex search and replace
- Keystroke macros
- Shell integration
- Word wrap
- Split windows
- Mark ring
- Bookmarks (as session local registers)
- Jump to symbol definition (Ctags)

## Installation

**Unix / Linux / macOS**

```bash
make && sudo make install
```

**Android (Termux)**   
- Excludes shell integration.

```bash
make android
```

**Git for Windows / MSYS2**  
- Run in an **MSYS2** terminal (not mingw64, which lacks termios).  
- Install compilers and libraries:  

```bash
pacman -S msys2-devel msys2-runtime-devel
```

* Build and install:

```bash
make && make install
```

### Internationalization Support

`emil` edits text in any any left-to-right script encoded in valid UTF-8. Editing of right-to-left scripts such as Arabic and Hebrew is not supported. 

Application messages and the man page can be set to any of the following languages at build time. Default is English.

| Language                 | CFLAG        | Man Dir |
| ------------------------ | ------------ |-------- |
| Chinese (Simplified)     | EMIL_LANG_ZH | zh      |
| Spanish (Latin American) | EMIL_LANG_ES | es      |

To build (for example, Spanish):

```bash
make CFLAGS="-DEMIL_LANG_ES"
sudo make install MAN_SOURCE=emil.es.1 MAN_SUBDIR=es
```
and to uninstall:

```
sudo make uninstall MAN_SUBDIR=es
```
 

## Getting Started

Open a file:

```
emil file.txt
```

### Essential Commands

| Action                 | Command             |
| ---------------------- | ------------------- |
| Open file              | `Ctrl-x Ctrl-f`     |
| Save file              | `Ctrl-x Ctrl-s`     |
| Quit emil              | `Ctrl-x Ctrl-c`     |
| Mark (to select text)  | `Ctrl-SPACE`        |
| Cut                    | `Ctrl-w`            |
| Copy                   | `Alt-w` or `Ctrl-c` |
| Paste                  | `Ctrl-y`            |
| Undo                   | `Ctrl-_`            |
| Search                 | `Ctrl-s`            |
| Cancel                 | `Ctrl-g`            |

For the complete command reference, see the man page:

```
man emil
```

## Shell-Oriented Editing

`emil` is designed to be used with the shell set to *emacs-mode* [^1] . 
In Bash the mode is set in the user's `~/.bashrc`:

```bash
set -o emacs
```
An entry in `~/.inputrc` is usually also needed for the copy and kill keybindings:

```inputrc
$include /etc/inputrc          # retain system-wide defaults
set bind-tty-special-chars off

"\C-w": kill-region
"\ew": copy-region-as-kill 
```


### Shell Integration

Shell integration is a compile-time option (enabled by default). It enables shell commands to be used on the buffer:

- **`Alt-|`** 
  Takes the current region, feeds it to the shell command you type, and **displays the output** in a `*Shell Output*` buffer.  

- **`Ctrl-u Alt-|`**    
  Takes the current region, feeds it to the shell command, and **replaces the region** with the output of the command.  

- **`Alt-!`**   
  Enter a shell command in the minibuffer and the output displays in *Shell Output*.

- **`Alt-x diff-buffer-with-file`**    
  Shows unsaved changes.

Shell integration can be disabled at build time with the compiler flag `-DEMIL_DISABLE_SHELL`.


#### Example uses of Shell Integration

Below are common "recipes" using standard Unix utilities. 

| Task                   | Command               | Keys To Use                 |
| ---------------------- | --------------------- | --------------------------- | 
| **Fill region**        | `fmt`                 | `Ctrl-u Alt-\|`             |
| **Sort lines**         | `sort`                | `Ctrl-u Alt-\|`             |
| **Align columns**      | `column -t`           | `Ctrl-u Alt-\|`             |
| **Align text table**   | `column -t -s '\|'` -o '\|' | `Ctrl-u Alt-\|`       |
| **Number lines**       | `cat -n`              | `Ctrl-u Alt-\|`             |
| **Word count**         | `wc`                  | `Alt-\|`                    |
| **Solve math**         | `bc`                  | `Alt-\|` or `Ctrl-u Alt-\|` |
| **Format JSON**        | `jq .`                | `Alt-\|` or `Ctrl-u Alt-\|` |
| **Find typos**         | `aspell list`         | `Alt-\|`                    |
| **Format C code**      | `make format`         | `Ctrl-u Alt-\|`             |
| **Lint shell script**  | `shellcheck`          | `Ctrl-u Alt-\|`             |
| **Trim whitespace**    | `sed 's/[[:space:]]\+$//'` | `Ctrl-u Alt-\|`        |
| **De-duplicate lines** | `awk '!seen[$0]++'`   | `Ctrl-u Alt-\|`             |


### Shell Drawer
`Ctrl-x Ctrl-z` suspends `emil` while preserving the current editor screen. This permits shell commands to be executed in the terminal below the editor content, after which editing may be resumed with `fg`.

Notes: 
   - `less` clears the terminal when it quits; `less -X` and `more` do not.
   - The named command `cd` (change directory) in `emil` does not also change the directory in the shell.

## System Clipboard Integration
`Ctrl-c` copies selected text to both the kill ring and the user's system clipboard when an OSC 52 enabled terminal client is used.

OSC 52 has a practical limit of 74,993 bytes. Selections larger than this are not
sent to the clipboard and a status message is displayed. Some terminal emulators
have lower limits and will silently fail after writing only the first part of the
text to the system clipboard.


## Editing Large Files

`emil` is not designed for editing very large files. Files larger than 1 GB cannot be opened.


## Internals

Each buffer is an array of logical lines (`erow`) holding raw UTF-8 bytes. All buffers contain valid UTF-8 exclusively; files that fail validation are rejected at load time. The buffer is never modified by rendering or text layout concerns.

Display widths are cached per-row and recomputed only when a row is edited. A cumulative screen-line cache maps logical rows to screen positions, enabling efficient scrolling when word wrap is active.

On each frame, the renderer reads raw bytes from the buffer and emits terminal-ready sequences directly into an append buffer. No intermediate render buffers exist. The append buffer is written to the terminal in a single `write()` call, then truncated.

The rendering system uses only cursor positioning (CSI H), erase-to-end-of-line (CSI K), reverse video (CSI 7m / CSI 0m), and clear-below (CSI J). 

All input is processed in a single loop:

1. Read keystroke
2. Execute command (may modify buffer)
3. Refresh screen: clamp window offsets, rebuild caches if stale, scroll, redraw, flush


## Contributing

Bug fixes, portability improvements, performance work, and general code quality 
PRs are welcome. Please do not propose any new features. 


## Credits and License

emil is a derivative of [`japanoise/emsys`](https://github.com/japanoise/emsys) and is distributed under the MIT License.

---

[^1]: Omitted from POSIX.1, see [Rationale](https://pubs.opengroup.org/onlinepubs/007904975/utilities/sh.html).

