#include <ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/statvfs.h>
#include <dirent.h>
#include <time.h>
#if defined(__FreeBSD__) || defined(__APPLE__)
#include <sys/param.h>
#include <sys/sysctl.h>
#endif
#ifdef __APPLE__
#include <mach/mach.h>
#include "simplestats-macos.h"
#endif
#include "simpleui.h"

#ifdef __FreeBSD__
static int sysctl_int(const char *name, int *value) {
    size_t len = sizeof(*value);

    return sysctlbyname(name, value, &len, NULL, 0) == 0;
}

static double sysctl_temperature_c(const char *name) {
    int temperature = 0;

    if (!sysctl_int(name, &temperature) || temperature <= 0)
        return -1;

    return temperature / 10.0 - 273.15;
}

static int percent_from_rssi(double rssi) {
    double percent;

    if (rssi < 0)
        percent = ((rssi + 90.0) / 60.0) * 100.0;
    else
        percent = (rssi / 40.0) * 100.0;

    if (percent < 0)
        return 0;
    if (percent > 100)
        return 100;

    return (int)(percent + 0.5);
}

static int simple_iface_name(const char *name) {
    if (strncmp(name, "wlan", 4) != 0)
        return 0;

    for (const char *p = name; *p; p++) {
        if ((*p >= 'a' && *p <= 'z') ||
            (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9') ||
            *p == '_')
            continue;
        return 0;
    }

    return 1;
}
#endif

static double ram_percent(void) {
#ifdef __FreeBSD__
    unsigned long long total = 0, free_pages = 0;
    unsigned int page_size = 0;
    size_t total_len = sizeof(total);
    size_t free_len = sizeof(free_pages);
    size_t page_len = sizeof(page_size);

    if (sysctlbyname("hw.physmem", &total, &total_len, NULL, 0) != 0 ||
        sysctlbyname("vm.stats.vm.v_free_count", &free_pages, &free_len,
                     NULL, 0) != 0 ||
        sysctlbyname("vm.stats.vm.v_page_size", &page_size, &page_len,
                     NULL, 0) != 0 || total == 0)
        return 0;
    return 100.0 * (double)(total - free_pages * page_size) / (double)total;
#elif defined(__APPLE__)
    uint64_t total = 0;
    size_t total_len = sizeof(total);
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    vm_statistics64_data_t vm;
    vm_size_t page_size = 0;
    host_t host = mach_host_self();
    kern_return_t page_result;
    kern_return_t stats_result;
    uint64_t free_bytes;

    if (host == MACH_PORT_NULL)
        return 0;
    page_result = host_page_size(host, &page_size);
    stats_result = host_statistics64(host, HOST_VM_INFO64,
                                     (host_info64_t)&vm, &count);
    mach_port_deallocate(mach_task_self(), host);
    if (sysctlbyname("hw.memsize", &total, &total_len, NULL, 0) != 0 ||
        total == 0 || page_result != KERN_SUCCESS ||
        stats_result != KERN_SUCCESS)
        return 0;
    free_bytes = ((uint64_t)vm.free_count +
                  (uint64_t)vm.speculative_count) * (uint64_t)page_size;
    if (free_bytes > total)
        free_bytes = total;
    return 100.0 * (double)(total - free_bytes) / (double)total;
#else
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
#endif
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
#ifdef __FreeBSD__
    char name[64];
    double sum = 0;
    int count = 0;

    for (int i = 0; i < 64; i++) {
        int mhz = 0;

        snprintf(name, sizeof(name), "dev.cpu.%d.freq", i);
        if (sysctl_int(name, &mhz) && mhz > 0) {
            sum += mhz;
            count++;
        }
    }

    return count ? sum / count : 0;
#elif defined(__APPLE__)
    uint64_t hz = 0;
    size_t len = sizeof(hz);

    if (sysctlbyname("hw.cpufrequency", &hz, &len, NULL, 0) != 0 || hz == 0) {
        len = sizeof(hz);
        if (sysctlbyname("hw.cpufrequency_max", &hz, &len, NULL, 0) != 0)
            return 0;
    }
    return (double)hz / 1000000.0;
#else
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
#endif
}

static double cpu_temp(void) {
#ifdef __FreeBSD__
    double highest;
    char name[64];

    highest = sysctl_temperature_c("dev.cpu.0.temperature");
    if (highest >= 0) {
        for (int i = 1; i < 64; i++) {
            double temp;

            snprintf(name, sizeof(name), "dev.cpu.%d.temperature", i);
            temp = sysctl_temperature_c(name);
            if (temp > highest)
                highest = temp;
        }

        return highest;
    }

    for (int i = 0; i < 16; i++) {
        double temp;

        snprintf(name, sizeof(name), "hw.acpi.thermal.tz%d.temperature", i);
        temp = sysctl_temperature_c(name);
        if (temp > highest)
            highest = temp;
    }

    return highest;
#elif defined(__APPLE__)
    /*
     * macOS has no supported public API for CPU temperature. Avoid private
     * SMC keys and privileged powermetrics parsing.
     */
    return -1;
#else
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
#endif
}


static void fan_status(char *buf, size_t size) {
#ifdef __FreeBSD__
    int value = 0;
    int active = -1;
    int saw_zone = 0;
    char name[64];

    if (sysctl_int("dev.acpi_ibm.0.fan_speed", &value)) {
        if (value > 8)
            snprintf(buf, size, "%d RPM", value);
        else if (value == 0)
            snprintf(buf, size, "off");
        else
            snprintf(buf, size, "Level %d", value);
        return;
    }

    if (sysctl_int("dev.acpi_ibm.0.fan_level", &value)) {
        snprintf(buf, size, "Level %d", value);
        return;
    }

    if (sysctl_int("dev.acpi_ibm.0.fan", &value)) {
        snprintf(buf, size, "%s", value ? "auto" : "manual");
        return;
    }

    for (int i = 0; i < 16; i++) {
        int zone_active;

        snprintf(name, sizeof(name), "hw.acpi.thermal.tz%d.active", i);
        if (!sysctl_int(name, &zone_active))
            continue;

        saw_zone = 1;
        if (zone_active >= 0 &&
            (active < 0 || zone_active < active))
            active = zone_active;
    }

    if (active >= 0)
        snprintf(buf, size, "Active L%d", active);
    else if (saw_zone)
        snprintf(buf, size, "firmware");
    else
        snprintf(buf, size, "unexposed");
#elif defined(__APPLE__)
    snprintf(buf, size, "system managed");
#else
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
#endif
}

static int battery_percent(void) {
#ifdef __FreeBSD__
    int life = -1;
    size_t len = sizeof(life);
    return sysctlbyname("hw.acpi.battery.life", &life, &len, NULL, 0) == 0
               ? life : -1;
#elif defined(__APPLE__)
    return simplestats_macos_battery_percent();
#else
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
#endif
}

static int wifi_strength(void) {
#ifdef __FreeBSD__
    FILE *ifs = popen("/sbin/ifconfig -l 2>/dev/null", "r");
    char iface[64];

    if (!ifs)
        return -1;

    while (fscanf(ifs, "%63s", iface) == 1) {
        FILE *sta;
        char cmd[160];
        char line[256];

        if (!simple_iface_name(iface))
            continue;

        snprintf(cmd,
                 sizeof(cmd),
                 "/sbin/ifconfig %s list sta 2>/dev/null",
                 iface);
        sta = popen(cmd, "r");
        if (!sta)
            continue;

        while (fgets(line, sizeof(line), sta)) {
            char addr[32];
            char rate[32];
            int aid, chan;
            double rssi;

            if (sscanf(line,
                       " %31s %d %d %31s %lf",
                       addr,
                       &aid,
                       &chan,
                       rate,
                       &rssi) == 5) {
                pclose(sta);
                pclose(ifs);
                return percent_from_rssi(rssi);
            }
        }

        pclose(sta);
    }

    pclose(ifs);
    return -1;
#elif defined(__APPLE__)
    return simplestats_macos_wifi_strength();
#else
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
#endif
}

static void uptime_string(char *buf, size_t size) {
#if defined(__FreeBSD__) || defined(__APPLE__)
    struct timeval boot;
    size_t len = sizeof(boot);
    time_t now = time(NULL);
    double up = 0;

    if (sysctlbyname("kern.boottime", &boot, &len, NULL, 0) == 0 &&
        now > boot.tv_sec)
        up = difftime(now, boot.tv_sec);
#else
    FILE *f = fopen("/proc/uptime", "r");
    double up = 0;

    if (f) {
        if (fscanf(f, "%lf", &up) != 1)
            up = 0;
        fclose(f);
    }
#endif

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
#ifdef __APPLE__
        if (mhz > 0)
            mvprintw(6, 2, "CPU speed:      %5.0f MHz", mhz);
        else
            mvprintw(6, 2, "CPU speed:      n/a (system managed)");
#else
        mvprintw(6, 2, "CPU avg speed:  %5.0f MHz", mhz);
#endif

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
