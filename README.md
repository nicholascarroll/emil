
# emil ([埃米尔](./README.zh-CN.md))

[![OpenSSF Best Practices](https://www.bestpractices.dev/projects/11997/badge)](https://www.bestpractices.dev/projects/11997)

`emil` is a small, portable terminal text editor designed specifically for UTF-8 files. It provides a subset of *emacs* commands on VT100-compatible terminals.

`emil` is written in standard C99 and depends only on a POSIX.1-2001–compliant environment. It eschews common sources of complexity: scripting support, plugin systems, configuration files, background network activity, or auto-save files.

## Status

Feature complete, but **unstable**.

Current work focuses on cleaning up the architecture, fixing bugs, and improving 
maintainability. Contributions that simplify internal structure, improve 
portability, or fix correctness issues are especially welcome.

## Functional Capabilities

- Kill ring
- Visual text selection
- Incremental regexp search and replace
- Word wrap
- Multiple windows
- Rectangle editing
- Keystroke macros
- Registers
- Shell integration

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

**Windows (MSYS2)**  
- Run in an **MSYS2** terminal (not mingw64, which lacks termios).  
- Install compilers and libraries:  

```bash
pacman -S msys2-devel msys2-runtime-devel
````

* Build and install:

```bash
make && make install
```

## Getting Started

Open a file:

```
emil file.txt
```

### Essential Commands

| Action                 | Command         |
| ---------------------- | --------------- |
| Open file              | `Ctrl-x Ctrl-f` |
| Save file              | `Ctrl-x Ctrl-s` |
| Quit emil              | `Ctrl-x Ctrl-c` |
| Mark (to select text)  | `Ctrl-SPACE`    |
| Cut                    | `Ctrl-w`        |
| Copy                   | `Alt-w`         |
| Paste                  | `Ctrl-y`        |
| Search                 | `Ctrl-s`        |
| Cancel                 | `Ctrl-g`        |

For the complete command reference, see the man page:

```
man emil
```

## Shell-Oriented Editing

Although POSIX.1 standardises only the *vi-mode* command-line editing interface, many shells (including Bash) also provide an *emacs-mode* (activated by `set -o emacs`). 
This mode was considered for standardisation but was ultimately omitted. 
The official [rationale](https://pubs.opengroup.org/onlinepubs/007904975/utilities/sh.html) explains:

> “In early proposals, the KornShell-derived *emacs* mode of command line editing was included, even though the *emacs* editor itself was not. The community of *emacs* proponents was adamant that the full *emacs* editor not be standardized because they were concerned that an attempt to standardize this very powerful environment would encourage vendors to ship strictly conforming versions lacking the extensibility required by the community. The author of the original *emacs* program also expressed his desire to omit the program. Furthermore, there were a number of historical systems that did not include *emacs*, or included it without supporting it, but there were very few that did not include and support *vi*. The shell *emacs* command line editing mode was finally omitted because it became apparent that the KornShell version and the editor being distributed with the GNU system had diverged in some respects. The author of *emacs* requested that the POSIX *emacs* mode either be deleted or have a significant number of unspecified conditions. Although the KornShell author agreed to consider changes to bring the shell into alignment, the standard developers decided to defer specification at that time. At the time, it was assumed that convergence on an acceptable definition would occur for a subsequent draft, but that has not happened, and there appears to be no impetus to do so."

The incompatibilities are minor; the tty driver has treated Ctrl-w as WERASE since 4BSD, and this is overridden by the following entry in ~/.inputrc:
```inputrc
$include /etc/inputrc          # retain system-wide defaults
set bind-tty-special-chars off

"\C-w": kill-region
"\M-w": copy-region-as-kill
```
In `emil`, `Ctrl-w` kills the marked region as in `emacs`, but if no region is marked, it kills the previous word.

In Readline and in `emil`, `Ctrl-h` deletes the previous character, whereas in `emacs` it is the help prefix key.

Readline supports two ways to delete to the beginning of the line: `Ctrl-x BACKSPACE` (supported in *emacs*) and the more ergonomic `Ctrl-u` which conflicts with the *emacs* universal argument. `emil` resolves the conflict by binding `Ctrl-u Ctrl-a` to delete to the beginning of the line.

### Shell Integration

Shell integration is a compile-time option (enabled by default). It enables shell commands to be used on the buffer:

- **`Alt-|`** (shell-command-on-region)    
  Takes the current region, feeds it to the shell command you type, and **displays the output** in a temporary `*Shell Command Output*` buffer (or the echo area if the output is small).  

- **`Ctrl-u Alt-|`**    
  Takes the current region, feeds it to the shell command, and **replaces the region** with the output of the command.  

- **`Alt-x diff-buffer-with-file`**    
  Shows unsaved changes.

Shell integration can be disabled at build time with the compiler flag `-DEMIL_DISABLE_SHELL`.


### Shell Drawer
`Ctrl-x Ctrl-z` suspends `emil` while preserving the current editor screen. This permits shell commands to be executed in the terminal below the editor content, after which editing may be resumed with `fg`.

Note: `less` clears the terminal when it quits; `less -X` and `more` do not.

### System Clipboard Integration
`Ctrl-c` copies selected text to both the kill ring and the user's system clipboard 
when an OSC 52 enabled terminal client is used.

—-

### Roadmap

1. **Version 0.1.0** [DONE] ✅
   - From here on we use `emil` to code `emil`

2. **Version 0.1.1 Feature complete**  [DONE] ✅.   

3. **Version 0.2.0 Stable Preview**  [WIP]⚠️🚧🔨👷    
   - Most known bugs fixed
   - First GitHub release (prerelease tag)  
   - Announced on forums (HN, Reddit, etc.)  

4. **Rendering system upgrade**
   Test performance over SSH and the rendering system.

5. **Remove dependency on `subprocess.h`**
   Internalize the code being used for pipe/exec/fork.

6. **Version 1.0.0 Bug free and loving it**
   - Tested on Solaris, AIX, Linux, BSD, MSYS2
     OSX, Android. 
   - Tested with IME and international keyboards
   - Included in Linux distribution repositories

### Display notes

On a raw Linux virtual console (Ctrl+Alt+F3 etc.) the in-kernel console cannot display Chinese.  
Use **kmscon** or **fbterm** instead.

## Contributing

Bug fixes, portability improvements, performance work, and general code quality 
PRs are welcome. If you’d like to propose a new feature, please open an issue 
first: `emil` is deliberately small.  


## Credits and License

emil is a derivative of [`japanoise/emsys`](https://github.com/japanoise/emsys) 
and is not affiliated with the Free Software Foundation or the GNU Project. 
Distributed under the MIT License.
