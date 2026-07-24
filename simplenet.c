#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <ncurses.h>
#include <ctype.h>
#include <errno.h>
#include <locale.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "simpleui.h"

#define MAX_APS 256
#define MAX_TEXT 4096
#define MAX_CMD 8192

typedef struct {
    int active;
    char ssid[128];
    char bssid[32];
    int channel;
    int frequency;
    int signal;
    char security[96];
    double gateway_ms;
    double internet_ms;
    double download_mbps;
    double packet_loss;
    int tested;
} AccessPoint;

typedef enum {
    VIEW_NETWORKS,
    VIEW_DETAILS,
    VIEW_CARE,
    VIEW_HELP
} View;

typedef enum {
    BACKEND_NONE,
    BACKEND_NETWORKMANAGER,
    BACKEND_IWD,
    BACKEND_WPA_SUPPLICANT
} Backend;

typedef struct {
    const char *driver_prefix;
    const char *module;
    const char *options;
    const char *title;
    const char *description;
} AdapterRemedy;

static const AdapterRemedy remedies[] = {
    {
        "rtw89_", "rtw89_pci",
        "disable_aspm_l1=y disable_aspm_l1ss=y disable_clkreq=y",
        "Realtek rtw89 PCIe stability",
        "Disable ASPM L1/L1SS and CLKREQ to prevent PCIe link drops."
    },
    {
        "rtw88_", "rtw88_pci", "disable_aspm=y",
        "Realtek rtw88 PCIe stability",
        "Disable PCIe ASPM when link power transitions cause disconnects."
    },
    {
        "mt7921e", "mt7921e", "disable_aspm=y",
        "MediaTek MT7921 PCIe stability",
        "Disable PCIe ASPM when adapter wakeups or reassociation are unreliable."
    },
    {
        "mt7925e", "mt7925e", "disable_aspm=y",
        "MediaTek MT7925 PCIe stability",
        "Disable PCIe ASPM when adapter wakeups or reassociation are unreliable."
    },
    {
        "iwlwifi", "iwlmvm", "power_scheme=1",
        "Intel Wi-Fi power stability",
        "Keep iwlmvm in its active power scheme when firmware drops the link."
    }
};

static AccessPoint aps[MAX_APS];
static int ap_count;
static int selected;
static int top;
static View view = VIEW_NETWORKS;
static char wifi_device[64];
static char connection_uuid[128];
static char gateway[128];
static char adapter[256];
static char driver[128];
static char message[MAX_TEXT] = "Ready.";
static int message_error;
static Backend backend;
static void draw(void);
static void copy_text(char *dest, size_t size, const char *source);
static int configured_bssid(char *bssid, size_t size);
static int pin_bssid(const char *bssid);
static int restore_bssid(const char *bssid);

static const char *backend_name(void)
{
    switch (backend) {
        case BACKEND_NETWORKMANAGER: return "NetworkManager";
        case BACKEND_IWD: return "iwd";
        case BACKEND_WPA_SUPPLICANT: return "wpa_supplicant";
        default: return "no manager";
    }
}

static void set_message(int error, const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    vsnprintf(message, sizeof(message), format, ap);
    va_end(ap);
    message_error = error;
}

static void trim(char *s)
{
    char *start = s;
    size_t len;
    while (*start && isspace((unsigned char)*start)) start++;
    if (start != s) memmove(s, start, strlen(start) + 1);
    len = strlen(s);
    while (len && isspace((unsigned char)s[len - 1])) s[--len] = '\0';
}

static void copy_text(char *dest, size_t size, const char *source)
{
    size_t length;
    if (!size) return;
    length = source ? strlen(source) : 0;
    if (length >= size) length = size - 1;
    if (length) memcpy(dest, source, length);
    dest[length] = '\0';
}

static void shell_quote(const char *source, char *dest, size_t size)
{
    size_t j = 0;
    if (!size) return;
    if (j + 1 < size) dest[j++] = '\'';
    for (size_t i = 0; source && source[i] && j + 5 < size; i++) {
        if (source[i] == '\'') {
            memcpy(dest + j, "'\\''", 4);
            j += 4;
        } else {
            dest[j++] = source[i];
        }
    }
    if (j + 1 < size) dest[j++] = '\'';
    dest[j] = '\0';
}

