#ifndef EMIL_MESSAGE_H
#define EMIL_MESSAGE_H

#include <stdarg.h>

/* Status message display */
void setStatusMessage(const char *fmt, ...);

/*
 * All user-facing messages for emil.
 * Build with: make CFLAGS="... -DEMIL_LANG_ZH"
 */

#if defined(EMIL_LANG_ZH)

/* 中文 */
static const char *const msg_quit = "退出";
static const char *const msg_canceled = "已取消。";

static const char *const msg_read_only = "缓冲区为只读";
static const char *const msg_writable = "缓冲区已设为可写";
static const char *const msg_buffer_empty = "缓冲区为空";
static const char *const msg_beginning_of_buffer = "缓冲区开头";
static const char *const msg_end_of_buffer = "缓冲区末尾";
static const char *const msg_new_file = "（新文件）";
static const char *const msg_find_file = "查找文件: %s";

static const char *const msg_mark_set = "标记已设置。";
static const char *const msg_mark_cleared = "标记已清除";
static const char *const msg_mark_invalid = "标记无效。";
static const char *const msg_mark_popped = "已弹出标记";
static const char *const msg_no_mark_set = "此缓冲区未设置标记。";

static const char *const msg_rectangle_on = "矩形模式已开启";
static const char *const msg_rectangle_off = "矩形模式已关闭";

static const char *const msg_kill_ring_empty = "剪切环为空。";
static const char *const msg_no_more_kill_entries =
	"剪切环中没有更多条目可粘贴！";
static const char *const msg_not_after_yank = "上一个命令不是粘贴";

static const char *const msg_undo = "撤销操作";
static const char *const msg_redo = "重做操作";
static const char *const msg_no_undo = "没有更多可撤销的操作";
static const char *const msg_no_redo = "没有更多可重做的操作";
static const char *const msg_unpaired_undo_redo =
	"未匹配的 %d 次撤销, %d 次重做。";

static const char *const msg_wrote_bytes = "已写入 %d 字节到 %s";
static const char *const msg_cant_open = "无法打开文件：%s";
static const char *const msg_save_aborted = "保存已中止。";
static const char *const msg_save_failed = "保存失败：%s";
static const char *const msg_file_not_found = "文件未找到：%s";
static const char *const msg_invalid_utf8 = "UTF-8 验证失败";
static const char *const msg_binary_file = "文件包含空字节（二进制文件？）";
static const char *const msg_no_glob_match = "没有匹配的文件: %s";

/*
 * Status-bar RHS warning messages.
 *
 * IMPORTANT: These three strings must fit within 13 display columns
 * once formatted.  The status bar's right-hand segment is 15 columns
 * wide with the leftmost 2 reserved as a gutter, so 13 columns are
 * available for content.  The renderer truncates at 13 display
 * columns regardless — but overflow means useful content (like the
 * PID) gets cut off.  Chinese glyphs are 2 columns each, so the
 * budget is roughly 6 CJK characters plus a few ASCII.
 */
static const char *const msg_file_locked = "[%d 锁定]";
static const char *const msg_file_changed_on_disk = "[文件已修改]";
static const char *const msg_memory_over_limit = "[内存超限!]";
/*-------------------------------------------------------------*/
static const char *const msg_lines_columns = "%d 行，%d 列";
static const char *const msg_dir_not_supported = "不支持编辑目录。";
static const char *const msg_inserted_lines = "从 %s 插入了 %d 行";
static const char *const msg_error_opening = "打开文件出错：%s";
static const char *const msg_changed_dir = "已更改目录";
static const char *const msg_current_dir = "当前目录: %s";
static const char *const msg_indeterminate_cd = "cd: 无法确定当前目录";

static const char *const msg_canceled_replace = "已取消字符串替换。";
static const char *const msg_canceled_query_replace = "已取消交互式替换。";
static const char *const msg_replaced_n = "已替换 %d 处";
static const char *const msg_regex_error = "正则表达式错误：%s";
static const char *const msg_regex_compile = "无法编译正则表达式：%s";
static const char *const msg_no_match = "未匹配到 %s";
static const char *const msg_no_match_bracket = "[无匹配]";

static const char *const msg_nothing_to_complete = "此处无可补全内容。";
static const char *const msg_possible_completions = "可能的补全 (%d):";
static const char *const msg_complete_not_unique = "[已补全，但有多个匹配项]";

static const char *const msg_indent_tabs = "缩进已设为制表符";
static const char *const msg_indent_spaces = "缩进已设为 %i 个空格";

static const char *const msg_cannot_transpose = "此处无法转置";

static const char *const msg_no_other_windows = "没有其他窗口可选择";
static const char *const msg_no_windows_delete = "没有其他窗口可删除";
static const char *const msg_cant_kill_last_window = "无法关闭最后一个窗口";

static const char *const msg_switched_to = "已切换到缓冲区 %s";
static const char *const msg_no_buffer_named = "没有名为 ‘%s’ 的缓冲区";
static const char *const msg_no_buffer_switch = "没有可切换的缓冲区";
static const char *const msg_buffer_switch_canceled = "缓冲区切换已取消";

