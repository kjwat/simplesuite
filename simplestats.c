#include <ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/statvfs.h>
#include <dirent.h>
#include <time.h>
#include "simpleui.h"

static double ram_percent(void) {
    FILE *f = fopen("/proc/meminfo", "r");
    char key[64];
    long val;
    char unit[32];
    long total = 0, avail = 0;

    while (f && fscanf(f, "%63s %ld %31s", key, &val, unit) == 3) {
        if (!strcmp(key, "MemTotal:"))
            total = val;
        if (!strcmp(key, "MemAvailable:"))
            avail = val;
    }

    if (f)
        fclose(f);

    if (!total)
        return 0;

    return 100.0 * (total - avail) / total;
}

static double disk_percent(const char *path) {
    struct statvfs s;

    if (statvfs(path, &s) != 0)
        return 0;

    unsigned long total = s.f_blocks;
    unsigned long freeb = s.f_bavail;

    if (!total)
        return 0;

    return 100.0 * (total - freeb) / total;
}

static double avg_cpu_mhz(void) {
    FILE *f = fopen("/proc/cpuinfo", "r");
    char line[256];
    double sum = 0;
    int count = 0;

    while (f && fgets(line, sizeof(line), f)) {
        if (strstr(line, "cpu MHz")) {
            double mhz;

            if (sscanf(line, "cpu MHz\t: %lf", &mhz) == 1) {
                sum += mhz;
                count++;
            }
        }
    }

    if (f)
        fclose(f);

    return count ? sum / count : 0;
}

static double cpu_temp(void) {
    FILE *f;
    char path[256];

    for (int i = 0; i < 32; i++) {
        snprintf(path,
                 sizeof(path),
                 "/sys/class/thermal/thermal_zone%d/temp",
                 i);

        f = fopen(path, "r");

        if (f) {
            long temp;

            if (fscanf(f, "%ld", &temp) == 1) {
                fclose(f);

                if (temp > 1000)
                    return temp / 1000.0;

                return temp;
            }

            fclose(f);
        }
    }

    return -1;
}


static void fan_status(char *buf, size_t size) {
    FILE *f;
    char path[512];
    char line[256];

    f = fopen("/proc/acpi/ibm/fan", "r");
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            int level;
            if (sscanf(line, "level:\t%d", &level) == 1 ||
                sscanf(line, "level: %d", &level) == 1) {
                snprintf(buf, size, "Level %d", level);
                fclose(f);
                return;
            }
        }
        fclose(f);
    }

    DIR *d = opendir("/sys/class/hwmon");
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.')
                continue;

            snprintf(path, sizeof(path),
                     "/sys/class/hwmon/%s/fan1_input",
                     e->d_name);

            f = fopen(path, "r");
            if (f) {
                int rpm;
                if (fscanf(f, "%d", &rpm) == 1 && rpm > 0) {
                    snprintf(buf, size, "%d RPM", rpm);
                    fclose(f);
                    closedir(d);
                    return;
                }
                fclose(f);
            }
        }
        closedir(d);
    }

    snprintf(buf, size, "n/a");
}

static int battery_percent(void) {
    DIR *d = opendir("/sys/class/power_supply");
    struct dirent *e;
    char path[512];
    int cap = -1;

    while (d && (e = readdir(d))) {
        if (strncmp(e->d_name, "BAT", 3) == 0) {
            snprintf(path,
                     sizeof(path),
                     "/sys/class/power_supply/%s/capacity",
                     e->d_name);

            FILE *f = fopen(path, "r");

            if (f) {
                if (fscanf(f, "%d", &cap) != 1)
                    cap = -1;
                fclose(f);
            }

            break;
        }
    }

    if (d)
        closedir(d);

    return cap;
}

static int wifi_strength(void) {
    FILE *f = fopen("/proc/net/wireless", "r");
    char line[256];
    int n = 0;

    while (f && fgets(line, sizeof(line), f)) {
        n++;

        if (n > 2) {
            char iface[32];
            double status, link;

            if (sscanf(line,
                       " %31[^:]: %lf %lf",
                       iface,
                       &status,
                       &link) >= 3) {
                fclose(f);
                return (int)((link / 70.0) * 100.0);
            }
        }
    }

    if (f)
        fclose(f);

    return -1;
}

static void uptime_string(char *buf, size_t size) {
    FILE *f = fopen("/proc/uptime", "r");
    double up = 0;

    if (f) {
        if (fscanf(f, "%lf", &up) != 1)
            up = 0;
        fclose(f);
    }

    int total = (int)up;
    int days  = total / 86400;
    int hours = (total % 86400) / 3600;
    int mins  = (total % 3600) / 60;

    snprintf(buf,
             size,
             "%dd %02dh %02dm",
             days,
             hours,
             mins);
}

int main(void) {
    double ram  = 0;
    double disk = 0;
    double mhz  = 0;
    double temp = 0;

    int bat  = -1;
    int wifi = -1;

    char up[64] = "";
    char fan[64] = "";

    SuiLoop sample_loop;

    initscr();
    cbreak();
    noecho();
    curs_set(0);
    sui_loop_init(&sample_loop, 1000);

    while (1) {
        if (sui_loop_tick_due(&sample_loop)) {
            ram  = ram_percent();
            disk = disk_percent("/");
            mhz  = avg_cpu_mhz();
            temp = cpu_temp();
            bat  = battery_percent();
            wifi = wifi_strength();
            uptime_string(up, sizeof(up));
            fan_status(fan, sizeof(fan));
            sui_loop_mark_dirty(&sample_loop);
        }

        timeout(sui_loop_timeout(&sample_loop, 1000));
        int ch = getch();

        if (ch == 'q' || ch == 'Q')
            break;

        if (ch == KEY_RESIZE) sui_loop_mark_dirty(&sample_loop);
        if (!sui_loop_take_dirty(&sample_loop)) continue;

        erase();

        mvprintw(1, 2, "simplestats");
        mvprintw(2, 2, "-----------");

        mvprintw(4, 2, "RAM used:       %5.1f%%", ram);
        mvprintw(5, 2, "Disk used /:    %5.1f%%", disk);
        mvprintw(6, 2, "CPU avg speed:  %5.0f MHz", mhz);

        if (temp >= 0)
            mvprintw(7, 2, "CPU temp:       %5.1f C", temp);
        else
            mvprintw(7, 2, "CPU temp:       n/a");

        mvprintw(8, 2, "Fan:            %s", fan);

        if (bat >= 0)
            mvprintw(9, 2, "Battery:        %5d%%", bat);
        else
            mvprintw(9, 2, "Battery:        n/a");

        if (wifi >= 0)
            mvprintw(10, 2, "WiFi strength:  %5d%%", wifi);
        else
            mvprintw(10, 2, "WiFi strength:  n/a");

        mvprintw(11, 2, "Uptime:         %s", up);

        mvprintw(14, 2, "q = quit");

        refresh();
    }

    endwin();
    return 0;
}
