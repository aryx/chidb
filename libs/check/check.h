/* Claude Code
 *
 * Copyright (C) 2026 Yoann Padioleau
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * THIS IS NOT THE CHECK LIBRARY. This is an independent, from-scratch
 * reimplementation - by Claude Code, at Yoann Padioleau's request -
 * of a small subset of the public API of Check, a C unit-testing
 * framework:
 *
 *     Check - a unit test framework for C
 *     https://libcheck.github.io/check/
 *     https://github.com/libcheck/check
 *
 * All credit for the original design (the Suite/TCase/SRunner model,
 * the START_TEST/END_TEST and ck_assert* macros, the per-test-fork
 * isolation, the CK_RUN_SUITE/CK_RUN_CASE/CK_FORK env vars) goes to
 * the Check authors and contributors; none of their source code was
 * copied here. That Suite/TCase/(S)Runner shape, in turn, traces back
 * to the xUnit lineage Kent Beck started with SUnit (Smalltalk) and
 * later JUnit (with Erich Gamma) - the small, elegant "run a tree of
 * test cases, collect the failures" idea Check itself is an imitation
 * of, for C. This file only covers what this project's test suites
 * (../../tests/) actually call: START_TEST/END_TEST, the ck_assert*
 * family, and the Suite/TCase/SRunner registration API. Test code
 * written against real Check needs no changes to build against this
 * header instead.
 *
 * Like real Check's default CK_FORK=yes mode, each test runs in its
 * own forked child process, so a crashing test is reported as a
 * failure instead of taking down the rest of the suite. Set CK_FORK=no
 * in the environment to run every test in-process instead (e.g. to
 * step through one under gdb). CK_RUN_SUITE / CK_RUN_CASE restrict the
 * run to a single suite/tcase by (exact) name, same as real Check.
 *
 * Why reimplement instead of just linking against libcheck: this
 * removes the external `check` package (and pkg-config/pthread/rt
 * detection for it) as a build dependency, so `make check` always
 * works with nothing but a C toolchain, and this same file can be
 * vendored as-is into other of Yoann Padioleau's projects (principia-
 * softwarica, goken - see ../../CLAUDE.md) that want the same
 * Check-flavored test code without pulling in the real library. No
 * dependency on anything else in this tree.
 */

#ifndef CHECK_H
#define CHECK_H

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Suite Suite;
typedef struct TCase TCase;
typedef struct SRunner SRunner;

typedef void (*TFun)(int _i);

typedef enum
{
    CK_SILENT,
    CK_MINIMAL,
    CK_NORMAL,
    CK_VERBOSE
} PrintOutput;

Suite *suite_create(const char *name);
TCase *tcase_create(const char *name);
void suite_add_tcase(Suite *s, TCase *tc);
void tcase_add_test_(TCase *tc, TFun fn, int start, int end);
#define tcase_add_test(tc, fn) tcase_add_test_((tc), (fn), 0, 1)
#define tcase_add_loop_test(tc, fn, start, end) tcase_add_test_((tc), (fn), (start), (end))

SRunner *srunner_create(Suite *s);
void srunner_add_suite(SRunner *sr, Suite *s);
void srunner_run_all(SRunner *sr, PrintOutput mode);
int srunner_ntests_failed(SRunner *sr);
void srunner_free(SRunner *sr);

/* Internal, used by the macros below - not meant to be called directly. */
void ck_test_start_(const char *test_name);
void ck_assert_failed_(const char *file, int line, const char *fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 3, 4)))
#endif
    ;

#define START_TEST(__testname) \
    static void __testname (int _i) \
    { \
        ck_test_start_(#__testname); \
        (void) _i;

#define END_TEST }

#define ck_assert_msg(expr, ...) \
    do { \
        if (!(expr)) \
            ck_assert_failed_(__FILE__, __LINE__, __VA_ARGS__); \
    } while (0)

#define ck_assert(expr) \
    ck_assert_msg((expr), "Assertion '%s' failed", #expr)

#define ck_abort_msg(...) \
    ck_assert_failed_(__FILE__, __LINE__, __VA_ARGS__)

#define ck_assert_int_eq(X, Y) \
    do { \
        intmax_t _ck_x = (X); \
        intmax_t _ck_y = (Y); \
        if (_ck_x != _ck_y) \
            ck_assert_failed_(__FILE__, __LINE__, \
                "Assertion '%s == %s' failed: %s == %jd, %s == %jd", \
                #X, #Y, #X, _ck_x, #Y, _ck_y); \
    } while (0)

#define ck_assert_str_eq(X, Y) \
    do { \
        const char *_ck_x = (X); \
        const char *_ck_y = (Y); \
        if (strcmp(_ck_x, _ck_y) != 0) \
            ck_assert_failed_(__FILE__, __LINE__, \
                "Assertion '%s == %s' failed: %s == \"%s\", %s == \"%s\"", \
                #X, #Y, #X, _ck_x, #Y, _ck_y); \
    } while (0)

#ifdef __cplusplus
}
#endif

#endif /* CHECK_H */
