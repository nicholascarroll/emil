/*
 * test_system_wcwidth.c — Exhaustive system wcwidth() probe.
 *
 * Compares this platform's libc wcwidth() against the bundled
 * ridiculousfish/widecharwidth tables (Unicode 17.0) across the
 * entire Unicode range, organized by capability categories.
 *
 * Usage:
 *   cc -std=c99 -D_XOPEN_SOURCE=700 -D_DEFAULT_SOURCE -I. \
 *      -o test_system_wcwidth tests/test_system_wcwidth.c
 *   ./test_system_wcwidth                    # summary only
 *   ./test_system_wcwidth mismatches.txt     # write details to file
 *
 * Exit code:
 *   0 = all categories match
 *   1 = at least one mismatch (not necessarily a problem — see report)
 */

#define _XOPEN_SOURCE 700

#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <stdint.h>

/* Include the bundled tables directly for reference comparison. */
#include "widechar_width_c.h"

/* ---- Category definitions ---- */

struct category {
	const char *name;
	int tested;
	int matched;
	int mismatched;
	int sys_missing;  /* system returned -1 where bundled has a width */
};

enum {
	CAT_ASCII = 0,
	CAT_LATIN,
	CAT_GREEK_CYRILLIC,
	CAT_ARABIC_HEBREW,
	CAT_DEVANAGARI_INDIC,
	CAT_CJK_UNIFIED,
	CAT_CJK_COMPAT,
	CAT_HANGUL,
	CAT_HIRAGANA_KATAKANA,
	CAT_COMBINING_MARKS,
	CAT_BOX_DRAWING,
	CAT_MATH_SYMBOLS,
	CAT_EMOJI_BASIC,
	CAT_EMOJI_SUPPLEMENTAL,
	CAT_EMOJI_EXTENDED,
	CAT_PRIVATE_USE,
	CAT_MISC_SYMBOLS,
	CAT_OTHER,
	CAT_COUNT
};

static struct category categories[CAT_COUNT] = {
	[CAT_ASCII]             = { "ASCII (U+0020..007E)",           0, 0, 0, 0 },
	[CAT_LATIN]             = { "Latin Extended (U+0080..024F)",  0, 0, 0, 0 },
	[CAT_GREEK_CYRILLIC]    = { "Greek/Cyrillic (U+0370..052F)",  0, 0, 0, 0 },
	[CAT_ARABIC_HEBREW]     = { "Arabic/Hebrew (U+0590..06FF)",   0, 0, 0, 0 },
	[CAT_DEVANAGARI_INDIC]  = { "Devanagari/Indic (U+0900..0DFF)",0, 0, 0, 0 },
	[CAT_CJK_UNIFIED]       = { "CJK Unified (U+4E00..9FFF)",    0, 0, 0, 0 },
	[CAT_CJK_COMPAT]        = { "CJK Compat (U+F900..FAFF)",     0, 0, 0, 0 },
	[CAT_HANGUL]            = { "Hangul (U+AC00..D7AF)",          0, 0, 0, 0 },
	[CAT_HIRAGANA_KATAKANA] = { "Hiragana/Katakana (U+3040..30FF)", 0, 0, 0, 0 },
	[CAT_COMBINING_MARKS]   = { "Combining Marks (U+0300..036F)", 0, 0, 0, 0 },
	[CAT_BOX_DRAWING]       = { "Box Drawing (U+2500..257F)",     0, 0, 0, 0 },
	[CAT_MATH_SYMBOLS]      = { "Math Symbols (U+2200..22FF)",    0, 0, 0, 0 },
	[CAT_EMOJI_BASIC]       = { "Emoji: Misc Symbols (U+2600..26FF)", 0, 0, 0, 0 },
	[CAT_EMOJI_SUPPLEMENTAL]= { "Emoji: Supplemental (U+1F300..1F5FF)", 0, 0, 0, 0 },
	[CAT_EMOJI_EXTENDED]    = { "Emoji: Faces+ (U+1F600..1FAFF)", 0, 0, 0, 0 },
	[CAT_PRIVATE_USE]       = { "Private Use (U+E000..F8FF)",     0, 0, 0, 0 },
	[CAT_MISC_SYMBOLS]      = { "Misc Symbols (U+2000..2BFF excl above)", 0, 0, 0, 0 },
	[CAT_OTHER]             = { "Other",                          0, 0, 0, 0 },
};

static int classify(uint32_t cp) {
	if (cp >= 0x0020 && cp <= 0x007E) return CAT_ASCII;
	if (cp >= 0x0080 && cp <= 0x024F) return CAT_LATIN;
	if (cp >= 0x0300 && cp <= 0x036F) return CAT_COMBINING_MARKS;
	if (cp >= 0x0370 && cp <= 0x052F) return CAT_GREEK_CYRILLIC;
	if (cp >= 0x0590 && cp <= 0x06FF) return CAT_ARABIC_HEBREW;
	if (cp >= 0x0900 && cp <= 0x0DFF) return CAT_DEVANAGARI_INDIC;
	if (cp >= 0x2200 && cp <= 0x22FF) return CAT_MATH_SYMBOLS;
	if (cp >= 0x2500 && cp <= 0x257F) return CAT_BOX_DRAWING;
	if (cp >= 0x2600 && cp <= 0x26FF) return CAT_EMOJI_BASIC;
	if (cp >= 0x2000 && cp <= 0x2BFF) return CAT_MISC_SYMBOLS;
	if (cp >= 0x3040 && cp <= 0x30FF) return CAT_HIRAGANA_KATAKANA;
	if (cp >= 0x4E00 && cp <= 0x9FFF) return CAT_CJK_UNIFIED;
	if (cp >= 0xAC00 && cp <= 0xD7AF) return CAT_HANGUL;
	if (cp >= 0xE000 && cp <= 0xF8FF) return CAT_PRIVATE_USE;
	if (cp >= 0xF900 && cp <= 0xFAFF) return CAT_CJK_COMPAT;
	if (cp >= 0x1F300 && cp <= 0x1F5FF) return CAT_EMOJI_SUPPLEMENTAL;
	if (cp >= 0x1F600 && cp <= 0x1FAFF) return CAT_EMOJI_EXTENDED;
	return CAT_OTHER;
}

