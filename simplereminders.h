#ifndef SIMPLEREMINDERS_H
#define SIMPLEREMINDERS_H

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#elif defined(__FreeBSD__)
#include <sys/sysctl.h>
#endif

/* Resolve the running image, independent of PATH and shortcut symlinks. */
static inline int ssr_executable_path(char *path, size_t size)
{
    if (!path || size < 2) return 0;
#ifdef __APPLE__
    if (size > UINT32_MAX) return 0;
    uint32_t length = (uint32_t)size;
    if (_NSGetExecutablePath(path, &length) != 0) return 0;
#elif defined(__FreeBSD__)
    size_t length = size;
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, (int)getpid() };
    if (sysctl(mib, 4, path, &length, NULL, 0) != 0 ||
        length == 0 || length > size) return 0;
#else
    ssize_t length = readlink("/proc/self/exe", path, size - 1);
    if (length <= 0 || (size_t)length >= size - 1) return 0;
    path[length] = '\0';
#endif
    path[size - 1] = '\0';
    return path[0] == '/' && access(path, X_OK) == 0;
}

/* systemd expands % specifiers and $ variables; cron processes % before the
 * shell sees its double-quoted command. Keep both formats literal. */
static inline int ssr_quote_executable(const char *path, char *output,
                                       size_t size, int for_cron)
{
    size_t used = 0;
    if (!path || path[0] != '/' || size < 3) return 0;
    output[used++] = '"';
    for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
        if (*p < 32 || *p == 127 || used + 3 >= size) return 0;
        if (*p == '\\' || *p == '"' ||
            (for_cron && (*p == '$' || *p == '`' || *p == '%')))
            output[used++] = '\\';
        else if (!for_cron && (*p == '%' || *p == '$'))
            output[used++] = (char)*p;
        output[used++] = (char)*p;
    }
    output[used++] = '"';
    output[used] = '\0';
    return 1;
}

static inline int ssr_current_command(char *output, size_t size,
                                      const char *argument, int for_cron)
{
    char executable[4096];
    char quoted[8192];
    int written;
    if (!ssr_executable_path(executable, sizeof executable) ||
        !ssr_quote_executable(executable, quoted, sizeof quoted, for_cron))
        return 0;
    /* systemd forbids quotes/backslashes in the executable token even after
     * unquoting. Pass the path as an argument to a fixed shell that immediately
     * execs it. It is never interpolated into shell code (including paths with
     * '=', which /usr/bin/env would treat as environment assignments). */
    written = snprintf(output, size, "%s%s %s",
                       for_cron ? "" : "/bin/sh -c 'exec \"$$@\"' -- ",
                       quoted, argument);
    return written > 0 && (size_t)written < size;
}

static inline int ssr_is_cron_reminder(const char *line, const char *program)
{
    char legacy[128], quoted[128];
    snprintf(legacy, sizeof legacy, "%s --check-reminders", program);
    snprintf(quoted, sizeof quoted, "%s\" --check-reminders", program);
    return strstr(line, legacy) != NULL || strstr(line, quoted) != NULL;
}

#endif
