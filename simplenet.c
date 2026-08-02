#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <ncurses.h>
#include <ctype.h>
#include <errno.h>
#if defined(__FreeBSD__) || defined(SIMPLENET_TEST_SHARED_BACKENDS)
#include <limits.h>
#endif
#ifdef __FreeBSD__
#include <sys/socket.h>
#include <arpa/inet.h>
#include <fcntl.h>
#endif
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
#if defined(__APPLE__) && !defined(SIMPLENET_TEST_SHARED_BACKENDS)
#define SIMPLENET_NATIVE_MACOS 1
#endif
#ifdef SIMPLENET_NATIVE_MACOS
#include "simplenet-macos.h"
#endif

#define MAX_APS 256
#define MAX_TEXT 4096
#define MAX_CMD 8192
#if defined(__FreeBSD__) || defined(SIMPLENET_TEST_SHARED_BACKENDS)
#define PREFERRED_AUTOCONNECT_PRIORITY "999"
#endif
#ifdef __FreeBSD__
#define MAX_WIFI_CARDS 16
#endif

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
#ifdef __FreeBSD__
    int hidden_ssid;
#endif
} AccessPoint;

#ifdef __FreeBSD__
typedef struct {
    int associated;
    int system_default;
    char interface_name[64];
    char parent[64];
    char driver[64];
    char name[256];
    char ssid[128];
    char address[64];
} WifiCard;
#endif
typedef enum {
    VIEW_NETWORKS,
    VIEW_DETAILS,
    VIEW_CARE,
#ifdef __FreeBSD__
    VIEW_CARDS,
#endif
    VIEW_HELP
} View;

