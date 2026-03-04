
# emil ([埃米尔](./README.zh-CN.md))

[![OpenSSF Best Practices](https://www.bestpractices.dev/projects/11997/badge)](https://www.bestpractices.dev/projects/11997)

`emil` is a small, portable text editor for UTF-8 files, providing a core subset of emacs commands in the terminal. 

Written in standard C99, `emil` runs on any system providing a minimal POSIX.1-2001 interface (single-process subset) and a VT100-compatible terminal. It eschews common sources of complexity: scripting, plugins, configuration files, background network activity, or auto-save files. 


## Status

Feature complete, but **unstable**.

Current work focuses on cleaning up the architecture, fixing bugs, and improving 
maintainability. Contributions that simplify internal structure, improve 
portability, or fix correctness issues are especially welcome.

## Functional Capabilities

- Kill ring ('clipboard history')
- Visual text selection
- Incremental regex search and replace
- Word wrap
- Multiple windows
- Rectangle editing
- Keystroke macros
- Registers
- Mark ring
- Shell integration
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

**Windows (MSYS2)**  
- Run in an **MSYS2** terminal (not mingw64, which lacks termios).  
- Install compilers and libraries:  

```bash
pacman -S msys2-devel msys2-runtime-devel
```

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
In `emil`, as in Readline,  `Ctrl-h` deletes the previous character, whereas in `emacs` it is the help prefix key.

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


#### Example uses of Shell Command

Below are common "recipes" using standard Unix utilities. 

| Task | Command | Recommended Binding |
| :--- | :--- | :--- |
| **Fill region** | `fmt` | `Ctrl-u Alt-\|` |
| **Sort lines** | `sort` | `Ctrl-u Alt-\|` |
| **Align Columns** | `column -t` | `Ctrl-u Alt-\|` |
| **Align Org Table** | `column -t -s '\|'` | `Ctrl-u Alt-\|` |
| **Number Lines** | `cat -n` | `Ctrl-u Alt-\|` |
| **Word Count** | `wc` | `Alt-\|` |
| **Solve Math** | `bc` | `Alt-\|` or `Ctrl-u Alt-\|` |
| **Format JSON** | `jq .` | `Alt-\|` or `Ctrl-u Alt-\|` |
| **Find Typos** | `aspell list` | `Alt-\|` |
| **Format C Code** | `make format` | `Ctrl-u Alt-\|` |
| **Lint Shell Script** | `shellcheck` | `Ctrl-u Alt-\|` |
| **Trim Trailing Whitespace** | `sed 's/[[:space:]]\+$//'` | `Ctrl-u Alt-\|` |
| **Remove Duplicate Lines** | `awk '!seen[$0]++'` | `Ctrl-u Alt-\|` |

More complex commands can be converted into shell scripts. For example: to add a dictionary lookup, create a file named `edict` in your `$PATH`:

```bash
#!/bin/sh
# edict: Look up the word provided via stdin
word=$(cat)
curl -s "dict://dict.org/d:${word}"
```

Now, you can simply highlight a word in emil and type `M-| edict` to see the definition.

ChatGPT:

```bash
#!/bin/sh
# Send stdin to an AI API and print the reply

[ -z "$OPENAI_API_KEY" ] && {
    echo "OPENAI_API_KEY not set" >&2
    exit 1
}

prompt=$(cat)

payload=$(jq -n \
  --arg input "$prompt" \
  '{model:"gpt-4.1-mini", input:$input}')

curl -s https://api.openai.com/v1/responses \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer '"$OPENAI_API_KEY"'" \
  -d "$payload"
```

### Shell Drawer
`Ctrl-x Ctrl-z` suspends `emil` while preserving the current editor screen. This permits shell commands to be executed in the terminal below the editor content, after which editing may be resumed with `fg`.

Notes: 
   - `less` clears the terminal when it quits; `less -X` and `more` do not.
   - The named command `cd` (change directory) in `emil` does not also change the directory in the shell.

### System Clipboard Integration
`Ctrl-c` copies selected text to both the kill ring and the user's system clipboard 
when an OSC 52 enabled terminal client is used.

—-

## Roadmap

1. **Version 0.1.0** [DONE] ✅
   - From here on we use `emil` to code `emil`

2. **Version 0.1.1 Feature complete**  [WIP] 🔨
   - Mark ring (local buffer)
   - Polishing up filename display UX
   - Visual row up/down (C-p / C-n)
   - Parenthesis matching with reverse video
   - jump to matching parenthesis

3. **Version 0.2.0 Stable Preview**  [WIP]⚠️🚧🔨👷    
   - Most known bugs fixed
   - First GitHub release (prerelease tag)  
   - Announced on forums (HN, Reddit, etc.)  

4. **Rendering optimizations**
   Reduce bytes over wire with a grab bag of   
   render hints sent by edit, move and scroll operations.
   - enabled by a run-time toggle.

5. **Remove dependency on `subprocess.h`**
   Internalize the code being used for pipe/exec/fork.

6. **Version 1.0.0 Bug free and loving it**
   - Tested on Solaris, AIX, Linux, BSD, MSYS2
     macOS, Android. 
   - Tested with raw console and various terminal emulators
   - Tested on RTEMS and NuttX
   - Tested with IME and international keyboards
   - Included in Linux distribution repositories

---

## Raw Console

On a raw Linux virtual console (Ctrl+Alt+F3 etc.) the in-kernel console cannot display Chinese. Use **kmscon** or **fbterm**.

## Internals

Each buffer is an array of logical lines (`erow`), where each line stores its raw
UTF-8 bytes. All buffers contain valid UTF-8; files that fail validation are rejected
at load time. The buffer is never modified by rendering or text layout concerns.

Display widths are cached per-row and recomputed only when a row is edited.
A cumulative screen-line cache maps logical rows to screen positions, enabling
efficient scrolling when word wrap is active.

On each frame, the renderer reads raw bytes from the buffer and emits
terminal-ready sequences directly into a disposable append buffer. No intermediate
render buffers exist. The append buffer is written to the terminal in a single
`write()` call.

The rendering system uses only cursor positioning (CSI H), erase-to-end-of-line
(CSI K), reverse video (CSI 7m / CSI 0m), and clear-below (CSI J). Scroll region
manipulation and line insert/delete are not used by the core renderer; they are
planned as optional render acceleration layer enabled by a run-time toggle.

All input is processed in a single loop:

1. Read keystroke
2. Execute command (may modify buffer)
3. Refresh screen: clamp window offsets, rebuild caches if stale, scroll, redraw, flush


## Contributing

Bug fixes, portability improvements, performance work, and general code quality 
PRs are welcome. If you’d like to propose a new feature, please open an issue 
first: `emil` is deliberately small.  


## Credits and License

emil is a derivative of [`japanoise/emsys`](https://github.com/japanoise/emsys) and is not affiliated with the Free Software Foundation or the GNU Project. 
Distributed under the MIT License.

---

[^1]: Omitted from POSIX.1, see [Rationale](https://pubs.opengroup.org/onlinepubs/007904975/utilities/sh.html).