static int command_output(const char *command, char *output, size_t size)
{
    FILE *pipe;
    char discard[1024];
    size_t used = 0;
    int status;

    if (!size) return 0;
    output[0] = '\0';
    pipe = popen(command, "r");
    if (!pipe) return 0;
    while (used + 1 < size) {
        size_t got = fread(output + used, 1, size - used - 1, pipe);
        used += got;
        if (!got) break;
    }
    while (fread(discard, 1, sizeof(discard), pipe) > 0) {}
    output[used] = '\0';
    status = pclose(pipe);
    return status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static int command_exists(const char *name)
{
    const char *path = getenv("PATH");
    char copy[4096];
    char candidate[4096];
    char *save = NULL;
    char *part;
    if (!path || strlen(path) >= sizeof(copy)) return 0;
    snprintf(copy, sizeof(copy), "%s", path);
    for (part = strtok_r(copy, ":", &save); part; part = strtok_r(NULL, ":", &save)) {
        snprintf(candidate, sizeof(candidate), "%s/%s", part, name);
        if (access(candidate, X_OK) == 0) return 1;
    }
    return 0;
}

/* Split nmcli terse output while decoding its backslash escapes. */
static int split_nmcli(char *line, char **fields, int maximum)
{
    int count = 0;
    char *read = line;
    char *write = line;
    if (maximum < 1) return 0;
    fields[count++] = write;
    while (*read) {
        if (*read == '\\' && read[1]) {
            read++;
            *write++ = *read++;
        } else if (*read == ':' && count < maximum) {
            *write++ = '\0';
            read++;
            fields[count++] = write;
        } else {
            *write++ = *read++;
        }
    }
    *write = '\0';
    return count;
}

static void read_first_line(const char *command, char *dest, size_t size)
{
    char text[MAX_TEXT];
    if (!command_output(command, text, sizeof(text))) {
        dest[0] = '\0';
        return;
    }
    text[strcspn(text, "\r\n")] = '\0';
    trim(text);
    copy_text(dest, size, text);
}

static void discover_iw_device(void)
{
    if (wifi_device[0] || !command_exists("iw")) return;
    read_first_line("iw dev 2>/dev/null | awk '$1==\"Interface\" {print $2; exit}'",
                    wifi_device, sizeof(wifi_device));
}

static void detect_backend(void)
{
    char command[MAX_CMD];
    char output[MAX_TEXT];
    char quoted[256];

    backend = BACKEND_NONE;
    wifi_device[0] = '\0';
    if (command_exists("nmcli")) {
        read_first_line(
            "nmcli -t -f DEVICE,TYPE,STATE device status 2>/dev/null | "
            "awk -F: '$2==\"wifi\" && $3!=\"unmanaged\" {print $1; exit}'",
            wifi_device, sizeof(wifi_device));
        if (wifi_device[0]) {
            backend = BACKEND_NETWORKMANAGER;
            return;
        }
    }

    discover_iw_device();
    if (!wifi_device[0]) return;
    shell_quote(wifi_device, quoted, sizeof(quoted));
    if (command_exists("iwctl")) {
        snprintf(command, sizeof(command), "iwctl station list 2>/dev/null");
        if (command_output(command, output, sizeof(output)) &&
            strstr(output, wifi_device)) {
            backend = BACKEND_IWD;
            return;
        }
    }
    if (command_exists("wpa_cli")) {
        snprintf(command, sizeof(command),
                 "wpa_cli -i %s ping 2>/dev/null", quoted);
        if (command_output(command, output, sizeof(output)) &&
            strstr(output, "PONG")) {
            backend = BACKEND_WPA_SUPPLICANT;
        }
    }
}

static void refresh_identity(void)
{
    char command[MAX_CMD];
    char quoted[256];
    char pci[MAX_TEXT];

    if (backend == BACKEND_NETWORKMANAGER) {
        read_first_line(
            "nmcli -t -f DEVICE,TYPE,STATE device status 2>/dev/null | "
            "awk -F: '$2==\"wifi\" && $3!=\"unmanaged\" {print $1; exit}'",
            wifi_device, sizeof(wifi_device));
    }
    discover_iw_device();
    connection_uuid[0] = gateway[0] = '\0';
    if (wifi_device[0]) {
        shell_quote(wifi_device, quoted, sizeof(quoted));
        if (backend == BACKEND_NETWORKMANAGER) {
            snprintf(command, sizeof(command),
                     "nmcli -g GENERAL.CON-UUID device show %s 2>/dev/null", quoted);
            read_first_line(command, connection_uuid, sizeof(connection_uuid));
        } else if (backend == BACKEND_WPA_SUPPLICANT) {
            snprintf(command, sizeof(command),
                     "wpa_cli -i %s status 2>/dev/null | "
                     "awk -F= '$1==\"id\" {print $2; exit}'", quoted);
            read_first_line(command, connection_uuid, sizeof(connection_uuid));
        }
        snprintf(command, sizeof(command),
                 "ip route show default dev %s 2>/dev/null | awk '{print $3; exit}'",
                 quoted);
        read_first_line(command, gateway, sizeof(gateway));
    }

    if (wifi_device[0]) {
        snprintf(command, sizeof(command),
                 "basename \"$(readlink -f /sys/class/net/%s/device/driver "
                 "2>/dev/null)\"", quoted);
        read_first_line(command, driver, sizeof(driver));
    } else {
        driver[0] = '\0';
    }
    if (!command_output(
            "lspci -mm 2>/dev/null | awk -F'\"' "
            "'tolower($0) ~ /network controller|wireless/ {print $4 \" \" $6; exit}'",
            pci, sizeof(pci)) || !pci[0]) {
        snprintf(adapter, sizeof(adapter), "%s", driver[0] ? driver : "Unknown Wi-Fi adapter");
    } else {
        pci[strcspn(pci, "\r\n")] = '\0';
        trim(pci);
        copy_text(adapter, sizeof(adapter), pci);
    }
}

static int scan_networks_nmcli(int rescan)
{
    FILE *pipe;
    char line[1024];
    char command[512];
    char previous_bssid[32] = "";
    int found_previous = 0;
    if (ap_count && selected >= 0 && selected < ap_count)
        copy_text(previous_bssid, sizeof(previous_bssid), aps[selected].bssid);
    ap_count = 0;

    snprintf(command, sizeof(command),
             "nmcli -w 20 -t -e yes -f IN-USE,SSID,BSSID,CHAN,FREQ,SIGNAL,SECURITY "
             "device wifi list --rescan %s 2>/dev/null", rescan ? "yes" : "no");
    pipe = popen(command, "r");
    if (!pipe) {
        set_message(1, "Could not run nmcli.");
        return 0;
    }
    while (ap_count < MAX_APS && fgets(line, sizeof(line), pipe)) {
        char *field[7];
        int n;
        line[strcspn(line, "\r\n")] = '\0';
        n = split_nmcli(line, field, 7);
        if (n != 7 || !field[1][0]) continue;
        aps[ap_count].active = !strcmp(field[0], "*") || !strcmp(field[0], "yes");
        snprintf(aps[ap_count].ssid, sizeof(aps[ap_count].ssid), "%s", field[1]);
        snprintf(aps[ap_count].bssid, sizeof(aps[ap_count].bssid), "%s", field[2]);
        aps[ap_count].channel = atoi(field[3]);
        aps[ap_count].frequency = atoi(field[4]);
        aps[ap_count].signal = atoi(field[5]);
        snprintf(aps[ap_count].security, sizeof(aps[ap_count].security), "%s",
                 field[6][0] ? field[6] : "open");
        aps[ap_count].gateway_ms = -1;
        aps[ap_count].internet_ms = -1;
        aps[ap_count].download_mbps = -1;
        aps[ap_count].packet_loss = -1;
        aps[ap_count].tested = 0;
        ap_count++;
    }
    {
        int status = pclose(pipe);
        if (status == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            set_message(1, "NetworkManager scan failed.");
            return 0;
        }
    }
    for (int i = 0; previous_bssid[0] && i < ap_count; i++) {
        if (!strcmp(previous_bssid, aps[i].bssid)) {
            selected = i;
            found_previous = 1;
            break;
        }
    }
    if (!found_previous) {
        selected = 0;
        for (int i = 0; i < ap_count; i++) {
            if (aps[i].active) {
                selected = i;
                break;
            }
        }
    }
    if (top > selected) top = selected;
    refresh_identity();
    set_message(0, "%d access points found on %s.", ap_count,
                wifi_device[0] ? wifi_device : "Wi-Fi");
    return 1;
}

static int frequency_channel(int frequency)
{
    if (frequency == 2484) return 14;
    if (frequency >= 2412 && frequency <= 2472) return (frequency - 2407) / 5;
    if (frequency >= 5000 && frequency < 5925) return (frequency - 5000) / 5;
    if (frequency >= 5955) return (frequency - 5950) / 5;
    return 0;
}

static int signal_percent(double dbm)
{
    int percent = (int)((dbm + 90.0) * (100.0 / 60.0) + 0.5);
    if (percent < 0) return 0;
    if (percent > 100) return 100;
    return percent;
}

static void append_iw_ap(const AccessPoint *candidate)
{
    if (!candidate->ssid[0] || !candidate->bssid[0] || ap_count >= MAX_APS)
        return;
    aps[ap_count++] = *candidate;
}

static int compare_ap_signal(const void *left, const void *right)
{
    const AccessPoint *a = left;
    const AccessPoint *b = right;
    return b->signal - a->signal;
}

static int parse_iw_scan(FILE *pipe)
{
    char line[2048];
    AccessPoint candidate;
    int in_bss = 0;
    memset(&candidate, 0, sizeof(candidate));
    candidate.gateway_ms = candidate.internet_ms =
        candidate.download_mbps = candidate.packet_loss = -1;
    while (fgets(line, sizeof(line), pipe)) {
        char *text = line;
        while (*text && isspace((unsigned char)*text)) text++;
        line[strcspn(line, "\r\n")] = '\0';
        if (!strncmp(text, "BSS ", 4)) {
            if (in_bss) append_iw_ap(&candidate);
            memset(&candidate, 0, sizeof(candidate));
            candidate.gateway_ms = candidate.internet_ms =
                candidate.download_mbps = candidate.packet_loss = -1;
            candidate.active = strstr(text, "-- associated") != NULL;
            if (sscanf(text + 4, "%31[^ (]", candidate.bssid) != 1)
                candidate.bssid[0] = '\0';
            snprintf(candidate.security, sizeof(candidate.security), "open");
            in_bss = 1;
        } else if (in_bss && !strncmp(text, "freq:", 5)) {
            candidate.frequency = atoi(text + 5);
            candidate.channel = frequency_channel(candidate.frequency);
        } else if (in_bss && !strncmp(text, "signal:", 7)) {
            candidate.signal = signal_percent(strtod(text + 7, NULL));
        } else if (in_bss && !strncmp(text, "SSID:", 5)) {
            text += 5;
            while (*text == ' ' || *text == '\t') text++;
            copy_text(candidate.ssid, sizeof(candidate.ssid), text);
        } else if (in_bss && !strncmp(text, "RSN:", 4)) {
            snprintf(candidate.security, sizeof(candidate.security), "WPA2/3");
        } else if (in_bss && !strncmp(text, "WPA:", 4) &&
                   !strcmp(candidate.security, "open")) {
            snprintf(candidate.security, sizeof(candidate.security), "WPA");
        } else if (in_bss && strstr(text, "Authentication suites: SAE")) {
            snprintf(candidate.security, sizeof(candidate.security), "WPA3");
        }
    }
    if (in_bss) append_iw_ap(&candidate);
    return ap_count > 0;
}

static int scan_networks_iw(int rescan)
{
    char command[MAX_CMD];
    char quoted[256];
    char previous_bssid[32] = "";
    char output[MAX_TEXT];
    FILE *pipe;
    int found_previous = 0;
    int status;

    if (!wifi_device[0]) return 0;
    if (ap_count && selected >= 0 && selected < ap_count)
        copy_text(previous_bssid, sizeof(previous_bssid), aps[selected].bssid);
    shell_quote(wifi_device, quoted, sizeof(quoted));
    if (rescan) {
        if (backend == BACKEND_NETWORKMANAGER) {
            snprintf(command, sizeof(command),
                     "nmcli -w 20 device wifi rescan ifname %s 2>&1", quoted);
        } else if (backend == BACKEND_IWD) {
            snprintf(command, sizeof(command),
                     "iwctl station %s scan 2>&1", quoted);
        } else {
            snprintf(command, sizeof(command),
                     "wpa_cli -i %s scan 2>&1", quoted);
        }
        if (!command_output(command, output, sizeof(output)) ||
            strstr(output, "FAIL")) {
            trim(output);
            set_message(1, "%s", output[0] ? output : "Wi-Fi scan request failed.");
            return 0;
        }
        sui_sleep_ms(2500);
    }
    snprintf(command, sizeof(command), "iw dev %s scan dump 2>/dev/null", quoted);
    pipe = popen(command, "r");
    if (!pipe) return 0;
    ap_count = 0;
    parse_iw_scan(pipe);
    status = pclose(pipe);
    if (status == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0 ||
        !ap_count) {
        set_message(1, "Could not read the kernel Wi-Fi scan cache.");
        return 0;
    }
    qsort(aps, (size_t)ap_count, sizeof(aps[0]), compare_ap_signal);
    for (int i = 0; previous_bssid[0] && i < ap_count; i++) {
        if (!strcasecmp(previous_bssid, aps[i].bssid)) {
            selected = i;
            found_previous = 1;
            break;
        }
    }
    if (!found_previous) {
        selected = 0;
        for (int i = 0; i < ap_count; i++) {
            if (aps[i].active) {
                selected = i;
                break;
            }
        }
    }
    refresh_identity();
    set_message(0, "%d access points found with %s on %s.",
                ap_count, backend_name(), wifi_device);
    return 1;
}

static int scan_networks(int rescan)
{
    if (backend == BACKEND_NETWORKMANAGER) return scan_networks_nmcli(rescan);
    if (command_exists("iw") && scan_networks_iw(rescan)) return 1;
    return 0;
}

static int hidden_prompt(const char *label, char *value, size_t size, int hidden)
{
    int row = LINES - 3;
    int ch;
    size_t len = 0;
    value[0] = '\0';
    curs_set(1);
    timeout(-1);
    for (;;) {
        move(row, 0);
        clrtoeol();
        attron(A_BOLD);
        mvprintw(row, 2, "%s", label);
        attroff(A_BOLD);
        if (hidden) {
            for (size_t i = 0; i < len; i++) addch('*');
        } else {
            addnstr(value, COLS - (int)strlen(label) - 4);
        }
        refresh();
        ch = getch();
        if (ch == 27) {
            value[0] = '\0';
            curs_set(0);
            return 0;
        }
        if (ch == '\n' || ch == KEY_ENTER) {
            curs_set(0);
            return 1;
        }
        if ((ch == KEY_BACKSPACE || ch == 127 || ch == 8) && len) {
            value[--len] = '\0';
        } else if (ch >= 32 && ch < 127 && len + 1 < size) {
            value[len++] = (char)ch;
            value[len] = '\0';
        }
    }
}

static int run_action(const char *command, const char *working)
{
    char output[MAX_TEXT];
    set_message(0, "%s", working);
    erase();
    mvprintw(1, 2, "simplenet");
    mvprintw(3, 2, "%s", message);
    refresh();
    if (!command_output(command, output, sizeof(output))) {
        trim(output);
        set_message(1, "%s", output[0] ? output : "Network action failed.");
        return 0;
    }
    trim(output);
    set_message(0, "%s", output[0] ? output : "Done.");
    return 1;
}

static void erase_secret(char *text, size_t size)
{
    volatile unsigned char *p = (volatile unsigned char *)text;
    while (size--) *p++ = 0;
}

static int command_argv_input(char *const argv[], char *secret,
                              size_t secret_size, char *output,
                              size_t output_size)
{
    int input_pipe[2];
    int output_pipe[2];
    pid_t child;
    size_t used = 0;
    int status = -1;
    char buffer[1024];
    ssize_t count;

    if (pipe(input_pipe) != 0) {
        erase_secret(secret, secret_size);
        return 0;
    }
    if (pipe(output_pipe) != 0) {
        close(input_pipe[0]);
        close(input_pipe[1]);
        erase_secret(secret, secret_size);
        return 0;
    }
    child = fork();
    if (child < 0) {
        close(input_pipe[0]);
        close(input_pipe[1]);
        close(output_pipe[0]);
        close(output_pipe[1]);
        erase_secret(secret, secret_size);
        return 0;
    }
    if (child == 0) {
        dup2(input_pipe[0], STDIN_FILENO);
        dup2(output_pipe[1], STDOUT_FILENO);
        dup2(output_pipe[1], STDERR_FILENO);
        close(input_pipe[0]);
        close(input_pipe[1]);
        close(output_pipe[0]);
        close(output_pipe[1]);
        execvp(argv[0], argv);
        _exit(127);
    }

    close(input_pipe[0]);
    close(output_pipe[1]);
    {
        size_t password_length = strlen(secret);
        size_t sent = 0;
        while (sent < password_length) {
            ssize_t written = write(input_pipe[1], secret + sent,
                                    password_length - sent);
            if (written < 0) {
                if (errno == EINTR) continue;
                break;
            }
            sent += (size_t)written;
        }
        (void)write(input_pipe[1], "\n", 1);
    }
    close(input_pipe[1]);
    erase_secret(secret, secret_size);

    if (output_size) output[0] = '\0';
    while ((count = read(output_pipe[0], buffer, sizeof(buffer))) > 0) {
        if (output_size && used + 1 < output_size) {
            size_t room = output_size - used - 1;
            size_t copy = (size_t)count < room ? (size_t)count : room;
            memcpy(output + used, buffer, copy);
            used += copy;
            output[used] = '\0';
        }
    }
    close(output_pipe[0]);
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static int nmcli_connect_password(const AccessPoint *ap, char *password,
                                  size_t password_size, char *output,
                                  size_t output_size)
{
    char *const argv[] = {
        "nmcli", "-w", "30", "--ask", "device", "wifi", "connect",
        (char *)ap->ssid, "bssid", (char *)ap->bssid, "ifname",
        wifi_device, NULL
    };
    return command_argv_input(argv, password, password_size, output, output_size);
}

static int current_bssid(char *bssid, size_t size)
{
    char q_device[256];
    char command[MAX_CMD];
    if (!wifi_device[0]) return 0;
    shell_quote(wifi_device, q_device, sizeof(q_device));
    snprintf(command, sizeof(command),
             "iw dev %s link 2>/dev/null | "
             "awk '/^Connected to / {print $3; exit}'", q_device);
    read_first_line(command, bssid, size);
    return bssid[0] != '\0';
}

static void refresh_active_marker(void)
{
    char bssid[32] = "";
    int active_index = -1;
    if (current_bssid(bssid, sizeof(bssid))) {
        for (int i = 0; i < ap_count; i++) {
            aps[i].active = !strcasecmp(aps[i].bssid, bssid);
            if (aps[i].active) active_index = i;
        }
    }
    if (active_index >= 0) selected = active_index;
    refresh_identity();
}

static int enforce_selected_bssid(const AccessPoint *ap)
{
    char previous[32] = "";
    char actual[32] = "";
    char q_uuid[512], q_device[256], q_bssid[128];
    char command[MAX_CMD];
    configured_bssid(previous, sizeof(previous));
    shell_quote(connection_uuid, q_uuid, sizeof(q_uuid));
    shell_quote(wifi_device, q_device, sizeof(q_device));
    shell_quote(ap->bssid, q_bssid, sizeof(q_bssid));
    snprintf(command, sizeof(command),
             "nmcli -w 20 connection modify uuid %s 802-11-wireless.bssid %s && "
             "nmcli -w 30 connection up uuid %s ifname %s 2>&1",
             q_uuid, q_bssid, q_uuid, q_device);
    if (!run_action(command, "Associating with the selected mesh node...")) {
        restore_bssid(previous);
        set_message(1, "Could not activate mesh node %s; restored the previous setting.",
                    ap->bssid);
        return 0;
    }
    sui_sleep_ms(1200);
    if (!current_bssid(actual, sizeof(actual)) ||
        strcasecmp(actual, ap->bssid) != 0) {
        restore_bssid(previous);
        set_message(1, "NetworkManager chose %s instead of %s; restored the previous setting.",
                    actual[0] ? actual : "an unknown node", ap->bssid);
        return 0;
    }
    return 1;
}

static void connect_selected_networkmanager(void)
{
    AccessPoint target;
    AccessPoint *ap = &target;
    char password[256];
    char q_ssid[512], q_bssid[128], q_device[256];
    char chosen_ssid[128], chosen_bssid[32], actual_bssid[32] = "";
    char command[MAX_CMD];
    char output[MAX_TEXT];
    int secured;
    int same_network = 0;
    if (!ap_count) return;
    target = aps[selected];
    copy_text(chosen_ssid, sizeof(chosen_ssid), ap->ssid);
    copy_text(chosen_bssid, sizeof(chosen_bssid), ap->bssid);
    set_message(0, "Switching %s to mesh node %s...", chosen_ssid, chosen_bssid);
    draw();
    if (current_bssid(actual_bssid, sizeof(actual_bssid)) &&
        !strcasecmp(actual_bssid, ap->bssid)) {
        set_message(0, "Already connected through mesh node %s.", ap->bssid);
        return;
    }
    for (int i = 0; i < ap_count; i++)
        if (aps[i].active && !strcmp(aps[i].ssid, ap->ssid))
            same_network = 1;
    if (same_network) {
        if (enforce_selected_bssid(ap)) {
            refresh_active_marker();
            set_message(0, "Pinned %s to mesh node %s.", chosen_ssid, chosen_bssid);
        }
        return;
    }
    secured = strcmp(ap->security, "open") != 0 && strcmp(ap->security, "--") != 0;
    password[0] = '\0';
    shell_quote(ap->ssid, q_ssid, sizeof(q_ssid));
    shell_quote(ap->bssid, q_bssid, sizeof(q_bssid));
    shell_quote(wifi_device, q_device, sizeof(q_device));
    snprintf(command, sizeof(command),
             "nmcli -w 30 device wifi connect %s bssid %s ifname %s 2>&1",
             q_ssid, q_bssid, q_device);
    if (run_action(command, "Connecting with saved credentials...")) {
        refresh_identity();
        if (enforce_selected_bssid(ap)) {
            refresh_active_marker();
            set_message(0, "Connected %s through mesh node %s.",
                        chosen_ssid, chosen_bssid);
        }
        return;
    }
    if (!secured) return;
    if (!hidden_prompt("Password (Esc cancels): ", password,
                       sizeof(password), 1)) {
        set_message(0, "Connection cancelled.");
        return;
    }
    set_message(0, "Connecting...");
    draw();
    if (nmcli_connect_password(ap, password, sizeof(password),
                               output, sizeof(output))) {
        trim(output);
        refresh_identity();
        if (enforce_selected_bssid(ap)) {
            refresh_active_marker();
            set_message(0, "Connected %s through mesh node %s.",
                        chosen_ssid, chosen_bssid);
        }
    } else {
        trim(output);
        set_message(1, "%s", output[0] ? output : "Connection failed.");
    }
}

static void connect_selected_iwd(void)
{
    AccessPoint *ap = &aps[selected];
    char q_device[256], q_ssid[512], command[MAX_CMD], output[MAX_TEXT];
    char password[256] = "";
    int secured = strcmp(ap->security, "open") != 0;
    shell_quote(wifi_device, q_device, sizeof(q_device));
    shell_quote(ap->ssid, q_ssid, sizeof(q_ssid));
    snprintf(command, sizeof(command),
             "iwctl --dont-ask station %s connect %s 2>&1", q_device, q_ssid);
    if (!run_action(command, "Connecting with saved iwd credentials...")) {
        if (!secured) return;
        if (!hidden_prompt("Passphrase (Esc cancels): ", password,
                           sizeof(password), 1)) {
            set_message(0, "Connection cancelled.");
            return;
        }
        {
            char *const argv[] = {
                "iwctl", "station", wifi_device, "connect", ap->ssid, NULL
            };
            set_message(0, "Connecting through iwd...");
            draw();
            if (!command_argv_input(argv, password, sizeof(password),
                                    output, sizeof(output))) {
                trim(output);
                set_message(1, "%s", output[0] ? output : "iwd connection failed.");
                return;
            }
        }
    }
    snprintf(command, sizeof(command),
             "iwctl debug %s roam %s >/dev/null 2>&1", q_device, ap->bssid);
    (void)command_output(command, output, sizeof(output));
    sui_sleep_ms(1500);
    refresh_active_marker();
}

static int wpa_network_id(const char *ssid, char *id, size_t id_size)
{
    char q_device[256], command[MAX_CMD], output[MAX_TEXT];
    char *line;
    char *save = NULL;
    shell_quote(wifi_device, q_device, sizeof(q_device));
    snprintf(command, sizeof(command),
             "wpa_cli -i %s list_networks 2>/dev/null", q_device);
    if (!command_output(command, output, sizeof(output))) return 0;
    for (line = strtok_r(output, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        char *first_tab = strchr(line, '\t');
        char *second_tab;
        if (!first_tab) continue;
        second_tab = strchr(first_tab + 1, '\t');
        if (!second_tab) continue;
        *first_tab = '\0';
        *second_tab = '\0';
        if (!strcmp(first_tab + 1, ssid)) {
            copy_text(id, id_size, line);
            return 1;
        }
    }
    return 0;
}

static void hex_encode(const char *source, char *dest, size_t size)
{
    static const char digits[] = "0123456789abcdef";
    size_t j = 0;
    for (size_t i = 0; source[i] && j + 2 < size; i++) {
        unsigned char byte = (unsigned char)source[i];
        dest[j++] = digits[byte >> 4];
        dest[j++] = digits[byte & 15];
    }
    dest[j] = '\0';
}

static void wpa_config_quote(const char *source, char *dest, size_t size)
{
    size_t j = 0;
    if (size) dest[j++] = '"';
    for (size_t i = 0; source[i] && j + 3 < size; i++) {
        if (source[i] == '\\' || source[i] == '"') dest[j++] = '\\';
        dest[j++] = source[i];
    }
    if (j + 1 < size) dest[j++] = '"';
    dest[j] = '\0';
}

static int wpa_select_network(const char *id, const char *bssid)
{
    char q_device[256], q_id[128], q_bssid[128], command[MAX_CMD], output[MAX_TEXT];
    shell_quote(wifi_device, q_device, sizeof(q_device));
    shell_quote(id, q_id, sizeof(q_id));
    shell_quote(bssid, q_bssid, sizeof(q_bssid));
    snprintf(command, sizeof(command),
             "[ \"$(wpa_cli -i %s bssid %s %s 2>/dev/null | tail -n1)\" = OK ] && "
             "wpa_cli -i %s select_network %s 2>&1",
             q_device, q_id, q_bssid, q_device, q_id);
    return command_output(command, output, sizeof(output)) &&
           !strstr(output, "FAIL");
}

static void connect_selected_wpa(void)
{
    AccessPoint *ap = &aps[selected];
    char id[32], password[256] = "", output[MAX_TEXT], command[MAX_CMD];
    char q_device[256], q_id[128];
    int secured = strcmp(ap->security, "open") != 0;
    if (wpa_network_id(ap->ssid, id, sizeof(id))) {
        if (!wpa_select_network(id, ap->bssid)) {
            set_message(1, "wpa_supplicant could not activate the saved network.");
            return;
        }
        sui_sleep_ms(1500);
        refresh_active_marker();
        return;
    }
    if (secured && !hidden_prompt("WPA passphrase (Esc cancels): ", password,
                                  sizeof(password), 1)) {
        set_message(0, "Connection cancelled.");
        return;
    }
    shell_quote(wifi_device, q_device, sizeof(q_device));
    snprintf(command, sizeof(command),
             "wpa_cli -i %s add_network 2>/dev/null", q_device);
    if (!command_output(command, output, sizeof(output))) {
        erase_secret(password, sizeof(password));
        set_message(1, "wpa_supplicant could not create a network profile.");
        return;
    }
    trim(output);
    {
        char *line;
        char *save = NULL;
        id[0] = '\0';
        for (line = strtok_r(output, "\n", &save); line;
             line = strtok_r(NULL, "\n", &save)) {
            trim(line);
            if (isdigit((unsigned char)line[0])) copy_text(id, sizeof(id), line);
        }
    }
    if (!id[0]) {
        erase_secret(password, sizeof(password));
        set_message(1, "wpa_supplicant returned an invalid network id.");
        return;
    }
    {
        char ssid_hex[257];
        char passphrase[520];
        char commands[2048];
        char *const argv[] = {"wpa_cli", "-i", wifi_device, NULL};
        hex_encode(ap->ssid, ssid_hex, sizeof(ssid_hex));
        if (secured) {
            wpa_config_quote(password, passphrase, sizeof(passphrase));
            snprintf(commands, sizeof(commands),
                     "set_network %s ssid %s\n"
                     "set_network %s psk %s\n"
                     "bssid %s %s\n"
                     "enable_network %s\nselect_network %s\nquit",
                     id, ssid_hex, id, passphrase, id, ap->bssid, id, id);
        } else {
            snprintf(commands, sizeof(commands),
                     "set_network %s ssid %s\n"
                     "set_network %s key_mgmt NONE\n"
                     "bssid %s %s\n"
                     "enable_network %s\nselect_network %s\nquit",
                     id, ssid_hex, id, id, ap->bssid, id, id);
        }
        erase_secret(password, sizeof(password));
        if (!command_argv_input(argv, commands, sizeof(commands),
                                output, sizeof(output)) ||
            strstr(output, "FAIL")) {
            shell_quote(id, q_id, sizeof(q_id));
            snprintf(command, sizeof(command),
                     "wpa_cli -i %s remove_network %s >/dev/null 2>&1",
                     q_device, q_id);
            command_output(command, output, sizeof(output));
            set_message(1, "wpa_supplicant rejected the new network profile.");
            return;
        }
    }
    snprintf(command, sizeof(command),
             "wpa_cli -i %s save_config >/dev/null 2>&1", q_device);
    (void)command_output(command, output, sizeof(output));
    sui_sleep_ms(1500);
    refresh_active_marker();
    if (!gateway[0])
        set_message(0, "Associated. Waiting for the system IP service to provide a route.");
}

static void connect_selected(void)
{
    if (!ap_count) return;
    switch (backend) {
        case BACKEND_NETWORKMANAGER: connect_selected_networkmanager(); break;
        case BACKEND_IWD: connect_selected_iwd(); break;
        case BACKEND_WPA_SUPPLICANT: connect_selected_wpa(); break;
        default: set_message(1, "No supported Wi-Fi manager was detected."); break;
    }
}

static double ping_average(const char *host, int count, double *loss_percent)
{
    char q_host[512];
    char command[MAX_CMD];
    char output[MAX_TEXT];
    char *summary;
    char *loss_text;
    char *equals;
    char *slash;
    char *end;
    double average;
    double loss = 100;
    if (loss_percent) *loss_percent = loss;
    shell_quote(host, q_host, sizeof(q_host));
    snprintf(command, sizeof(command),
             "(ping -n -c %d -i 0.2 -W 2 %s 2>/dev/null || true)",
             count, q_host);
    if (!command_output(command, output, sizeof(output))) return -1;
    loss_text = strstr(output, "% packet loss");
    if (loss_text) {
        char *start = loss_text;
        while (start > output && isspace((unsigned char)start[-1])) start--;
        while (start > output &&
               (isdigit((unsigned char)start[-1]) || start[-1] == '.')) start--;
        loss = strtod(start, NULL);
    }
    if (loss_percent) *loss_percent = loss;
    summary = strstr(output, "\nrtt ");
    if (!summary) summary = strstr(output, "\nround-trip ");
    equals = summary ? strchr(summary, '=') : NULL;
    if (!equals) return -1;
    (void)strtod(equals + 1, &slash);
    if (!slash || *slash != '/') return -1;
    average = strtod(slash + 1, &end);
    if (end == slash + 1) return -1;
    return average;
}

static double download_mbps_test(int wanted_samples, long bytes, int timeout_seconds)
{
    double samples[3];
    int count = 0;
    if (wanted_samples < 1) wanted_samples = 1;
    if (wanted_samples > 3) wanted_samples = 3;
    for (int i = 0; i < wanted_samples; i++) {
        char speed[128] = "";
        char command[MAX_CMD];
        snprintf(command, sizeof(command),
                 "curl -L --max-time %d -sS -o /dev/null "
                 "-w '%%{speed_download}' "
                 "'https://speed.cloudflare.com/__down?bytes=%ld' 2>/dev/null",
                 timeout_seconds, bytes);
        if (command_output(command, speed, sizeof(speed)) && speed[0]) {
            double bytes_per_second = strtod(speed, NULL);
            if (bytes_per_second > 0)
                samples[count++] = bytes_per_second * 8.0 / 1000000.0;
        }
    }
    if (!count) return -1;
    for (int i = 0; i < count; i++)
        for (int j = i + 1; j < count; j++)
            if (samples[j] < samples[i]) {
                double swap = samples[i];
                samples[i] = samples[j];
                samples[j] = swap;
            }
    return samples[count / 2];
}

static double download_mbps(void)
{
    return download_mbps_test(3, 5000000, 20);
}

static void audit_current(void)
{
    double local;
    double internet;
    double local_loss;
    double internet_loss;
    double mbps = -1;
    char active_ssid[128] = "";
    for (int i = 0; i < ap_count; i++) {
        if (aps[i].active) {
            snprintf(active_ssid, sizeof(active_ssid), "%s", aps[i].ssid);
            break;
        }
    }
    if (!gateway[0]) {
        set_message(1, "No connected network or default gateway.");
        return;
    }
    set_message(0, "Auditing %s: gateway latency...", active_ssid);
    draw();
    local = ping_average(gateway, 8, &local_loss);
    set_message(0, "Auditing %s: internet latency...", active_ssid);
    draw();
    internet = ping_average("1.1.1.1", 8, &internet_loss);
    if (command_exists("curl")) {
        set_message(0, "Auditing %s: download throughput...", active_ssid);
        draw();
        mbps = download_mbps();
    }
    if (mbps >= 0) {
        set_message(0, "%s  router %.1f ms/%.0f%% loss  internet %.1f ms/%.0f%%  %.1f Mbps",
                    active_ssid, local, local_loss, internet, internet_loss, mbps);
    } else {
        set_message(0, "%s  router %.1f ms/%.0f%% loss  internet %.1f ms/%.0f%%%s",
                    active_ssid, local, local_loss, internet, internet_loss,
                    command_exists("curl") ? "" : "  (install curl for throughput)");
    }
}

static int active_ssid(char *ssid, size_t size)
{
    for (int i = 0; i < ap_count; i++) {
        if (aps[i].active) {
            snprintf(ssid, size, "%s", aps[i].ssid);
            return 1;
        }
    }
    return 0;
}

static int pin_bssid_networkmanager(const char *bssid)
{
    char q_uuid[512], q_bssid[128], command[MAX_CMD], output[MAX_TEXT];
    if (!connection_uuid[0]) return 0;
    shell_quote(connection_uuid, q_uuid, sizeof(q_uuid));
    shell_quote(bssid, q_bssid, sizeof(q_bssid));
    snprintf(command, sizeof(command),
             "nmcli -w 20 connection modify uuid %s 802-11-wireless.bssid %s && "
             "nmcli -w 30 connection up uuid %s 2>&1", q_uuid, q_bssid, q_uuid);
    return command_output(command, output, sizeof(output));
}

static int configured_bssid(char *bssid, size_t size)
{
    char q_uuid[512], command[MAX_CMD];
    char q_device[256];
    if (!wifi_device[0]) return 0;
    shell_quote(wifi_device, q_device, sizeof(q_device));
    if (backend == BACKEND_NETWORKMANAGER) {
        if (!connection_uuid[0]) return 0;
        shell_quote(connection_uuid, q_uuid, sizeof(q_uuid));
        snprintf(command, sizeof(command),
                 "nmcli -e no -g 802-11-wireless.bssid connection show uuid %s 2>/dev/null",
                 q_uuid);
    } else if (backend == BACKEND_WPA_SUPPLICANT) {
        if (!connection_uuid[0]) return 0;
        shell_quote(connection_uuid, q_uuid, sizeof(q_uuid));
        snprintf(command, sizeof(command),
                 "wpa_cli -i %s get_network %s bssid 2>/dev/null",
                 q_device, q_uuid);
    } else {
        snprintf(command, sizeof(command),
                 "iw dev %s link 2>/dev/null | "
                 "awk '/^Connected to / {print $3; exit}'", q_device);
    }
    read_first_line(command, bssid, size);
    if (!strcmp(bssid, "--") || !strcmp(bssid, "any") ||
        !strcmp(bssid, "FAIL")) bssid[0] = '\0';
    return 1;
}

static int pin_bssid(const char *bssid)
{
    char q_device[256], q_uuid[512], q_bssid[128], command[MAX_CMD], output[MAX_TEXT];
    if (!wifi_device[0]) return 0;
    if (backend == BACKEND_NETWORKMANAGER)
        return pin_bssid_networkmanager(bssid);
    shell_quote(wifi_device, q_device, sizeof(q_device));
    shell_quote(bssid ? bssid : "", q_bssid, sizeof(q_bssid));
    if (backend == BACKEND_IWD) {
        snprintf(command, sizeof(command),
                 "iwctl debug %s roam %s 2>&1", q_device, q_bssid);
    } else if (backend == BACKEND_WPA_SUPPLICANT && connection_uuid[0]) {
        shell_quote(connection_uuid, q_uuid, sizeof(q_uuid));
        snprintf(command, sizeof(command),
                 "[ \"$(wpa_cli -i %s bssid %s %s 2>/dev/null | tail -n1)\" = OK ] && "
                 "wpa_cli -i %s reassociate 2>&1",
                 q_device, q_uuid, q_bssid, q_device);
    } else {
        return 0;
    }
    return command_output(command, output, sizeof(output)) &&
           !strstr(output, "FAIL");
}

static int restore_bssid(const char *bssid)
{
    if (backend == BACKEND_IWD && (!bssid || !bssid[0])) return 1;
    return pin_bssid(bssid ? bssid : "");
}

static void unpin(void)
{
    char q_device[256], q_uuid[512], command[MAX_CMD];
    if (!wifi_device[0]) {
        set_message(1, "No active Wi-Fi connection.");
        return;
    }
    if (backend == BACKEND_IWD) {
        set_message(0, "iwd roaming is already automatic; specific-node choices are temporary.");
        return;
    }
    if (!connection_uuid[0]) {
        set_message(1, "No active Wi-Fi profile was found.");
        return;
    }
    shell_quote(wifi_device, q_device, sizeof(q_device));
    shell_quote(connection_uuid, q_uuid, sizeof(q_uuid));
    if (backend == BACKEND_NETWORKMANAGER) {
        snprintf(command, sizeof(command),
                 "nmcli -w 20 connection modify uuid %s 802-11-wireless.bssid '' && "
                 "nmcli -w 30 connection up uuid %s 2>&1", q_uuid, q_uuid);
    } else {
        snprintf(command, sizeof(command),
                 "[ \"$(wpa_cli -i %s bssid %s any 2>/dev/null | tail -n1)\" = OK ] && "
                 "wpa_cli -i %s reassociate 2>&1", q_device, q_uuid, q_device);
    }
    if (run_action(command, "Restoring automatic access-point selection..."))
        scan_networks(0);
}

static void disable_powersave(void)
{
    char q_uuid[512], q_device[256], command[MAX_CMD];
    int status;
    if (!wifi_device[0]) {
        set_message(1, "No active Wi-Fi connection.");
        return;
    }
    if (backend == BACKEND_NETWORKMANAGER && connection_uuid[0]) {
        shell_quote(connection_uuid, q_uuid, sizeof(q_uuid));
        snprintf(command, sizeof(command),
                 "nmcli -w 20 connection modify uuid %s 802-11-wireless.powersave 2 && "
                 "nmcli -w 30 connection up uuid %s 2>&1", q_uuid, q_uuid);
        if (run_action(command, "Disabling Wi-Fi power saving..."))
            scan_networks(0);
        return;
    }
    shell_quote(wifi_device, q_device, sizeof(q_device));
    def_prog_mode();
    endwin();
    printf("simplenet Adapter care\n----------------------\n"
           "Disable kernel Wi-Fi power saving on %s for this boot? [y/N] ",
           wifi_device);
    fflush(stdout);
    int answer = getchar();
    if (answer != 'y' && answer != 'Y') {
        status = 0;
    } else {
        snprintf(command, sizeof(command),
                 "sudo iw dev %s set power_save off", q_device);
        status = system(command);
    }
    puts(status == 0 ? "\nDone." : "\nCould not change kernel power saving.");
    puts("Press Enter to return to simplenet.");
    while (getchar() != '\n' && !feof(stdin)) {}
    reset_prog_mode();
    refresh();
    set_message(status != 0, status == 0 ? "Power-saving action complete." :
                "Power-saving action failed.");
}

static void optimize_mesh(void)
{
    char ssid[128];
    char original_bssid[32] = "";
    char best_bssid[32] = "";
    char actual_bssid[32] = "";
    int best_signal = -1;
    int visible_candidates = 0;
    if (!active_ssid(ssid, sizeof(ssid)) || !gateway[0] ||
        (backend != BACKEND_IWD && !connection_uuid[0])) {
        set_message(1, "Connect to a network before optimizing its mesh.");
        return;
    }
    for (int i = 0; i < ap_count; i++)
        if (!strcmp(aps[i].ssid, ssid) && aps[i].signal >= 30)
            visible_candidates++;
    if (visible_candidates < 2) {
        set_message(0, "Only one usable %s node is visible; nothing to optimize.", ssid);
        return;
    }
    configured_bssid(original_bssid, sizeof(original_bssid));
    for (int i = 0; i < ap_count; i++) {
        if (strcmp(aps[i].ssid, ssid) || aps[i].signal < 30) continue;
        if (aps[i].signal > best_signal) {
            best_signal = aps[i].signal;
            snprintf(best_bssid, sizeof(best_bssid), "%s", aps[i].bssid);
        }
    }
    if (!best_bssid[0]) {
        set_message(1, "No usable node was found.");
        return;
    }
    set_message(0, "Selecting strongest %s node: %s (%d%%)...",
                ssid, best_bssid, best_signal);
    draw();
    if (!pin_bssid(best_bssid)) {
        restore_bssid(original_bssid);
        refresh_active_marker();
        set_message(1, "Could not activate the strongest node; restored the previous setting.");
        return;
    }
    sui_sleep_ms(1200);
    if (!current_bssid(actual_bssid, sizeof(actual_bssid)) ||
        strcasecmp(actual_bssid, best_bssid)) {
        restore_bssid(original_bssid);
        refresh_active_marker();
        set_message(1, "Network manager chose %s instead of %s; restored the previous setting.",
                    actual_bssid[0] ? actual_bssid : "another node", best_bssid);
        return;
    }
    refresh_active_marker();
    if (backend == BACKEND_IWD)
        set_message(0, "Selected strongest %s node: %s at %d%% (iwd may roam later).",
                    ssid, best_bssid, best_signal);
    else
        set_message(0, "Pinned strongest %s node: %s at %d%%.",
                    ssid, best_bssid, best_signal);
}

static int file_contains(const char *path, const char *needle)
{
    FILE *file = fopen(path, "r");
    char line[512];
    if (!file) return 0;
    while (fgets(line, sizeof(line), file)) {
        if (strstr(line, needle)) {
            fclose(file);
            return 1;
        }
    }
    fclose(file);
    return 0;
}

static int remedy_parameters_supported(const AdapterRemedy *remedy)
{
    char command[512];
    char parameters[MAX_TEXT];
    char options[512];
    char *save = NULL;
    char *option;
    if (!remedy || !command_exists("modinfo")) return 0;
    snprintf(command, sizeof(command), "modinfo -p %s 2>/dev/null", remedy->module);
    if (!command_output(command, parameters, sizeof(parameters))) return 0;
    copy_text(options, sizeof(options), remedy->options);
    for (option = strtok_r(options, " ", &save); option;
         option = strtok_r(NULL, " ", &save)) {
        char *equals = strchr(option, '=');
        if (equals) *equals = '\0';
        if (!option[0] || !strstr(parameters, option)) return 0;
    }
    return 1;
}

static const AdapterRemedy *active_remedy(void)
{
    for (size_t i = 0; i < sizeof(remedies) / sizeof(remedies[0]); i++) {
        size_t prefix_length = strlen(remedies[i].driver_prefix);
        if (!strncmp(driver, remedies[i].driver_prefix, prefix_length) &&
            remedy_parameters_supported(&remedies[i]))
            return &remedies[i];
    }
    return NULL;
}

static int remedy_configured(const AdapterRemedy *remedy)
{
    if (!remedy) return 0;
    if (file_contains("/etc/modprobe.d/70-simplenet-adapter-stability.conf",
                      remedy->module))
        return 1;
    return !strcmp(remedy->module, "rtw89_pci") &&
           file_contains("/etc/modprobe.d/70-rtw89-pcie-power.conf",
                         "disable_aspm_l1=y");
}

static void terminal_maintenance(const char *action)
{
    const AdapterRemedy *remedy = active_remedy();
    char command[MAX_CMD];
    int status;
    int cancelled = 0;
    if (!remedy) return;
    def_prog_mode();
    endwin();
    if (!strcmp(action, "apply")) {
        puts("simplenet Adapter care");
        puts("----------------------");
        printf("%s\n%s\n", remedy->title, remedy->description);
        puts("It writes one modprobe file and rebuilds the initramfs.");
        printf("Apply this reversible stability remedy? [y/N] ");
        fflush(stdout);
        int answer = getchar();
        if (answer != 'y' && answer != 'Y') {
            status = 0;
            cancelled = 1;
        } else {
            snprintf(command, sizeof(command),
                "sudo sh -c 'set -eu; "
                "cfg=/etc/modprobe.d/70-simplenet-adapter-stability.conf; "
                "install -d -m 0755 /etc/modprobe.d; "
                "backup=$(mktemp); staged=$(mktemp); had=0; "
                "cleanup(){ rm -f \"$backup\" \"$staged\"; }; trap cleanup EXIT; "
                "if [ -f \"$cfg\" ]; then cp -p \"$cfg\" \"$backup\"; had=1; fi; "
                "printf \"%%s\\n\" \"options %s %s\" >\"$staged\"; "
                "install -m 0644 \"$staged\" \"$cfg\"; "
                "rebuild(){ if command -v mkinitcpio >/dev/null 2>&1; then mkinitcpio -P; "
                "elif command -v update-initramfs >/dev/null 2>&1; then update-initramfs -u; "
                "elif command -v dracut >/dev/null 2>&1; then dracut --regenerate-all --force; "
                "else echo \"No supported initramfs builder found\" >&2; return 1; fi; }; "
                "if ! rebuild; then "
                "if [ \"$had\" = 1 ]; then cp -p \"$backup\" \"$cfg\"; else rm -f \"$cfg\"; fi; "
                "rebuild >/dev/null 2>&1 || true; exit 1; fi'",
                remedy->module, remedy->options);
            /* The interpolated values come only from the compiled remedy table. */
            status = system(command);
        }
    } else {
        puts("simplenet Adapter care");
        puts("----------------------");
        printf("Remove %s? [y/N] ", remedy->title);
        fflush(stdout);
        int answer = getchar();
        if (answer != 'y' && answer != 'Y') {
            status = 0;
            cancelled = 1;
        } else {
            snprintf(command, sizeof(command),
                "sudo sh -c 'set -eu; backup=$(mktemp -d); "
                "cleanup(){ rm -rf \"$backup\"; }; trap cleanup EXIT; "
                "files=\"/etc/modprobe.d/70-simplenet-adapter-stability.conf %s\"; "
                "for f in $files; do [ ! -f \"$f\" ] || cp -p \"$f\" \"$backup/\"; done; "
                "rm -f $files; "
                "rebuild(){ if command -v mkinitcpio >/dev/null 2>&1; then mkinitcpio -P; "
                "elif command -v update-initramfs >/dev/null 2>&1; then update-initramfs -u; "
                "elif command -v dracut >/dev/null 2>&1; then dracut --regenerate-all --force; "
                "else echo \"No supported initramfs builder found\" >&2; return 1; fi; }; "
                "if ! rebuild; then for f in \"$backup\"/*; do [ ! -f \"$f\" ] || "
                "cp -p \"$f\" /etc/modprobe.d/; done; "
                "rebuild >/dev/null 2>&1 || true; exit 1; fi'",
                !strcmp(remedy->module, "rtw89_pci")
                    ? "/etc/modprobe.d/70-simplenet-rtw89-stability.conf "
                      "/etc/modprobe.d/70-rtw89-pcie-power.conf"
                    : "");
            status = system(command);
        }
    }
    if (cancelled) puts("\nCancelled. Nothing was changed.");
    else if (status == 0) puts("\nDone. Reboot is required for the changed driver remedy.");
    else puts("\nThe maintenance command failed.");
    puts("Press Enter to return to simplenet.");
    while (getchar() != '\n' && !feof(stdin)) {}
    reset_prog_mode();
    refresh();
    if (cancelled) set_message(0, "Adapter care cancelled.");
    else set_message(status != 0, status == 0 ? "Adapter care complete." :
                     "Adapter care failed; previous settings were restored.");
}

static const char *band_name(int frequency)
{
    if (frequency >= 5925) return "6";
    if (frequency >= 4900) return "5";
    return "2.4";
}

static void draw_networks(void)
{
    int rows = LINES - 9;
    int end;
    if (rows < 1) rows = 1;
    if (selected < top) top = selected;
    if (selected >= top + rows) top = selected - rows + 1;
    end = top + rows < ap_count ? top + rows : ap_count;

    if (COLS >= 98)
        mvprintw(3, 2, "%-2s %-28s %-17s %4s %6s %7s  %s",
                 "", "network", "mesh node", "band", "signal", "latency", "security");
    else
        mvprintw(3, 2, "%-2s %-22s %-17s %4s %6s  %s",
                 "", "network", "mesh node", "band", "signal", "security");
    for (int i = top, row = 4; i < end; i++, row++) {
        AccessPoint *ap = &aps[i];
        char latency[24] = "—";
        if (ap->tested)
            snprintf(latency, sizeof(latency), "%.1fms", ap->gateway_ms);
        else if (ap->gateway_ms >= 0)
            snprintf(latency, sizeof(latency), "loss");
        if (i == selected) attron(A_REVERSE);
        if (COLS >= 98)
            mvprintw(row, 2, "%-2s %-28.28s %-17s %4s %5d%% %7s  %-20.20s",
                     ap->active ? "●" : "", ap->ssid, ap->bssid,
                     band_name(ap->frequency), ap->signal,
                     latency, ap->security);
        else
            mvprintw(row, 2, "%-2s %-22.22s %-17s %4s %5d%%  %-12.12s",
                     ap->active ? "●" : "", ap->ssid, ap->bssid,
                     band_name(ap->frequency), ap->signal, ap->security);
        if (i == selected) attroff(A_REVERSE);
    }
    if (!ap_count) mvprintw(5, 4, "No Wi-Fi networks found. Press s to scan.");
}

static void draw_details(void)
{
    AccessPoint *ap = ap_count ? &aps[selected] : NULL;
    mvprintw(3, 2, "network");
    mvprintw(4, 2, "-------");
    if (!ap) {
        mvprintw(6, 2, "No access point selected.");
        return;
    }
    mvprintw(6, 2, "SSID             %s", ap->ssid);
    mvprintw(7, 2, "mesh node        %s", ap->bssid);
    mvprintw(8, 2, "channel          %d  (%d MHz / %s GHz)",
             ap->channel, ap->frequency, band_name(ap->frequency));
    mvprintw(9, 2, "signal           %d%%", ap->signal);
    mvprintw(10, 2, "security         %s", ap->security);
    mvprintw(11, 2, "active           %s", ap->active ? "yes" : "no");
    if (ap->tested) mvprintw(12, 2, "router latency   %.1f ms", ap->gateway_ms);
    mvprintw(14, 2, "Enter connect/pin this node   o optimize this mesh");
}

static void draw_care(void)
{
    const AdapterRemedy *remedy = active_remedy();
    char powersave[64] = "unknown";
    char q_uuid[512], q_device[256], command[MAX_CMD];
    if (backend == BACKEND_NETWORKMANAGER && connection_uuid[0]) {
        shell_quote(connection_uuid, q_uuid, sizeof(q_uuid));
        snprintf(command, sizeof(command),
                 "nmcli -g 802-11-wireless.powersave connection show uuid %s 2>/dev/null",
                 q_uuid);
        read_first_line(command, powersave, sizeof(powersave));
    } else if (wifi_device[0] && command_exists("iw")) {
        shell_quote(wifi_device, q_device, sizeof(q_device));
        snprintf(command, sizeof(command),
                 "iw dev %s get power_save 2>/dev/null | "
                 "awk -F': ' '/Power save/ {print $2; exit}'", q_device);
        read_first_line(command, powersave, sizeof(powersave));
    }
    mvprintw(3, 2, "adapter care");
    mvprintw(4, 2, "------------");
    mvprintw(6, 2, "adapter          %.60s", adapter);
    mvprintw(7, 2, "driver           %s", driver[0] ? driver : "unknown");
    mvprintw(8, 2, "device           %s", wifi_device[0] ? wifi_device : "none");
    mvprintw(9, 2, "manager          %s", backend_name());
    mvprintw(10, 2, "power saving     %s%s", powersave,
             !strcmp(powersave, "2") ? " (disabled)" : "");
    mvprintw(12, 2, "generic remedy");
    mvprintw(13, 4, "p  disable Wi-Fi power saving");
    mvprintw(15, 2, "driver remedy");
    if (remedy) {
        mvprintw(16, 4, "%s", remedy->title);
        mvprintw(17, 4, "%s", remedy->description);
        mvprintw(18, 4, "state: %s", remedy_configured(remedy) ? "configured" : "not configured");
        mvprintw(20, 4, "A apply   R remove   (sudo and reboot required)");
    } else {
        mvprintw(16, 4, "No driver-specific remedy is recommended for this adapter.");
        mvprintw(17, 4, "simplenet will not apply unrelated module settings.");
    }
}

static void draw_help(void)
{
    static const char *lines[] = {
        "↑/↓ or j/k     choose an access point",
        "Enter          connect; credentials are masked",
        "s              rescan nearby networks",
        "d              selected network details",
        "a              audit router, internet latency, and download speed",
        "o              select the strongest visible node of the active mesh",
        "u              remove a mesh-node pin",
        "c              Adapter care and stability remedies",
        "p              disable power saving for the active connection",
        "?              this help",
        "q              quit",
        "",
        "Optimization selects and pins the strongest visible same-SSID node.",
        "Driver remedies are detected, reversible, and never applied silently."
    };
    mvprintw(3, 2, "help");
    mvprintw(4, 2, "----");
    for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]) && 6 + (int)i < LINES - 3; i++)
        mvprintw(6 + (int)i, 2, "%s", lines[i]);
}

