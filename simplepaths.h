#ifndef SIMPLEPATHS_H
#define SIMPLEPATHS_H

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
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

static inline int ss_asset_path(char *path, size_t size, const char *name)
{
    char executable[4096];
    const char *override = getenv("SIMPLESUITE_DATADIR");
    const char *system_dirs[] = {"/usr/local/share/simplesuite",
                                 "/usr/share/simplesuite"};
    int length;

    if (!path || !size || !name || !*name || strchr(name, '/')) return 0;
    if (override && *override) {
        length = snprintf(path, size, "%s/%s", override, name);
        return length >= 0 && (size_t)length < size;
    }
    /* PREFIX overrides and staged installations keep assets beside bin/. */
    if (ssr_executable_path(executable, sizeof(executable))) {
        char *slash = strrchr(executable, '/');
        if (slash) {
            *slash = '\0';
            slash = strrchr(executable, '/');
            if (slash && (!strcmp(slash + 1, "bin") || !strcmp(slash + 1, "sbin"))) {
                *slash = '\0';
                length = snprintf(path, size, "%s/share/simplesuite/%s", executable, name);
                if (length >= 0 && (size_t)length < size && access(path, R_OK) == 0)
                    return 1;
            }
        }
    }
    for (size_t i = 0; i < sizeof(system_dirs) / sizeof(system_dirs[0]); i++) {
        length = snprintf(path, size, "%s/%s", system_dirs[i], name);
        if (length >= 0 && (size_t)length < size && access(path, R_OK) == 0)
            return 1;
    }
    length = snprintf(path, size, "%s/%s", system_dirs[0], name);
    return length >= 0 && (size_t)length < size;
}

#endif