typedef enum {
    BACKEND_NONE,
    BACKEND_NETWORKMANAGER,
    BACKEND_IWD,
    BACKEND_WPA_SUPPLICANT
#ifdef SIMPLENET_NATIVE_MACOS
    ,
    BACKEND_COREWLAN
#endif
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
#ifdef __FreeBSD__
static WifiCard wifi_cards[MAX_WIFI_CARDS];
static int wifi_card_count;
static int wifi_card_selected;
static int wifi_card_top;
#endif
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
#ifdef __FreeBSD__
static int current_bssid(char *bssid, size_t size);
static int active_ssid(char *ssid, size_t size);
#endif
#ifdef __FreeBSD__
static double ping_average(const char *host, int count, double *loss_percent);
static void refresh_freebsd_cards(void);

static const char *ap_ssid_label(const AccessPoint *ap)
{
    if (!ap) return "";
    return ap->hidden_ssid ? "(hidden SSID)" : ap->ssid;
}
#endif

static const char *backend_name(void)
{
    switch (backend) {
        case BACKEND_NETWORKMANAGER: return "NetworkManager";
        case BACKEND_IWD: return "iwd";
        case BACKEND_WPA_SUPPLICANT: return "wpa_supplicant";
#ifdef SIMPLENET_NATIVE_MACOS
        case BACKEND_COREWLAN: return "CoreWLAN";
#endif
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

#ifdef __FreeBSD__
static int freebsd_device_name_valid(const char *name)
{
    if (!name || !name[0]) return 0;
    for (size_t i = 0; name[i]; i++)
        if (!isalnum((unsigned char)name[i]) && name[i] != '_' &&
            name[i] != '-' && name[i] != '.')
            return 0;
    return 1;
}

static int freebsd_word_list_contains(const char *text, const char *word)
{
    size_t word_length;

    if (!text || !word || !word[0]) return 0;
    word_length = strlen(word);
    while (*text) {
        const char *start;
        while (*text && isspace((unsigned char)*text)) text++;
        start = text;
        while (*text && !isspace((unsigned char)*text)) text++;
        if ((size_t)(text - start) == word_length &&
            !memcmp(start, word, word_length))
            return 1;
    }
    return 0;
}

static void copy_span(char *dest, size_t size, const char *start,
                      const char *end)
{
    size_t length;
    if (!dest || !size) return;
    if (!start || !end || end < start) {
        dest[0] = '\0';
        return;
    }
    length = (size_t)(end - start);
    if (length >= size) length = size - 1;
    if (length) memcpy(dest, start, length);
    dest[length] = '\0';
}

static void freebsd_driver_parts(const char *parent, char *driver,
                                 size_t driver_size, char *unit,
                                 size_t unit_size)
{
    size_t split;
    if (driver_size) driver[0] = '\0';
    if (unit_size) unit[0] = '\0';
    if (!parent) return;
    split = strlen(parent);
    while (split && isdigit((unsigned char)parent[split - 1])) split--;
    copy_span(driver, driver_size, parent, parent + split);
    copy_text(unit, unit_size, parent + split);
}

static WifiCard *freebsd_card_by_parent(const char *parent)
{
    for (int i = 0; i < wifi_card_count; i++)
        if (!strcmp(wifi_cards[i].parent, parent)) return &wifi_cards[i];
    return NULL;
}

static WifiCard *freebsd_card_by_interface(const char *interface_name)
{
    for (int i = 0; i < wifi_card_count; i++)
        if (!strcmp(wifi_cards[i].interface_name, interface_name))
            return &wifi_cards[i];
    return NULL;
}

static WifiCard *freebsd_add_card_parent(const char *parent)
{
    WifiCard *card;
    char unit[32];
    if (!freebsd_device_name_valid(parent)) return NULL;
    card = freebsd_card_by_parent(parent);
    if (card) return card;
    if (wifi_card_count >= MAX_WIFI_CARDS) return NULL;
    card = &wifi_cards[wifi_card_count++];
    memset(card, 0, sizeof(*card));
    copy_text(card->parent, sizeof(card->parent), parent);
    freebsd_driver_parts(parent, card->driver, sizeof(card->driver),
                         unit, sizeof(unit));
    return card;
}

static int parse_freebsd_card_parents(char *text)
{
    char *save = NULL;
    char *parent;
    int added = 0;
    for (parent = strtok_r(text, " \t\r\n", &save); parent;
         parent = strtok_r(NULL, " \t\r\n", &save)) {
        int before = wifi_card_count;
        if (freebsd_add_card_parent(parent) && wifi_card_count > before)
            added++;
    }
    return added;
}

static void parse_freebsd_ssid(const char *line, char *ssid, size_t size)
{
    const char *start = line + 5;
    const char *end;
    if (*start == '"') {
        start++;
        end = strchr(start, '"');
    } else {
        end = strstr(start, " channel ");
    }
    if (!end) end = start + strlen(start);
    copy_span(ssid, size, start, end);
}

static int parse_freebsd_card_ifconfig(const char *interface_name, char *text)
{
    WifiCard parsed;
    WifiCard *card;
    char *save = NULL;
    char *line;
    int wireless = 0;

    if (!freebsd_device_name_valid(interface_name)) return 0;
    memset(&parsed, 0, sizeof(parsed));
    copy_text(parsed.interface_name, sizeof(parsed.interface_name),
              interface_name);
    for (line = strtok_r(text, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        trim(line);
        if (!strncmp(line, "groups:", 7)) {
            if (freebsd_word_list_contains(line + 7, "wlan")) wireless = 1;
        } else if (!strncmp(line, "parent interface:", 17)) {
            copy_text(parsed.parent, sizeof(parsed.parent), line + 17);
            trim(parsed.parent);
        } else if (!strncmp(line, "status:", 7)) {
            const char *status = line + 7;
            while (*status && isspace((unsigned char)*status)) status++;
            parsed.associated = strcmp(status, "associated") == 0;
        } else if (!strncmp(line, "ssid ", 5)) {
            parse_freebsd_ssid(line, parsed.ssid, sizeof(parsed.ssid));
        } else if (!parsed.address[0] && !strncmp(line, "inet ", 5)) {
            const char *start = line + 5;
            const char *end = start;
            while (*end && !isspace((unsigned char)*end)) end++;
            copy_span(parsed.address, sizeof(parsed.address), start, end);
        }
    }
    if (!wireless || !freebsd_device_name_valid(parsed.parent)) return 0;
    card = freebsd_add_card_parent(parsed.parent);
    if (!card) return 0;
    if (!card->interface_name[0] || parsed.associated ||
        !card->associated) {
        copy_text(card->interface_name, sizeof(card->interface_name),
                  parsed.interface_name);
        copy_text(card->ssid, sizeof(card->ssid), parsed.ssid);
        copy_text(card->address, sizeof(card->address), parsed.address);
        card->associated = parsed.associated;
    }
    return 1;
}

static int freebsd_pciconf_value(const char *text, const char *field,
                                 char *value, size_t size)
{
    const char *line = text;
    size_t field_length = strlen(field);
    while (line && *line) {
        const char *end = strchr(line, '\n');
        const char *start = line;
        const char *equals;
        const char *quote;
        while (*start && isspace((unsigned char)*start)) start++;
        if (!strncmp(start, field, field_length) &&
            isspace((unsigned char)start[field_length])) {
            equals = strchr(start + field_length, '=');
            quote = equals ? strchr(equals + 1, '\'') : NULL;
            if (quote && (!end || quote < end)) {
                const char *close = strchr(quote + 1, '\'');
                if (close && (!end || close <= end)) {
                    copy_span(value, size, quote + 1, close);
                    return 1;
                }
            }
        }
        line = end ? end + 1 : NULL;
    }
    if (size) value[0] = '\0';
    return 0;
}

static void identify_freebsd_card(WifiCard *card)
{
    char command[MAX_CMD];
    char output[MAX_TEXT];
    char vendor[128] = "";
    char device[256] = "";
    char unit[32];
    char quoted[256];

    if (!card) return;
    shell_quote(card->parent, quoted, sizeof(quoted));
    if (command_exists("pciconf")) {
        snprintf(command, sizeof(command), "pciconf -lv %s 2>/dev/null",
                 quoted);
        if (command_output(command, output, sizeof(output))) {
            (void)freebsd_pciconf_value(output, "vendor", vendor,
                                        sizeof(vendor));
            (void)freebsd_pciconf_value(output, "device", device,
                                        sizeof(device));
        }
    }
    if (device[0]) {
        if (vendor[0])
            snprintf(card->name, sizeof(card->name), "%s %s", vendor, device);
        else
            copy_text(card->name, sizeof(card->name), device);
        return;
    }
    freebsd_driver_parts(card->parent, card->driver, sizeof(card->driver),
                         unit, sizeof(unit));
    if (card->driver[0] && unit[0]) {
        snprintf(command, sizeof(command), "sysctl -n dev.%s.%s.%%desc 2>/dev/null",
                 card->driver, unit);
        read_first_line(command, card->name, sizeof(card->name));
        char *detail = strstr(card->name, ", class ");
        if (detail) *detail = '\0';
    }
    if (!card->name[0])
        snprintf(card->name, sizeof(card->name), "Wi-Fi device %s",
                 card->parent);
}

static void refresh_freebsd_cards(void)
{
    char output[MAX_TEXT];
    char interfaces[MAX_TEXT];
    char default_interface[64] = "";
    char *save = NULL;
    char *interface_name;
    int group_list;

    memset(wifi_cards, 0, sizeof(wifi_cards));
    wifi_card_count = 0;
    if (command_output("sysctl -n net.wlan.devices 2>/dev/null", output,
                       sizeof(output)))
        (void)parse_freebsd_card_parents(output);
    group_list = command_output("ifconfig -g wlan 2>/dev/null", interfaces,
                                sizeof(interfaces)) && interfaces[0];
    if (!group_list)
        (void)command_output("ifconfig -l 2>/dev/null", interfaces,
                             sizeof(interfaces));
    if (interfaces[0]) {
        for (interface_name = strtok_r(interfaces, " \t\r\n", &save);
             interface_name;
             interface_name = strtok_r(NULL, " \t\r\n", &save)) {
            char command[MAX_CMD];
            char quoted[256];
            if (!freebsd_device_name_valid(interface_name)) continue;
            shell_quote(interface_name, quoted, sizeof(quoted));
            snprintf(command, sizeof(command), "ifconfig %s 2>/dev/null",
                     quoted);
            if (command_output(command, output, sizeof(output)))
                (void)parse_freebsd_card_ifconfig(interface_name, output);
        }
    }
    read_first_line(
        "route -n get -inet default 2>/dev/null | "
        "awk '/interface:/ {print $2; exit}'",
        default_interface, sizeof(default_interface));
    for (int i = 0; i < wifi_card_count; i++) {
        identify_freebsd_card(&wifi_cards[i]);
        wifi_cards[i].system_default =
            wifi_cards[i].interface_name[0] &&
            !strcmp(wifi_cards[i].interface_name, default_interface);
        if (!strcmp(wifi_cards[i].interface_name, wifi_device))
            wifi_card_selected = i;
    }
    if (wifi_card_selected >= wifi_card_count)
        wifi_card_selected = wifi_card_count ? wifi_card_count - 1 : 0;
}

static Backend freebsd_backend_for_device(const char *interface_name)
{
    char command[MAX_CMD];
    char output[MAX_TEXT];
    char quoted[256];

    if (!freebsd_device_name_valid(interface_name)) return BACKEND_NONE;
    shell_quote(interface_name, quoted, sizeof(quoted));
    if (command_exists("nmcli")) {
        snprintf(command, sizeof(command),
                 "nmcli -g GENERAL.TYPE device show %s 2>/dev/null", quoted);
        if (command_output(command, output, sizeof(output)) &&
            strstr(output, "wifi"))
            return BACKEND_NETWORKMANAGER;
    }
    if (command_exists("iwctl")) {
        if (command_output("iwctl station list 2>/dev/null", output,
                           sizeof(output)) &&
            freebsd_word_list_contains(output, interface_name))
            return BACKEND_IWD;
    }
    if (command_exists("wpa_cli")) {
        snprintf(command, sizeof(command), "wpa_cli -i %s ping 2>/dev/null",
                 quoted);
        if (command_output(command, output, sizeof(output)) &&
            strstr(output, "PONG"))
            return BACKEND_WPA_SUPPLICANT;
    }
    return BACKEND_NONE;
}
#endif
static void discover_iw_device(void)
{
    if (wifi_device[0] || !command_exists("iw")) return;
    read_first_line("iw dev 2>/dev/null | awk '$1==\"Interface\" {print $2; exit}'",
                    wifi_device, sizeof(wifi_device));
}

#ifdef __FreeBSD__
static Backend discover_freebsd_backend(void)
{
    Backend best_backend = BACKEND_NONE;
    int best_card = -1;
    int best_score = -1;

    if (wifi_device[0] || !command_exists("ifconfig")) return BACKEND_NONE;
    refresh_freebsd_cards();
    for (int i = 0; i < wifi_card_count; i++) {
        WifiCard *card = &wifi_cards[i];
        Backend candidate;
        int score;

        if (!card->interface_name[0]) continue;
        candidate = freebsd_backend_for_device(card->interface_name);
        if (candidate == BACKEND_NONE) continue;
        score = (card->associated ? 20 : 0) +
                (card->system_default ? 10 : 0);
        if (score <= best_score) continue;
        best_score = score;
        best_card = i;
        best_backend = candidate;
    }
    if (best_card < 0) return BACKEND_NONE;
    copy_text(wifi_device, sizeof(wifi_device),
              wifi_cards[best_card].interface_name);
    wifi_card_selected = best_card;
    return best_backend;
}
#endif

#ifdef __FreeBSD__
static int wpa_status_value(const char *field, char *value, size_t size)
{
    char command[MAX_CMD];
    char quoted[256];

    if (!wifi_device[0] || !field || !field[0]) return 0;
    shell_quote(wifi_device, quoted, sizeof(quoted));
    snprintf(command, sizeof(command),
             "wpa_cli -i %s status 2>/dev/null | "
             "awk -F= '$1==\"%s\" {print $2; exit}'",
             quoted, field);
    read_first_line(command, value, size);
    return value[0] != '\0';
}
#endif
static void detect_backend(void)
{
    char command[MAX_CMD];
    char output[MAX_TEXT];
    char quoted[256];

    backend = BACKEND_NONE;
    wifi_device[0] = '\0';
#ifdef SIMPLENET_NATIVE_MACOS
    {
        int powered = 0;

        if (simplenet_macos_interface(wifi_device, sizeof(wifi_device),
                                     NULL, 0, NULL, 0, &powered)) {
            backend = BACKEND_COREWLAN;
            if (!powered)
                set_message(1, "Wi-Fi is off; turn it on in Control Center.");
        }
        return;
    }
#endif
#ifdef __FreeBSD__
    backend = discover_freebsd_backend();
    if (backend != BACKEND_NONE) return;
#endif
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
#ifndef SIMPLENET_NATIVE_MACOS
    char quoted[256];
#endif
#if !defined(__FreeBSD__) && !defined(SIMPLENET_NATIVE_MACOS)
    char pci[MAX_TEXT];
#endif

#ifdef SIMPLENET_NATIVE_MACOS
    {
        char ssid[128] = "";
        char bssid[32] = "";
        int powered = 0;

        connection_uuid[0] = gateway[0] = '\0';
        if (!simplenet_macos_interface(wifi_device, sizeof(wifi_device),
                                      ssid, sizeof(ssid), bssid, sizeof(bssid),
                                      &powered)) {
            wifi_device[0] = '\0';
        }
        if (wifi_device[0]) {
            snprintf(command, sizeof(command),
                     "route -n get default 2>/dev/null | "
                     "awk '/gateway:/ {print $2; exit}'");
            read_first_line(command, gateway, sizeof(gateway));
        }
        snprintf(adapter, sizeof(adapter), "%s",
                 wifi_device[0] ? "Apple Wi-Fi adapter" : "No Wi-Fi adapter");
        snprintf(driver, sizeof(driver), "%s",
                 powered ? "CoreWLAN (powered)" : "CoreWLAN (off)");
    }
#else

    if (backend == BACKEND_NETWORKMANAGER) {
#ifdef __FreeBSD__
        if (!wifi_device[0])
            read_first_line(
                "nmcli -t -f DEVICE,TYPE,STATE device status 2>/dev/null | "
                "awk -F: '$2==\"wifi\" && $3!=\"unmanaged\" {print $1; exit}'",
                wifi_device, sizeof(wifi_device));
#else
        read_first_line(
            "nmcli -t -f DEVICE,TYPE,STATE device status 2>/dev/null | "
            "awk -F: '$2==\"wifi\" && $3!=\"unmanaged\" {print $1; exit}'",
            wifi_device, sizeof(wifi_device));
#endif
    }
#if defined(__FreeBSD__)
    if (!wifi_device[0]) {
        Backend discovered = discover_freebsd_backend();
        if (discovered != BACKEND_NONE) backend = discovered;
    }
#endif
    discover_iw_device();
    connection_uuid[0] = gateway[0] = '\0';
    if (wifi_device[0]) {
        shell_quote(wifi_device, quoted, sizeof(quoted));
        if (backend == BACKEND_NETWORKMANAGER) {
            snprintf(command, sizeof(command),
                     "nmcli -g GENERAL.CON-UUID device show %s 2>/dev/null", quoted);
            read_first_line(command, connection_uuid, sizeof(connection_uuid));
        } else if (backend == BACKEND_WPA_SUPPLICANT) {
#ifdef __FreeBSD__
            wpa_status_value("id", connection_uuid, sizeof(connection_uuid));
#else
            snprintf(command, sizeof(command),
                     "wpa_cli -i %s status 2>/dev/null | "
                     "awk -F= '$1==\"id\" {print $2; exit}'", quoted);
            read_first_line(command, connection_uuid, sizeof(connection_uuid));
#endif
        }
#ifdef __FreeBSD__
        snprintf(command, sizeof(command),
                 "route -n get -inet default 2>/dev/null | "
                 "awk '/gateway:/ {print $2; exit}'");
#else
        snprintf(command, sizeof(command),
                 "ip route show default dev %s 2>/dev/null | awk '{print $3; exit}'",
                 quoted);
#endif
        read_first_line(command, gateway, sizeof(gateway));
    }

#ifdef __FreeBSD__
    driver[0] = '\0';
    adapter[0] = '\0';
    if (wifi_device[0]) {
        WifiCard *card;
        refresh_freebsd_cards();
        card = freebsd_card_by_interface(wifi_device);
        if (card) {
            copy_text(adapter, sizeof(adapter), card->name);
            copy_text(driver, sizeof(driver), card->driver);
        }
        if (!adapter[0])
            snprintf(adapter, sizeof(adapter), "FreeBSD Wi-Fi interface %s",
                     wifi_device);
    }
#else
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
#endif
#endif
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

#if defined(__FreeBSD__) || defined(SIMPLENET_NATIVE_MACOS)
static int channel_frequency(int channel)
{
    if (channel == 14) return 2484;
    if (channel >= 1 && channel <= 13) return 2407 + channel * 5;
    if (channel >= 36 && channel <= 177) return 5000 + channel * 5;
    if (channel >= 181 && channel <= 233) return 5950 + channel * 5;
    return 0;
}
#endif
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

#ifdef __FreeBSD__
static int parse_wpa_scan_results(FILE *pipe)
{
    char line[2048];
    int header = 1;

    while (ap_count < MAX_APS && fgets(line, sizeof(line), pipe)) {
        char *bssid;
        char *frequency;
        char *level;
        char *flags;
        char *ssid;
        char *tab;
        AccessPoint *ap;

        line[strcspn(line, "\r\n")] = '\0';
        if (header) {
            header = 0;
            if (strstr(line, "bssid") && strstr(line, "frequency"))
                continue;
        }
        bssid = line;
        tab = strchr(bssid, '\t');
        if (!tab) continue;
        *tab++ = '\0';
        frequency = tab;
        tab = strchr(frequency, '\t');
        if (!tab) continue;
        *tab++ = '\0';
        level = tab;
        tab = strchr(level, '\t');
        if (!tab) continue;
        *tab++ = '\0';
        flags = tab;
        tab = strchr(flags, '\t');
        if (!tab) continue;
        *tab++ = '\0';
        ssid = tab;
        if (!bssid[0] || !ssid[0]) continue;

        ap = &aps[ap_count++];
        memset(ap, 0, sizeof(*ap));
        copy_text(ap->bssid, sizeof(ap->bssid), bssid);
        copy_text(ap->ssid, sizeof(ap->ssid), ssid);
        ap->frequency = atoi(frequency);
        ap->channel = frequency_channel(ap->frequency);
        ap->signal = signal_percent(strtod(level, NULL));
        if (strstr(flags, "WPA3") || strstr(flags, "SAE"))
            copy_text(ap->security, sizeof(ap->security), "WPA3");
        else if (strstr(flags, "WPA2") || strstr(flags, "RSN"))
            copy_text(ap->security, sizeof(ap->security), "WPA2");
        else if (strstr(flags, "WPA"))
            copy_text(ap->security, sizeof(ap->security), "WPA");
        else if (strstr(flags, "WEP"))
            copy_text(ap->security, sizeof(ap->security), "WEP");
        else
            copy_text(ap->security, sizeof(ap->security), "open");
        ap->gateway_ms = ap->internet_ms =
            ap->download_mbps = ap->packet_loss = -1;
    }
    return ap_count > 0;
}

static int bssid_text_at(const char *text)
{
    for (int i = 0; i < 17; i++) {
        if ((i + 1) % 3 == 0) {
            if (text[i] != ':') return 0;
        } else if (!isxdigit((unsigned char)text[i])) {
            return 0;
        }
    }
    return text[17] == '\0' || isspace((unsigned char)text[17]);
}

static char *find_bssid_text(char *line)
{
    for (char *p = line; *p; p++) {
        if ((p == line || isspace((unsigned char)p[-1])) &&
            bssid_text_at(p))
            return p;
    }
    return NULL;
}

static void freebsd_security_label(const char *caps, char *dest, size_t size)
{
    char first[32] = "";
    int privacy = 0;
    if (caps) {
        sscanf(caps, "%31s", first);
        privacy = strchr(first, 'P') != NULL;
    }
    if (caps && (strstr(caps, "SAE") || strstr(caps, "WPA3")))
        copy_text(dest, size, "WPA3");
    else if (caps && strstr(caps, "RSN"))
        copy_text(dest, size, "WPA2");
    else if (caps && strstr(caps, "WPA"))
        copy_text(dest, size, "WPA");
    else if (caps && (strstr(caps, "WEP") || privacy))
        copy_text(dest, size, "WEP");
    else
        copy_text(dest, size, "open");
}

static int parse_freebsd_scan(FILE *pipe)
{
    char line[2048];

    while (ap_count < MAX_APS && fgets(line, sizeof(line), pipe)) {
        char bssid_text[32];
        char *bssid;
        char *ssid;
        char *after;
        char *end;
        int channel;
        double signal;
        AccessPoint *ap;

        line[strcspn(line, "\r\n")] = '\0';
        bssid = find_bssid_text(line);
        if (!bssid) continue;
        memcpy(bssid_text, bssid, 17);
        bssid_text[17] = '\0';
        after = bssid + 17;
        *bssid = '\0';
        ssid = line;
        trim(ssid);

        while (*after && isspace((unsigned char)*after)) after++;
        channel = (int)strtol(after, &end, 10);
        if (end == after) continue;
        after = end;
        while (*after && isspace((unsigned char)*after)) after++;
        while (*after && !isspace((unsigned char)*after)) after++;
        while (*after && isspace((unsigned char)*after)) after++;
        signal = strtod(after, &end);
        if (end == after) continue;
        after = end;
        while (*after && !isspace((unsigned char)*after)) after++;
        while (*after && isspace((unsigned char)*after)) after++;
        while (*after && !isspace((unsigned char)*after)) after++;
        while (*after && isspace((unsigned char)*after)) after++;

        ap = &aps[ap_count++];
        memset(ap, 0, sizeof(*ap));
        copy_text(ap->ssid, sizeof(ap->ssid), ssid);
        copy_text(ap->bssid, sizeof(ap->bssid), bssid_text);
        ap->hidden_ssid = !ssid[0];
        ap->channel = channel;
        ap->frequency = channel_frequency(channel);
        ap->signal = signal_percent(signal);
        freebsd_security_label(after, ap->security, sizeof(ap->security));
        ap->gateway_ms = ap->internet_ms =
            ap->download_mbps = ap->packet_loss = -1;
    }
    return ap_count > 0;
}

static int scan_networks_wpa(int rescan)
{
    char command[MAX_CMD];
    char quoted[256];
    char output[MAX_TEXT];
    char active_bssid[32] = "";
    FILE *pipe;
    int status;

    if (!wifi_device[0]) return 0;
    shell_quote(wifi_device, quoted, sizeof(quoted));
    if (rescan) {
        snprintf(command, sizeof(command),
                 "wpa_cli -i %s scan 2>/dev/null", quoted);
        if (!command_output(command, output, sizeof(output)) ||
            strstr(output, "FAIL")) {
            set_message(1, "Wi-Fi scan request failed.");
            return 0;
        }
        sui_sleep_ms(2500);
    }
    snprintf(command, sizeof(command),
             "wpa_cli -i %s scan_results 2>/dev/null", quoted);
    pipe = popen(command, "r");
    if (!pipe) return 0;
    ap_count = 0;
    parse_wpa_scan_results(pipe);
    status = pclose(pipe);
    if (status == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0 ||
        !ap_count) {
        set_message(1, "Could not read the wpa_supplicant scan cache.");
        return 0;
    }
    qsort(aps, (size_t)ap_count, sizeof(aps[0]), compare_ap_signal);
    current_bssid(active_bssid, sizeof(active_bssid));
    selected = 0;
    for (int i = 0; i < ap_count; i++) {
        aps[i].active = active_bssid[0] &&
                        !strcasecmp(aps[i].bssid, active_bssid);
        if (aps[i].active) selected = i;
    }
    refresh_identity();
    set_message(0, "%d access points found with %s on %s.",
                ap_count, backend_name(), wifi_device);
    return 1;
}
#endif

#ifdef __FreeBSD__
static int scan_networks_freebsd(int rescan)
{
    char command[MAX_CMD];
    char quoted[256];
    char previous_bssid[32] = "";
    char active_bssid[32] = "";
    FILE *pipe;
    int found_previous = 0;
    int status;

    if (!wifi_device[0]) return 0;
    if (ap_count && selected >= 0 && selected < ap_count)
        copy_text(previous_bssid, sizeof(previous_bssid), aps[selected].bssid);
    shell_quote(wifi_device, quoted, sizeof(quoted));
    snprintf(command, sizeof(command), "ifconfig %s %s 2>/dev/null", quoted,
             rescan ? "scan" : "list scan");
    pipe = popen(command, "r");
    if (!pipe) return 0;
    ap_count = 0;
    parse_freebsd_scan(pipe);
    status = pclose(pipe);
    if (status == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0 ||
        !ap_count) {
        set_message(1, "Could not read the FreeBSD Wi-Fi scan table.");
        return 0;
    }
    qsort(aps, (size_t)ap_count, sizeof(aps[0]), compare_ap_signal);
    current_bssid(active_bssid, sizeof(active_bssid));
    for (int i = 0; i < ap_count; i++) {
        aps[i].active = active_bssid[0] &&
                        !strcasecmp(aps[i].bssid, active_bssid);
        if (!found_previous && previous_bssid[0] &&
            !strcasecmp(previous_bssid, aps[i].bssid)) {
            selected = i;
            found_previous = 1;
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
    set_message(0, "%d access points found with ifconfig on %s.",
                ap_count, wifi_device);
    return 1;
}
#endif
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

#ifdef SIMPLENET_NATIVE_MACOS
static int scan_networks_macos(int rescan)
{
    SimpleNetMacAccessPoint native_points[MAX_APS];
    char previous_bssid[32] = "";
    char error[MAX_TEXT] = "";
    int found_previous = 0;
    int count;

    (void)rescan;
    if (ap_count && selected >= 0 && selected < ap_count)
        copy_text(previous_bssid, sizeof(previous_bssid), aps[selected].bssid);
    count = simplenet_macos_scan(native_points, MAX_APS, error, sizeof(error));
    ap_count = 0;
    if (count < 0) {
        set_message(1, "%s", error[0] ? error : "CoreWLAN scan failed.");
        return 0;
    }
    for (int i = 0; i < count && ap_count < MAX_APS; i++) {
        AccessPoint *ap = &aps[ap_count++];

        memset(ap, 0, sizeof(*ap));
        copy_text(ap->ssid, sizeof(ap->ssid), native_points[i].ssid);
        copy_text(ap->bssid, sizeof(ap->bssid), native_points[i].bssid);
        ap->channel = native_points[i].channel;
        ap->frequency = channel_frequency(ap->channel);
        ap->signal = native_points[i].signal;
        ap->active = native_points[i].active;
        copy_text(ap->security, sizeof(ap->security),
                  native_points[i].security);
        ap->gateway_ms = ap->internet_ms =
            ap->download_mbps = ap->packet_loss = -1;
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
    if (top > selected)
        top = selected;
    refresh_identity();
    if (ap_count == 0) {
        set_message(1, "%s", error[0] ? error :
                    "No Wi-Fi networks were returned by CoreWLAN.");
        return 0;
    }
    set_message(0, "%d access points found with CoreWLAN on %s.",
                ap_count, wifi_device);
    return 1;
}
#endif

static int scan_networks(int rescan)
{
#ifdef SIMPLENET_NATIVE_MACOS
    if (backend == BACKEND_COREWLAN)
        return scan_networks_macos(rescan);
#endif
    if (backend == BACKEND_NETWORKMANAGER) return scan_networks_nmcli(rescan);
#ifdef __FreeBSD__
    if (backend == BACKEND_WPA_SUPPLICANT && scan_networks_freebsd(rescan))
        return 1;
    if (backend == BACKEND_WPA_SUPPLICANT && !command_exists("iw"))
        return scan_networks_wpa(rescan);
#endif
    if (command_exists("iw") && scan_networks_iw(rescan)) return 1;
    return 0;
}

#ifdef __FreeBSD__
static void select_freebsd_card(void)
{
    WifiCard *card;
    Backend selected_backend;
    char interface_name[64];
    char card_name[256];
    char default_interface[64] = "";
    int scan_ok;

    if (!wifi_card_count || wifi_card_selected < 0 ||
        wifi_card_selected >= wifi_card_count) {
        set_message(1, "No Wi-Fi card is available to select.");
        return;
    }
    card = &wifi_cards[wifi_card_selected];
    if (!card->interface_name[0]) {
        set_message(1, "%s is detected, but it has no Wi-Fi interface.",
                    card->name);
        return;
    }
    copy_text(interface_name, sizeof(interface_name), card->interface_name);
    copy_text(card_name, sizeof(card_name), card->name);
    selected_backend = freebsd_backend_for_device(interface_name);
    if (selected_backend == BACKEND_NONE) {
        set_message(1, "%s has no supported Wi-Fi manager control interface.",
                    interface_name);
        return;
    }
    if (!strcmp(interface_name, wifi_device)) {
        view = VIEW_NETWORKS;
        set_message(0, "SimpleNet is already using %s (%s).",
                    interface_name, card_name);
        return;
    }

    copy_text(wifi_device, sizeof(wifi_device), interface_name);
    backend = selected_backend;
    ap_count = 0;
    selected = 0;
    top = 0;
    set_message(0, "Switching SimpleNet to %s...", interface_name);
    draw();
    refresh_identity();
    scan_ok = scan_networks(0);
    refresh_freebsd_cards();
    for (int i = 0; i < wifi_card_count; i++)
        if (wifi_cards[i].system_default)
            copy_text(default_interface, sizeof(default_interface),
                      wifi_cards[i].interface_name);
    view = VIEW_NETWORKS;
    if (!scan_ok) {
        set_message(1, "SimpleNet selected %s, but its network scan failed.",
                    interface_name);
    } else if (default_interface[0] &&
               strcmp(default_interface, interface_name)) {
        set_message(0, "SimpleNet uses %s (%s); default remains %s until "
                    "you connect.", interface_name, card_name,
                    default_interface);
    } else {
        set_message(0, "SimpleNet now uses %s (%s).", interface_name,
                    card_name);
    }
}
#endif
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
#if defined(__FreeBSD__) || defined(SIMPLENET_TEST_SHARED_BACKENDS)
    int input_failed = 0;
#endif
    char buffer[1024];
    ssize_t count;

#ifdef __FreeBSD__
    if (output_size) output[0] = '\0';
#endif
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
#if defined(__FreeBSD__) || defined(SIMPLENET_TEST_SHARED_BACKENDS)
    if (secret) {
#else
    {
#endif
        size_t password_length = strlen(secret);
        size_t sent = 0;
        while (sent < password_length) {
            ssize_t written = write(input_pipe[1], secret + sent,
                                    password_length - sent);
            if (written < 0) {
                if (errno == EINTR) continue;
#if defined(__FreeBSD__) || defined(SIMPLENET_TEST_SHARED_BACKENDS)
                input_failed = 1;
#endif
                break;
            }
            sent += (size_t)written;
        }
#if defined(__FreeBSD__) || defined(SIMPLENET_TEST_SHARED_BACKENDS)
        if (!input_failed) {
            ssize_t written;

            do {
                written = write(input_pipe[1], "\n", 1);
            } while (written < 0 && errno == EINTR);
            if (written != 1)
                input_failed = 1;
        }
#else
        (void)write(input_pipe[1], "\n", 1);
#endif
    }
    close(input_pipe[1]);
#if defined(__FreeBSD__) || defined(SIMPLENET_TEST_SHARED_BACKENDS)
    if (secret) erase_secret(secret, secret_size);
#else
    erase_secret(secret, secret_size);
#endif

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
#if defined(__FreeBSD__) || defined(SIMPLENET_TEST_SHARED_BACKENDS)
    return !input_failed && WIFEXITED(status) && WEXITSTATUS(status) == 0;
#else
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
}

#ifdef __FreeBSD__
static void append_output(char *output, size_t output_size, size_t *used,
                          const char *buffer, size_t count)
{
    if (!output_size || !used || !buffer) return;
    if (*used + 1 < output_size) {
        size_t room = output_size - *used - 1;
        size_t copy = count < room ? count : room;
        memcpy(output + *used, buffer, copy);
        *used += copy;
        output[*used] = '\0';
    }
}

static int command_argv_input_timeout(char *const argv[], char *secret,
                                      size_t secret_size, char *output,
                                      size_t output_size, int timeout_ms)
{
    int input_pipe[2];
    int output_pipe[2];
    pid_t child;
    int status = -1;
    int flags;
    size_t used = 0;
    int64_t deadline;
    char buffer[1024];

    if (output_size) output[0] = '\0';
    if (pipe(input_pipe) != 0) {
        if (secret) erase_secret(secret, secret_size);
        return 0;
    }
    if (pipe(output_pipe) != 0) {
        close(input_pipe[0]);
        close(input_pipe[1]);
        if (secret) erase_secret(secret, secret_size);
        return 0;
    }
    child = fork();
    if (child < 0) {
        close(input_pipe[0]);
        close(input_pipe[1]);
        close(output_pipe[0]);
        close(output_pipe[1]);
        if (secret) erase_secret(secret, secret_size);
        return 0;
    }
    if (child == 0) {
        setpgid(0, 0);
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
    setpgid(child, child);

    close(input_pipe[0]);
    close(output_pipe[1]);
    if (secret) {
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
        erase_secret(secret, secret_size);
    }
    close(input_pipe[1]);

    flags = fcntl(output_pipe[0], F_GETFL, 0);
    if (flags >= 0)
        (void)fcntl(output_pipe[0], F_SETFL, flags | O_NONBLOCK);

    deadline = timeout_ms > 0 ? sui_monotonic_ms() + timeout_ms : 0;
    for (;;) {
        ssize_t count;
        while ((count = read(output_pipe[0], buffer, sizeof(buffer))) > 0)
            append_output(output, output_size, &used, buffer, (size_t)count);
        if (count == 0 && waitpid(child, &status, WNOHANG) == child)
            break;
        if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
            errno != EINTR)
            break;
        if (waitpid(child, &status, WNOHANG) == child) {
            while ((count = read(output_pipe[0], buffer, sizeof(buffer))) > 0)
                append_output(output, output_size, &used, buffer, (size_t)count);
            break;
        }
        if (deadline && sui_monotonic_ms() >= deadline) {
            kill(-child, SIGTERM);
            sui_sleep_ms(250);
            if (waitpid(child, &status, WNOHANG) != child) {
                kill(-child, SIGKILL);
                while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
            }
            append_output(output, output_size, &used, "Timed out.", 10);
            close(output_pipe[0]);
            return 0;
        }
        sui_sleep_ms(25);
    }
    close(output_pipe[0]);
    while (waitpid(child, &status, WNOHANG) == 0)
        sui_sleep_ms(10);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
#endif
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
#ifdef SIMPLENET_NATIVE_MACOS
    char interface_name[64];
    int powered = 0;

    return simplenet_macos_interface(interface_name, sizeof(interface_name),
                                     NULL, 0, bssid, size, &powered) &&
           powered && bssid[0];
#else
    char q_device[256];
    char command[MAX_CMD];
    if (!wifi_device[0]) return 0;
    shell_quote(wifi_device, q_device, sizeof(q_device));
#ifdef __FreeBSD__
    if (backend == BACKEND_WPA_SUPPLICANT) {
        return wpa_status_value("bssid", bssid, size);
    }
#endif
    snprintf(command, sizeof(command),
             "iw dev %s link 2>/dev/null | "
             "awk '/^Connected to / {print $3; exit}'", q_device);
    read_first_line(command, bssid, size);
    return bssid[0] != '\0';
#endif
}

#ifdef SIMPLENET_NATIVE_MACOS
static int wait_for_macos_bssid(const char *wanted, char *actual,
                                size_t actual_size, int timeout_ms)
{
    int waited = 0;
    const int step = 250;

    if (actual && actual_size)
        actual[0] = '\0';
    while (waited <= timeout_ms) {
        char observed[32] = "";

        if (current_bssid(observed, sizeof(observed))) {
            if (actual && actual_size)
                copy_text(actual, actual_size, observed);
            if (!strcasecmp(observed, wanted))
                return 1;
        }
        sui_sleep_ms(step);
        waited += step;
    }
    return 0;
}
#endif

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
#ifdef __FreeBSD__
static int wait_for_bssid(const char *wanted, int timeout_ms)
{
    char actual[32];
    int waited = 0;
    int step = 250;
    while (waited <= timeout_ms) {
        actual[0] = '\0';
        if (current_bssid(actual, sizeof(actual)) &&
            !strcasecmp(actual, wanted))
            return 1;
        sui_sleep_ms(step);
        waited += step;
    }
    return 0;
}
#endif

#if defined(__FreeBSD__) || defined(SIMPLENET_TEST_SHARED_BACKENDS)
static int freebsd_dhcp_state_needs_refresh(const char *current_ssid,
                                            const char *target_ssid,
                                            const char *selected_interface,
                                            const char *default_interface,
                                            int selected_has_ipv4)
{
    if (!target_ssid || !target_ssid[0] ||
        !selected_interface || !selected_interface[0])
        return 1;
    if (!current_ssid || !current_ssid[0] ||
        strcmp(current_ssid, target_ssid) != 0)
        return 1;
    if (!selected_has_ipv4 || !default_interface ||
        strcmp(default_interface, selected_interface) != 0)
        return 1;
    return 0;
}
#endif

#ifdef __FreeBSD__
typedef struct {
    int use_sudo_password;
    char sudo_password[256];
} FreebsdDhcpAuth;

static void freebsd_route_field(const char *route_output, const char *field,
                                char *value, size_t size)
{
    const char *line = route_output;
    size_t field_length = strlen(field);

    if (!value || !size) return;
    value[0] = '\0';
    while (line && *line) {
        const char *end = strchr(line, '\n');
        const char *start = line;

        while (*start == ' ' || *start == '\t') start++;
        if (!strncmp(start, field, field_length) &&
            start[field_length] == ':') {
            start += field_length + 1;
            while (*start == ' ' || *start == '\t') start++;
            if (!end) end = start + strlen(start);
            copy_span(value, size, start, end);
            trim(value);
            return;
        }
        line = end ? end + 1 : NULL;
    }
}

static int freebsd_route_lookup(const char *destination,
                                char *route_gateway, size_t gateway_size,
                                char *route_interface,
                                size_t interface_size)
{
    struct in_addr address;
    char command[MAX_CMD];
    char output[MAX_TEXT];
    char observed_gateway[128] = "";
    char observed_interface[64] = "";
    char quoted[256];

    if (!destination ||
        (strcmp(destination, "default") != 0 &&
         inet_pton(AF_INET, destination, &address) != 1))
        return 0;
    shell_quote(destination, quoted, sizeof(quoted));
    snprintf(command, sizeof(command),
             "route -n get -inet %s 2>/dev/null", quoted);
    if (command_output(command, output, sizeof(output))) {
        freebsd_route_field(output, "gateway", observed_gateway,
                            sizeof(observed_gateway));
        freebsd_route_field(output, "interface", observed_interface,
                            sizeof(observed_interface));
    }
    if (route_gateway && gateway_size)
        copy_text(route_gateway, gateway_size, observed_gateway);
    if (route_interface && interface_size)
        copy_text(route_interface, interface_size, observed_interface);
    return observed_interface[0] &&
           (strcmp(destination, "default") != 0 || observed_gateway[0]);
}

static int freebsd_default_route(char *route_gateway, size_t gateway_size,
                                 char *route_interface,
                                 size_t interface_size)
{
    return freebsd_route_lookup("default", route_gateway, gateway_size,
                                route_interface, interface_size);
}

static int freebsd_interface_ipv4(const char *interface_name,
                                   char *address, size_t size)
{
    char command[MAX_CMD];
    char quoted[256];

    if (address && size) address[0] = '\0';
    if (!address || !size ||
        !freebsd_device_name_valid(interface_name))
        return 0;
    shell_quote(interface_name, quoted, sizeof(quoted));
    snprintf(command, sizeof(command),
             "ifconfig %s inet 2>/dev/null | "
             "awk '$1 == \"inet\" {print $2; exit}'",
             quoted);
    read_first_line(command, address, size);
    return address[0] != '\0';
}

static int freebsd_interface_has_ipv4(const char *interface_name,
                                      const char *address)
{
    struct in_addr wanted;
    char command[MAX_CMD];
    char output[MAX_TEXT];
    char quoted[256];
    char *line;
    char *save = NULL;

    if (!freebsd_device_name_valid(interface_name) || !address ||
        inet_pton(AF_INET, address, &wanted) != 1)
        return 0;
    shell_quote(interface_name, quoted, sizeof(quoted));
    snprintf(command, sizeof(command), "ifconfig %s inet 2>/dev/null",
             quoted);
    if (!command_output(command, output, sizeof(output))) return 0;
    for (line = strtok_r(output, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        struct in_addr observed;
        const char *start;
        const char *end;
        char candidate[INET_ADDRSTRLEN];

        trim(line);
        if (strncmp(line, "inet ", 5)) continue;
        start = line + 5;
        while (*start && isspace((unsigned char)*start)) start++;
        end = start;
        while (*end && !isspace((unsigned char)*end)) end++;
        copy_span(candidate, sizeof(candidate), start, end);
        if (inet_pton(AF_INET, candidate, &observed) == 1 &&
            !memcmp(&wanted, &observed, sizeof(wanted)))
            return 1;
    }
    return 0;
}

static int freebsd_selected_route_ready(void)
{
    char route_gateway[128] = "";
    char route_interface[64] = "";
    char gateway_interface[64] = "";
    char address[64] = "";

    if (!freebsd_device_name_valid(wifi_device) ||
        !freebsd_default_route(route_gateway, sizeof(route_gateway),
                               route_interface,
                               sizeof(route_interface)) ||
        strcmp(route_interface, wifi_device) != 0)
        return 0;
    if (!freebsd_interface_ipv4(wifi_device, address, sizeof(address)))
        return 0;
    if (!freebsd_route_lookup(route_gateway, NULL, 0, gateway_interface,
                              sizeof(gateway_interface)))
        return 0;
    return strcmp(gateway_interface, wifi_device) == 0;
}

static int freebsd_card_has_disconnected_ipv4(const WifiCard *card,
                                              const char *selected_interface)
{
    return card && card->interface_name[0] && card->address[0] &&
           !card->associated &&
           (!selected_interface ||
            strcmp(card->interface_name, selected_interface) != 0);
}

static int freebsd_connection_needs_dhcp(const char *current,
                                         const char *target)
{
    char route_interface[64] = "";
    char address[64] = "";
    int has_route;
    int has_ipv4;

    has_route = freebsd_default_route(NULL, 0, route_interface,
                                      sizeof(route_interface));
    has_ipv4 = freebsd_interface_ipv4(wifi_device, address,
                                      sizeof(address));
    return freebsd_dhcp_state_needs_refresh(
        current, target, wifi_device,
        has_route ? route_interface : "", has_ipv4);
}

static int freebsd_privilege_prefix(char *prefix, size_t size)
{
    char output[64];
    char *const argv[] = {"sudo", "-n", "true", NULL};

    if (!prefix || !size) return 0;
    prefix[0] = '\0';
    if (geteuid() == 0) return 1;
    if (command_exists("sudo") &&
        command_argv_input_timeout(argv, NULL, 0, output, sizeof(output),
                                   5000)) {
        copy_text(prefix, size, "sudo -n ");
        return 1;
    }
    return 0;
}

static int freebsd_dhcp_restart_available(void)
{
    char prefix[32];
    return command_exists("service") &&
           freebsd_privilege_prefix(prefix, sizeof(prefix));
}

static void freebsd_clear_dhcp_auth(FreebsdDhcpAuth *auth)
{
    if (!auth) return;
    erase_secret(auth->sudo_password, sizeof(auth->sudo_password));
    auth->use_sudo_password = 0;
}

static int freebsd_prepare_dhcp_auth(FreebsdDhcpAuth *auth,
                                     int activation_needed,
                                     int allow_prompt)
{
    char verify_password[256];
    char output[MAX_TEXT];
    char *const argv[] = {"sudo", "-S", "-p", "", "-v", NULL};

    if (!auth) return 0;
    freebsd_clear_dhcp_auth(auth);
    if (!activation_needed)
        return 1;
    if (!command_exists("service") || !command_exists("route")) {
        set_message(1, "FreeBSD network activation needs service and route.");
        return 0;
    }
    if (freebsd_dhcp_restart_available()) return 1;
    if (!allow_prompt) {
        set_message(1, "FreeBSD needs root to activate the selected Wi-Fi route.");
        return 0;
    }
    if (!command_exists("sudo")) {
        set_message(1, "FreeBSD needs sudo to activate the selected Wi-Fi route.");
        return 0;
    }
    if (!hidden_prompt("Sudo password for DHCP and route update (Esc cancels): ",
                       auth->sudo_password, sizeof(auth->sudo_password), 1)) {
        set_message(0, "Connection cancelled.");
        return 0;
    }
    if (!auth->sudo_password[0]) {
        set_message(1, "Sudo password required to activate the selected Wi-Fi route.");
        return 0;
    }
    copy_text(verify_password, sizeof(verify_password), auth->sudo_password);
    if (!command_argv_input_timeout(argv, verify_password,
                                    sizeof(verify_password), output,
                                    sizeof(output), 10000)) {
        trim(output);
        freebsd_clear_dhcp_auth(auth);
        set_message(1, "%s", output[0] ? output :
                    "Could not authenticate sudo for DHCP renewal.");
        return 0;
    }
    auth->use_sudo_password = 1;
    return 1;
}

static int freebsd_run_privileged(char *const action_argv[],
                                  FreebsdDhcpAuth *auth,
                                  char *output, size_t output_size,
                                  int timeout_ms)
{
    char prefix[32];
    char secret[256] = "";
    char *sudo_argv[20];
    size_t action_count = 0;
    size_t sudo_count = 0;
    int use_password = 0;

    if (!action_argv || !action_argv[0]) return 0;
    if (geteuid() == 0)
        return command_argv_input_timeout(action_argv, NULL, 0, output,
                                          output_size, timeout_ms);
    while (action_argv[action_count]) action_count++;
    if (action_count + 5 > sizeof(sudo_argv) / sizeof(sudo_argv[0]))
        return 0;
    sudo_argv[sudo_count++] = "sudo";
    if (freebsd_privilege_prefix(prefix, sizeof(prefix))) {
        sudo_argv[sudo_count++] = "-n";
    } else if (auth && auth->use_sudo_password &&
               auth->sudo_password[0]) {
        sudo_argv[sudo_count++] = "-S";
        sudo_argv[sudo_count++] = "-p";
        sudo_argv[sudo_count++] = "";
        copy_text(secret, sizeof(secret), auth->sudo_password);
        use_password = 1;
    } else {
        return 0;
    }
    for (size_t i = 0; i < action_count; i++)
        sudo_argv[sudo_count++] = action_argv[i];
    sudo_argv[sudo_count] = NULL;
    return command_argv_input_timeout(
        sudo_argv, use_password ? secret : NULL,
        use_password ? sizeof(secret) : 0, output, output_size, timeout_ms);
}

static int freebsd_lease_text_has_address(const char *text,
                                          const char *address)
{
    static const char field[] = "fixed-address";
    struct in_addr wanted;
    const char *cursor;

    if (!text || !address || inet_pton(AF_INET, address, &wanted) != 1)
        return 0;
    cursor = text;
    while ((cursor = strstr(cursor, field)) != NULL) {
        const char *start;
        const char *end;
        struct in_addr observed;
        char candidate[INET_ADDRSTRLEN];

        if (cursor != text && !isspace((unsigned char)cursor[-1]) &&
            cursor[-1] != '{' && cursor[-1] != ';') {
            cursor += sizeof(field) - 1;
            continue;
        }
        start = cursor + sizeof(field) - 1;
        if (!isspace((unsigned char)*start)) {
            cursor = start;
            continue;
        }
        while (*start && isspace((unsigned char)*start)) start++;
        end = start;
        while (*end && !isspace((unsigned char)*end) && *end != ';' &&
               *end != '}')
            end++;
        copy_span(candidate, sizeof(candidate), start, end);
        if (inet_pton(AF_INET, candidate, &observed) == 1 &&
            !memcmp(&wanted, &observed, sizeof(wanted)))
            return 1;
        cursor = end;
    }
    return 0;
}

static int freebsd_dhcp_lease_owns_address(const char *interface_name,
                                           const char *address,
                                           FreebsdDhcpAuth *auth)
{
    char lease_path[PATH_MAX];
    char output[MAX_TEXT];
    char *cat_argv[] = {"cat", lease_path, NULL};
    int length;

    if (!freebsd_device_name_valid(interface_name) || !address ||
        !command_exists("cat"))
        return 0;
    length = snprintf(lease_path, sizeof(lease_path),
                      "/var/db/dhclient.leases.%s", interface_name);
    if (length < 0 || (size_t)length >= sizeof(lease_path)) return 0;
    if (!freebsd_run_privileged(cat_argv, auth, output, sizeof(output), 5000))
        return 0;
    return freebsd_lease_text_has_address(output, address);
}

static int freebsd_interface_associated(const char *interface_name)
{
    char command[MAX_CMD];
    char output[MAX_TEXT];
    char quoted[256];
    char *line;
    char *save = NULL;

    if (!freebsd_device_name_valid(interface_name)) return 0;
    shell_quote(interface_name, quoted, sizeof(quoted));
    snprintf(command, sizeof(command), "ifconfig %s 2>/dev/null", quoted);
    if (!command_output(command, output, sizeof(output))) return 0;
    for (line = strtok_r(output, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        trim(line);
        if (!strcmp(line, "status: associated")) return 1;
    }
    return 0;
}

static int freebsd_remove_disconnected_ipv4(const char *gateway,
                                            FreebsdDhcpAuth *auth)
{
    char output[MAX_TEXT];

    if (!gateway || !gateway[0]) return 1;
    refresh_freebsd_cards();
    for (int i = 0; i < wifi_card_count; i++) {
        WifiCard card = wifi_cards[i];
        char route_interface[64] = "";
        int dhclient_running;

        if (!freebsd_card_has_disconnected_ipv4(&card, wifi_device))
            continue;
        if (!freebsd_route_lookup(gateway, NULL, 0, route_interface,
                                  sizeof(route_interface)) ||
            strcmp(route_interface, card.interface_name))
            continue;
        if (freebsd_interface_associated(card.interface_name))
            continue;
        {
            char *const status_argv[] = {
                "service", "dhclient", "onestatus", card.interface_name,
                NULL
            };
            dhclient_running = command_argv_input_timeout(
                status_argv, NULL, 0, output, sizeof(output), 5000);
        }
        if (!dhclient_running ||
            !freebsd_dhcp_lease_owns_address(card.interface_name,
                                              card.address, auth))
            continue;
        if (freebsd_interface_associated(card.interface_name)) continue;
        {
            char *const stop_argv[] = {
                "service", "dhclient", "onestop", card.interface_name,
                NULL
            };
            output[0] = '\0';
            if (!freebsd_run_privileged(stop_argv, auth, output,
                                        sizeof(output), 10000)) {
                trim(output);
                set_message(1, "Could not stop stale DHCP on %s%s%s.",
                            card.interface_name, output[0] ? ": " : "",
                            output);
                return 0;
            }
        }
        if (freebsd_interface_associated(card.interface_name)) {
            char *const start_argv[] = {
                "service", "dhclient", "onestart", card.interface_name,
                NULL
            };
            (void)freebsd_run_privileged(start_argv, auth, output,
                                         sizeof(output), 10000);
            set_message(1, "%s reassociated during stale-route cleanup; "
                        "activation stopped.", card.interface_name);
            return 0;
        }
        if (freebsd_interface_has_ipv4(card.interface_name, card.address)) {
            char *const delete_argv[] = {
                "ifconfig", card.interface_name, "inet", card.address,
                "delete", NULL
            };
            output[0] = '\0';
            if (!freebsd_run_privileged(delete_argv, auth, output,
                                        sizeof(output), 10000)) {
                trim(output);
                set_message(1, "Could not remove stale IPv4 %s from %s%s%s.",
                            card.address, card.interface_name,
                            output[0] ? ": " : "", output);
                return 0;
            }
        }
        if (freebsd_interface_has_ipv4(card.interface_name, card.address)) {
            set_message(1, "Disconnected %s still owns IPv4 %s; route "
                        "activation stopped.", card.interface_name,
                        card.address);
            return 0;
        }
    }
    return 1;
}

static int wait_for_selected_route(int timeout_ms)
{
    int waited = 0;
    const int step = 250;

    while (waited <= timeout_ms) {
        if (freebsd_selected_route_ready()) return 1;
        sui_sleep_ms(step);
        waited += step;
    }
    return 0;
}

static int freebsd_restore_default_route(const char *old_gateway,
                                         const char *old_interface,
                                         FreebsdDhcpAuth *auth)
{
    char current_gateway[128] = "";
    char current_interface[64] = "";
    char output[MAX_TEXT];
    int current_route;

    if (!old_gateway || !old_gateway[0] ||
        !freebsd_device_name_valid(old_interface))
        return 0;
    current_route = freebsd_default_route(
        current_gateway, sizeof(current_gateway), current_interface,
        sizeof(current_interface));
    if (current_route && !strcmp(current_gateway, old_gateway) &&
        !strcmp(current_interface, old_interface))
        return 1;
    if (current_route) {
        char *const delete_argv[] = {
            "route", "-n", "delete", "default", current_gateway, NULL
        };
        if (strcmp(current_interface, wifi_device) != 0 ||
            !freebsd_run_privileged(delete_argv, auth, output,
                                    sizeof(output), 10000))
            return 0;
    }
    {
        char *const add_argv[] = {
            "route", "-n", "add", "default", (char *)old_gateway,
            "-ifp", (char *)old_interface, NULL
        };
        if (!freebsd_run_privileged(add_argv, auth, output,
                                    sizeof(output), 10000))
            return 0;
    }
    if (!freebsd_default_route(current_gateway, sizeof(current_gateway),
                               current_interface,
                               sizeof(current_interface)))
        return 0;
    return !strcmp(current_gateway, old_gateway) &&
           !strcmp(current_interface, old_interface);
}

static int renew_freebsd_dhcp(const char *ssid, FreebsdDhcpAuth *auth,
                              int force_refresh)
{
    char old_gateway[128] = "";
    char old_interface[64] = "";
    char current_gateway[128] = "";
    char current_interface[64] = "";
    char output[MAX_TEXT] = "";
    char route_output[MAX_TEXT] = "";
    int had_old_route;
    int removed_old_route = 0;
    int dhcp_ok;
    int restored = 0;

    if (!freebsd_device_name_valid(wifi_device) ||
        !command_exists("ifconfig") || !command_exists("service") ||
        !command_exists("route"))
        return 0;
    if (!force_refresh && freebsd_selected_route_ready()) return 1;
    had_old_route = freebsd_default_route(
        old_gateway, sizeof(old_gateway), old_interface,
        sizeof(old_interface));
    if (!freebsd_remove_disconnected_ipv4(
            had_old_route ? old_gateway : NULL, auth))
        return 0;
    if (had_old_route && strcmp(old_interface, wifi_device) != 0) {
        char *const delete_argv[] = {
            "route", "-n", "delete", "default", old_gateway, NULL
        };

        set_message(0, "Moving the default route from %s to %s...",
                    old_interface, wifi_device);
        draw();
        if (freebsd_run_privileged(delete_argv, auth, route_output,
                                   sizeof(route_output), 10000)) {
            removed_old_route = 1;
        } else {
            current_gateway[0] = current_interface[0] = '\0';
            if (freebsd_default_route(
                    current_gateway, sizeof(current_gateway),
                    current_interface, sizeof(current_interface))) {
                if (freebsd_selected_route_ready()) return 1;
                trim(route_output);
                set_message(1, "%s", route_output[0] ? route_output :
                            "Could not release the previous default route.");
                return 0;
            }
            removed_old_route = 1;
        }
    }
    route_output[0] = '\0';

    set_message(0, "Activating DHCP and the default route on %s...",
                wifi_device);
    draw();
    {
        char *const service_argv[] = {
            "service", "dhclient", "onerestart", wifi_device, NULL
        };
        dhcp_ok = freebsd_run_privileged(service_argv, auth, output,
                                         sizeof(output), 45000);
    }
    if (dhcp_ok && wait_for_selected_route(5000)) return 1;

    current_gateway[0] = current_interface[0] = '\0';
    if (dhcp_ok &&
        freebsd_default_route(current_gateway, sizeof(current_gateway),
                              current_interface,
                              sizeof(current_interface)) &&
        strcmp(current_interface, wifi_device) != 0) {
        char *const change_argv[] = {
            "route", "-n", "change", "default", current_gateway,
            "-ifp", wifi_device, NULL
        };
        if (freebsd_run_privileged(change_argv, auth, route_output,
                                   sizeof(route_output), 10000) &&
            wait_for_selected_route(2000))
            return 1;
    }

    if (removed_old_route)
        restored = freebsd_restore_default_route(old_gateway, old_interface,
                                                 auth);
    trim(output);
    trim(route_output);
    if (!dhcp_ok && output[0]) {
        set_message(1, "%s%s", output,
                    restored ? " (previous route restored)" : "");
    } else {
        set_message(1,
                    "Associated with %s, but %s did not become the usable "
                    "default route%s%s%s.",
                    ssid, wifi_device,
                    restored ? "; previous route restored" : "",
                    route_output[0] ? ": " : "", route_output);
    }
    return 0;
}
#endif

#if defined(__FreeBSD__) || defined(SIMPLENET_TEST_SHARED_BACKENDS)
static int command_argv_value_equals(char *const argv[], const char *expected,
                                     char *error, size_t error_size)
{
    char output[MAX_TEXT];

    if (!command_argv_input(argv, NULL, 0, output, sizeof(output))) {
        trim(output);
        if (error && error_size)
            snprintf(error, error_size, "%s",
                     output[0] ? output : "Network preference query failed.");
        return 0;
    }
    trim(output);
    if (strcmp(output, expected) != 0) {
        if (error && error_size)
            snprintf(error, error_size,
                     "Expected %.1024s, but the manager reported %.1024s.",
                     expected, output[0] ? output : "an empty value");
        return 0;
    }
    return 1;
}

static int networkmanager_prefer_current(char *error, size_t error_size)
{
    char output[MAX_TEXT];
    char *const modify_argv[] = {
        "nmcli", "-w", "20", "connection", "modify", "uuid",
        connection_uuid, "connection.autoconnect", "yes",
        "connection.autoconnect-priority", PREFERRED_AUTOCONNECT_PRIORITY,
        NULL
    };
    char *const autoconnect_argv[] = {
        "nmcli", "-g", "connection.autoconnect", "connection", "show",
        "uuid", connection_uuid, NULL
    };
    char *const priority_argv[] = {
        "nmcli", "-g", "connection.autoconnect-priority", "connection",
        "show", "uuid", connection_uuid, NULL
    };

    if (error && error_size) error[0] = '\0';
    if (!connection_uuid[0]) {
        if (error && error_size)
            snprintf(error, error_size,
                     "NetworkManager did not expose the active connection profile.");
        return 0;
    }
    if (!command_argv_input(modify_argv, NULL, 0, output, sizeof(output))) {
        trim(output);
        if (error && error_size)
            snprintf(error, error_size, "%s", output[0] ? output :
                     "NetworkManager could not save the boot preference.");
        return 0;
    }
    if (!command_argv_value_equals(autoconnect_argv, "yes", error,
                                   error_size) ||
        !command_argv_value_equals(priority_argv,
                                   PREFERRED_AUTOCONNECT_PRIORITY, error,
                                   error_size))
        return 0;
    return 1;
}

static int iwd_prefer_network(const char *ssid, char *error,
                              size_t error_size)
{
    char output[MAX_TEXT];
    char *const set_argv[] = {
        "iwctl", "known-networks", (char *)ssid, "set-property",
        "AutoConnect", "yes", NULL
    };
    char *const show_argv[] = {
        "iwctl", "known-networks", (char *)ssid, "show", NULL
    };

    if (error && error_size) error[0] = '\0';
    if (!ssid || !ssid[0]) {
        if (error && error_size)
            snprintf(error, error_size, "iwd did not receive a network name.");
        return 0;
    }
    if (!command_argv_input(set_argv, NULL, 0, output, sizeof(output))) {
        trim(output);
        if (error && error_size)
            snprintf(error, error_size, "%s", output[0] ? output :
                     "iwd could not enable automatic reconnection.");
        return 0;
    }
    if (!command_argv_input(show_argv, NULL, 0, output, sizeof(output))) {
        trim(output);
        if (error && error_size)
            snprintf(error, error_size, "%s", output[0] ? output :
                     "iwd could not verify automatic reconnection.");
        return 0;
    }
    if (!strstr(output, "AutoConnect") || !strstr(output, "yes")) {
        if (error && error_size)
            snprintf(error, error_size,
                     "iwd did not confirm automatic reconnection for %s.", ssid);
        return 0;
    }
    return 1;
}
#endif

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

#ifdef __FreeBSD__
static int networkmanager_finish_preference(const char *ssid,
                                            const char *bssid)
{
    char error[MAX_TEXT];

    refresh_identity();
    if (!networkmanager_prefer_current(error, sizeof(error))) {
        set_message(1, "Connected %s through %s, but it is not saved as the "
                    "boot preference: %s", ssid, bssid,
                    error[0] ? error : "NetworkManager rejected the change.");
        return 0;
    }
    set_message(0, "Connected %s through mesh node %s; preferred after reboot.",
                ssid, bssid);
    return 1;
}
#endif

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
#ifdef __FreeBSD__
        (void)networkmanager_finish_preference(chosen_ssid, chosen_bssid);
#else
        set_message(0, "Already connected through mesh node %s.", ap->bssid);
#endif
        return;
    }
    for (int i = 0; i < ap_count; i++)
        if (aps[i].active && !strcmp(aps[i].ssid, ap->ssid))
            same_network = 1;
    if (same_network) {
        if (enforce_selected_bssid(ap)) {
            refresh_active_marker();
#ifdef __FreeBSD__
            (void)networkmanager_finish_preference(chosen_ssid, chosen_bssid);
#else
            set_message(0, "Pinned %s to mesh node %s.", chosen_ssid, chosen_bssid);
#endif
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
#ifdef __FreeBSD__
            (void)networkmanager_finish_preference(chosen_ssid, chosen_bssid);
#else
            set_message(0, "Connected %s through mesh node %s.",
                        chosen_ssid, chosen_bssid);
#endif
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
#ifdef __FreeBSD__
            (void)networkmanager_finish_preference(chosen_ssid, chosen_bssid);
#else
            set_message(0, "Connected %s through mesh node %s.",
                        chosen_ssid, chosen_bssid);
#endif
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
#ifdef __FreeBSD__
    char connected_ssid[128] = "";
    char error[MAX_TEXT] = "";
#endif
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
#ifdef __FreeBSD__
    if (!active_ssid(connected_ssid, sizeof(connected_ssid)) ||
        strcmp(connected_ssid, ap->ssid) != 0) {
        set_message(1, "iwd did not finish connecting to %s.", ap->ssid);
        return;
    }
    if (!iwd_prefer_network(ap->ssid, error, sizeof(error))) {
        set_message(1, "Connected %s, but it is not saved for automatic "
                    "reconnection: %s", ap->ssid,
                    error[0] ? error : "iwd rejected the change.");
        return;
    }
    set_message(0, "Connected %s; preferred after reboot.", ap->ssid);
#endif
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

#if defined(__FreeBSD__) || defined(SIMPLENET_TEST_SHARED_BACKENDS)
static int wpa_network_id_valid(const char *id)
{
    if (!id || !id[0]) return 0;
    for (size_t i = 0; id[i]; i++)
        if (!isdigit((unsigned char)id[i])) return 0;
    return 1;
}

static int wpa_command_ok(char *const argv[], char *detail,
                          size_t detail_size)
{
    char output[MAX_TEXT];
    int ran = command_argv_input(argv, NULL, 0, output, sizeof(output));

    trim(output);
    if (detail && detail_size)
        snprintf(detail, detail_size, "%s", output);
    return ran && strcmp(output, "OK") == 0;
}

static int wpa_network_priority(const char *id, int *priority)
{
    char output[MAX_TEXT];
    char *end = NULL;
    long parsed;
    char *const argv[] = {
        "wpa_cli", "-i", wifi_device, "get_network", (char *)id,
        "priority", NULL
    };

    if (!priority || !wpa_network_id_valid(id) ||
        !command_argv_input(argv, NULL, 0, output, sizeof(output)))
        return 0;
    trim(output);
    errno = 0;
    parsed = strtol(output, &end, 10);
    if (errno || end == output || *end || parsed < INT_MIN || parsed > INT_MAX)
        return 0;
    *priority = (int)parsed;
    return 1;
}

static int wpa_highest_other_priority(const char *selected_id,
                                      int *highest, int *found)
{
    char output[MAX_TEXT];
    char *line;
    char *save = NULL;
    char *const argv[] = {
        "wpa_cli", "-i", wifi_device, "list_networks", NULL
    };

    if (!highest || !found || !wpa_network_id_valid(selected_id) ||
        !command_argv_input(argv, NULL, 0, output, sizeof(output)))
        return 0;
    *highest = INT_MIN;
    *found = 0;
    for (line = strtok_r(output, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        char id[32];
        char *tab = strchr(line, '\t');
        size_t length;
        int priority;

        if (!tab) continue;
        length = (size_t)(tab - line);
        if (!length || length >= sizeof(id)) continue;
        memcpy(id, line, length);
        id[length] = '\0';
        if (!wpa_network_id_valid(id) || strcmp(id, selected_id) == 0)
            continue;
        if (!wpa_network_priority(id, &priority)) return 0;
        if (!*found || priority > *highest) *highest = priority;
        *found = 1;
    }
    return 1;
}

static int wpa_set_priority(const char *id, int priority,
                            char *detail, size_t detail_size)
{
    char value[32];
    char *argv[] = {
        "wpa_cli", "-i", wifi_device, "set_network", (char *)id,
        "priority", value, NULL
    };

    snprintf(value, sizeof(value), "%d", priority);
    return wpa_command_ok(argv, detail, detail_size);
}

static int wpa_enable_all(char *detail, size_t detail_size)
{
    char *const argv[] = {
        "wpa_cli", "-i", wifi_device, "enable_network", "all", NULL
    };
    return wpa_command_ok(argv, detail, detail_size);
}

static int wpa_save_config(char *detail, size_t detail_size)
{
    char *const argv[] = {
        "wpa_cli", "-i", wifi_device, "save_config", NULL
    };
    return wpa_command_ok(argv, detail, detail_size);
}

static int wpa_global_value(const char *name, char *value, size_t value_size)
{
    char output[MAX_TEXT];
    char *const argv[] = {
        "wpa_cli", "-i", wifi_device, "get", (char *)name, NULL
    };

    if (!name || !name[0] || !value || !value_size ||
        !command_argv_input(argv, NULL, 0, output, sizeof(output)))
        return 0;
    trim(output);
    if (!output[0] || !strcmp(output, "FAIL")) return 0;
    copy_text(value, value_size, output);
    return 1;
}

static int wpa_set_global(const char *name, const char *value,
                          char *detail, size_t detail_size)
{
    char *const argv[] = {
        "wpa_cli", "-i", wifi_device, "set", (char *)name,
        (char *)value, NULL
    };

    return wpa_command_ok(argv, detail, detail_size);
}

static int wpa_prepare_persistence(char *error, size_t error_size)
{
    char initial_detail[MAX_TEXT] = "";
    char detail[MAX_TEXT] = "";
    char update_config[32] = "";

    if (error && error_size) error[0] = '\0';
    if (wpa_save_config(initial_detail, sizeof(initial_detail))) return 1;
    if (!wpa_global_value("update_config", update_config,
                          sizeof(update_config)) ||
        strcmp(update_config, "0") != 0) {
        if (error && error_size)
            snprintf(error, error_size,
                     "wpa_supplicant refused to save its configuration%s%s.",
                     initial_detail[0] ? ": " : "", initial_detail);
        return 0;
    }
    if (!wpa_set_global("update_config", "1", detail, sizeof(detail))) {
        if (error && error_size)
            snprintf(error, error_size,
                     "wpa_supplicant will not enable persistent configuration%s%s.",
                     detail[0] ? ": " : "", detail);
        return 0;
    }
    if (!wpa_global_value("update_config", update_config,
                          sizeof(update_config)) ||
        strcmp(update_config, "1") != 0) {
        (void)wpa_set_global("update_config", "0", NULL, 0);
        if (error && error_size)
            snprintf(error, error_size,
                     "wpa_supplicant did not retain persistent configuration mode.");
        return 0;
    }
    if (!wpa_save_config(detail, sizeof(detail))) {
        (void)wpa_set_global("update_config", "0", NULL, 0);
        if (error && error_size)
            snprintf(error, error_size,
                     "wpa_supplicant enabled persistence but could not write its "
                     "configuration%s%s.", detail[0] ? ": " : "", detail);
        return 0;
    }
    return 1;
}

static int wpa_prefer_network(const char *id, char *error, size_t error_size)
{
    int previous;
    int highest;
    int found;
    int preferred;
    int verified;
    int changed = 0;
    char detail[MAX_TEXT] = "";

    if (error && error_size) error[0] = '\0';
    if (!wpa_network_id_valid(id) || !wpa_network_priority(id, &previous) ||
        !wpa_highest_other_priority(id, &highest, &found)) {
        if (error && error_size)
            snprintf(error, error_size,
                     "wpa_supplicant could not read the saved network priorities.");
        return 0;
    }
    preferred = previous;
    if (found && previous <= highest) {
        if (highest == INT_MAX) {
            if (error && error_size)
                snprintf(error, error_size,
                         "another wpa_supplicant profile already has the maximum priority.");
            return 0;
        }
        preferred = highest + 1;
    }
    if (preferred != previous) {
        if (!wpa_set_priority(id, preferred, detail, sizeof(detail))) {
            if (error && error_size)
                snprintf(error, error_size, "%s", detail[0] ? detail :
                         "wpa_supplicant rejected the preferred priority.");
            return 0;
        }
        changed = 1;
    }
    if (!wpa_enable_all(detail, sizeof(detail))) {
        if (changed) (void)wpa_set_priority(id, previous, NULL, 0);
        if (error && error_size)
            snprintf(error, error_size, "%s", detail[0] ? detail :
                     "wpa_supplicant could not retain fallback networks.");
        return 0;
    }
    if (!wpa_save_config(detail, sizeof(detail))) {
        if (changed) (void)wpa_set_priority(id, previous, NULL, 0);
        (void)wpa_enable_all(NULL, 0);
        if (error && error_size)
            snprintf(error, error_size,
                     "wpa_supplicant refused to save its configuration%s%s.",
                     detail[0] ? ": " : "", detail);
        return 0;
    }
    if (!wpa_network_priority(id, &verified) || verified != preferred) {
        if (changed) (void)wpa_set_priority(id, previous, NULL, 0);
        (void)wpa_enable_all(NULL, 0);
        (void)wpa_save_config(NULL, 0);
        if (error && error_size)
            snprintf(error, error_size,
                     "wpa_supplicant did not retain the preferred priority.");
        return 0;
    }
    return 1;
}
#endif

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
#ifdef __FreeBSD__
    snprintf(command, sizeof(command),
             "[ \"$(wpa_cli -i %s bssid %s %s 2>/dev/null | tail -n1)\" = OK ] && "
             "wpa_cli -i %s select_network %s 2>&1 && "
             "wpa_cli -i %s reassociate 2>&1",
             q_device, q_id, q_bssid, q_device, q_id, q_device);
#else
    snprintf(command, sizeof(command),
             "[ \"$(wpa_cli -i %s bssid %s %s 2>/dev/null | tail -n1)\" = OK ] && "
             "wpa_cli -i %s select_network %s 2>&1",
             q_device, q_id, q_bssid, q_device, q_id);
#endif
    return command_output(command, output, sizeof(output)) &&
           !strstr(output, "FAIL");
}

#ifndef __FreeBSD__
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
#else
static void connect_selected_wpa(void)
{
    AccessPoint target;
    AccessPoint *ap = &target;
    char id[32], password[256] = "", output[MAX_TEXT], command[MAX_CMD];
    char q_device[256], q_id[128];
    char previous_ssid[128] = "";
    char preference_error[MAX_TEXT] = "";
    int secured;
    int force_dhcp;
    FreebsdDhcpAuth dhcp_auth = {0};

    target = aps[selected];
    if (!wpa_prepare_persistence(output, sizeof(output))) {
        set_message(1, "Connection not attempted: %s", output[0] ? output :
                    "wpa_supplicant cannot save reboot-safe changes.");
        goto cleanup;
    }
    if (ap->hidden_ssid) {
        if (!hidden_prompt("SSID (Esc cancels): ", ap->ssid,
                           sizeof(ap->ssid), 0)) {
            set_message(0, "Connection cancelled.");
            return;
        }
        trim(ap->ssid);
        if (!ap->ssid[0]) {
            set_message(1, "Hidden networks need an SSID.");
            return;
        }
        ap->hidden_ssid = 0;
    }
    secured = strcmp(ap->security, "open") != 0;
    active_ssid(previous_ssid, sizeof(previous_ssid));
    force_dhcp = freebsd_connection_needs_dhcp(previous_ssid, ap->ssid);
    if (!freebsd_prepare_dhcp_auth(&dhcp_auth, force_dhcp, 1))
        goto cleanup;
    if (wpa_network_id(ap->ssid, id, sizeof(id))) {
        set_message(0, "Associating with %s through mesh node %s...",
                    ap->ssid, ap->bssid);
        draw();
        if (!wpa_select_network(id, ap->bssid)) {
            set_message(1, "wpa_supplicant could not activate the saved network.");
            goto cleanup;
        }
        if (!wait_for_bssid(ap->bssid, 8000)) {
            refresh_active_marker();
            set_message(1, "wpa_supplicant did not reassociate with mesh node %s.",
                        ap->bssid);
            goto cleanup;
        }
        refresh_active_marker();
        if (!wpa_prefer_network(id, preference_error,
                                sizeof(preference_error))) {
            set_message(1, "Connected %s, but it is not saved as the boot "
                        "preference: %s", ap->ssid, preference_error);
            goto cleanup;
        }
        if (!wait_for_bssid(ap->bssid, 3000)) {
            refresh_active_marker();
            set_message(1, "Saving fallback networks moved the connection "
                        "away from mesh node %s.", ap->bssid);
            goto cleanup;
        }
        if (!renew_freebsd_dhcp(ap->ssid, &dhcp_auth, force_dhcp))
            goto cleanup;
        refresh_active_marker();
        set_message(0, "Connected %s through mesh node %s; preferred after reboot.",
                    ap->ssid, ap->bssid);
        goto cleanup;
    }
    if (secured && !hidden_prompt("WPA passphrase (Esc cancels): ", password,
                                  sizeof(password), 1)) {
        set_message(0, "Connection cancelled.");
        goto cleanup;
    }
    set_message(0, "Configuring wpa_supplicant for %s...", ap->ssid);
    draw();
    shell_quote(wifi_device, q_device, sizeof(q_device));
    snprintf(command, sizeof(command),
             "wpa_cli -i %s add_network 2>/dev/null", q_device);
    if (!command_output(command, output, sizeof(output))) {
        erase_secret(password, sizeof(password));
        set_message(1, "wpa_supplicant could not create a network profile.");
        goto cleanup;
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
        goto cleanup;
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
            goto cleanup;
        }
    }
    if (!wait_for_bssid(ap->bssid, 8000)) {
        refresh_active_marker();
        set_message(1, "wpa_supplicant created the profile but did not associate with %s.",
                    ap->bssid);
        goto cleanup;
    }
    refresh_active_marker();
    if (!wpa_prefer_network(id, preference_error, sizeof(preference_error))) {
        set_message(1, "Connected %s, but it is not saved as the boot "
                    "preference: %s", ap->ssid, preference_error);
        goto cleanup;
    }
    if (!wait_for_bssid(ap->bssid, 3000)) {
        refresh_active_marker();
        set_message(1, "Saving fallback networks moved the connection away "
                    "from mesh node %s.", ap->bssid);
        goto cleanup;
    }
    if (!renew_freebsd_dhcp(ap->ssid, &dhcp_auth, force_dhcp))
        goto cleanup;
    refresh_active_marker();
    set_message(0, "Connected %s through mesh node %s; preferred after reboot.",
                ap->ssid, ap->bssid);

cleanup:
    erase_secret(password, sizeof(password));
    freebsd_clear_dhcp_auth(&dhcp_auth);
}
#endif

#ifdef SIMPLENET_NATIVE_MACOS
static void connect_selected_macos(void)
{
    AccessPoint target;
    char password[256] = "";
    char error[MAX_TEXT] = "";
    char actual_bssid[32] = "";
    int result;
    int preference_saved;

    if (!ap_count)
        return;
    target = aps[selected];
    if (current_bssid(actual_bssid, sizeof(actual_bssid)) &&
        !strcasecmp(actual_bssid, target.bssid)) {
        if (!simplenet_macos_prefer_network(target.ssid, target.bssid,
                                            error, sizeof(error))) {
            set_message(1, "Connected %s, but macOS did not save it as the "
                        "boot preference: %s", target.ssid,
                        error[0] ? error : "configuration commit failed.");
            return;
        }
        set_message(0, "Connected %s; preferred after reboot.", target.ssid);
        return;
    }
    set_message(0, "Connecting %s through %s with saved credentials...",
                target.ssid, target.bssid);
    draw();
    result = simplenet_macos_connect(target.ssid, target.bssid, NULL, 1,
                                     error, sizeof(error));
    if (result == SIMPLENET_MACOS_PASSWORD_REQUIRED) {
        if (!hidden_prompt("Password (Esc cancels): ", password,
                           sizeof(password), 1)) {
            set_message(0, "Connection cancelled.");
            return;
        }
        set_message(0, "Associating through CoreWLAN...");
        draw();
        result = simplenet_macos_connect(target.ssid, target.bssid,
                                         password, 0, error, sizeof(error));
    }
    erase_secret(password, sizeof(password));
    if (result == SIMPLENET_MACOS_ENTERPRISE_UNSUPPORTED) {
        set_message(1, "%s Use macOS Wi-Fi settings for 802.1X networks.",
                    error[0] ? error : "Enterprise association is system-managed.");
        return;
    }
    if (result != SIMPLENET_MACOS_CONNECT_OK &&
        result != SIMPLENET_MACOS_CONNECTED_NOT_SAVED) {
        set_message(1, "%s", error[0] ? error : "CoreWLAN association failed.");
        return;
    }
    preference_saved = result == SIMPLENET_MACOS_CONNECT_OK;
    if (!wait_for_macos_bssid(target.bssid, actual_bssid,
                              sizeof(actual_bssid), 5000)) {
        refresh_active_marker();
        set_message(1, "macOS associated with %s instead of selected node %s.",
                    actual_bssid[0] ? actual_bssid : "another node",
                    target.bssid);
        return;
    }
    refresh_active_marker();
    if (!preference_saved) {
        set_message(1, "Connected %s, but macOS did not save it as the boot "
                    "preference: %s", target.ssid,
                    error[0] ? error : "configuration commit failed.");
        return;
    }
    set_message(0, "Connected %s through mesh node %s; preferred after reboot "
                "(macOS may roam later).", target.ssid, target.bssid);
}
#endif

static void connect_selected(void)
{
    if (!ap_count) return;
    switch (backend) {
        case BACKEND_NETWORKMANAGER: connect_selected_networkmanager(); break;
        case BACKEND_IWD: connect_selected_iwd(); break;
        case BACKEND_WPA_SUPPLICANT: connect_selected_wpa(); break;
#ifdef SIMPLENET_NATIVE_MACOS
        case BACKEND_COREWLAN: connect_selected_macos(); break;
#endif
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
#if defined(__FreeBSD__) || defined(__APPLE__)
    snprintf(command, sizeof(command),
             "(ping -n -c %d -i 1 -W 2000 %s 2>/dev/null || true)",
             count, q_host);
#else
    snprintf(command, sizeof(command),
             "(ping -n -c %d -i 0.2 -W 2 %s 2>/dev/null || true)",
             count, q_host);
#endif
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
#ifdef __FreeBSD__
            snprintf(active_ssid, sizeof(active_ssid), "%s",
                     ap_ssid_label(&aps[i]));
#else
            snprintf(active_ssid, sizeof(active_ssid), "%s", aps[i].ssid);
#endif
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
#ifdef __FreeBSD__
    if (backend == BACKEND_WPA_SUPPLICANT &&
        wpa_status_value("ssid", ssid, size))
        return 1;
#endif
    for (int i = 0; i < ap_count; i++) {
        if (aps[i].active) {
            snprintf(ssid, size, "%s", aps[i].ssid);
            return 1;
        }
    }
    return 0;
}

#ifndef SIMPLENET_NATIVE_MACOS
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
#endif

static int configured_bssid(char *bssid, size_t size)
{
#ifdef SIMPLENET_NATIVE_MACOS
    return current_bssid(bssid, size);
#else
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
#endif
}

static int pin_bssid(const char *bssid)
{
#ifdef SIMPLENET_NATIVE_MACOS
    char error[MAX_TEXT] = "";
    int result;

    if (!bssid || !bssid[0])
        return 1;
    for (int i = 0; i < ap_count; i++) {
        if (strcasecmp(aps[i].bssid, bssid))
            continue;
        result = simplenet_macos_connect(aps[i].ssid, aps[i].bssid,
                                         NULL, 1, error, sizeof(error));
        if (result == SIMPLENET_MACOS_CONNECT_OK)
            return 1;
        set_message(1, "%s", result == SIMPLENET_MACOS_PASSWORD_REQUIRED
                    ? "The saved Wi-Fi password is unavailable; select the node and press Enter."
                    : (error[0] ? error : "CoreWLAN association failed."));
        return 0;
    }
    set_message(1, "The requested mesh node is no longer visible.");
    return 0;
#else
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
#endif
}

static int restore_bssid(const char *bssid)
{
    if (backend == BACKEND_IWD && (!bssid || !bssid[0])) return 1;
    return pin_bssid(bssid ? bssid : "");
}

static void unpin(void)
{
#ifdef SIMPLENET_NATIVE_MACOS
    set_message(0, "CoreWLAN node choices are temporary; macOS roaming is already automatic.");
    return;
#else
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
#endif
}

static void disable_powersave(void)
{
#ifdef SIMPLENET_NATIVE_MACOS
    set_message(0, "macOS manages Wi-Fi power policy; no supported per-network override exists.");
    return;
#else
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
#ifdef __FreeBSD__
        snprintf(command, sizeof(command),
                 "sudo ifconfig %s -powersave", q_device);
#else
        snprintf(command, sizeof(command),
                 "sudo iw dev %s set power_save off", q_device);
#endif
        status = system(command);
    }
    puts(status == 0 ? "\nDone." : "\nCould not change kernel power saving.");
    puts("Press Enter to return to simplenet.");
    while (getchar() != '\n' && !feof(stdin)) {}
    reset_prog_mode();
    refresh();
    set_message(status != 0, status == 0 ? "Power-saving action complete." :
                "Power-saving action failed.");
#endif
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
#ifdef SIMPLENET_NATIVE_MACOS
        (backend != BACKEND_IWD && backend != BACKEND_COREWLAN &&
         !connection_uuid[0])) {
#else
        (backend != BACKEND_IWD && !connection_uuid[0])) {
#endif
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
#ifdef SIMPLENET_NATIVE_MACOS
    if (backend == BACKEND_COREWLAN) {
        if (!wait_for_macos_bssid(best_bssid, actual_bssid,
                                  sizeof(actual_bssid), 5000)) {
            restore_bssid(original_bssid);
            refresh_active_marker();
            set_message(1, "macOS chose %s instead of %s; restored the previous node.",
                        actual_bssid[0] ? actual_bssid : "another node",
                        best_bssid);
            return;
        }
    } else {
#endif
    sui_sleep_ms(1200);
    if (!current_bssid(actual_bssid, sizeof(actual_bssid)) ||
        strcasecmp(actual_bssid, best_bssid)) {
        restore_bssid(original_bssid);
        refresh_active_marker();
        set_message(1, "Network manager chose %s instead of %s; restored the previous setting.",
                    actual_bssid[0] ? actual_bssid : "another node", best_bssid);
        return;
    }
#ifdef SIMPLENET_NATIVE_MACOS
    }
#endif
    refresh_active_marker();
    if (backend == BACKEND_IWD)
        set_message(0, "Selected strongest %s node: %s at %d%% (iwd may roam later).",
                    ssid, best_bssid, best_signal);
#ifdef SIMPLENET_NATIVE_MACOS
    else if (backend == BACKEND_COREWLAN)
        set_message(0, "Selected strongest %s node: %s at %d%% (macOS may roam later).",
                    ssid, best_bssid, best_signal);
#endif
    else
        set_message(0, "Pinned strongest %s node: %s at %d%%.",
                    ssid, best_bssid, best_signal);
}

#ifndef SIMPLENET_NATIVE_MACOS
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
#endif

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

#ifndef SIMPLENET_NATIVE_MACOS
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
#endif

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
#ifdef __FreeBSD__
            mvprintw(row, 2, "%-2s %-28.28s %-17s %4s %5d%% %7s  %-20.20s",
                     ap->active ? "●" : "", ap_ssid_label(ap), ap->bssid,
                     band_name(ap->frequency), ap->signal,
                     latency, ap->security);
#else
            mvprintw(row, 2, "%-2s %-28.28s %-17s %4s %5d%% %7s  %-20.20s",
                     ap->active ? "●" : "", ap->ssid, ap->bssid,
                     band_name(ap->frequency), ap->signal,
                     latency, ap->security);
#endif
        else
#ifdef __FreeBSD__
            mvprintw(row, 2, "%-2s %-22.22s %-17s %4s %5d%%  %-12.12s",
                     ap->active ? "●" : "", ap_ssid_label(ap), ap->bssid,
                     band_name(ap->frequency), ap->signal, ap->security);
#else
            mvprintw(row, 2, "%-2s %-22.22s %-17s %4s %5d%%  %-12.12s",
                     ap->active ? "●" : "", ap->ssid, ap->bssid,
                     band_name(ap->frequency), ap->signal, ap->security);
#endif
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
#ifdef __FreeBSD__
    mvprintw(6, 2, "SSID             %s", ap_ssid_label(ap));
#else
    mvprintw(6, 2, "SSID             %s", ap->ssid);
#endif
    mvprintw(7, 2, "mesh node        %s", ap->bssid);
    mvprintw(8, 2, "channel          %d  (%d MHz / %s GHz)",
             ap->channel, ap->frequency, band_name(ap->frequency));
    mvprintw(9, 2, "signal           %d%%", ap->signal);
    mvprintw(10, 2, "security         %s", ap->security);
    mvprintw(11, 2, "active           %s", ap->active ? "yes" : "no");
    if (ap->tested) mvprintw(12, 2, "router latency   %.1f ms", ap->gateway_ms);
#ifdef SIMPLENET_NATIVE_MACOS
    mvprintw(14, 2, "Enter connect to this node   o choose strongest (macOS may roam)");
#else
    mvprintw(14, 2, "Enter connect/pin this node   o optimize this mesh");
#endif
}

#ifdef __FreeBSD__
static void draw_cards(void)
{
    int rows = LINES - 11;
    int end;
    if (rows < 1) rows = 1;
    if (wifi_card_selected < wifi_card_top)
        wifi_card_top = wifi_card_selected;
    if (wifi_card_selected >= wifi_card_top + rows)
        wifi_card_top = wifi_card_selected - rows + 1;
    end = wifi_card_top + rows < wifi_card_count
        ? wifi_card_top + rows : wifi_card_count;

    mvprintw(3, 2, "card");
    mvprintw(4, 2, "----");
    mvprintw(5, 2, "Enter chooses the card SimpleNet scans and connects through.");
    mvprintw(6, 2, "Connecting then activates IPv4 and the default route on it.");
    if (COLS >= 105)
        mvprintw(7, 2, "%-2s %-9s %-10s %-38s %-10s %-18s %s",
                 "", "interface", "physical", "Wi-Fi card", "driver",
                 "network", "route");
    else
        mvprintw(7, 2, "%-2s %-7s %-9s %-28s %-15s %s", "", "iface",
                 "physical", "Wi-Fi card", "network", "route");
    for (int i = wifi_card_top, row = 8; i < end; i++, row++) {
        WifiCard *card = &wifi_cards[i];
        const char *network = card->associated
            ? (card->ssid[0] ? card->ssid : "associated")
            : (card->interface_name[0] ? "not connected" : "no Wi-Fi interface");
        if (i == wifi_card_selected) attron(A_REVERSE);
        if (COLS >= 105)
            mvprintw(row, 2, "%-2s %-9.9s %-10.10s %-38.38s %-10.10s %-18.18s %-7s",
                     !strcmp(card->interface_name, wifi_device) ? "●" : "",
                     card->interface_name[0] ? card->interface_name : "—",
                     card->parent, card->name, card->driver, network,
                     card->system_default ? "default" : "");
        else
            mvprintw(row, 2, "%-2s %-7.7s %-9.9s %-28.28s %-15.15s %-7s",
                     !strcmp(card->interface_name, wifi_device) ? "●" : "",
                     card->interface_name[0] ? card->interface_name : "—",
                     card->parent, card->name, network,
                     card->system_default ? "default" : "");
        if (i == wifi_card_selected) attroff(A_REVERSE);
    }
    if (!wifi_card_count)
        mvprintw(9, 4, "No FreeBSD Wi-Fi cards were found. Press r to refresh.");
}
#endif
static void draw_care(void)
{
    const AdapterRemedy *remedy = active_remedy();
    char powersave[64] = "unknown";
    char q_uuid[512], q_device[256], command[MAX_CMD];
#ifdef SIMPLENET_NATIVE_MACOS
    if (backend == BACKEND_COREWLAN)
        copy_text(powersave, sizeof(powersave), "system managed");
    else
#endif
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
#ifdef SIMPLENET_NATIVE_MACOS
    mvprintw(12, 2, "macOS policy");
    mvprintw(13, 4, "Wi-Fi power and driver policy are managed by macOS.");
    mvprintw(14, 4, "SimpleNet will not use private driver or SMC controls.");
    mvprintw(16, 2, "permissions");
    mvprintw(17, 4, "Nearby SSIDs/BSSIDs require Location Services permission");
    mvprintw(18, 4, "for the terminal application running SimpleNet.");
    (void)remedy;
    return;
#else
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
#endif
}

static void draw_help(void)
{
#ifdef SIMPLENET_NATIVE_MACOS
    static const char *lines[] = {
        "↑/↓ or j/k     choose an access point",
        "Enter          connect to the exact visible node",
        "s              rescan nearby networks",
        "d              selected network details",
        "a              audit router, internet latency, and download speed",
        "o              choose the strongest visible node of the active mesh",
        "u              explain automatic macOS roaming",
        "c              adapter and permission status",
        "?              this help",
        "q              quit",
        "",
        "CoreWLAN uses saved Keychain credentials before prompting.",
        "A selected node is temporary because macOS retains roaming control."
    };
#else
#ifdef __FreeBSD__
    static const char *lines[] = {
        "↑/↓ or j/k     choose an access point",
        "Enter          connect; credentials are masked",
        "s              rescan nearby networks",
        "d              selected network details",
        "a              audit router, internet latency, and download speed",
        "o              select the strongest visible node of the active mesh",
        "u              remove a mesh-node pin",
        "C              card screen; choose which Wi-Fi card SimpleNet uses",
        "c              Adapter care and stability remedies",
        "p              disable power saving for the active connection",
        "?              this help",
        "q              quit",
        "",
        "Card selection alone does not move routes; connecting activates that card.",
        "Optimization selects and pins the strongest visible same-SSID node."
    };
#else
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
#endif
#endif
    mvprintw(3, 2, "help");
    mvprintw(4, 2, "----");
    for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]) && 6 + (int)i < LINES - 3; i++)
        mvprintw(6 + (int)i, 2, "%s", lines[i]);
}

static const char *footer_text(void)
{
    switch (view) {
        case VIEW_DETAILS:
#ifdef __FreeBSD__
            return "Esc networks  Enter connect  o optimize  C card  c care  ? help  q quit";
#else
            return "Esc networks  Enter connect  o optimize  c care  ? help  q quit";
#endif
        case VIEW_CARE:
#ifdef SIMPLENET_NATIVE_MACOS
            return "Esc networks  ? help  q quit";
#elif defined(__FreeBSD__)
            return "p power off  A apply  R remove  C card  Esc networks  ? help  q quit";
#else
            return "p power off  A apply  R remove  Esc networks  ? help  q quit";
#endif
#ifdef __FreeBSD__
        case VIEW_CARDS:
            return "↑↓ move  Enter use  r refresh  Esc networks  ? help  q quit";
#endif
        case VIEW_HELP:
            return "Esc networks  q quit";
        case VIEW_NETWORKS:
        default:
#ifdef __FreeBSD__
            return "↑↓ move  Enter join  s scan  d info  a audit  o strongest  C card  c care  ?  q quit";
#else
            return "↑↓ move  Enter join  s scan  d info  a audit  o strongest  c care  ?  q quit";
#endif
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
#ifdef __FreeBSD__
        case VIEW_CARDS: draw_cards(); break;
#endif
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
#ifdef SIMPLENET_NATIVE_MACOS
    if (!command_exists("route") || !command_exists("ping")) {
        fputs("simplenet requires the macOS route and ping tools.\n", stderr);
        return 1;
    }
#elif defined(__FreeBSD__)
    if (!command_exists("ifconfig") || !command_exists("route") ||
        !command_exists("ping")) {
        fputs("simplenet requires ifconfig, route, and ping.\n", stderr);
        return 1;
    }
#else
    if (!command_exists("ip") || !command_exists("ping")) {
        fputs("simplenet requires iproute2 and ping.\n", stderr);
        return 1;
    }
#endif
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
#ifdef SIMPLENET_NATIVE_MACOS
        fputs("simplenet could not obtain a Wi-Fi interface from CoreWLAN.\n",
              stderr);
#elif defined(__FreeBSD__)
        fputs("simplenet could not detect a supported Wi-Fi manager or "
              "wpa_supplicant control interface.\n", stderr);
#else
        fputs("simplenet could not detect NetworkManager, iwd, or a standalone "
              "wpa_supplicant control interface.\n", stderr);
#endif
        return 1;
    }
#ifdef SIMPLENET_NATIVE_MACOS
    if (backend != BACKEND_COREWLAN &&
        backend != BACKEND_NETWORKMANAGER && !command_exists("iw")) {
#elif defined(__FreeBSD__)
    if (backend != BACKEND_NETWORKMANAGER &&
        backend != BACKEND_WPA_SUPPLICANT && !command_exists("iw")) {
#else
    if (backend != BACKEND_NETWORKMANAGER && !command_exists("iw")) {
#endif
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
#ifdef __FreeBSD__
            if (view == VIEW_CARDS) {
                if (wifi_card_selected > 0) wifi_card_selected--;
            } else {
                if (selected > 0) selected--;
                view = VIEW_NETWORKS;
            }
#else
            if (selected > 0) selected--;
            view = VIEW_NETWORKS;
#endif
        } else if (ch == KEY_DOWN || ch == 'j') {
#ifdef __FreeBSD__
            if (view == VIEW_CARDS) {
                if (wifi_card_selected + 1 < wifi_card_count)
                    wifi_card_selected++;
            } else {
                if (selected + 1 < ap_count) selected++;
                view = VIEW_NETWORKS;
            }
#else
            if (selected + 1 < ap_count) selected++;
            view = VIEW_NETWORKS;
#endif
        } else if (ch == KEY_PPAGE) {
#ifdef __FreeBSD__
            if (view == VIEW_CARDS) {
                wifi_card_selected -= 10;
                if (wifi_card_selected < 0) wifi_card_selected = 0;
            } else {
                selected -= 10;
                if (selected < 0) selected = 0;
                view = VIEW_NETWORKS;
            }
#else
            selected -= 10;
            if (selected < 0) selected = 0;
            view = VIEW_NETWORKS;
#endif
        } else if (ch == KEY_NPAGE) {
#ifdef __FreeBSD__
            if (view == VIEW_CARDS) {
                wifi_card_selected += 10;
                if (wifi_card_selected >= wifi_card_count)
                    wifi_card_selected = wifi_card_count
                        ? wifi_card_count - 1 : 0;
            } else {
                selected += 10;
                if (selected >= ap_count)
                    selected = ap_count ? ap_count - 1 : 0;
                view = VIEW_NETWORKS;
            }
#else
            selected += 10;
            if (selected >= ap_count) selected = ap_count ? ap_count - 1 : 0;
            view = VIEW_NETWORKS;
#endif
        } else if (ch == 's') {
            view = VIEW_NETWORKS;
            set_message(0, "Scanning...");
            draw();
            scan_networks(1);
        } else if (ch == '\n' || ch == KEY_ENTER) {
#ifdef __FreeBSD__
            if (view == VIEW_CARDS)
                select_freebsd_card();
            else {
                connect_selected();
                view = VIEW_NETWORKS;
            }
#else
            connect_selected();
            view = VIEW_NETWORKS;
#endif
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
#ifdef __FreeBSD__
        } else if (ch == 'C') {
            refresh_freebsd_cards();
            wifi_card_top = 0;
            view = VIEW_CARDS;
            set_message(0, "%d Wi-Fi card%s found; Enter selects one for SimpleNet.",
                        wifi_card_count, wifi_card_count == 1 ? "" : "s");
        } else if (ch == 'r' && view == VIEW_CARDS) {
            refresh_freebsd_cards();
            set_message(0, "Wi-Fi card list refreshed.");
#endif
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
