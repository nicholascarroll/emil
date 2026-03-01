## Hardening Options

`emil` allows you to strip high-risk features at compile time for use in restricted environments:

* **Shell Integration:** Use `-DEMIL_DISABLE_SHELL` to disable all external command execution.
* **Macro System:** Use `-DEMIL_DISABLE_MACROS` to completely remove the macro engine from the build.

## Macro Guardrails

If macros are enabled, they are governed by several security constraints:

* **Session-only:** Macros are stored in memory and cleared when the editor closes.
* **Non-recursive:** Macros cannot record or execute macros.
* **Restricted Commands:** Dangerous actions (specifically: opening file; save files
) are blocked during recording and execution to prevent accidental or malicious impacts in the filesystem.
* **Fixed Bindings:** Macros recording and execution can only be triggered via specific keys, preventing the hijacking of standard keys.
* **Read-only Registers:** To prevent payload injection, macro registers cannot be manually edited or modified.

---