static const char *footer_text(void)
{
    switch (view) {
        case VIEW_DETAILS:
            return "Esc networks  Enter connect  o optimize  c care  ? help  q quit";
        case VIEW_CARE:
            return "p power off  A apply  R remove  Esc networks  ? help  q quit";
        case VIEW_HELP:
            return "Esc networks  q quit";
        case VIEW_NETWORKS:
        default:
            return "↑↓ move  Enter join  s scan  d info  a audit  o strongest  c care  ?  q quit";
    }
}

static void draw(void)
{
    erase();
    attron(A_BOLD);
    mvprintw(1, 2, "simplenet");
    attroff(A_BOLD);
    mvprintw(1, 14, "%s  %s  %s", wifi_device[0] ? wifi_device : "no adapter",
             backend_name(), gateway[0] ? "online" : "offline");
    switch (view) {
        case VIEW_NETWORKS: draw_networks(); break;
        case VIEW_DETAILS: draw_details(); break;
        case VIEW_CARE: draw_care(); break;
        case VIEW_HELP: draw_help(); break;
    }
    if (message_error) attron(A_BOLD);
    mvprintw(LINES - 2, 2, "%.*s", COLS > 4 ? COLS - 4 : 0, message);
    if (message_error) attroff(A_BOLD);
    mvhline(LINES - 1, 0, ' ', COLS);
    if (COLS > 2) mvaddnstr(LINES - 1, 1, footer_text(), COLS - 2);
    refresh();
}

