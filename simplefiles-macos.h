#ifndef SIMPLEFILES_MACOS_H
#define SIMPLEFILES_MACOS_H

#include <limits.h>

#ifndef NAME_MAX
#define NAME_MAX 255
#endif

typedef struct {
    char id[PATH_MAX];
    char name[NAME_MAX + 1];
    char device[PATH_MAX];
    char uuid[128];
    char mount_path[PATH_MAX];
    int mounted;
    int can_mount;
    int removable;
} SimpleFilesMacVolume;

int simplefiles_macos_list_volumes(SimpleFilesMacVolume *volumes, int maximum);

#endif
