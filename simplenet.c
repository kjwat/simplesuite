#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <ncurses.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_NETWORKS 256
#define MAX_SSID 128
#define MAX_MESSAGE 512
#define MAX_OUTPUT (256 * 1024)
#define COMMAND_TIMEOUT_MS 35000

typedef enum {
    BACKEND_AUTO,
    BACKEND_NETWORKMANAGER,
    BACKEND_WPA_SUPPLICANT
} Backend;

typedef enum {
    SECURITY_OPEN,
    SECURITY_PERSONAL,
    SECURITY_ENTERPRISE,
    SECURITY_WEP
} Security;

typedef struct {
    char ssid[MAX_SSID];
    char bssid[18];
    char security_label[40];
    int signal;
    bool active;
    bool sae;
    bool personal_psk;
    Security security;
} Network;

typedef struct {
    Backend backend;
    char interface_name[64];
    char requested_interface[64];
    char wpa_remote[sizeof(((struct sockaddr_un *)0)->sun_path)];
    char wpa_local[sizeof(((struct sockaddr_un *)0)->sun_path)];
    char wpa_directory[64];
    int wpa_fd;
    Network networks[MAX_NETWORKS];
    int network_count;
    int selected;
    int top;
    char message[MAX_MESSAGE];
    bool message_error;
} App;

static App app = {.backend = BACKEND_AUTO, .wpa_fd = -1};
static volatile sig_atomic_t stop_requested;