static void usage(const char *program)
{
    printf("Usage: %s [--help]\n", program);
    puts("A SimpleSuite Wi-Fi manager, mesh optimizer, network auditor, and adapter care tool.");
}

int main(int argc, char **argv)
{
    int ch;
    if (argc > 1) {
        usage(argv[0]);
        return !strcmp(argv[1], "--help") || !strcmp(argv[1], "-h") ? 0 : 2;
    }
    if (!command_exists("ip") || !command_exists("ping")) {
        fputs("simplenet requires iproute2 and ping.\n", stderr);
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    setlocale(LC_ALL, "");
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    if (has_colors()) {
        start_color();
        use_default_colors();
    }
    set_message(0, "Detecting Wi-Fi manager...");
    draw();
    detect_backend();
    if (backend == BACKEND_NONE) {
        endwin();
        fputs("simplenet could not detect NetworkManager, iwd, or a standalone "
              "wpa_supplicant control interface.\n", stderr);
        return 1;
    }
    if (backend != BACKEND_NETWORKMANAGER && !command_exists("iw")) {
        endwin();
        fputs("simplenet requires iw with the iwd and wpa_supplicant backends.\n",
              stderr);
        return 1;
    }
    set_message(0, "Reading connection state...");
    draw();
    refresh_identity();
    set_message(0, "Loading nearby networks...");
    draw();
    scan_networks(0);
    for (;;) {
        draw();
        timeout(-1);
        ch = getch();
        if (ch == 'q' || ch == 'Q') break;
        if (ch == KEY_RESIZE) continue;
        if (ch == KEY_UP || ch == 'k') {
            if (selected > 0) selected--;
            view = VIEW_NETWORKS;
        } else if (ch == KEY_DOWN || ch == 'j') {
            if (selected + 1 < ap_count) selected++;
            view = VIEW_NETWORKS;
        } else if (ch == KEY_PPAGE) {
            selected -= 10;
            if (selected < 0) selected = 0;
            view = VIEW_NETWORKS;
        } else if (ch == KEY_NPAGE) {
            selected += 10;
            if (selected >= ap_count) selected = ap_count ? ap_count - 1 : 0;
            view = VIEW_NETWORKS;
        } else if (ch == 's') {
            view = VIEW_NETWORKS;
            set_message(0, "Scanning...");
            draw();
            scan_networks(1);
        } else if (ch == '\n' || ch == KEY_ENTER) {
            connect_selected();
            view = VIEW_NETWORKS;
        } else if (ch == 'd') {
            view = VIEW_DETAILS;
        } else if (ch == 'a') {
            audit_current();
        } else if (ch == 'o') {
            optimize_mesh();
            view = VIEW_NETWORKS;
        } else if (ch == 'u') {
            unpin();
            view = VIEW_NETWORKS;
        } else if (ch == 'p') {
            disable_powersave();
        } else if (ch == 'c') {
            view = VIEW_CARE;
        } else if ((ch == 'A') && view == VIEW_CARE && active_remedy()) {
            terminal_maintenance("apply");
        } else if ((ch == 'R') && view == VIEW_CARE && active_remedy()) {
            terminal_maintenance("remove");
        } else if (ch == '?' || ch == 'h') {
            view = VIEW_HELP;
        } else if (ch == 27) {
            view = VIEW_NETWORKS;
        }
    }
    endwin();
    return 0;
}
