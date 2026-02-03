
# emil

`emil` is a minimalist, terminal-based text editor.

The editing experience will feel immediately familiar to users of **Emacs** or BSD’s `mg`, while remaining lightweight and approachable for users coming from simpler editors such as `nano`.

Editing and navigation keys are consistent with **Bash and zsh** shells. Shell integration (pipe text to shell command, replace regions with command output) is a compile-time option.

It has no external dependencies other than standard POSIX libraries and avoids common sources of complexity such as scripting languages, plugins, runtime configuration files, background network activity, or auto-save files.

`emil`is built in **C99** with **POSIX.1-2001** compliance, maximising portability and long-term buildability across Unix-like systems, including older, embedded, and future platforms.

This project is a fork of [`japanoise/emsys`](https://github.com/japanoise/emsys).

---
## Status

Feature complete, but **unstable**.

Current work focuses on cleaning up the architecture, fixing bugs, and improving maintainability. Contributions that simplify internal structure, improve portability, or fix correctness issues are especially welcome.

---

## Comparison to Alternatives

| Feature                           | emil | namo | mg  |
|----------------------------------|------|------|-----|
| Native UTF-8 support             | Yes  | Yes  | No  |
| Highlighted selection            | Yes  | Yes  | No  |
| Soft line wrapping               | Yes  | Yes  | No  |
| Readline-style editing (Bash/zsh)| Yes  | No   | Yes |
| Rectangle (column) editing       | Yes  | No   | Yes |
| Built-in file manager (Dired)    | No   | No   | Yes |
| CUA shortcuts (Ctrl-C / Ctrl-V)  | Yes  | No   | No  |


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