static const char *const msg_recording = "正在录制宏…";
static const char *const msg_already_recording = "已在录制中";
static const char *const msg_not_recording = "未在录制";
static const char *const msg_macro_recorded = "宏已录制（%d 键）";
static const char *const msg_no_macro = "没有已录制的宏";
static const char *const msg_macro_depth = "宏递归深度超限";
static const char *const msg_macro_blocked = "录制/回放宏时不可用";

static const char *const msg_shell_canceled = "已取消 Shell 命令。";
static const char *const msg_shell_read_bytes = "已读取 %d 字节";
static const char *const msg_shell_exit_status = "Shell 命令退出状态为 %d";
static const char *const msg_shell_disabled = "Shell 集成已禁用";

static const char *const msg_register_empty = "寄存器 %s 为空。";
static const char *const msg_no_command = "未找到命令";

static const char *const msg_trailing_removed = "已删除 %d 个尾随字符";
static const char *const msg_no_change = "无变化。";

static const char *const msg_unknown_ctrl = "未知命令 C-%c";
static const char *const msg_unknown_cx = "未知命令 C-x %c";
static const char *const msg_unknown_meta = "未知命令 M-%s";

static const char *const msg_unsaved_quit =
	"有未保存的更改。确定要退出吗？（y 或 n）";
static const char *const msg_buffer_modified_kill =
	"缓冲区 %.20s 已修改；仍要关闭吗？（y 或 n）";

static const char *const msg_no_piped_input = "标准输入: 无管道输入";
static const char *const msg_no_symbol_at_point = "当前光标处无符号";
static const char *const msg_tag_not_found = "未找到标签: %s";
static const char *const msg_tag = "标签: %s";
static const char *const msg_tag_stack_empty = "标签栈为空";
static const char *const msg_no_file_extension = "无文件扩展名";
static const char *const msg_no_ext_mapping = "没有 %s 的头文件/主体映射";
static const char *const msg_no_ext_file = "没有对应的文件: %s";
static const char *const msg_buffer_without_file = "缓冲区未关联文件";
static const char *const msg_diff_buffer_matches_file = "缓冲区内容与文件一致";
static const char *const msg_diff_cannot_create_temp =
	"Diff 失败: 无法创建临时文件";
static const char *const msg_diff_cannot_write = "Diff 失败: 写入错误";
static const char *const msg_diff_cannot_subprocess =
	"Diff 失败: 无法创建子进程";
static const char *const msg_diff_no_differences = "无差异";
static const char *const msg_diff_failed = "Diff 失败 (退出状态 %d)";
static const char *const msg_unknown_cx_x = "未知命令 C-x x %c";
static const char *const msg_memory_limit = "打开文件总量超限";

#else

/* ENGLISH (default) */
static const char *const msg_quit = "Quit";
static const char *const msg_canceled = "Canceled.";

static const char *const msg_read_only = "Buffer is read-only";
static const char *const msg_writable = "Buffer set to writable";
static const char *const msg_buffer_empty = "Buffer is empty";
static const char *const msg_beginning_of_buffer = "Beginning of buffer";
static const char *const msg_end_of_buffer = "End of buffer";
static const char *const msg_new_file = "(New file)";
static const char *const msg_find_file = "Find File: %s";

static const char *const msg_mark_set = "Mark set.";
static const char *const msg_mark_cleared = "Mark deactivated";
static const char *const msg_mark_invalid = "Mark invalid.";
static const char *const msg_mark_popped = "Mark popped.";
static const char *const msg_no_mark_set = "No mark set in this buffer.";

static const char *const msg_rectangle_on = "Rectangle mode ON";
static const char *const msg_rectangle_off = "Rectangle mode OFF";

static const char *const msg_kill_ring_empty = "Kill ring empty.";
static const char *const msg_no_more_kill_entries =
	"No more kill ring entries to yank!";
static const char *const msg_not_after_yank = "Previous command was not a yank";

static const char *const msg_undo = "Undo.";
static const char *const msg_redo = "Redo.";
static const char *const msg_no_undo = "No further undo information.";
static const char *const msg_no_redo = "No further redo information.";
static const char *const msg_unpaired_undo_redo =
	"Unpaired %d undos, %d redos.";

static const char *const msg_wrote_bytes = "Wrote %d bytes to %s";
static const char *const msg_cant_open = "Can't open file: %s";
static const char *const msg_save_aborted = "Save aborted.";
static const char *const msg_save_failed = "Save failed: %s";
static const char *const msg_file_not_found = "File not found: %s";
static const char *const msg_invalid_utf8 = "Failed UTF-8 validation";
static const char *const msg_binary_file =
	"File contains null bytes (binary file?)";
