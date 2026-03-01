
## Reporting a Vulnerability

We use GitHub's **Private Vulnerability Reporting**.

Please navigate to the [Security tab](https://github.com/nicholascarroll/emil/security) and click "Report a vulnerability."

**Do not report security issues via public PRs or Issues.**

### Response Times

You will receive an initial response within 14 days.

---

## Hardening Options

`emil` allows you to strip high-risk features at compile time for use in restricted environments:

* **Shell Integration:** Use `-DEMIL_DISABLE_SHELL` to disable all external command execution.
* **Macro System:** Use `-DEMIL_DISABLE_MACROS` to completely remove the macro engine from the build.

### Macro Guardrails

If macros are enabled, they are governed by several security constraints:

* **Session-only:** Macros are stored in memory and cleared when the editor closes.
* **Non-recursive:** Macros cannot record or execute macros.
* **Restricted Commands:** Dangerous actions (specifically: opening file; save files
) are blocked during recording and execution to prevent accidental or malicious impacts in the filesystem.
* **Fixed Bindings:** Macros recording and execution can only be triggered via specific keys, preventing the hijacking of standard keys.
* **Read-only Registers:** To prevent payload injection, macro registers cannot be manually edited or modified.

---

## Project Roadmap

`emil` is currently at version **0.1.0** (Development).

* **Early 2026:** Planned Prerelease.
* **Early 2027:** Version 1.0 Release.
