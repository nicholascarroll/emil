
# emil

`emil` is a minimalist, Emacs-style editor for editing UTF-8 text files on VT100-compatible terminals.

`emil` has no external dependencies other than standard POSIX libraries and avoids common sources of complexity such as scripting languages, plugins, runtime configuration files, background network activity, or auto-save files.

`emil`is built in **C99** with **POSIX.1-2001** compliance, maximising portability and long-term buildability across Unix-like systems, including older, embedded, and future platforms.

This project is a fork of [`japanoise/emsys`](https://github.com/japanoise/emsys).

---
## Status

Feature complete, but **unstable**.

Current work focuses on cleaning up the architecture, fixing bugs, and improving maintainability. Contributions that simplify internal structure, improve portability, or fix correctness issues are especially welcome.


### Installation

**Unix / Linux / macOS**

```bash
make && sudo make install
```

**Android (Termux)**
- This will not include shall integration.
```bash
make android
```

**Windows (MSYS2)**  
- Run inside **MSYS2 proper** (not mingw64) — termios is required.  
- Install compilers and libraries:  
```bash
pacman -S msys2-devel msys2-runtime-devel
````

* Build and install:

```bash
make && make install
```

---

## Getting Started

Open a file:

```
emil file.txt
```

### Essential Commands

| Action                 | Keys            |
| ---------------------- | --------------- |
| Open file              | `Ctrl-x Ctrl-f` |
| Save file              | `Ctrl-x Ctrl-s` |
| Quit                   | `Ctrl-x Ctrl-c` |
| Cancel current command | `Ctrl-g`        |
| Undo                   | `Ctrl-z`        |

## CUA Shortcuts

* `Ctrl-C` / `Ctrl-V` / `Ctrl-X` — copy, paste, cut
* `Ctrl-Z` / `Ctrl-Shift-Z` — undo, redo

*These shortcuts coexist with Emacs bindings and do not replace them.*

---

## Shell-Oriented Editing

Basic movement and line-editing keys align with readline conventions found in Bash and zsh.

Optional shell integration (enabled at compile time) allows regions to be filtered through external commands or replaced with command output.

---

## Roadmap

1. **feature completeness** — ✅ DONE

2. **stability** IN PROGRESS

3. **Buffer memory management upgrade**
   Investigate improved internal representations (e.g. gap buffer or arena-based approaches).

4. **Rendering system upgrade**

5. **Remove dependency on `subprocess.h`**

## Contributing

Bug fixes, portability improvements, performance work, and general code quality PRs are welcome.
If you’d like to propose a new feature, please open an issue first: `emil` is deliberately small.  


## Acknowledgements

Thanks to japanoise for creating `emsys` which was the foundation that made this project possible.
