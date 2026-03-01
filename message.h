#ifndef EMIL_MESSAGE_H
#define EMIL_MESSAGE_H

#include <stdarg.h>

/* Status message display */
void editorSetStatusMessage(const char *fmt, ...);

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

static const char *const msg_mark_set = "标记已设置。";
static const char *const msg_mark_cleared = "标记已清除";
static const char *const msg_mark_invalid = "标记无效。";
static const char *const msg_rectangle_on = "矩形模式已开启";
static const char *const msg_rectangle_off = "矩形模式已关闭";

static const char *const msg_kill_ring_empty = "剪切环为空。";
static const char *const msg_no_more_kill_entries =
	"剪切环中没有更多条目可粘贴！";
static const char *const msg_not_after_yank = "上一个命令不是粘贴";

static const char *const msg_no_undo = "没有更多撤销信息。";
static const char *const msg_no_redo = "没有更多重做信息。";

static const char *const msg_wrote_bytes = "已写入 %d 字节到 %s";
static const char *const msg_cant_open = "无法打开文件：%s";
static const char *const msg_save_aborted = "保存已中止。";
static const char *const msg_save_failed = "保存失败：%s";
static const char *const msg_file_not_found = "文件未找到：%s";
static const char *const msg_file_bad_utf8 = "文件 UTF-8 验证失败";
static const char *const msg_file_locked = "[文件被 PID %d 锁定]";
static const char *const msg_file_changed_on_disk = "[文件已被外部修改]";
static const char *const msg_lines_columns = "%d 行，%d 列";
static const char *const msg_dir_not_supported = "不支持编辑目录。";
static const char *const msg_inserted_lines = "从 %s 插入了 %d 行";
static const char *const msg_error_opening = "打开文件出错：%s";

static const char *const msg_canceled_replace = "已取消字符串替换。";
static const char *const msg_canceled_query_replace = "已取消交互式替换。";
static const char *const msg_replaced_n = "已替换 %d 处";
static const char *const msg_regex_error = "正则表达式错误：%s";
static const char *const msg_regex_compile = "无法编译正则表达式：%s";
static const char *const msg_no_match = "未匹配到 %s";
static const char *const msg_no_match_bracket = "[无匹配]";
static const char *const msg_backward_regex_todo = "反向正则搜索尚未实现";

static const char *const msg_nothing_to_complete = "此处无可补全内容。";

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

static const char *const msg_shell_canceled = "已取消 Shell 命令。";
static const char *const msg_shell_read_bytes = "已读取 %d 字节";
static const char *const msg_shell_exit_status = "Shell 命令退出状态为 %d";
static const char *const msg_pipe_unavailable = "此平台不支持管道命令";
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

static const char *const msg_mark_set = "Mark set.";
static const char *const msg_mark_cleared = "Mark Cleared";
static const char *const msg_mark_invalid = "Mark invalid.";
static const char *const msg_rectangle_on = "Rectangle mode ON";
static const char *const msg_rectangle_off = "Rectangle mode OFF";

static const char *const msg_kill_ring_empty = "Kill ring empty.";
static const char *const msg_no_more_kill_entries =
	"No more kill ring entries to yank!";
static const char *const msg_not_after_yank = "Previous command was not a yank";

static const char *const msg_no_undo = "No further undo information.";
static const char *const msg_no_redo = "No further redo information.";

static const char *const msg_wrote_bytes = "Wrote %d bytes to %s";
static const char *const msg_cant_open = "Can't open file: %s";
static const char *const msg_save_aborted = "Save aborted.";
static const char *const msg_save_failed = "Save failed: %s";
static const char *const msg_file_not_found = "File not found: %s";
static const char *const msg_file_bad_utf8 = "File failed UTF-8 validation";
static const char *const msg_file_locked = "[FILE LOCKED BY PID %d]";
static const char *const msg_file_changed_on_disk = "[FILE CHANGED ON DISK]";
static const char *const msg_lines_columns = "%d lines, %d columns";
static const char *const msg_dir_not_supported =
	"Directory editing not supported.";
static const char *const msg_inserted_lines = "Inserted %d lines from %s";
static const char *const msg_error_opening = "Error opening file: %s";

static const char *const msg_canceled_replace = "Canceled replace-string.";
static const char *const msg_canceled_query_replace = "Canceled query-replace.";
static const char *const msg_replaced_n = "Replaced %d instances";
static const char *const msg_regex_error = "Regex error: %s";
static const char *const msg_regex_compile = "Could not compile regex: %s";
static const char *const msg_no_match = "No match for %s";
static const char *const msg_no_match_bracket = "[No match]";
static const char *const msg_backward_regex_todo =
	"Backward regex search not yet implemented";

static const char *const msg_nothing_to_complete = "Nothing to complete here.";

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

static const char *const msg_shell_canceled = "Canceled shell command.";
static const char *const msg_shell_read_bytes = "Read %d bytes";
static const char *const msg_shell_exit_status =
	"Shell command exited with status %d";
static const char *const msg_pipe_unavailable =
	"Pipe command not available on this platform";
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

#endif

#endif /* EMIL_MESSAGE_H */
