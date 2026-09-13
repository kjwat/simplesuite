#define _POSIX_C_SOURCE 200809L
#include "../simplepaths.h"

int main(void) {
    char path[4096];

    if (!ss_asset_path(path, sizeof(path), "simplecal-alarm.mp3"))
        return 1;
    puts(path);
    return 0;
}
