   
# emil

[![OpenSSF Best Practices](https://www.bestpractices.dev/projects/11997/badge)](https://www.bestpractices.dev/projects/11997)

`emil` is a small, portable text editor for UTF-8 files, providing a core subset of *emacs* commands in the terminal.

Written in C99, `emil` is single-threaded and runs on any system providing POSIX.1-2001 and a VT100-compatible terminal. It eschews common sources of complexity: scripting, plugins, configuration files, background network activity, and auto-save files.

## Capabilities

- Edit any left-to-right Unicode script  [^1]
- Visual text selection
- Edit rectangular text regions
- Kill ring ("clipboard history")
- Snippets (as session-local registers)
- Regular expression search (POSIX ERE)
- Keystroke macros
- Shell integration
- Word wrap
- Split windows
- Mark ring
- Bookmarks (as session-local registers)
- Jump to symbol definition (ctags)

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

- Build and install:

```bash
make && make install
```

**WebAssembly (WASIX)**

`emil` builds as a standalone Wasm binary against
[WASIX](https://wasix.org) using
[wasi-sdk](https://github.com/WebAssembly/wasi-sdk) +
[wasix-libc](https://github.com/wasix-org/wasix-libc), and runs on any
[Wasmer](https://wasmer.io) host. 

```bash
./tests/wasix_setup.sh ~/opt          # pinned toolchain, one-time
make wasix WASI_SDK=~/opt/wasi-sdk WASIX_SYSROOT=~/opt/wasix-sysroot/sysroot
wasmer run emil.wasm --dir . -- FILE
```
- Shell integration is excluded. WASIX supplies `fork`, `exec` and
  `posix_spawn`, so this is a deferral rather than a platform limit:
  the shell drawer is left out because it has not been exercised under
  a Wasm runtime.
- Advisory file locking is unavailable. `wasix-libc` declares
  `struct flock` but not the `F_GETLK`/`F_SETLK` commands, so the lock
  is compiled out.


## Getting Started

Open a file:

```bash
emil file.txt
```

### Essential Commands

| Action                 | Command             |
| ---------------------- | ------------------- |
| Open file              | `Ctrl-x Ctrl-f`     |
| Save file              | `Ctrl-x Ctrl-s`     |
| Quit emil              | `Ctrl-x Ctrl-c`     |
| Mark (to select text)  | `Ctrl-Space`        |
| Cut                    | `Ctrl-w`            |
| Copy                   | `Alt-w` or `Ctrl-c` |
| Paste                  | `Ctrl-y`            |
| Undo                   | `Ctrl-_`            |
| Search                 | `Ctrl-s`            |
| Cancel                 | `Ctrl-g`            |

For the complete command reference, see the man page:

```bash
man emil
```

## Shell-Oriented Editing

`emil` is designed to be used with the shell set to *emacs-mode* [^2].
In Bash the mode is set in the user's `~/.bashrc`:

```bash
set -o emacs
```

Entries in `~/.inputrc` are usually also needed for the copy and kill keybindings:

```inputrc
# retain system-wide defaults
$include /etc/inputrc
set bind-tty-special-chars off

"\C-w": kill-region
"\ew": copy-region-as-kill
```


### Shell Integration

Shell integration is enabled by default and is disabled at build time with `-DEMIL_DISABLE_SHELL`. It allows shell commands to be run on the buffer:

- **`Alt-|`**
  Takes the current region, feeds it to the shell command entered in the minibuffer, and **displays the output** in a `*Shell Output*` buffer.

- **`Ctrl-u Alt-|`**
  Takes the current region, feeds it to the shell command, and **replaces the region** with the output of the command.

- **`Alt-!`**
  Takes a shell command in the minibuffer and displays the output in `*Shell Output*`.

- **`Alt-x diff-buffer-with-file`**
  Shows unsaved changes.


#### Example uses of Shell Integration

| Task                   | Command                          | Keys To Use                 |
| ---------------------- | -------------------------------- | --------------------------- |
| **Fill region**        | `fmt`                            | `Ctrl-u Alt-\|`             |
| **Sort lines**         | `sort`                           | `Ctrl-u Alt-\|`             |
| **Align columns**      | `column -t`                      | `Ctrl-u Alt-\|`             |
| **Align text table**   | `column -t -s '\|' -o '\|'`      | `Ctrl-u Alt-\|`            |
| **Number lines**       | `cat -n`                         | `Ctrl-u Alt-\|`             |
| **Word count**         | `wc`                             | `Alt-\|`                    |
| **Solve math**         | `bc`                             | `Alt-\|` or `Ctrl-u Alt-\|` |
| **Format JSON**        | `jq .`                           | `Alt-\|` or `Ctrl-u Alt-\|` |
| **Find typos**         | `aspell list`                    | `Alt-\|`                    |
| **Format C code**      | `clang-format`                   | `Ctrl-u Alt-\|`             |
| **Lint shell script**  | `shellcheck -`                   | `Alt-\|`                    |
| **Trim whitespace**    | `sed 's/[[:space:]]*$//'`        | `Ctrl-u Alt-\|`             |
| **De-duplicate lines** | `awk '!seen[$0]++'`              | `Ctrl-u Alt-\|`             |


### Shell Drawer
`Ctrl-x Ctrl-z` suspends `emil` while preserving the current editor screen. This permits shell commands to be executed in the terminal below the editor content, after which editing may be resumed with `fg`.

Notes:
   - `less` clears the terminal when it quits; `less -X` and `more` do not.
   - The named command `cd` (change directory) in `emil` does not also change the directory in the shell.

## System Clipboard Integration
`Ctrl-c` copies selected text to both the kill ring and the user's system clipboard when an OSC 52-enabled terminal client is used.

Selections larger than  74,993 bytes are not sent to the clipboard and a status message is displayed. Some terminal emulators have lower limits and will silently fail after writing only the first part of the text to the system clipboard.


## Editing Large Files

`emil` is not designed for editing very large files. Files larger than 1 GiB cannot be opened. A very large file filled with only very short lines will consume a large amount of memory. On extremely long lines typing will be slow (but mitigated by keyboard bursting).


## Internals

Each buffer is an array of logical lines (`erow`) holding raw UTF-8 bytes. Every buffer contains only valid UTF-8; files that fail validation are rejected at load time. Rendering and text layout never modify the buffer.

A row's display width is cached on the row and recomputed when the row is edited. The renderer calculates wrap positions only for the rows on screen, not for the whole buffer.

On each frame, the renderer reads raw bytes from the buffer and emits terminal-ready sequences directly into an append buffer. No intermediate render buffers exist. The append buffer is then written to the terminal and truncated.

All input is processed in a single loop:

1. Read keystroke
2. Execute command (may modify buffer)
3. Refresh screen: clamp window offsets, scroll, redraw, flush


## Contributing

Bug fixes, portability improvements, performance work, and general code quality
PRs are welcome. Please do not propose any new features.


## Credits and License

emil is a derivative of [`japanoise/emsys`](https://github.com/japanoise/emsys) and is distributed under the MIT License.

---

[^1]: [^1]: Cursor movement and deletion step by codepoint, not grapheme cluster. Word wrap and sentence navigation use script-specific heuristics for CJK, Thai, Lao, Khmer, and Indic scripts.
[^2]: Omitted from POSIX.1, see [Rationale](https://pubs.opengroup.org/onlinepubs/007904975/utilities/sh.html).
