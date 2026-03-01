
## Reporting a Vulnerability

We use GitHub's **Private Vulnerability Reporting**.
Please navigate to the [Security tab](https://github.com/nicholascarroll/emil/security) and click "Report a vulnerability" to submit a report privately.

**Do not report security issues via public Pull Requests or Issues.**

### Issue Response Times

* **Initial Acknowledgment:** Within 7 days.
* **Status Updates:** Every 14 days until resolution.

---

## Security Architecture & Hardening

`emil` is designed for high-assurance environments. We provide two levels of hardening to satisfy different compliance requirements.

### 1. Compile-Time Feature Stripping

For environments requiring a minimal attack surface, specific high-risk features can be physically removed from the binary at compile time:

| Feature | Compiler Flag | Description |
| --- | --- | --- |
| **Shell Integration** | `-DEMIL_DISABLE_SHELL` | Disables all system shell escapes and external command execution. |
| **Macro System** | `-DEMIL_DISABLE_MACROS` | Completely removes the macro recording and playback engine. |

### 2. Runtime Macro Guardrails

When macros are enabled, `emil` enforces a **"High-Assurance Macro"** model:

* **Volatile Memory Only:** Macros exist only in RAM and are wiped immediately upon session exit.
* **Command Filtering:** High-risk operations (e.g., `Save`, `Open File`, `Quit`) are blocked during the recording phase.
* **Anti-Hijacking:** Macros are mapped only to fixed `@` registers; standard keybindings cannot be remapped to macros.
* **Integrity Protection:** Macro registers are "Opaque." They cannot be manually edited, "pasted" into, or modified, preventing malicious payload injection.
* **Recursion Block:** To prevent DoS attacks, a macro cannot call another macro or itself.

---

## Stable Releases

`emil` is currently in active development (**Version 0.1.0**). We are prioritizing security architecture over feature parity during this phase.

* **Early 2026:** Alpha Prerelease (Feature Freeze).
* **Early 2027:** Version 1.0 (Stable Release & Security Audit).

