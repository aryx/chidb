/*
 *  A small level-based logging library, extracted from chidb (where it
 *  was include/chidb/log.h's chilog()/chilog_hex(), CHILOG_H_-guarded
 *  and CRITICAL..TRACE-leveled) into a project-independent form: no
 *  chidb dependency, and an API renamed to the naming convention most
 *  small C logging libraries use (log_set_level(), log_trace() ..
 *  log_fatal(), ascending LOG_TRACE..LOG_FATAL severity - see
 *  https://github.com/rxi/log.c for the library this shape is modeled
 *  on) instead of the original chilog()-with-a-chi_-prefix one.
 *
 */

/*
 *  Copyright (c) 2009-2015, The University of Chicago
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or withsend
 *  modification, are permitted provided that the following conditions are met:
 *
 *  - Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 *  - Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 *  - Neither the name of The University of Chicago nor the names of its
 *    contributors may be used to endorse or promote products derived from this
 *    software withsend specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY send OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *
 */

#ifndef LOG_H
#define LOG_H

/* Log levels, in ascending severity (opposite of the old
 * CRITICAL=10..TRACE=60 chidb scheme) - the standard order used by
 * syslog(3) and most small C logging libraries. */
typedef enum
{
    LOG_TRACE,
    LOG_DEBUG,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR,
    LOG_FATAL
} log_level_t;

/*
 * log_set_level - Sets the logging threshold.
 *
 * Messages at this level or more severe are printed; e.g. after
 * log_set_level(LOG_WARN), LOG_WARN/LOG_ERROR/LOG_FATAL messages
 * print and LOG_TRACE/LOG_DEBUG/LOG_INFO ones don't.
 */
void log_set_level(log_level_t level);

/*
 * log_log - Prints one log message; use the log_trace()/../log_fatal()
 * macros below instead of calling this directly (they fill in
 * file/line for you).
 */
void log_log(log_level_t level, const char *file, int line, const char *fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 4, 5)))
#endif
    ;

#define log_trace(...) log_log(LOG_TRACE, __FILE__, __LINE__, __VA_ARGS__)
#define log_debug(...) log_log(LOG_DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#define log_info(...)  log_log(LOG_INFO,  __FILE__, __LINE__, __VA_ARGS__)
#define log_warn(...)  log_log(LOG_WARN,  __FILE__, __LINE__, __VA_ARGS__)
#define log_error(...) log_log(LOG_ERROR, __FILE__, __LINE__, __VA_ARGS__)
#define log_fatal(...) log_log(LOG_FATAL, __FILE__, __LINE__, __VA_ARGS__)

/*
 * log_hexdump - Logs `len` bytes at `data` in hexdump style, at the
 * given level (one log_log() call per 16-byte line).
 */
#define log_hexdump(level, data, len) log_hexdump_(level, __FILE__, __LINE__, data, len)
void log_hexdump_(log_level_t level, const char *file, int line, const void *data, int len);

#endif /* LOG_H */