static const char *const msg_no_glob_match = "No matching files: %s";
/*
 * Status-bar RHS warning messages.
 *
 * The status bar's right-hand segment is 15 columns, but the first
 * 2 are reserved as a visual gutter (sep), leaving 13 display
 * columns for actual content.  The renderer truncates at 13 if the
 * formatted string is longer — so PIDs are placed early in
 * msg_file_locked to keep the PID visible when large values would
 * otherwise push the closing bracket off the end.
 */
static const char *const msg_file_locked = "[LOCK %d]";
static const char *const msg_file_changed_on_disk = "[DISK CHG!]";
static const char *const msg_memory_over_limit = "[MEM OVER!]";
/*------------------------------------------------------------*/

static const char *const msg_lines_columns = "%d lines, %d columns";
static const char *const msg_dir_not_supported =
	"Directory editing not supported.";
static const char *const msg_inserted_lines = "Inserted %d lines from %s";
static const char *const msg_error_opening = "Error opening file: %s";
static const char *const msg_changed_dir = "Changed directory";
static const char *const msg_current_dir = "Current directory: %s";
static const char *const msg_indeterminate_cd =
	"cd: cannot determine current directory";

static const char *const msg_canceled_replace = "Canceled replace-string.";
static const char *const msg_canceled_query_replace = "Canceled query-replace.";
static const char *const msg_replaced_n = "Replaced %d instances";
static const char *const msg_regex_error = "Regex error: %s";
static const char *const msg_regex_compile = "Could not compile regex: %s";
static const char *const msg_no_match = "No match for %s";
static const char *const msg_no_match_bracket = "[No match]";

static const char *const msg_nothing_to_complete = "Nothing to complete here.";
static const char *const msg_possible_completions =
	"Possible completions (%d):";
static const char *const msg_complete_not_unique = "[complete, but not unique]";

static const char *const msg_indent_tabs = "Indentation set to tabs";
static const char *const msg_indent_spaces = "Indentation set to %i spaces";

static const char *const msg_cannot_transpose = "Cannot transpose here";

static const char *const msg_no_other_windows = "No other windows to select";
static const char *const msg_no_windows_delete = "No other windows to delete";
static const char *const msg_cant_kill_last_window = "Can't kill last window";

static const char *const msg_switched_to = "Switched to buffer %s";
static const char *const msg_no_buffer_named = "No buffer named '%s'";
static const char *const msg_no_buffer_switch = "No buffer to switch to";
static const char *const msg_buffer_switch_canceled = "Buffer switch canceled";

static const char *const msg_recording = "Recording macro...";
static const char *const msg_already_recording = "Already recording";
static const char *const msg_not_recording = "Not recording";
static const char *const msg_macro_recorded = "Macro recorded (%d keys)";
static const char *const msg_no_macro = "No macro recorded";
static const char *const msg_macro_depth = "Macro recursion depth exceeded";
static const char *const msg_macro_blocked = "Not available during macro";

static const char *const msg_shell_canceled = "Canceled shell command.";
static const char *const msg_shell_read_bytes = "Read %d bytes";
static const char *const msg_shell_exit_status =
	"Shell command exited with status %d";
static const char *const msg_shell_disabled = "Shell integration disabled";

static const char *const msg_register_empty = "Register %s is empty.";
static const char *const msg_no_command = "No command found";

static const char *const msg_trailing_removed =
	"%d trailing characters removed";
static const char *const msg_no_change = "No change.";

static const char *const msg_unknown_ctrl = "Unknown command C-%c";
static const char *const msg_unknown_cx = "Unknown command C-x %c";
static const char *const msg_unknown_meta = "Unknown command M-%s";

static const char *const msg_unsaved_quit =
	"There are unsaved changes. Really quit? (y or n)";
static const char *const msg_buffer_modified_kill =
	"Buffer %.20s modified; kill anyway? (y or n)";

static const char *const msg_no_piped_input = "stdin: no piped input";
static const char *const msg_no_symbol_at_point = "No symbol at point";
static const char *const msg_tag_not_found = "Tag not found: %s";
static const char *const msg_tag = "Tag: %s";
static const char *const msg_tag_stack_empty = "Tag stack empty";
static const char *const msg_no_file_extension = "No file extension";
static const char *const msg_no_ext_mapping = "No header/body mapping for %s";
static const char *const msg_no_ext_file = "No counterpart file: %s";
static const char *const msg_buffer_without_file = "Buffer has no file";
static const char *const msg_diff_buffer_matches_file = "Buffer matches file";
static const char *const msg_diff_cannot_create_temp =
	"Diff failed: cannot create temp file";
static const char *const msg_diff_cannot_write = "Diff failed: write error";
static const char *const msg_diff_cannot_subprocess =
	"Diff failed: cannot create subprocess";
static const char *const msg_diff_no_differences = "No differences";
static const char *const msg_diff_failed = "Diff failed (exit status %d)";
static const char *const msg_unknown_cx_x = "Unknown command C-x x %c";
static const char *const msg_memory_limit = "Open-file limit exceeded";

#endif

#endif /* EMIL_MESSAGE_H */