static void request_stop(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

static void copy_text(char *dest, size_t size, const char *source)
{
    if (size) snprintf(dest, size, "%s", source ? source : "");
}

static void set_message(bool error, const char *format, ...)
{
    va_list arguments;

    va_start(arguments, format);
    vsnprintf(app.message, sizeof(app.message), format, arguments);
    va_end(arguments);
    app.message_error = error;
}

static long long monotonic_ms(void)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void pause_ms(int milliseconds)
{
    struct timespec delay;

    delay.tv_sec = milliseconds / 1000;
    delay.tv_nsec = (long)(milliseconds % 1000) * 1000000L;
    while (nanosleep(&delay, &delay) < 0 && errno == EINTR) {}
}

/* No shell. Secrets go through stdin, never argv or a temporary file. */
static int run_program(char *const argv[], const char *input,
                       char *output, size_t output_size, int timeout_ms)
{
    int output_pipe[2];
    int input_pipe[2] = {-1, -1};
    pid_t child;
    size_t used = 0;
    int status = 0;
    bool exited = false;
    long long deadline;

    if (!argv || !argv[0] || !output || output_size < 2) return -1;
    output[0] = '\0';
    if (pipe(output_pipe) < 0) return -1;
    if (input && pipe(input_pipe) < 0) {
        close(output_pipe[0]);
        close(output_pipe[1]);
        return -1;
    }
    child = fork();
    if (child < 0) {
        close(output_pipe[0]);
        close(output_pipe[1]);
        if (input) { close(input_pipe[0]); close(input_pipe[1]); }
        return -1;
    }
    if (child == 0) {
        if (input) dup2(input_pipe[0], STDIN_FILENO);
        else {
            int null_fd = open("/dev/null", O_RDONLY);
            if (null_fd >= 0) { dup2(null_fd, STDIN_FILENO); close(null_fd); }
        }
        dup2(output_pipe[1], STDOUT_FILENO);
        dup2(output_pipe[1], STDERR_FILENO);
        close(output_pipe[0]);
        close(output_pipe[1]);
        if (input) { close(input_pipe[0]); close(input_pipe[1]); }
        setenv("LC_ALL", "C", 1);
        execvp(argv[0], argv);
        _exit(errno == ENOENT ? 127 : 126);
    }

    close(output_pipe[1]);
    if (input) {
        size_t left = strlen(input);
        const char *cursor = input;
        close(input_pipe[0]);
        while (left) {
            ssize_t count = write(input_pipe[1], cursor, left);
            if (count > 0) { cursor += count; left -= (size_t)count; }
            else if (count < 0 && errno == EINTR) continue;
            else break;
        }
        close(input_pipe[1]);
    }
    fcntl(output_pipe[0], F_SETFL,
          fcntl(output_pipe[0], F_GETFL, 0) | O_NONBLOCK);
    deadline = monotonic_ms() + timeout_ms;
    for (;;) {
        struct pollfd descriptor = {output_pipe[0], POLLIN | POLLHUP, 0};
        char chunk[4096];
        ssize_t count;

        poll(&descriptor, 1, 50);
        do {
            count = read(output_pipe[0], chunk, sizeof(chunk));
            if (count > 0 && used + 1 < output_size) {
                size_t available = output_size - used - 1;
                size_t keep = (size_t)count < available
                    ? (size_t)count : available;
                memcpy(output + used, chunk, keep);
                used += keep;
            }
        } while (count > 0);
        if (!exited) {
            pid_t waited = waitpid(child, &status, WNOHANG);
            if (waited == child) exited = true;
        }
        if (exited && (descriptor.revents & POLLHUP)) break;
        if (monotonic_ms() >= deadline) break;
    }
    if (!exited) {
        kill(child, SIGTERM);
        pause_ms(150);
        if (waitpid(child, &status, WNOHANG) == 0) kill(child, SIGKILL);
        waitpid(child, &status, 0);
        status = -1;
    }
    close(output_pipe[0]);
    output[used] = '\0';
    if (status == -1) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int split_escaped(char *line, char **fields, int maximum,
                         char separator)
{
    char *read = line;
    char *write = line;
    int count = 1;

    if (!line || !fields || maximum < 1) return 0;
    fields[0] = write;
    while (*read) {
        if (*read == '\\' && read[1]) {
            read++;
            *write++ = *read++;
        } else if (*read == separator && count < maximum) {
            *write++ = '\0';
            fields[count++] = write;
            read++;
        } else {
            *write++ = *read++;
        }
    }
    *write = '\0';
    return count;
}

static int split_plain(char *line, char **fields, int maximum, char separator)
{
    int count = 0;
    char *cursor = line;

    if (!line || !fields || maximum < 1) return 0;
    while (count < maximum) {
        fields[count++] = cursor;
        cursor = strchr(cursor, separator);
        if (!cursor) break;
        *cursor++ = '\0';
    }
    return count;
}

static Security classify_security(const char *text, bool *sae)
{
    if (sae) *sae = false;
    if (!text || !*text || !strcmp(text, "--") || !strcmp(text, "[ESS]"))
        return SECURITY_OPEN;
    if (strstr(text, "WEP")) return SECURITY_WEP;
    if (strstr(text, "802.1X") || strstr(text, "EAP") ||
        strstr(text, "IEEE8021X")) return SECURITY_ENTERPRISE;
    if (sae && (strstr(text, "SAE") || strstr(text, "WPA3"))) *sae = true;
    if (strstr(text, "PSK") || strstr(text, "SAE") ||
        strstr(text, "WPA") || strstr(text, "RSN")) return SECURITY_PERSONAL;
    return SECURITY_OPEN;
}

static const char *friendly_security(const char *flags, Security security,
                                     bool sae)
{
    if (security == SECURITY_OPEN) return "open";
    if (security == SECURITY_WEP) return "WEP";
    if (security == SECURITY_ENTERPRISE) return "enterprise";
    if (sae && flags && (strstr(flags, "PSK") || strstr(flags, "WPA2")))
        return "WPA2/WPA3";
    if (sae) return "WPA3";
    if (flags && (strstr(flags, "WPA2") || strstr(flags, "RSN")))
        return "WPA2";
    return "WPA";
}

static void add_network(const Network *candidate)
{
    int i;

    if (!candidate || !candidate->ssid[0]) return;
    for (i = 0; i < app.network_count; i++) {
        Network *known = &app.networks[i];
        if (!strcmp(known->ssid, candidate->ssid)) {
            known->active = known->active || candidate->active;
            if (candidate->signal > known->signal) {
                bool active = known->active;
                *known = *candidate;
                known->active = active;
            }
            return;
        }
    }
    if (app.network_count < MAX_NETWORKS)
        app.networks[app.network_count++] = *candidate;
}

static int compare_networks(const void *left, const void *right)
{
    const Network *a = left;
    const Network *b = right;

    if (a->active != b->active) return b->active - a->active;
    if (a->signal != b->signal) return b->signal - a->signal;
    return strcasecmp(a->ssid, b->ssid);
}

static bool valid_interface_name(const char *name)
{
    const unsigned char *cursor = (const unsigned char *)name;

    if (!name || !*name || strlen(name) >= sizeof(app.interface_name))
        return false;
    while (*cursor) {
        if (!isalnum(*cursor) && *cursor != '_' && *cursor != '-' &&
            *cursor != '.' && *cursor != ':') return false;
        cursor++;
    }
    return true;
}

static bool nm_detect(void)
{
    char output[8192];
    char *save = NULL;
    char *line;
    char *argv[] = {"nmcli", "-t", "--escape", "yes", "-f",
                    "DEVICE,TYPE,STATE", "device", "status", NULL};

    if (run_program(argv, NULL, output, sizeof(output), 5000) != 0)
        return false;
    for (line = strtok_r(output, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        char *fields[3];
        if (split_escaped(line, fields, 3, ':') != 3) continue;
        if (strcmp(fields[1], "wifi") || !valid_interface_name(fields[0]))
            continue;
        if (!strcmp(fields[2], "unmanaged") ||
            !strcmp(fields[2], "unavailable")) continue;
        if (app.requested_interface[0] &&
            strcmp(app.requested_interface, fields[0])) continue;
        copy_text(app.interface_name, sizeof(app.interface_name), fields[0]);
        return true;
    }
    return false;
}

static bool nm_scan(void)
{
    char *output = malloc(MAX_OUTPUT);
    char *save = NULL;
    char *line;
    char *argv[] = {"nmcli", "-t", "--escape", "yes", "-f",
                    "IN-USE,SSID,BSSID,SIGNAL,SECURITY", "device", "wifi",
                    "list", "ifname", app.interface_name, "--rescan", "yes",
                    NULL};
    int status;
    bool cached = false;

    if (!output) return false;
    status = run_program(argv, NULL, output, MAX_OUTPUT, COMMAND_TIMEOUT_MS);
    if (status != 0) {
        argv[12] = "no";
        status = run_program(argv, NULL, output, MAX_OUTPUT,
                             COMMAND_TIMEOUT_MS);
        cached = status == 0;
    }
    if (status != 0) {
        set_message(true, "NetworkManager scan failed: %.300s", output);
        free(output);
        return false;
    }
    app.network_count = 0;
    for (line = strtok_r(output, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        char *fields[5];
        Network network = {0};
        if (split_escaped(line, fields, 5, ':') != 5 || !fields[1][0])
            continue;
        copy_text(network.ssid, sizeof(network.ssid), fields[1]);
        copy_text(network.bssid, sizeof(network.bssid), fields[2]);
        network.signal = atoi(fields[3]);
        if (network.signal < 0) network.signal = 0;
        if (network.signal > 100) network.signal = 100;
        network.active = !strcmp(fields[0], "*");
        network.security = classify_security(fields[4], &network.sae);
        network.personal_psk = strstr(fields[4], "PSK") ||
                               strstr(fields[4], "WPA1") ||
                               strstr(fields[4], "WPA2");
        copy_text(network.security_label, sizeof(network.security_label),
                  friendly_security(fields[4], network.security, network.sae));
        add_network(&network);
    }
    free(output);
    qsort(app.networks, (size_t)app.network_count, sizeof(app.networks[0]),
          compare_networks);
    app.selected = app.top = 0;
    set_message(false, "%d network%s found%s.", app.network_count,
                app.network_count == 1 ? "" : "s",
                cached ? " (cached scan)" : "");
    return true;
}

static bool nm_connect(const Network *network, const char *password)
{
    char output[8192];
    char input[256];
    int status;
    char *open_argv[] = {"nmcli", "--wait", "30", "device", "wifi",
                         "connect", (char *)network->ssid, "ifname",
                         app.interface_name, NULL};
    char *secure_argv[] = {"nmcli", "--wait", "30", "--ask", "device",
                           "wifi", "connect", (char *)network->ssid,
                           "ifname", app.interface_name, NULL};

    if (network->security == SECURITY_OPEN)
        status = run_program(open_argv, NULL, output, sizeof(output),
                             COMMAND_TIMEOUT_MS);
    else {
        snprintf(input, sizeof(input), "%s\n", password);
        status = run_program(secure_argv, input, output, sizeof(output),
                             COMMAND_TIMEOUT_MS);
        memset(input, 0, sizeof(input));
    }
    if (status != 0) {
        char *newline = strpbrk(output, "\r\n");
        if (newline) *newline = '\0';
        set_message(true, "Could not connect to %s: %.240s", network->ssid,
                    output[0] ? output : "NetworkManager returned an error");
        return false;
    }
    set_message(false, "Connected to %s.", network->ssid);
    return true;
}

static void close_wpa(void)
{
    if (app.wpa_fd >= 0) close(app.wpa_fd);
    app.wpa_fd = -1;
    if (app.wpa_local[0]) unlink(app.wpa_local);
    if (app.wpa_directory[0]) rmdir(app.wpa_directory);
    app.wpa_local[0] = app.wpa_directory[0] = '\0';
}

static bool wpa_request(const char *command, char *reply, size_t reply_size,
                        int timeout_ms)
{
    long long deadline = monotonic_ms() + timeout_ms;

    if (app.wpa_fd < 0 || !command || !reply || reply_size < 2) return false;
    if (send(app.wpa_fd, command, strlen(command), 0) < 0) return false;
    while (monotonic_ms() < deadline) {
        struct pollfd descriptor = {app.wpa_fd, POLLIN, 0};
        int remaining = (int)(deadline - monotonic_ms());
        ssize_t count;
        if (poll(&descriptor, 1, remaining) <= 0) return false;
        count = recv(app.wpa_fd, reply, reply_size - 1, 0);
        if (count < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        reply[count] = '\0';
        if (reply[0] == '<' && isdigit((unsigned char)reply[1])) continue;
        return true;
    }
    return false;
}

static bool wpa_open_path(const char *path, const char *interface_name)
{
    struct sockaddr_un local = {0};
    struct sockaddr_un remote = {0};
    char directory[] = "/tmp/simplenet.XXXXXX";
    char reply[64];
    int descriptor;

    if (strlen(path) >= sizeof(remote.sun_path) || !mkdtemp(directory))
        return false;
    descriptor = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (descriptor < 0) { rmdir(directory); return false; }
    local.sun_family = AF_UNIX;
    snprintf(local.sun_path, sizeof(local.sun_path), "%s/control", directory);
    if (bind(descriptor, (struct sockaddr *)&local, sizeof(local)) < 0) {
        close(descriptor); rmdir(directory); return false;
    }
    remote.sun_family = AF_UNIX;
    copy_text(remote.sun_path, sizeof(remote.sun_path), path);
    if (connect(descriptor, (struct sockaddr *)&remote, sizeof(remote)) < 0) {
        close(descriptor); unlink(local.sun_path); rmdir(directory); return false;
    }
    app.wpa_fd = descriptor;
    copy_text(app.wpa_remote, sizeof(app.wpa_remote), path);
    copy_text(app.wpa_local, sizeof(app.wpa_local), local.sun_path);
    copy_text(app.wpa_directory, sizeof(app.wpa_directory), directory);
    if (!wpa_request("PING", reply, sizeof(reply), 1500) ||
        strncmp(reply, "PONG", 4)) {
        close_wpa();
        return false;
    }
    copy_text(app.interface_name, sizeof(app.interface_name), interface_name);
    return true;
}

static bool wpa_detect(void)
{
    static const char *directories[] = {
        "/run/wpa_supplicant", "/var/run/wpa_supplicant", NULL
    };
    int i;

    for (i = 0; directories[i]; i++) {
        if (app.requested_interface[0]) {
            char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
            snprintf(path, sizeof(path), "%s/%s", directories[i],
                     app.requested_interface);
            if (wpa_open_path(path, app.requested_interface)) return true;
        } else {
            DIR *directory = opendir(directories[i]);
            struct dirent *entry;
            if (!directory) continue;
            while ((entry = readdir(directory)) != NULL) {
                char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
                struct stat info;
                size_t directory_length = strlen(directories[i]);
                size_t name_length = strlen(entry->d_name);
                if (entry->d_name[0] == '.' || !strcmp(entry->d_name, "global") ||
                    !valid_interface_name(entry->d_name)) continue;
                if (directory_length + name_length + 2 > sizeof(path))
                    continue;
                memcpy(path, directories[i], directory_length);
                path[directory_length] = '/';
                memcpy(path + directory_length + 1, entry->d_name,
                       name_length + 1);
                if (stat(path, &info) < 0 || !S_ISSOCK(info.st_mode)) continue;
                if (wpa_open_path(path, entry->d_name)) {
                    closedir(directory);
                    return true;
                }
            }
            closedir(directory);
        }
    }
    return false;
}

static int hex_digit(char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

static void decode_wpa_text(const char *source, char *dest, size_t size)
{
    size_t used = 0;

    while (source && *source && used + 1 < size) {
        if (source[0] == '\\' && source[1] == 'x' &&
            hex_digit(source[2]) >= 0 && hex_digit(source[3]) >= 0) {
            unsigned char value = (unsigned char)((hex_digit(source[2]) << 4) |
                                                  hex_digit(source[3]));
            if (value && !iscntrl(value)) dest[used++] = (char)value;
            source += 4;
        } else if (source[0] == '\\' && source[1]) {
            source++;
            if (*source == 'n' || *source == 'r' || *source == 't') {
                dest[used++] = ' ';
                source++;
            } else dest[used++] = *source++;
        } else {
            unsigned char value = (unsigned char)*source++;
            if (!iscntrl(value)) dest[used++] = (char)value;
        }
    }
    dest[used] = '\0';
}

static void wpa_status(char *ssid, size_t ssid_size,
                       char *bssid, size_t bssid_size, bool *completed)
{
    char reply[4096];
    char *save = NULL;
    char *line;

    if (ssid_size) ssid[0] = '\0';
    if (bssid_size) bssid[0] = '\0';
    if (completed) *completed = false;
    if (!wpa_request("STATUS", reply, sizeof(reply), 2000)) return;
    for (line = strtok_r(reply, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        if (!strncmp(line, "ssid=", 5))
            decode_wpa_text(line + 5, ssid, ssid_size);
        else if (!strncmp(line, "bssid=", 6))
            copy_text(bssid, bssid_size, line + 6);
        else if (completed && !strcmp(line, "wpa_state=COMPLETED"))
            *completed = true;
    }
}

static int dbm_to_percent(int dbm)
{
    int percent = 2 * (dbm + 100);
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    return percent;
}

static bool parse_wpa_scan(char *reply)
{
    char active_ssid[MAX_SSID] = "";
    char active_bssid[18] = "";
    char *save = NULL;
    char *line;
    bool completed = false;

    app.network_count = 0;
    wpa_status(active_ssid, sizeof(active_ssid), active_bssid,
               sizeof(active_bssid), &completed);
    (void)strtok_r(reply, "\n", &save);
    while ((line = strtok_r(NULL, "\n", &save)) != NULL) {
        char *fields[5];
        Network network = {0};
        if (split_plain(line, fields, 5, '\t') != 5) continue;
        decode_wpa_text(fields[4], network.ssid, sizeof(network.ssid));
        if (!network.ssid[0]) continue;
        copy_text(network.bssid, sizeof(network.bssid), fields[0]);
        network.signal = dbm_to_percent(atoi(fields[2]));
        network.security = classify_security(fields[3], &network.sae);
        network.personal_psk = strstr(fields[3], "PSK") != NULL;
        copy_text(network.security_label, sizeof(network.security_label),
                  friendly_security(fields[3], network.security, network.sae));
        network.active = completed &&
            (!strcasecmp(active_bssid, network.bssid) ||
             !strcmp(active_ssid, network.ssid));
        add_network(&network);
    }
    qsort(app.networks, (size_t)app.network_count, sizeof(app.networks[0]),
          compare_networks);
    return app.network_count > 0;
}

static bool wpa_scan(void)
{
    char *reply = malloc(MAX_OUTPUT);
    long long deadline;
    bool cached = false;

    if (!reply) return false;
    if (!wpa_request("SCAN", reply, MAX_OUTPUT, 3000) ||
        strncmp(reply, "OK", 2)) {
        cached = wpa_request("SCAN_RESULTS", reply, MAX_OUTPUT, 3000) &&
                 parse_wpa_scan(reply);
        if (!cached) {
            set_message(true, "wpa_supplicant refused the scan.");
            free(reply);
            return false;
        }
    }
    if (!cached) {
        deadline = monotonic_ms() + 10000;
        pause_ms(1200);
        do {
            if (wpa_request("SCAN_RESULTS", reply, MAX_OUTPUT, 3000) &&
                parse_wpa_scan(reply)) break;
            pause_ms(500);
        } while (monotonic_ms() < deadline);
    }
    free(reply);
    app.selected = app.top = 0;
    if (!app.network_count) {
        set_message(true, "No networks found; press r to scan again.");
        return false;
    }
    set_message(false, "%d network%s found%s.", app.network_count,
                app.network_count == 1 ? "" : "s",
                cached ? " (cached scan)" : "");
    return true;
}

static void hex_encode(const char *source, char *dest, size_t size)
{
    static const char digits[] = "0123456789abcdef";
    size_t used = 0;
    while (source && *source && used + 2 < size) {
        unsigned char value = (unsigned char)*source++;
        dest[used++] = digits[value >> 4];
        dest[used++] = digits[value & 15];
    }
    dest[used] = '\0';
}

static bool wpa_set(int id, const char *field, const char *value)
{
    char command[1024];
    char reply[256];
    bool succeeded;

    snprintf(command, sizeof(command), "SET_NETWORK %d %s %s", id, field,
             value);
    succeeded = wpa_request(command, reply, sizeof(reply), 3000) &&
                !strncmp(reply, "OK", 2);
    memset(command, 0, sizeof(command));
    return succeeded;
}

static bool quote_wpa_secret(const char *secret, char *quoted, size_t size)
{
    size_t used = 0;

    if (size < 3) return false;
    quoted[used++] = '"';
    while (*secret) {
        unsigned char value = (unsigned char)*secret++;
        if (value < 32 || value == 127) return false;
        if ((value == '"' || value == '\\') && used + 2 < size)
            quoted[used++] = '\\';
        if (used + 1 >= size) return false;
        quoted[used++] = (char)value;
    }
    if (used + 2 > size) return false;
    quoted[used++] = '"';
    quoted[used] = '\0';
    return true;
}

static bool is_hex_psk(const char *password)
{
    size_t i;

    if (strlen(password) != 64) return false;
    for (i = 0; i < 64; i++) if (hex_digit(password[i]) < 0) return false;
    return true;
}

static bool wpa_connected_to(const char *ssid)
{
    char current[MAX_SSID];
    char bssid[18];
    bool completed;
    wpa_status(current, sizeof(current), bssid, sizeof(bssid), &completed);
    return completed && !strcmp(current, ssid);
}

static int wpa_matching_networks(const char *ssid, int *ids, int maximum)
{
    char reply[16384];
    char *save = NULL;
    char *line;
    int count = 0;

    if (!wpa_request("LIST_NETWORKS", reply, sizeof(reply), 3000)) return -1;
    (void)strtok_r(reply, "\n", &save);
    while (count < maximum && (line = strtok_r(NULL, "\n", &save)) != NULL) {
        char *fields[4];
        char decoded[MAX_SSID];
        char *end;
        long id;

        if (split_plain(line, fields, 4, '\t') < 2) continue;
        decode_wpa_text(fields[1], decoded, sizeof(decoded));
        if (strcmp(decoded, ssid)) continue;
        id = strtol(fields[0], &end, 10);
        if (end == fields[0] || *end || id < 0 || id > 1000000) continue;
        ids[count++] = (int)id;
    }
    return count;
}

static int wpa_enabled_networks(int *ids, int maximum)
{
    char reply[16384];
    char *save = NULL;
    char *line;
    int count = 0;

    if (!wpa_request("LIST_NETWORKS", reply, sizeof(reply), 3000)) return -1;
    (void)strtok_r(reply, "\n", &save);
    while (count < maximum && (line = strtok_r(NULL, "\n", &save)) != NULL) {
        char *fields[4];
        char *end;
        long id;
        int field_count = split_plain(line, fields, 4, '\t');

        if (field_count < 1 ||
            (field_count >= 4 && strstr(fields[3], "[DISABLED]"))) continue;
        id = strtol(fields[0], &end, 10);
        if (end == fields[0] || *end || id < 0 || id > 1000000) continue;
        ids[count++] = (int)id;
    }
    return count;
}

static bool id_is_listed(int id, const int *ids, int count)
{
    for (int i = 0; i < count; i++) if (ids[i] == id) return true;
    return false;
}

static bool wpa_restore_enabled(const int *ids, int count,
                                const int *removed, int removed_count)
{
    char command[128];
    char reply[256];
    bool restored = true;

    for (int i = 0; i < count; i++) {
        if (id_is_listed(ids[i], removed, removed_count)) continue;
        snprintf(command, sizeof(command), "ENABLE_NETWORK %d", ids[i]);
        if (!wpa_request(command, reply, sizeof(reply), 3000) ||
            strncmp(reply, "OK", 2)) restored = false;
    }
    return restored;
}

static bool wpa_connect(const Network *network, const char *password)
{
    char reply[4096];
    char command[128];
    char ssid_hex[MAX_SSID * 2 + 1];
    char secret[300] = "";
    long id;
    char *end;
    bool saved;
    long long deadline;
    int old_ids[64];
    int old_count = wpa_matching_networks(network->ssid, old_ids,
                                          (int)(sizeof(old_ids) /
                                                sizeof(old_ids[0])));
    int enabled_ids[256];
    int enabled_count = wpa_enabled_networks(
        enabled_ids, (int)(sizeof(enabled_ids) / sizeof(enabled_ids[0])));

    if (old_count < 0 || enabled_count < 0) {
        set_message(true, "Could not read wpa_supplicant's saved profiles.");
        return false;
    }

    if (!wpa_request("ADD_NETWORK", reply, sizeof(reply), 3000)) goto failed;
    errno = 0;
    id = strtol(reply, &end, 10);
    if (errno || end == reply || id < 0 || id > 1000000) goto failed;
    hex_encode(network->ssid, ssid_hex, sizeof(ssid_hex));
    if (!wpa_set((int)id, "ssid", ssid_hex)) goto remove;
    if (network->security == SECURITY_OPEN) {
        if (!wpa_set((int)id, "key_mgmt", "NONE")) goto remove;
    } else {
        if (is_hex_psk(password)) copy_text(secret, sizeof(secret), password);
        else if (!quote_wpa_secret(password, secret, sizeof(secret))) goto remove;
        if (network->sae) {
            bool use_psk = network->personal_psk && strlen(password) >= 8;
            if (!wpa_set((int)id, "key_mgmt",
                         use_psk ? "SAE WPA-PSK" : "SAE") ||
                !wpa_set((int)id, "sae_password", secret)) goto remove;
            if (use_psk && !wpa_set((int)id, "psk", secret)) goto remove;
            if (!use_psk && !wpa_set((int)id, "ieee80211w", "2"))
                goto remove;
        } else if (!wpa_set((int)id, "psk", secret)) goto remove;
        memset(secret, 0, sizeof(secret));
    }
    snprintf(command, sizeof(command), "SELECT_NETWORK %ld", id);
    if (!wpa_request(command, reply, sizeof(reply), 3000) ||
        strncmp(reply, "OK", 2)) goto remove;
    deadline = monotonic_ms() + 30000;
    do {
        if (wpa_connected_to(network->ssid)) break;
        pause_ms(300);
    } while (monotonic_ms() < deadline);
    if (!wpa_connected_to(network->ssid)) goto remove;
    if (!wpa_restore_enabled(enabled_ids, enabled_count, NULL, 0)) goto remove;
    for (int i = 0; i < old_count; i++) {
        if (old_ids[i] == id) continue;
        snprintf(command, sizeof(command), "REMOVE_NETWORK %d", old_ids[i]);
        wpa_request(command, reply, sizeof(reply), 3000);
    }
    saved = wpa_request("SAVE_CONFIG", reply, sizeof(reply), 3000) &&
            !strncmp(reply, "OK", 2);
    if (saved) set_message(false, "Connected to %s.", network->ssid);
    else set_message(false, "Connected to %s (profile is session-only).",
                     network->ssid);
    return true;

remove:
    memset(secret, 0, sizeof(secret));
    snprintf(command, sizeof(command), "REMOVE_NETWORK %ld", id);
    wpa_request(command, reply, sizeof(reply), 3000);
    wpa_restore_enabled(enabled_ids, enabled_count, NULL, 0);
    wpa_request("RECONNECT", reply, sizeof(reply), 3000);
failed:
    set_message(true, "Could not connect to %s; check the password.",
                network->ssid);
    return false;
}

static const char *backend_name(void)
{
    return app.backend == BACKEND_NETWORKMANAGER
        ? "NetworkManager" : "wpa_supplicant";
}

static bool detect_backend(Backend requested)
{
    if ((requested == BACKEND_AUTO || requested == BACKEND_NETWORKMANAGER) &&
        nm_detect()) {
        app.backend = BACKEND_NETWORKMANAGER;
        return true;
    }
    if ((requested == BACKEND_AUTO || requested == BACKEND_WPA_SUPPLICANT) &&
        wpa_detect()) {
        app.backend = BACKEND_WPA_SUPPLICANT;
        return true;
    }
    return false;
}

static bool scan_networks(void)
{
    return app.backend == BACKEND_NETWORKMANAGER ? nm_scan() : wpa_scan();
}

static bool password_valid(const Network *network, const char *password)
{
    size_t length = strlen(password);

    if (network->sae) return length >= 1 && length <= 63;
    if (length >= 8 && length <= 63) return true;
    return is_hex_psk(password);
}

static bool prompt_password(const Network *network, char *password, size_t size)
{
    int width = COLS < 66 ? COLS - 4 : 62;
    int row = LINES / 2 - 2;
    int column = (COLS - width) / 2;
    size_t length = 0;

    if (width < 24 || LINES < 8) {
        set_message(true, "Terminal is too small for the password prompt.");
        return false;
    }
    password[0] = '\0';
    curs_set(1);
    for (;;) {
        int key;
        int field_width = width - 4;
        int cursor_offset;
        erase();
        attron(A_BOLD);
        mvprintw(row, column + 2, "Connect to %.*s", width - 15,
                 network->ssid);
        attroff(A_BOLD);
        mvprintw(row + 1, column + 2, "Password:");
        mvhline(row + 2, column + 2, ' ', field_width);
        mvaddch(row + 2, column + 2, '[');
        mvaddch(row + 2, column + width - 3, ']');
        for (size_t i = 0; i < length && (int)i < field_width - 2; i++)
            mvaddch(row + 2, column + 3 + (int)i, '*');
        mvprintw(row + 4, column + 2, "Enter connect   Esc cancel");
        cursor_offset = length < (size_t)(field_width - 2)
            ? (int)length : field_width - 3;
        move(row + 2, column + 3 + cursor_offset);
        refresh();
        key = getch();
        if (key == 27) {
            memset(password, 0, size);
            curs_set(0);
            set_message(false, "Connection cancelled.");
            return false;
        }
        if (key == '\n' || key == KEY_ENTER) {
            if (password_valid(network, password)) {
                curs_set(0);
                return true;
            }
            beep();
            continue;
        }
        if (key == KEY_BACKSPACE || key == 127 || key == 8) {
            if (length) password[--length] = '\0';
        } else if (key == 21) {
            memset(password, 0, size);
            length = 0;
        } else if (key >= 32 && key <= 255 && length + 1 < size) {
            password[length++] = (char)key;
            password[length] = '\0';
        }
    }
}

static void connect_selected(void)
{
    Network *network;
    char password[128] = "";
    bool connected;

    if (!app.network_count) return;
    network = &app.networks[app.selected];
    if (network->security == SECURITY_ENTERPRISE) {
        set_message(true, "%s uses enterprise authentication; this simple "
                    "client supports open and personal networks.", network->ssid);
        return;
    }
    if (network->security == SECURITY_WEP) {
        set_message(true, "%s uses obsolete WEP security, which is not supported.",
                    network->ssid);
        return;
    }
    if (network->security != SECURITY_OPEN &&
        !prompt_password(network, password, sizeof(password))) return;
    set_message(false, "Connecting to %s...", network->ssid);
    if (app.backend == BACKEND_NETWORKMANAGER)
        connected = nm_connect(network, password);
    else connected = wpa_connect(network, password);
    memset(password, 0, sizeof(password));
    if (connected) {
        char connected_ssid[MAX_SSID];
        copy_text(connected_ssid, sizeof(connected_ssid), network->ssid);
        for (int i = 0; i < app.network_count; i++)
            app.networks[i].active = !strcmp(app.networks[i].ssid,
                                             connected_ssid);
        qsort(app.networks, (size_t)app.network_count,
              sizeof(app.networks[0]), compare_networks);
        app.selected = app.top = 0;
    }
}

static void draw(void)
{
    int visible = LINES - 7;
    int end;

    erase();
    if (LINES < 8 || COLS < 36) {
        mvprintw(0, 0, "simplenet: terminal too small");
        refresh();
        return;
    }
    if (visible < 1) visible = 1;
    if (app.selected < app.top) app.top = app.selected;
    if (app.selected >= app.top + visible)
        app.top = app.selected - visible + 1;
    end = app.top + visible;
    if (end > app.network_count) end = app.network_count;

    attron(A_BOLD);
    mvprintw(1, 2, "simplenet");
    attroff(A_BOLD);
    mvprintw(1, 14, "%s · %s", backend_name(), app.interface_name);
    mvprintw(3, 2, "  %-*s %7s  %s", COLS > 76 ? 42 : COLS - 32,
             "network", "signal", "security");
    for (int i = app.top, row = 4; i < end; i++, row++) {
        Network *network = &app.networks[i];
        int name_width = COLS > 76 ? 42 : COLS - 32;
        if (i == app.selected) attron(A_REVERSE);
        mvprintw(row, 2, "%s %-*.*s %6d%%  %-12.12s",
                 network->active ? "●" : " ", name_width, name_width,
                 network->ssid, network->signal, network->security_label);
        if (i == app.selected) attroff(A_REVERSE);
    }
    if (!app.network_count) mvprintw(5, 4, "No networks found. Press r to scan.");
    if (app.message_error) attron(A_BOLD);
    mvaddnstr(LINES - 2, 2, app.message, COLS - 4);
    if (app.message_error) attroff(A_BOLD);
    mvhline(LINES - 1, 0, ' ', COLS);
    mvaddnstr(LINES - 1, 2,
              "↑/↓ choose   Enter connect   r rescan   q quit", COLS - 4);
    refresh();
}

static void usage(const char *program)
{
    printf("Usage: %s [-b auto|nm|wpa] [-i interface]\n", program);
    puts("Scan for Wi-Fi networks and connect using NetworkManager or "
         "wpa_supplicant.");
}

#ifndef SIMPLENET_TEST
int main(int argc, char **argv)
{
    Backend requested = BACKEND_AUTO;
    struct sigaction stop_action = {0};
    int option;
    int key;

    if (argc == 2 && !strcmp(argv[1], "--help")) {
        usage(argv[0]);
        return 0;
    }
    while ((option = getopt(argc, argv, "b:i:h")) != -1) {
        switch (option) {
            case 'b':
                if (!strcmp(optarg, "auto")) requested = BACKEND_AUTO;
                else if (!strcmp(optarg, "nm"))
                    requested = BACKEND_NETWORKMANAGER;
                else if (!strcmp(optarg, "wpa"))
                    requested = BACKEND_WPA_SUPPLICANT;
                else { usage(argv[0]); return 2; }
                break;
            case 'i':
                if (!valid_interface_name(optarg)) {
                    fputs("simplenet: invalid interface name\n", stderr);
                    return 2;
                }
                copy_text(app.requested_interface,
                          sizeof(app.requested_interface), optarg);
                break;
            case 'h': usage(argv[0]); return 0;
            default: usage(argv[0]); return 2;
        }
    }
    if (optind != argc) { usage(argv[0]); return 2; }
    signal(SIGPIPE, SIG_IGN);
    stop_action.sa_handler = request_stop;
    sigemptyset(&stop_action.sa_mask);
    sigaction(SIGINT, &stop_action, NULL);
    sigaction(SIGTERM, &stop_action, NULL);
    atexit(close_wpa);
    if (!detect_backend(requested)) {
        if (requested == BACKEND_WPA_SUPPLICANT)
            fputs("simplenet: no accessible wpa_supplicant control socket "
                  "was found\n", stderr);
        else if (requested == BACKEND_NETWORKMANAGER)
            fputs("simplenet: NetworkManager is not managing a Wi-Fi "
                  "interface\n", stderr);
        else
            fputs("simplenet: no NetworkManager-managed interface or "
                  "accessible wpa_supplicant control socket was found\n", stderr);
        return 1;
    }
    setlocale(LC_ALL, "");
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    set_message(false, "Scanning...");
    draw();
    scan_networks();
    while (!stop_requested) {
        draw();
        key = getch();
        if (key == 'q' || key == 'Q') break;
        if ((key == KEY_UP || key == 'k') && app.selected > 0)
            app.selected--;
        else if ((key == KEY_DOWN || key == 'j') &&
                 app.selected + 1 < app.network_count) app.selected++;
        else if (key == KEY_PPAGE) {
            app.selected -= 10;
            if (app.selected < 0) app.selected = 0;
        } else if (key == KEY_NPAGE) {
            app.selected += 10;
            if (app.selected >= app.network_count)
                app.selected = app.network_count ? app.network_count - 1 : 0;
        } else if (key == 'r' || key == 'R') {
            set_message(false, "Scanning...");
            draw();
            scan_networks();
        } else if (key == '\n' || key == KEY_ENTER) connect_selected();
    }
    endwin();
    close_wpa();
    return 0;
}
#endif
