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

/* Implementation notes: see check.h's top comment for what this is
 * (an independent reimplementation of a small subset of the Check
 * unit-testing framework's API - https://libcheck.github.io/check/ -
 * not a copy of its source) and why it exists (no external test-
 * library dependency, easy to vendor into other projects). */

#include "check.h"

#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

struct test_entry_
{
    TFun fn;
    int start;
    int end; /* exclusive */
};

struct TCase
{
    char *name;
    struct test_entry_ *tests;
    int n_tests, cap_tests;
};

struct Suite
{
    char *name;
    TCase **tcases;
    int n_tcases, cap_tcases;
};

struct SRunner
{
    Suite **suites;
    int n_suites, cap_suites;
    int n_run, n_failed;
};

/* State for the test currently running in this process (see the
 * "one process per test" comment in srunner_run_all below - there is
 * never more than one test active at a time in a given process, fork
 * or no fork, so plain globals are enough). */
static const char *cur_suite_name_;
static const char *cur_tcase_name_;
static const char *cur_test_name_;
static jmp_buf test_env_;
static int test_failed_;

static void *grow_(void *arr, int *cap, int n, size_t elem_size)
{
    if (n >= *cap)
    {
        *cap = *cap ? *cap * 2 : 8;
        arr = realloc(arr, (size_t) *cap * elem_size);
    }
    return arr;
}

Suite *suite_create(const char *name)
{
    Suite *s = calloc(1, sizeof(*s));
    s->name = strdup(name);
    return s;
}

TCase *tcase_create(const char *name)
{
    TCase *tc = calloc(1, sizeof(*tc));
    tc->name = strdup(name);
    return tc;
}

void suite_add_tcase(Suite *s, TCase *tc)
{
    s->tcases = grow_(s->tcases, &s->cap_tcases, s->n_tcases, sizeof(s->tcases[0]));
    s->tcases[s->n_tcases++] = tc;
}

void tcase_add_test_(TCase *tc, TFun fn, int start, int end)
{
    tc->tests = grow_(tc->tests, &tc->cap_tests, tc->n_tests, sizeof(tc->tests[0]));
    tc->tests[tc->n_tests].fn = fn;
    tc->tests[tc->n_tests].start = start;
    tc->tests[tc->n_tests].end = end;
    tc->n_tests++;
}

SRunner *srunner_create(Suite *s)
{
    SRunner *sr = calloc(1, sizeof(*sr));
    if (s)
        srunner_add_suite(sr, s);
    return sr;
}

void srunner_add_suite(SRunner *sr, Suite *s)
{
    if (!s)
        return;
    sr->suites = grow_(sr->suites, &sr->cap_suites, sr->n_suites, sizeof(sr->suites[0]));
    sr->suites[sr->n_suites++] = s;
}

void ck_test_start_(const char *test_name)
{
    cur_test_name_ = test_name;
}

void ck_assert_failed_(const char *file, int line, const char *fmt, ...)
{
    va_list ap;

    fflush(stdout);
    fprintf(stderr, "%s:%d:F:%s:%s: ", file, line,
            cur_tcase_name_ ? cur_tcase_name_ : "?",
            cur_test_name_ ? cur_test_name_ : "?");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");

    test_failed_ = 1;
    longjmp(test_env_, 1);
}

/* Runs one instance (loop index _i) of a test in the calling process,
 * catching ck_assert()/ck_assert_msg()/ck_abort_msg() failures via the
 * setjmp/longjmp pair above instead of unwinding the whole test
 * binary. Returns non-zero if the test failed. */
static int run_test_body_(TFun fn, int i)
{
    test_failed_ = 0;
    if (setjmp(test_env_) == 0)
        fn(i);
    return test_failed_;
}

/* Runs one test instance, optionally isolated in a forked child (like
 * real Check's default CK_FORK=yes) so that a crash (segfault, abort,
 * ...) fails just that one test instead of the whole binary. Returns
 * non-zero if the test failed. */
static int run_test_instance_(Suite *s, TCase *tc, TFun fn, int i, int do_fork)
{
    if (!do_fork)
        return run_test_body_(fn, i);

    /* Flush any output buffered by the parent (e.g. the "Running
     * suite: ..." progress line) before forking, otherwise the child
     * inherits a copy of that unflushed buffer and can end up
     * printing it a second time - e.g. via the fflush(stdout) in
     * ck_assert_failed_ below, or if the child's own test code exits
     * through a path that flushes stdio. */
    fflush(stdout);
    fflush(stderr);

    pid_t pid = fork();
    if (pid < 0)
    {
        perror("check: fork");
        exit(EXIT_FAILURE);
    }
    if (pid == 0)
        _exit(run_test_body_(fn, i) ? EXIT_FAILURE : EXIT_SUCCESS);

    int status;
    if (waitpid(pid, &status, 0) < 0)
    {
        perror("check: waitpid");
        exit(EXIT_FAILURE);
    }
    if (WIFEXITED(status))
        return WEXITSTATUS(status) != 0;
    if (WIFSIGNALED(status))
    {
        int sig = WTERMSIG(status);
        fprintf(stderr, "%s:%s: test (loop index %d) terminated by signal %d (%s)\n",
                s->name, tc->name, i, sig, strsignal(sig));
        return 1;
    }
    return 1;
}

void srunner_run_all(SRunner *sr, PrintOutput mode)
{
    (void) mode;

    const char *want_suite = getenv("CK_RUN_SUITE");
    const char *want_case = getenv("CK_RUN_CASE");
    const char *fork_env = getenv("CK_FORK");
    int do_fork = !(fork_env && strcmp(fork_env, "no") == 0);

    sr->n_run = 0;
    sr->n_failed = 0;

    for (int si = 0; si < sr->n_suites; si++)
    {
        Suite *s = sr->suites[si];
        if (want_suite && strcmp(want_suite, s->name) != 0)
            continue;
        cur_suite_name_ = s->name;

        printf("Running suite: %s\n", s->name);

        for (int ti = 0; ti < s->n_tcases; ti++)
        {
            TCase *tc = s->tcases[ti];
            if (want_case && strcmp(want_case, tc->name) != 0)
                continue;
            cur_tcase_name_ = tc->name;

            for (int e = 0; e < tc->n_tests; e++)
            {
                struct test_entry_ *t = &tc->tests[e];
                for (int i = t->start; i < t->end; i++)
                {
                    sr->n_run++;
                    if (run_test_instance_(s, tc, t->fn, i, do_fork))
                        sr->n_failed++;
                }
            }
        }
    }

    int passed = sr->n_run - sr->n_failed;
    int pct = sr->n_run ? (100 * passed / sr->n_run) : 100;
    printf("%d%%: Checks: %d, Failures: %d, Errors: 0\n", pct, sr->n_run, sr->n_failed);
}

int srunner_ntests_failed(SRunner *sr)
{
    return sr->n_failed;
}

void srunner_free(SRunner *sr)
{
    if (!sr)
        return;

    for (int si = 0; si < sr->n_suites; si++)
    {
        Suite *s = sr->suites[si];
        for (int ti = 0; ti < s->n_tcases; ti++)
        {
            TCase *tc = s->tcases[ti];
            free(tc->tests);
            free(tc->name);
            free(tc);
        }
        free(s->tcases);
        free(s->name);
        free(s);
    }
    free(sr->suites);
    free(sr);
}
