# emil

`emil` is a minimalist, nonextensible , terminal-based editor with Emacs keybindings, and a strong emphasis on portability and simplicity.

This project is a fork of `japanoise/emsys`.

## Status

feature complete but unstable.

## Comparison to BSD's mg

### Advantages

- UTF8 support
- CUA mode option (if compiled in)
- Visual mark mode
- Soft line wrap

### Disadvantages

- Lacks Dired


## Roadmap

1. **feature completeness** — ✅ DONE

2. **stability** IN PROGRESS

3. **Buffer memory management upgrade**
   Investigate improved internal representations (e.g. gap buffer or arena-based approaches).

4. *Rendering system upgrade**

5. **Remove dependency on `subprocess.h`**

## Contributing

 Bug fixes, portability improvements, performance work, and general code quality PRs are welcome.
If you’d like to propose a new feature, please open an issue first: this project intentionally constrains scope to preserve a small codebase.


## Acknowledgements

Thanks to japanoise for creating `emsys` which was the foundation that made this project possible.
