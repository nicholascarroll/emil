
## [0.5.0] - 2026-05-08
- CJK and Indic sentence movement: #69, #71, #72, #73, #74
- Replaced the Memory Budget feature with simpler File size limit (1GB)
- Removed undo limit
- Palette for emojis and symbol chars #84.
- Now uses system wcwidth
- Temporarily added option to debug build `M-x toggle-wcwidth` to use bundled wcwidth
- Fixed #46, #54, #65, #66.

## [0.4.0] - 2026-04-23
- Remove dependency on subprocess.h
- Removed the dict shell script
- Refactored to use a mutation layer
- Implemented warning message system in RHS of status bar
- Remapped C-h to help message
- Resolved issues #29, #30, #31, #33, #40, #41, #49, #56, #57, #58
- Implemented Find File Read Only
- Open read-only if locked
- Changed memory budget to a simplified model
- Renamed EMIL_MAX_OPEN_BYTES to EMIL_BYTES_BUDGET

## [0.3.0] - 2026-03-30
Architecture and robustness improvements. Unstable.
- Unified memory budget (EMIL_MAX_TOTAL_BYTES, default 1 GB). View budget with
  `M-x editor-status`. Set budget at build time via -D flag.
- 2038 date cutover safe.
- Better signal  handling
- Mitigation of heap fragmention - compaction on save.
- Refactored to fully embrace global state and tidied naming conventions.
 
## [0.2.1] - 2026-03-19
Initial prerelease, The editor is unstable and not reliable for production use.
- Feature complete.
- Documentation complete.