/* Map widechar_wcwidth negative values to the same scheme as
 * emil_bundled_wcwidth (in wcwidth.c). */
static int bundled_width(uint32_t cp) {
	int w = widechar_wcwidth(cp);
	if (w >= 0) return w;
	switch (w) {
	case widechar_nonprint:       return -1; /* keep -1 for "not printable" */
	case widechar_combining:      return 0;
	case widechar_ambiguous:      return 1;
	case widechar_private_use:    return 1;
	case widechar_unassigned:     return -1; /* treat as "not assigned" */
	case widechar_widened_in_9:   return 2;
	case widechar_non_character:  return -1;
	default:                      return 1;
	}
}

int main(int argc, char **argv) {
	FILE *detail = NULL;
	if (argc > 1) {
		detail = fopen(argv[1], "w");
		if (!detail) {
			fprintf(stderr, "Cannot open %s for writing\n", argv[1]);
			return 2;
		}
	}

	/* Set up locale — try the same cascade as the editor. */
	const char *attempts[] = { "", "C.UTF-8", "en_US.UTF-8", NULL };
	const char *active_locale = "NONE";
	for (int i = 0; attempts[i]; i++) {
		const char *r = setlocale(LC_CTYPE, attempts[i]);
		if (r && wcwidth((wchar_t)0x4E00) == 2) {
			active_locale = r;
			break;
		}
	}

	printf("System wcwidth() probe\n");
	printf("======================\n");
	printf("Active locale: %s\n", active_locale);
	printf("Reference:     ridiculousfish/widecharwidth (Unicode 17.0)\n");
	printf("\n");

	if (detail) {
		fprintf(detail, "# System wcwidth() mismatch details\n");
		fprintf(detail, "# Locale: %s\n", active_locale);
		fprintf(detail, "# Format: U+XXXX  sys=N  ref=N  category\n\n");
	}

	int total_tested = 0;
	int total_matched = 0;
	int total_mismatched = 0;

	/* Scan the entire Unicode range. Skip surrogates. */
	for (uint32_t cp = 0; cp <= 0x10FFFF; cp++) {
		/* Skip surrogates */
		if (cp >= 0xD800 && cp <= 0xDFFF) continue;

		int ref = bundled_width(cp);

		/* Skip codepoints the reference considers non-printable
		 * or unassigned — not meaningful to compare. */
		if (ref < 0) continue;

		int sys = wcwidth((wchar_t)cp);

		int cat = classify(cp);
		categories[cat].tested++;
		total_tested++;

		/* Normalize: system -1 for "unknown" */
		if (sys < 0) {
			categories[cat].sys_missing++;
			categories[cat].mismatched++;
			total_mismatched++;
			if (detail) {
				fprintf(detail, "U+%04X  sys=%d  ref=%d  %s\n",
					cp, sys, ref, categories[cat].name);
			}
			continue;
		}

		if (sys == ref) {
			categories[cat].matched++;
			total_matched++;
		} else {
			categories[cat].mismatched++;
			total_mismatched++;
			if (detail) {
				fprintf(detail, "U+%04X  sys=%d  ref=%d  %s\n",
					cp, sys, ref, categories[cat].name);
			}
		}
	}

	/* Print summary */
	printf("%-46s %7s %7s %7s %7s\n",
	       "Category", "Tested", "Match", "Differ", "Sys-1");
	printf("%-46s %7s %7s %7s %7s\n",
	       "----------------------------------------------",
	       "-------", "-------", "-------", "-------");

	for (int i = 0; i < CAT_COUNT; i++) {
		if (categories[i].tested == 0) continue;
		const char *status;
		if (categories[i].mismatched == 0)
			status = "  OK";
		else if (categories[i].mismatched == categories[i].sys_missing)
			status = "  WARN (sys returns -1)";
		else
			status = "  DIFFER";

		printf("%-46s %7d %7d %7d %7d%s\n",
		       categories[i].name,
		       categories[i].tested,
		       categories[i].matched,
		       categories[i].mismatched,
		       categories[i].sys_missing,
		       status);
	}

	printf("%-46s %7s %7s %7s\n",
	       "----------------------------------------------",
	       "-------", "-------", "-------");
	printf("%-46s %7d %7d %7d\n",
	       "TOTAL", total_tested, total_matched, total_mismatched);
	printf("\n");

	if (total_mismatched == 0) {
		printf("Result: PASS — system wcwidth matches reference for all %d printable codepoints\n",
		       total_tested);
	} else {
		printf("Result: %d of %d codepoints differ (%.1f%%)\n",
		       total_mismatched, total_tested,
		       100.0 * total_mismatched / total_tested);
		if (detail) {
			printf("Details written to: %s\n", argv[1]);
		} else {
			printf("Run with a filename argument to capture mismatch details.\n");
		}
	}

	if (detail) fclose(detail);
	return total_mismatched > 0 ? 1 : 0;
}
