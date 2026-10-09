#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <ncurses.h>
#include <ctype.h>
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
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_DEVICES 256
#define MAX_DEVICE_NAME 160
#define MAX_MESSAGE 512
#define MAX_OUTPUT (512 * 1024)
#define COMMAND_TIMEOUT_MS 10000
#define PAIR_TIMEOUT_MS 60000
#define DEVICE_REFRESH_MS 5000
#define SERVICE_NAME_UUID "00000720-0000-1000-8000-00805f9b34fb"

typedef enum {
    SETUP_GENERAL,
    SETUP_CLI_MISSING,
    SETUP_SERVICE_UNAVAILABLE,
    SETUP_NO_CONTROLLER
} SetupReason;

typedef struct {
    char address[18];
    char name[MAX_DEVICE_NAME];
    char service_name[MAX_DEVICE_NAME];
    char icon[64];
    int rssi;
    int battery;
    int manufacturer;
    bool paired;
    bool trusted;
    bool connected;
    bool blocked;
} Device;

typedef struct {
    char address[18];
    char name[MAX_DEVICE_NAME];
    bool powered;
    bool discovering;
} Adapter;

typedef struct {
    Adapter adapter;
    Device devices[MAX_DEVICES];
    int device_count;
    int selected;
    int top;
    char message[MAX_MESSAGE];
    bool message_error;
    bool message_summary;
} App;

static App app;
static volatile sig_atomic_t stop_requested;
static pid_t background_action_pid = -1;
static int background_result_fd = -1;
static FILE *background_scan_result;
static char background_action[16];
static char background_name[MAX_DEVICE_NAME];
static App background_scan_baseline;
static bool background_scan_announced;
static pid_t discovery_pid = -1;
static int discovery_input_fd = -1;
static int discovery_output_fd = -1;
static char discovery_line[4096];
static size_t discovery_line_used;
static bool discovery_line_overflow;
static bool discovery_refresh_pending;
static long long next_device_refresh;
static long long last_device_refresh;
static bool show_unnamed;

static void draw(void);
static bool write_bytes(int fd, const char *bytes, size_t size);
static void stop_background_action(void);

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
    app.message_summary = false;
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

static void stop_child(pid_t child, bool process_group)
{
    long long deadline = monotonic_ms() + 200;
    pid_t target = process_group ? -child : child;

    (void)kill(target, SIGTERM);
    do {
        pid_t result = waitpid(child, NULL, WNOHANG);
        if (result == child || (result < 0 && errno == ECHILD)) {
            if (process_group) (void)kill(target, SIGKILL);
            return;
        }
        pause_ms(10);
    } while (monotonic_ms() < deadline);
    (void)kill(target, SIGKILL);
    while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {}
}

/* No shell: device names and addresses never become command text. */
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

static bool program_available(const char *program)
{
    const char *path_value = getenv("PATH");
    char *path;
    char *save = NULL;
    char *directory;

    if (!program || !*program) return false;
    if (strchr(program, '/')) return access(program, X_OK) == 0;
    path = strdup(path_value && *path_value
        ? path_value : "/usr/local/bin:/usr/bin:/bin");
    if (!path) return false;
    for (directory = strtok_r(path, ":", &save); directory;
         directory = strtok_r(NULL, ":", &save)) {
        char candidate[4096];
        if (snprintf(candidate, sizeof(candidate), "%s/%s", directory,
                     program) >= (int)sizeof(candidate)) continue;
        if (access(candidate, X_OK) == 0) {
            free(path);
            return true;
        }
    }
    free(path);
    return false;
}

static bool valid_address(const char *address)
{
    bool any_nonzero = false;
    bool any_not_f = false;

    if (!address || strlen(address) != 17) return false;
    for (int i = 0; i < 17; i++) {
        if (i % 3 == 2) {
            if (address[i] != ':') return false;
        } else {
            if (!isxdigit((unsigned char)address[i])) return false;
            if (address[i] != '0') any_nonzero = true;
            if (address[i] != 'f' && address[i] != 'F') any_not_f = true;
        }
    }
    return any_nonzero && any_not_f;
}

static void normalize_address(char address[18])
{
    for (int i = 0; i < 17; i++)
        address[i] = (char)toupper((unsigned char)address[i]);
}

/* bluetoothctl uses readline even when stdout is a pipe. Remove its redraws. */
static void strip_terminal_sequences(char *text)
{
    char *read = text;
    char *write = text;
    char *line_start = text;

    while (read && *read) {
        unsigned char value = (unsigned char)*read++;
        if (value == 0x1b) {
            if (*read == '[') {
                read++;
                while (*read) {
                    unsigned char part = (unsigned char)*read++;
                    if (part >= 0x40 && part <= 0x7e) break;
                }
            } else if (*read) read++;
            continue;
        }
        if (value == '\r') {
            if (write > line_start && write[-1] != '\n') *write++ = '\n';
            line_start = write;
            continue;
        }
        if (value == '\b' || value == 0x7f) {
            if (write > line_start) write--;
            continue;
        }
        if (value == '\n') {
            *write++ = '\n';
            line_start = write;
        } else if (value == '\t' || value >= 0x20) {
            *write++ = (char)value;
        }
    }
    if (write) *write = '\0';
}

static char *trim(char *text)
{
    char *end;

    while (*text && isspace((unsigned char)*text)) text++;
    end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) end--;
    *end = '\0';
    return text;
}

static void sanitize_name(char *text)
{
    char *read = text;
    char *write = text;
    bool previous_space = false;

    while (*read) {
        unsigned char value = (unsigned char)*read++;
        if (iscntrl(value)) value = ' ';
        if (isspace(value)) {
            if (write == text || previous_space) continue;
            value = ' ';
            previous_space = true;
        } else previous_space = false;
        *write++ = (char)value;
    }
    while (write > text && write[-1] == ' ') write--;
    *write = '\0';
}

static bool contains_ci(const char *haystack, const char *needle)
{
    size_t length;

    if (!haystack || !needle) return false;
    length = strlen(needle);
    if (!length) return true;
    while (*haystack) {
        if (!strncasecmp(haystack, needle, length)) return true;
        haystack++;
    }
    return false;
}

static bool output_failed(const char *output)
{
    return contains_ci(output, "failed") ||
           contains_ci(output, "not available") ||
           contains_ci(output, "no default controller") ||
           contains_ci(output, "invalid command") ||
           contains_ci(output, "error:");
}

static void output_summary(const char *source, char *summary, size_t size)
{
    char buffer[4096];
    char fallback[512] = "";
    char *save = NULL;
    char *line;

    copy_text(buffer, sizeof(buffer), source);
    strip_terminal_sequences(buffer);
    summary[0] = '\0';
    for (line = strtok_r(buffer, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        char *clean = trim(line);
        if (!*clean || strstr(clean, "[bluetoothctl]") ||
            !strncmp(clean, "Waiting to connect", 18)) continue;
        copy_text(fallback, sizeof(fallback), clean);
        if (output_failed(clean)) {
            copy_text(summary, size, clean);
            return;
        }
    }
    copy_text(summary, size, fallback);
}

static Device *device_in_app(App *state, const char *address)
{
    for (int i = 0; i < state->device_count; i++)
        if (!strcasecmp(state->devices[i].address, address))
            return &state->devices[i];
    return NULL;
}

static Device *find_device(const char *address)
{
    return device_in_app(&app, address);
}

static bool address_name(const Device *device, const char *name)
{
    char address[18];

    if (!name || !*name) return true;
    if (strlen(name) != 17) return false;
    copy_text(address, sizeof(address), name);
    for (int i = 2; i < 17; i += 3)
        if (address[i] == '-') address[i] = ':';
    return !strcasecmp(address, device->address);
}

static void update_device_name(Device *device, const char *name)
{
    char candidate[MAX_DEVICE_NAME];

    copy_text(candidate, sizeof(candidate), name);
    sanitize_name(candidate);
    /* BlueZ can expose an address as Alias before resolving the real name. */
    if (!address_name(device, candidate))
        copy_text(device->name, sizeof(device->name), candidate);
    else if (!device->name[0])
        copy_text(device->name, sizeof(device->name), device->address);
}

static bool device_has_name(const Device *device)
{
    return !address_name(device, device->name) || device->service_name[0];
}

static bool device_visible(const Device *device)
{
    /* GNOME waits for a name before showing a discovery result. Keep saved
     * and connected devices accessible even if their name is unavailable. */
    return show_unnamed || device_has_name(device) || device->paired ||
           device->trusted || device->connected;
}

static int visible_device_count(void)
{
    int count = 0;

    for (int i = 0; i < app.device_count; i++)
        if (device_visible(&app.devices[i])) count++;
    return count;
}

static void update_device_summary(void)
{
    int visible = visible_device_count();
    int hidden = app.device_count - visible;
    int unnamed = 0;

    for (int i = 0; i < app.device_count; i++)
        if (!device_has_name(&app.devices[i])) unnamed++;
    if (!visible && hidden)
        set_message(false, "Looking for named devices; %d unnamed hidden (u to show).",
                    hidden);
    else if (hidden)
        set_message(false, "%d Bluetooth device%s listed; %d unnamed hidden (u to show).",
                    visible, visible == 1 ? "" : "s", hidden);
    else if (show_unnamed && unnamed)
        set_message(false, "%d Bluetooth device%s listed; %d unnamed (u to hide).",
                    visible, visible == 1 ? "" : "s", unnamed);
    else if (visible)
        set_message(false, "%d Bluetooth device%s listed.",
                    visible, visible == 1 ? "" : "s");
    else
        set_message(false, "Looking for nearby devices; put one in pairing mode.");
    app.message_summary = true;
}

static const char *manufacturer_hint(int identifier)
{
    /* Common Bluetooth SIG company identifiers, not address/OUI guesses.
     * https://bitbucket.org/bluetooth-SIG/public/src/main/assigned_numbers/company_identifiers/company_identifiers.yaml */
    switch (identifier) {
    case 0x0006: return "Microsoft";
    case 0x004c: return "Apple";
    case 0x0075: return "Samsung";
    case 0x0087: return "Garmin";
    case 0x009e: return "Bose";
    case 0x00e0: case 0x018e: return "Google";
    case 0x012d: return "Sony";
    default: return NULL;
    }
}

static void format_device_name(const Device *device, char *name, size_t size)
{
    const char *manufacturer = manufacturer_hint(device->manufacturer);

    if (!address_name(device, device->name))
        copy_text(name, size, device->name);
    else if (device->service_name[0])
        copy_text(name, size, device->service_name);
    else if (manufacturer)
        snprintf(name, size, "Unnamed device (%s data) [%s]",
                 manufacturer, device->address);
    else
        snprintf(name, size, "Unnamed device [%s]", device->address);
}

static Device *add_device(const char *address, const char *name)
{
    Device *device;

    if (!valid_address(address)) return NULL;
    device = find_device(address);
    if (!device) {
        if (app.device_count >= MAX_DEVICES) return NULL;
        device = &app.devices[app.device_count++];
        memset(device, 0, sizeof(*device));
        copy_text(device->address, sizeof(device->address), address);
        normalize_address(device->address);
        device->rssi = -127;
        device->battery = -1;
        device->manufacturer = -1;
    }
    update_device_name(device, name);
    return device;
}

static bool parse_controller_list(char *output, Adapter *adapter)
{
    char *save = NULL;
    char *line;
    Adapter first = {0};
    bool have_first = false;

    strip_terminal_sequences(output);
    for (line = strtok_r(output, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        char *start = trim(line);
        char address[18];
        char name[MAX_DEVICE_NAME];
        char *default_marker;

        if (strncmp(start, "Controller ", 11) || strlen(start) < 28)
            continue;
        memcpy(address, start + 11, 17);
        address[17] = '\0';
        if (!valid_address(address)) continue;
        copy_text(name, sizeof(name), start + 28);
        default_marker = strstr(name, " [default]");
        if (default_marker) *default_marker = '\0';
        sanitize_name(name);
        if (!have_first) {
            copy_text(first.address, sizeof(first.address), address);
            normalize_address(first.address);
            copy_text(first.name, sizeof(first.name), name);
            have_first = true;
        }
        if (strstr(start, "[default]")) {
            *adapter = first;
            copy_text(adapter->address, sizeof(adapter->address), address);
            normalize_address(adapter->address);
            copy_text(adapter->name, sizeof(adapter->name), name);
            return true;
        }
    }
    if (have_first) *adapter = first;
    return have_first;
}

static void parse_adapter_info(char *output, Adapter *adapter)
{
    char *save = NULL;
    char *line;

    strip_terminal_sequences(output);
    for (line = strtok_r(output, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        char *property = trim(line);
        if (!strncmp(property, "Name:", 5) && !adapter->name[0]) {
            copy_text(adapter->name, sizeof(adapter->name), trim(property + 5));
            sanitize_name(adapter->name);
        } else if (!strncmp(property, "Alias:", 6)) {
            copy_text(adapter->name, sizeof(adapter->name), trim(property + 6));
            sanitize_name(adapter->name);
        } else if (!strncmp(property, "Powered:", 8)) {
            adapter->powered = !strcasecmp(trim(property + 8), "yes");
        } else if (!strncmp(property, "Discovering:", 12)) {
            adapter->discovering = !strcasecmp(trim(property + 12), "yes");
        }
    }
}

static SetupReason detect_adapter(void)
{
    char output[16384];
    char *list_argv[] = {"bluetoothctl", "list", NULL};
    char *show_argv[] = {"bluetoothctl", "show", app.adapter.address, NULL};
    int status;

    if (!program_available("bluetoothctl")) return SETUP_CLI_MISSING;
    status = run_program(list_argv, NULL, output, sizeof(output), 5000);
    if (status != 0) return SETUP_SERVICE_UNAVAILABLE;
    if (!parse_controller_list(output, &app.adapter))
        return SETUP_NO_CONTROLLER;
    status = run_program(show_argv, NULL, output, sizeof(output), 5000);
    if (status != 0 || output_failed(output))
        return SETUP_SERVICE_UNAVAILABLE;
    parse_adapter_info(output, &app.adapter);
    return SETUP_GENERAL;
}

static bool refresh_adapter(void)
{
    char output[16384];
    char *argv[] = {"bluetoothctl", "show", app.adapter.address, NULL};
    Adapter refreshed = app.adapter;
    int status = run_program(argv, NULL, output, sizeof(output), 5000);

    if (status != 0 || output_failed(output)) return false;
    parse_adapter_info(output, &refreshed);
    app.adapter = refreshed;
    return true;
}

static void read_linux_identity(char *identity, size_t size)
{
    FILE *file = fopen("/etc/os-release", "r");
    char line[512];

    identity[0] = '\0';
    if (!file) return;
    while (fgets(line, sizeof(line), file)) {
        char *value;
        char *end;
        if (strncmp(line, "ID=", 3) && strncmp(line, "ID_LIKE=", 8))
            continue;
        value = strchr(line, '=') + 1;
        value = trim(value);
        if ((*value == '\'' || *value == '"') && strlen(value) >= 2) {
            char quote = *value++;
            end = strrchr(value, quote);
            if (end) *end = '\0';
        }
        if (identity[0]) strncat(identity, " ", size - strlen(identity) - 1);
        strncat(identity, value, size - strlen(identity) - 1);
    }
    fclose(file);
}

static void print_install_command(const char *identity)
{
    if (contains_ci(identity, "debian") || contains_ci(identity, "ubuntu") ||
        contains_ci(identity, "mint"))
        puts("  sudo apt update && sudo apt install bluez rfkill");
    else if (contains_ci(identity, "fedora") || contains_ci(identity, "rhel") ||
             contains_ci(identity, "centos"))
        puts("  sudo dnf install bluez");
    else if (contains_ci(identity, "arch") || contains_ci(identity, "manjaro"))
        puts("  sudo pacman -S bluez bluez-utils");
    else if (contains_ci(identity, "void"))
        puts("  sudo xbps-install -S bluez");
    else if (contains_ci(identity, "alpine"))
        puts("  sudo apk add bluez bluez-openrc");
    else if (contains_ci(identity, "suse"))
        puts("  sudo zypper install bluez");
    else if (contains_ci(identity, "gentoo"))
        puts("  sudo emerge --ask net-wireless/bluez");
    else if (contains_ci(identity, "nixos")) {
        puts("  # Add `hardware.bluetooth.enable = true;` to /etc/nixos/configuration.nix");
        puts("  sudo nixos-rebuild switch");
    } else
        puts("  # Install your distribution's BlueZ package (it must provide bluetoothctl).");
}

static void print_service_commands(const char *identity)
{
    if (contains_ci(identity, "void")) {
        puts("  sudo ln -s /etc/sv/dbus /var/service/       # if D-Bus is not enabled");
        puts("  sudo ln -s /etc/sv/bluetoothd /var/service/");
    } else if (contains_ci(identity, "alpine")) {
        puts("  sudo rc-update add bluetooth default");
        puts("  sudo rc-service bluetooth start");
    } else if (contains_ci(identity, "gentoo") ||
               access("/sbin/openrc", X_OK) == 0) {
        puts("  sudo rc-update add bluetooth default");
        puts("  sudo rc-service bluetooth start");
    } else if (!contains_ci(identity, "nixos")) {
        puts("  sudo systemctl enable --now bluetooth.service");
    }
}

static void print_setup_help(SetupReason reason)
{
    char identity[256];

    read_linux_identity(identity, sizeof(identity));
    if (reason == SETUP_CLI_MISSING) {
        fputs("simpleblue: Bluetooth support is not installed yet.\n"
              "SimpleBlue uses the optional BlueZ stack; SimpleSuite leaves "
              "that choice to you.\n\n", stderr);
    }
    else if (reason == SETUP_SERVICE_UNAVAILABLE)
        fputs("simpleblue: the BlueZ Bluetooth service is not available.\n\n",
              stderr);
    else if (reason == SETUP_NO_CONTROLLER)
        fputs("simpleblue: BlueZ is running, but no Bluetooth controller was found.\n\n",
              stderr);
    else
        puts("SimpleBlue needs BlueZ, a running Bluetooth service, and a controller.");

    puts(reason == SETUP_CLI_MISSING
         ? "To add Bluetooth support, run:"
         : "Run the commands that apply to this machine:");
    if (reason == SETUP_GENERAL || reason == SETUP_CLI_MISSING)
        print_install_command(identity);
    print_service_commands(identity);
    puts("  rfkill list bluetooth");
    puts("  sudo rfkill unblock bluetooth");
    puts("  bluetoothctl list");
    puts("");
    puts("If `bluetoothctl list` is still empty, enable Bluetooth in the firmware");
    puts("or hardware switch, or attach a supported USB Bluetooth adapter.");
    puts("Then run simpleblue again (or use `simpleblue --setup-help`).");
}

static void parse_device_listing(char *output)
{
    char *save = NULL;
    char *line;

    strip_terminal_sequences(output);
    for (line = strtok_r(output, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        char *start = trim(line);
        char address[18];
        char *name;
        if (strncmp(start, "Device ", 7) || strlen(start) < 24) continue;
        memcpy(address, start + 7, 17);
        address[17] = '\0';
        name = strlen(start) > 25 ? start + 25 : "";
        add_device(address, name);
    }
}

static int parse_first_integer(const char *text, int fallback)
{
    char *end;
    long value;
    const char *decimal = strchr(text, '(');

    /* BlueZ prints signed properties as e.g. 0xffffffae (-82). */
    if (decimal) {
        errno = 0;
        value = strtol(decimal + 1, &end, 10);
        if (!errno && end > decimal + 1 && *end == ')' &&
            value >= -100000 && value <= 100000) return (int)value;
    }
    while (*text && isspace((unsigned char)*text)) text++;
    errno = 0;
    value = strtol(text, &end, 0);
    if (errno || end == text || value < -100000 || value > 100000)
        return fallback;
    return (int)value;
}

/* Read the byte columns only, never bluetoothctl's ASCII rendering. */
static size_t parse_hex_line(const char *line, unsigned char bytes[16])
{
    size_t count = 0;

    while (count < 16 && isxdigit((unsigned char)line[0]) && line[1] &&
           isxdigit((unsigned char)line[1]) &&
           (!line[2] || isspace((unsigned char)line[2]))) {
        char hex[3] = {line[0], line[1], '\0'};
        bytes[count++] = (unsigned char)strtoul(hex, NULL, 16);
        line += 2;
        if (!*line || (isspace((unsigned char)line[0]) &&
                      isspace((unsigned char)line[1]))) break;
        line++;
    }
    return count;
}

static bool printable_utf8(const unsigned char *text, size_t length)
{
    for (size_t i = 0; i < length;) {
        unsigned char first = text[i++];
        size_t extra;
        unsigned int codepoint;

        if (first < 0x80) {
            if (first < 0x20 || first == 0x7f) return false;
            continue;
        }
        if (first >= 0xc2 && first <= 0xdf) {
            extra = 1;
            codepoint = first & 0x1f;
        } else if (first >= 0xe0 && first <= 0xef) {
            extra = 2;
            codepoint = first & 0x0f;
        } else if (first >= 0xf0 && first <= 0xf4) {
            extra = 3;
            codepoint = first & 0x07;
        } else return false;
        if (extra > length - i) return false;
        for (size_t j = 0; j < extra; j++) {
            unsigned char next = text[i++];
            if ((next & 0xc0) != 0x80) return false;
            codepoint = (codepoint << 6) | (next & 0x3f);
        }
        if ((extra == 2 && codepoint < 0x800) ||
            (extra == 3 && codepoint < 0x10000) || codepoint > 0x10ffff ||
            (codepoint >= 0xd800 && codepoint <= 0xdfff) ||
            (codepoint >= 0x80 && codepoint <= 0x9f)) return false;
    }
    return true;
}

static void update_service_name(Device *device, const unsigned char *bytes,
                                size_t count)
{
    size_t length = 0;

    while (length < count && bytes[length]) length++;
    if (!device || !length || length >= sizeof(device->service_name) ||
        !printable_utf8(bytes, length)) return;
    /* This name field can be padded with NULs; reject structured/binary data. */
    for (size_t i = length; i < count; i++) if (bytes[i]) return;
    memcpy(device->service_name, bytes, length);
    device->service_name[length] = '\0';
    sanitize_name(device->service_name);
}

static void parse_info_output(char *output)
{
    char *save = NULL;
    char *line;
    Device *current = NULL;
    unsigned char name_bytes[MAX_DEVICE_NAME];
    size_t name_count = 0;
    bool reading_name = false;

    strip_terminal_sequences(output);
    for (line = strtok_r(output, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        char *clean = trim(line);
        char *marker = strstr(clean, "Device ");

        if (reading_name) {
            unsigned char bytes[16];
            size_t count = parse_hex_line(clean, bytes);
            if (count) {
                if (count > sizeof(name_bytes) - name_count) {
                    reading_name = false;
                    name_count = 0;
                } else {
                    memcpy(name_bytes + name_count, bytes, count);
                    name_count += count;
                }
                continue;
            }
            update_service_name(current, name_bytes, name_count);
            reading_name = false;
        }
        /* Discovery events interleaved with an info reply do not change which
         * device owns the following indented properties. */
        if (strstr(clean, "[CHG]") || strstr(clean, "[NEW]") ||
            strstr(clean, "[DEL]")) continue;
        if (marker && strlen(marker) >= 24) {
            char address[18];
            memcpy(address, marker + 7, 17);
            address[17] = '\0';
            current = valid_address(address) ? find_device(address) : NULL;
            continue;
        }
        if (!current) continue;
        if (!strncmp(clean, "Name:", 5)) {
            update_device_name(current, trim(clean + 5));
        } else if (!strncmp(clean, "Alias:", 6)) {
            update_device_name(current, trim(clean + 6));
        } else if (!strncmp(clean, "Icon:", 5)) {
            copy_text(current->icon, sizeof(current->icon), trim(clean + 5));
        } else if (!strncmp(clean, "Paired:", 7)) {
            current->paired = !strcasecmp(trim(clean + 7), "yes");
        } else if (!strncmp(clean, "Bonded:", 7) &&
                   !strcasecmp(trim(clean + 7), "yes")) {
            current->paired = true;
        } else if (!strncmp(clean, "Trusted:", 8)) {
            current->trusted = !strcasecmp(trim(clean + 8), "yes");
        } else if (!strncmp(clean, "Connected:", 10)) {
            current->connected = !strcasecmp(trim(clean + 10), "yes");
        } else if (!strncmp(clean, "Blocked:", 8)) {
            current->blocked = !strcasecmp(trim(clean + 8), "yes");
        } else if (!strncmp(clean, "RSSI:", 5)) {
            current->rssi = parse_first_integer(clean + 5, current->rssi);
        } else if (!strncmp(clean, "ManufacturerData.Key:", 21)) {
            int manufacturer = parse_first_integer(clean + 21, -1);
            if (manufacturer >= 0 && manufacturer <= 65535)
                current->manufacturer = manufacturer;
        } else if (!strcasecmp(clean, "ServiceData." SERVICE_NAME_UUID ":")) {
            /* Captured Motorola advertisements put their UTF-8 device name
             * in this service field instead of BlueZ's Name property. */
            reading_name = true;
            name_count = 0;
        } else if (!strncmp(clean, "Battery Percentage:", 19)) {
            int battery = parse_first_integer(clean + 19, -1);
            if (battery >= 0 && battery <= 100) current->battery = battery;
        }
    }
    if (reading_name) update_service_name(current, name_bytes, name_count);
}

static void parse_scan_output(char *output)
{
    char *save = NULL;
    char *line;

    strip_terminal_sequences(output);
    for (line = strtok_r(output, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        char *marker = strstr(line, "Device ");
        char address[18];
        char *property;
        Device *device;

        if (!marker || strlen(marker) < 24) continue;
        memcpy(address, marker + 7, 17);
        address[17] = '\0';
        if (!valid_address(address)) continue;
        property = trim(marker + 24);
        if (strstr(line, "[DEL]")) {
            for (int i = 0; i < app.device_count; i++) {
                if (strcasecmp(app.devices[i].address, address)) continue;
                memmove(&app.devices[i], &app.devices[i + 1],
                        (size_t)(app.device_count - i - 1) * sizeof(Device));
                app.device_count--;
                break;
            }
            continue;
        }
        device = find_device(address);
        if (!device && (strstr(line, "[NEW]") || strstr(line, "[CHG]") ||
                        !strncmp(property, "Name:", 5) ||
                        !strncmp(property, "Alias:", 6)))
            device = add_device(address, "");
        if (!device) continue;
        if (!strncmp(property, "RSSI:", 5))
            device->rssi = parse_first_integer(property + 5, device->rssi);
        else if (!strncmp(property, "ManufacturerData.Key:", 21)) {
            int manufacturer = parse_first_integer(property + 21, -1);
            if (manufacturer >= 0 && manufacturer <= 65535)
                device->manufacturer = manufacturer;
        }
        else if (!strncmp(property, "Icon:", 5))
            copy_text(device->icon, sizeof(device->icon), trim(property + 5));
        else if (!strncmp(property, "Connected:", 10))
            device->connected = !strcasecmp(trim(property + 10), "yes");
        else if (!strncmp(property, "Paired:", 7))
            device->paired = !strcasecmp(trim(property + 7), "yes");
        else if (!strncmp(property, "Trusted:", 8))
            device->trusted = !strcasecmp(trim(property + 8), "yes");
        else if (!strncmp(property, "Blocked:", 8))
            device->blocked = !strcasecmp(trim(property + 8), "yes");
        else if (!strncmp(property, "Name:", 5) ||
                 !strncmp(property, "Alias:", 6)) {
            char *name = strchr(property, ':') + 1;
            update_device_name(device, trim(name));
        } else if (strstr(line, "[NEW]") && *property) {
            update_device_name(device, property);
        }
    }
}

static int compare_devices(const void *left, const void *right)
{
    const Device *a = left;
    const Device *b = right;

    /* Visible rows occupy the front of the array, so selection and device
     * actions keep using the same indices while anonymous results stay cached. */
    if (device_visible(a) != device_visible(b))
        return device_visible(b) - device_visible(a);
    if (a->connected != b->connected) return b->connected - a->connected;
    if (a->paired != b->paired) return b->paired - a->paired;
    if (a->trusted != b->trusted) return b->trusted - a->trusted;
    if ((a->rssi > -127) != (b->rssi > -127))
        return (b->rssi > -127) - (a->rssi > -127);
    if (a->rssi != b->rssi) return b->rssi - a->rssi;
    return strcasecmp(a->name, b->name);
}

static void sort_devices(const char *selected_address)
{
    int visible;

    qsort(app.devices, (size_t)app.device_count, sizeof(app.devices[0]),
          compare_devices);
    app.selected = 0;
    if (selected_address && *selected_address) {
        for (int i = 0; i < app.device_count; i++) {
            if (!strcasecmp(app.devices[i].address, selected_address)) {
                app.selected = i;
                break;
            }
        }
    }
    visible = visible_device_count();
    if (app.selected >= visible) app.selected = visible ? visible - 1 : 0;
    if (app.top > app.selected) app.top = app.selected;
}

static void toggle_unnamed_devices(void)
{
    char selected_address[18] = "";

    if (visible_device_count())
        copy_text(selected_address, sizeof(selected_address),
                  app.devices[app.selected].address);
    show_unnamed = !show_unnamed;
    sort_devices(selected_address);
    if (app.message_summary) update_device_summary();
}

static bool load_devices(void)
{
    Device previous[MAX_DEVICES];
    int previous_count = app.device_count;
    char *listing = malloc(MAX_OUTPUT);
    char *details = malloc(MAX_OUTPUT);
    char selected_address[18] = "";
    char *script = NULL;
    char *list_argv[] = {"bluetoothctl", "devices", NULL};
    char *shell_argv[] = {"bluetoothctl", NULL};
    int status;

    if (!listing || !details) goto allocation_failed;
    if (app.device_count && app.selected < app.device_count)
        copy_text(selected_address, sizeof(selected_address),
                  app.devices[app.selected].address);
    status = run_program(list_argv, NULL, listing, MAX_OUTPUT,
                         COMMAND_TIMEOUT_MS);
    if (status != 0 || output_failed(listing)) {
        char summary[256];
        output_summary(listing, summary, sizeof(summary));
        set_message(true, "Could not list Bluetooth devices%s%s.",
                    summary[0] ? ": " : "", summary);
        free(listing);
        free(details);
        return false;
    }
    memcpy(previous, app.devices, sizeof(previous));
    memset(app.devices, 0, sizeof(app.devices));
    app.device_count = 0;
    parse_device_listing(listing);

    script = malloc((size_t)app.device_count * 32 + 8);
    if (!script) goto allocation_failed;
    script[0] = '\0';
    for (int i = 0; i < app.device_count; i++) {
        strcat(script, "info ");
        strcat(script, app.devices[i].address);
        strcat(script, "\n");
    }
    strcat(script, "quit\n");
    status = run_program(shell_argv, script, details, MAX_OUTPUT,
                         COMMAND_TIMEOUT_MS);
    if (status == 0 || details[0]) parse_info_output(details);
    for (int i = 0; i < app.device_count; i++) {
        Device *device = &app.devices[i];
        for (int j = 0; j < previous_count; j++) {
            if (!strcasecmp(device->address, previous[j].address)) {
                if (address_name(device, device->name))
                    update_device_name(device, previous[j].name);
                if (!device->service_name[0])
                    copy_text(device->service_name, sizeof(device->service_name),
                              previous[j].service_name);
                break;
            }
        }
    }
    sort_devices(selected_address);
    free(script);
    free(listing);
    free(details);
    return true;

allocation_failed:
    free(script);
    free(listing);
    free(details);
    set_message(true, "Not enough memory to read Bluetooth devices.");
    return false;
}

static pid_t start_discovery(FILE *log, int seconds)
{
    pid_t child = fork();

    if (child == 0) {
        char duration[16];
        int null_fd = open("/dev/null", O_RDWR);
        int output_fd = log ? fileno(log) : null_fd;
        char *argv[] = {"bluetoothctl", "--timeout", duration, "scan", "on", NULL};

        if (null_fd < 0) _exit(126);
        snprintf(duration, sizeof(duration), "%d", seconds);
        dup2(null_fd, STDIN_FILENO);
        dup2(output_fd, STDOUT_FILENO);
        dup2(output_fd, STDERR_FILENO);
        if (null_fd > STDERR_FILENO) close(null_fd);
        if (log && output_fd > STDERR_FILENO) close(output_fd);
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGPIPE, SIG_DFL);
        setenv("LC_ALL", "C", 1);
        execvp(argv[0], argv);
        _exit(errno == ENOENT ? 127 : 126);
    }
    return child;
}

static void close_discovery_fds(void)
{
    if (discovery_input_fd >= 0) close(discovery_input_fd);
    if (discovery_output_fd >= 0) close(discovery_output_fd);
    discovery_input_fd = discovery_output_fd = -1;
}

static void stop_live_discovery(void)
{
    if (discovery_pid > 0) {
        long long deadline = monotonic_ms() + 200;
        bool exited = false;

        if (discovery_input_fd >= 0)
            (void)write_bytes(discovery_input_fd, "scan off\nquit\n", 14);
        close_discovery_fds();
        do {
            pid_t result = waitpid(discovery_pid, NULL, WNOHANG);
            if (result == discovery_pid || (result < 0 && errno == ECHILD)) {
                exited = true;
                break;
            }
            pause_ms(10);
        } while (monotonic_ms() < deadline);
        if (!exited) stop_child(discovery_pid, true);
    }
    close_discovery_fds();
    discovery_pid = -1;
    discovery_line_used = 0;
    discovery_line_overflow = false;
    discovery_refresh_pending = false;
}

static bool start_live_discovery(void)
{
    int input_pipe[2];
    int output_pipe[2];
    pid_t child;
    char commands[128];

    if (discovery_pid > 0) return true;
    if (pipe(input_pipe) < 0) goto failed;
    if (pipe(output_pipe) < 0) {
        close(input_pipe[0]);
        close(input_pipe[1]);
        goto failed;
    }
    child = fork();
    if (child == 0) {
        char *argv[] = {"bluetoothctl", NULL};

        setpgid(0, 0);
        dup2(input_pipe[0], STDIN_FILENO);
        dup2(output_pipe[1], STDOUT_FILENO);
        dup2(output_pipe[1], STDERR_FILENO);
        close(input_pipe[0]); close(input_pipe[1]);
        close(output_pipe[0]); close(output_pipe[1]);
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGPIPE, SIG_DFL);
        setenv("LC_ALL", "C", 1);
        execvp(argv[0], argv);
        _exit(errno == ENOENT ? 127 : 126);
    }
    close(input_pipe[0]);
    close(output_pipe[1]);
    if (child < 0) {
        close(input_pipe[1]); close(output_pipe[0]);
        goto failed;
    }
    (void)setpgid(child, child);
    discovery_pid = child;
    discovery_input_fd = input_pipe[1];
    discovery_output_fd = output_pipe[0];
    fcntl(discovery_input_fd, F_SETFD, FD_CLOEXEC);
    fcntl(discovery_output_fd, F_SETFD, FD_CLOEXEC);
    fcntl(discovery_output_fd, F_SETFL,
          fcntl(discovery_output_fd, F_GETFL, 0) | O_NONBLOCK);
    discovery_line_used = 0;
    discovery_line_overflow = false;
    /* Interactive output is flushed as events arrive. Noninteractive scan
     * output can remain buffered until bluetoothctl exits. The pairing client
     * owns its own agent; the discovery client needs no agent. */
    /* Match GNOME's Discoverable discovery filter. BlueZ releases this
     * session's visibility setting when its discovery client disconnects. */
    snprintf(commands, sizeof(commands),
             "agent off\nselect %s\nmenu scan\ndiscoverable on\nback\nscan on\n",
             app.adapter.address);
    if (!write_bytes(discovery_input_fd, commands, strlen(commands))) {
        stop_live_discovery();
        goto failed;
    }
    discovery_refresh_pending = true;
    next_device_refresh = 0;
    return true;

failed:
    set_message(true, "Could not start Bluetooth discovery.");
    return false;
}

static void apply_discovery_line(char *line)
{
    char selected_address[18] = "";
    char *clean;

    strip_terminal_sequences(line);
    clean = trim(line);
    if (strstr(clean, "Device ")) {
        bool details_changed = strstr(clean, "[NEW]") ||
            strstr(clean, "[DEL]") || strstr(clean, "ServiceData.") ||
            strstr(clean, "Name:") || strstr(clean, "Alias:");

        if (app.device_count && app.selected < app.device_count)
            copy_text(selected_address, sizeof(selected_address),
                      app.devices[app.selected].address);
        parse_scan_output(clean);
        sort_devices(selected_address);
        if (app.message_summary) update_device_summary();
        if (details_changed) discovery_refresh_pending = true;
    } else if (strstr(clean, "Controller ")) {
        char *power = strstr(clean, "Powered:");
        if (power) app.adapter.powered = !strcasecmp(trim(power + 8), "yes");
    } else if (strstr(clean, "Failed to ") ||
               strstr(clean, "No default controller available")) {
        set_message(true, "Bluetooth discovery failed: %s", clean);
        stop_live_discovery();
    }
}

static void consume_discovery_output(const char *bytes, size_t size)
{
    for (size_t i = 0; i < size; i++) {
        if (bytes[i] == '\n' || bytes[i] == '\r') {
            if (!discovery_line_overflow && discovery_line_used) {
                discovery_line[discovery_line_used] = '\0';
                apply_discovery_line(discovery_line);
            }
            discovery_line_used = 0;
            discovery_line_overflow = false;
        } else if (discovery_line_used + 1 < sizeof(discovery_line)) {
            discovery_line[discovery_line_used++] = bytes[i];
        } else discovery_line_overflow = true;
    }
}

static void poll_live_discovery(void)
{
    char bytes[4096];
    int status;
    pid_t result;

    if (discovery_pid <= 0) return;
    /* Bound each drain so a busy advertiser cannot starve keyboard input. */
    for (int i = 0; i < 16 && discovery_output_fd >= 0; i++) {
        ssize_t count = read(discovery_output_fd, bytes, sizeof(bytes));
        if (count > 0) consume_discovery_output(bytes, (size_t)count);
        else if (count < 0 && errno == EINTR) continue;
        else break;
    }
    if (discovery_pid <= 0) return;
    result = waitpid(discovery_pid, &status, WNOHANG);
    if (result == discovery_pid) {
        close_discovery_fds();
        discovery_pid = -1;
        set_message(true, "Bluetooth discovery stopped; press r to scan again.");
    } else if (!app.adapter.powered) stop_live_discovery();
}

static bool refresh_devices(void)
{
    if (!app.adapter.powered) {
        load_devices();
        set_message(true, "Bluetooth is powered off; press p to turn it on.");
        return false;
    }
    if (!load_devices()) return false;
    refresh_adapter();
    if (!app.adapter.powered) {
        set_message(true, "Bluetooth is powered off; press p to turn it on.");
        return false;
    }
    update_device_summary();
    return true;
}

static bool run_action(const char *verb, const char *address,
                       char *error, size_t error_size)
{
    char output[16384];
    char *argv[] = {"bluetoothctl", (char *)verb, (char *)address, NULL};
    int status = run_program(argv, NULL, output, sizeof(output),
                             COMMAND_TIMEOUT_MS);

    if (status != 0 || output_failed(output)) {
        output_summary(output, error, error_size);
        if (!error[0]) copy_text(error, error_size,
                                 status < 0 ? "command timed out" :
                                 "BlueZ returned an error");
        return false;
    }
    if (error_size) error[0] = '\0';
    return true;
}

static bool start_background_action(const char *verb, const char *address,
                                    const char *name)
{
    int result_pipe[2];
    pid_t child;

    if (background_action_pid > 0 && !strcmp(background_action, "scan"))
        stop_background_action();
    if (background_action_pid > 0) {
        set_message(true, "A Bluetooth operation is already in progress.");
        return false;
    }
    if (pipe(result_pipe) < 0) {
        set_message(true, "Could not create the Bluetooth result pipe.");
        return false;
    }
    child = fork();
    if (child < 0) {
        close(result_pipe[0]);
        close(result_pipe[1]);
        set_message(true, "Could not start the Bluetooth operation.");
        return false;
    }
    if (child == 0) {
        char error[256];
        close(result_pipe[0]);
        close_discovery_fds();
        setpgid(0, 0);
        bool succeeded = run_action(verb, address, error, sizeof(error));
        if (!succeeded && error[0]) {
            /* Retry interrupted/short writes; the exit status reports failure
             * even if the parent cannot receive the optional error detail. */
            (void)write_bytes(result_pipe[1], error, strlen(error));
        }
        close(result_pipe[1]);
        _exit(succeeded ? 0 : 1);
    }
    close(result_pipe[1]);
    (void)setpgid(child, child);
    background_action_pid = child;
    background_result_fd = result_pipe[0];
    copy_text(background_action, sizeof(background_action), verb);
    copy_text(background_name, sizeof(background_name), name);
    set_message(false, "%s %s in the background...",
                !strcmp(verb, "connect") ? "Connecting to" : "Disconnecting",
                name);
    return true;
}

static bool start_background_refresh(bool announce)
{
    FILE *result_file;
    pid_t child;

    if (background_action_pid > 0) {
        if (announce)
            set_message(true, "A Bluetooth operation is already in progress.");
        return false;
    }
    if (app.adapter.powered && !start_live_discovery()) return false;
    /* A complete device list can exceed pipe capacity. Use an anonymous file
     * so the worker never blocks waiting for the UI to receive its results. */
    result_file = tmpfile();
    if (!result_file) {
        set_message(true, "Could not create the Bluetooth scan result file.");
        return false;
    }
    fcntl(fileno(result_file), F_SETFD, FD_CLOEXEC);
    child = fork();
    if (child < 0) {
        fclose(result_file);
        set_message(true, "Could not start the Bluetooth scan.");
        return false;
    }
    if (child == 0) {
        bool succeeded;
        setpgid(0, 0);
        close_discovery_fds();
        succeeded = refresh_devices();
        if (fwrite(&app, sizeof(app), 1, result_file) != 1 ||
            fflush(result_file) != 0) succeeded = false;
        fclose(result_file);
        _exit(succeeded ? 0 : 1);
    }
    (void)setpgid(child, child);
    background_action_pid = child;
    background_scan_result = result_file;
    background_scan_baseline = app;
    background_scan_announced = announce;
    discovery_refresh_pending = false;
    last_device_refresh = monotonic_ms();
    next_device_refresh = last_device_refresh + DEVICE_REFRESH_MS;
    copy_text(background_action, sizeof(background_action), "scan");
    background_name[0] = '\0';
    if (announce)
        set_message(false, "Scanning for nearby Bluetooth devices...");
    return true;
}

static bool start_background_scan(void)
{
    return start_background_refresh(true);
}

/* A metadata query runs independently of discovery. Keep any live changes
 * received after the worker's snapshot, including a newly resolved name. */
static void merge_discovery_updates(App *scanned)
{
    for (int i = 0; i < scanned->device_count; i++) {
        const char *address = scanned->devices[i].address;
        if (!device_in_app(&background_scan_baseline, address) ||
            find_device(address)) continue;
        memmove(&scanned->devices[i], &scanned->devices[i + 1],
                (size_t)(scanned->device_count - i - 1) * sizeof(Device));
        scanned->device_count--;
        i--;
    }
    for (int i = 0; i < app.device_count; i++) {
        Device *live = &app.devices[i];
        Device *before = device_in_app(&background_scan_baseline, live->address);
        Device *after = device_in_app(scanned, live->address);

        if (!after) {
            if (!before && scanned->device_count < MAX_DEVICES)
                scanned->devices[scanned->device_count++] = *live;
            continue;
        }
        if (!before) {
            if (!address_name(live, live->name))
                copy_text(after->name, sizeof(after->name), live->name);
            continue;
        }
        if (strcmp(live->name, before->name))
            copy_text(after->name, sizeof(after->name), live->name);
        if (strcmp(live->icon, before->icon))
            copy_text(after->icon, sizeof(after->icon), live->icon);
        if (live->rssi != before->rssi) after->rssi = live->rssi;
        if (live->manufacturer != before->manufacturer)
            after->manufacturer = live->manufacturer;
        if (live->paired != before->paired) after->paired = live->paired;
        if (live->trusted != before->trusted) after->trusted = live->trusted;
        if (live->connected != before->connected) after->connected = live->connected;
        if (live->blocked != before->blocked) after->blocked = live->blocked;
    }
    if (app.adapter.powered != background_scan_baseline.adapter.powered)
        scanned->adapter.powered = app.adapter.powered;
}

static void poll_background_action(void)
{
    char detail[256] = "";
    int status;
    pid_t result;

    if (background_action_pid <= 0) return;
    result = waitpid(background_action_pid, &status, WNOHANG);
    if (result <= 0) return;
    if (background_result_fd >= 0) {
        ssize_t count;
        do {
            count = read(background_result_fd, detail, sizeof(detail) - 1);
        } while (count < 0 && errno == EINTR);
        if (count > 0) detail[count] = '\0';
        close(background_result_fd);
        background_result_fd = -1;
    }
    background_action_pid = -1;
    if (!strcmp(background_action, "scan")) {
        App scanned;
        char selected_address[18] = "";
        int top = app.top;
        bool received = false;

        if (app.device_count && app.selected < app.device_count)
            copy_text(selected_address, sizeof(selected_address),
                      app.devices[app.selected].address);
        if (background_scan_result) {
            rewind(background_scan_result);
            received = fread(&scanned, sizeof(scanned), 1,
                             background_scan_result) == 1;
            fclose(background_scan_result);
            background_scan_result = NULL;
        }
        if (received) {
            merge_discovery_updates(&scanned);
            if ((!background_scan_announced ||
                 (app.message_error &&
                  strcmp(app.message, background_scan_baseline.message))) &&
                WIFEXITED(status) &&
                WEXITSTATUS(status) == 0) {
                copy_text(scanned.message, sizeof(scanned.message), app.message);
                scanned.message_error = app.message_error;
                scanned.message_summary = app.message_summary;
            }
            app = scanned;
            app.top = top;
            sort_devices(selected_address);
            if (app.message_summary) update_device_summary();
        }
        if (!received || !WIFEXITED(status))
            set_message(true, "Bluetooth scan ended before returning its results.");
        else if (WEXITSTATUS(status) != 0 && !app.message_error)
            set_message(true, "Could not save the Bluetooth scan results.");
        return;
    }
    load_devices();
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        set_message(false, "%s %s.",
                    !strcmp(background_action, "connect")
                        ? "Connected to" : "Disconnected",
                    background_name);
    else
        set_message(true, "Could not %s %s%s%s.", background_action,
                    background_name, detail[0] ? ": " : "", detail);
}

static void stop_background_action(void)
{
    if (background_action_pid > 0) {
        stop_child(background_action_pid, true);
        background_action_pid = -1;
    }
    if (background_result_fd >= 0) {
        close(background_result_fd);
        background_result_fd = -1;
    }
    if (background_scan_result) {
        fclose(background_scan_result);
        background_scan_result = NULL;
    }
}

static void poll_device_discovery(void)
{
    long long now;

    poll_live_discovery();
    if (discovery_pid <= 0 || background_action_pid > 0 ||
        !app.adapter.powered) return;
    now = monotonic_ms();
    if (now >= next_device_refresh ||
        (discovery_refresh_pending && now - last_device_refresh >= 500))
        start_background_refresh(false);
}

typedef struct {
    char line[4096];
    size_t used;
    int escape;
    char prompt[512];
    char error[256];
    bool waiting_input;
    bool finished;
    bool succeeded;
} PairOutput;

static void parse_pair_line(PairOutput *output, bool complete)
{
    char line[sizeof(output->line)];
    char *clean;
    char *shell_prompt;

    if (output->finished) return;
    copy_text(line, sizeof(line), output->line);
    clean = trim(line);
    shell_prompt = clean[0] == '[' ? strstr(clean, "]> ") : NULL;
    if (shell_prompt) clean = trim(shell_prompt + 3);
    if (!strncmp(clean, "[agent] ", 8)) {
        if (strcmp(output->prompt, clean)) {
            /* Prompts have no newline. Show them as soon as they arrive, but
             * suppress identical readline redraws amid discovery events. */
            printf("\r\033[K%s%s", clean, complete ? "\n" : " ");
            fflush(stdout);
            copy_text(output->prompt, sizeof(output->prompt), clean);
        }
        if (clean[strlen(clean) - 1] == ':' &&
            (strstr(clean, "(yes/no):") ||
             strstr(clean, "Enter PIN code:") ||
             strstr(clean, "Enter passkey (number in 0-999999):")))
            output->waiting_input = true;
    }
    if (!complete) return;
    if (!strcmp(clean, "Pairing successful")) {
        output->finished = true;
        output->succeeded = true;
    } else if (!strncmp(clean, "Failed to ", 10) ||
               !strncmp(clean, "No default controller", 21) ||
               !strncmp(clean, "Invalid command", 15) ||
               (!strncmp(clean, "Device ", 7) &&
                contains_ci(clean, "not available"))) {
        copy_text(output->error, sizeof(output->error), clean);
        output->finished = true;
    }
}

static void parse_pair_output(PairOutput *output, const char *bytes, size_t size)
{
    for (size_t i = 0; i < size; i++) {
        unsigned char value = (unsigned char)bytes[i];
        if (output->escape) {
            if (output->escape == 1)
                output->escape = value == '[' ? 2 : 0;
            else if (value >= 0x40 && value <= 0x7e) output->escape = 0;
            continue;
        }
        if (value == 0x1b) { output->escape = 1; continue; }
        if (value == '\r' || value == '\n') {
            parse_pair_line(output, true);
            output->used = 0;
        } else if (value == '\b' || value == 0x7f) {
            if (output->used) output->used--;
        } else if ((value == '\t' || value >= 0x20) &&
                   output->used + 1 < sizeof(output->line)) {
            output->line[output->used++] = (char)value;
        }
        output->line[output->used] = '\0';
    }
    parse_pair_line(output, false);
}

static bool write_bytes(int fd, const char *bytes, size_t size)
{
    while (size) {
        ssize_t count = write(fd, bytes, size);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        bytes += count;
        size -= (size_t)count;
    }
    return true;
}

static int run_pair_command(const char *address, char *error, size_t error_size,
                            int timeout_ms)
{
    int input_pipe[2], output_pipe[2];
    pid_t child;
    bool exited = false;
    PairOutput output = {0};
    char command[32];
    long long deadline = monotonic_ms() + timeout_ms;

    copy_text(error, error_size, "could not start pairing");
    if (stop_requested) {
        copy_text(error, error_size, "pairing cancelled");
        return -1;
    }
    if (!valid_address(address) || pipe(input_pipe) < 0) return -1;
    if (pipe(output_pipe) < 0) {
        close(input_pipe[0]); close(input_pipe[1]);
        return -1;
    }
    child = fork();
    if (child == 0) {
        /* Keep the shell interactive so BlueZ accepts PIN/confirmation input.
         * --timeout delays even successful commands until its timer expires;
         * SimpleBlue instead watches completion and owns the upper deadline. */
        char *argv[] = {"bluetoothctl", "--agent", "KeyboardDisplay", NULL};
        dup2(input_pipe[0], STDIN_FILENO);
        dup2(output_pipe[1], STDOUT_FILENO);
        dup2(output_pipe[1], STDERR_FILENO);
        close(input_pipe[0]); close(input_pipe[1]);
        close(output_pipe[0]); close(output_pipe[1]);
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGPIPE, SIG_DFL);
        setenv("LC_ALL", "C", 1);
        execvp(argv[0], argv);
        _exit(errno == ENOENT ? 127 : 126);
    }
    close(input_pipe[0]); close(output_pipe[1]);
    if (child < 0) {
        close(input_pipe[1]); close(output_pipe[0]);
        return -1;
    }
    fcntl(output_pipe[0], F_SETFL,
          fcntl(output_pipe[0], F_GETFL, 0) | O_NONBLOCK);
    snprintf(command, sizeof(command), "pair %s\n", address);
    if (!write_bytes(input_pipe[1], command, strlen(command))) goto done;
    while (!stop_requested && monotonic_ms() < deadline) {
        struct pollfd descriptors[] = {
            {output_pipe[0], POLLIN | POLLHUP, 0},
            {output.waiting_input ? STDIN_FILENO : -1, POLLIN | POLLHUP, 0}
        };
        char chunk[4096];
        ssize_t count;

        (void)poll(descriptors, 2, 50);
        do {
            count = read(output_pipe[0], chunk, sizeof(chunk));
            if (count > 0) parse_pair_output(&output, chunk, (size_t)count);
        } while (count > 0 && !output.finished && !stop_requested &&
                 monotonic_ms() < deadline);
        if (output.finished) break;
        if (descriptors[1].revents & (POLLIN | POLLHUP)) {
            count = read(STDIN_FILENO, chunk, sizeof(chunk));
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0 ||
                !write_bytes(input_pipe[1], chunk, (size_t)count)) {
                copy_text(output.error, sizeof(output.error), "pairing cancelled");
                break;
            }
            output.waiting_input = false;
        }
        if (!exited && waitpid(child, NULL, WNOHANG) == child) exited = true;
        if (exited && count == 0) {
            parse_pair_line(&output, true);
            break;
        }
    }
done:
    close(input_pipe[1]);
    close(output_pipe[0]);
    if (!exited) stop_child(child, false);
    if (output.succeeded) {
        if (error_size) error[0] = '\0';
        return 0;
    }
    copy_text(error, error_size, output.error[0] ? output.error :
              stop_requested ? "pairing cancelled" :
              monotonic_ms() >= deadline ? "pairing timed out" :
              "pairing session ended before completion");
    return -1;
}

static int run_interactive_pair(const Device *device, char *error,
                                size_t error_size)
{
    pid_t scanner = -1;
    int status = -1;
    bool pretrusted = false;
    char name[MAX_DEVICE_NAME];

    def_prog_mode();
    stop_background_action();
    endwin();
    format_device_name(device, name, sizeof(name));
    printf("\nPairing with %s...\n", name);
    puts("If asked, confirm matching codes or enter the PIN. Ctrl-C cancels.\n");
    fflush(stdout);

    /*
     * An unpaired BlueZ device can disappear as soon as discovery stops. Keep
     * a separate bluetoothctl discovery client alive while the pairing client
     * resolves and bonds the selected address.
     */
    scanner = start_discovery(NULL, 65);
    if (scanner > 0) {
        char trust_error[256];
        long long deadline = monotonic_ms() + 8000;

        /*
         * The Onyx and some other audio devices discard an untrusted bond
         * immediately after pairing. Trust the discovered device before the
         * pairing handshake so there is no post-pair race.
         */
        do {
            if (run_action("trust", device->address, trust_error,
                           sizeof(trust_error))) {
                pretrusted = true;
                break;
            }
            pause_ms(250);
        } while (monotonic_ms() < deadline && !stop_requested);
    }
    status = run_pair_command(device->address, error, error_size,
                              PAIR_TIMEOUT_MS);
    if (scanner > 0) {
        char stop_output[4096];
        char *stop_argv[] = {"bluetoothctl", "scan", "off", NULL};
        stop_child(scanner, false);
        (void)run_program(stop_argv, NULL, stop_output, sizeof(stop_output),
                          5000);
    }
    reset_prog_mode();
    keypad(stdscr, TRUE);
    curs_set(0);
    clearok(stdscr, TRUE);
    refresh();
    if (status != 0 && pretrusted && !device->trusted) {
        char trust_error[256];
        (void)run_action("untrust", device->address, trust_error,
                         sizeof(trust_error));
    }
    return status;
}

static void connect_selected(void)
{
    char address[18];
    char name[MAX_DEVICE_NAME];
    char error[256];
    Device *device;

    if (!visible_device_count()) return;
    device = &app.devices[app.selected];
    copy_text(address, sizeof(address), device->address);
    format_device_name(device, name, sizeof(name));
    if (device->blocked) {
        set_message(true, "%s is blocked; press b to unblock it first.", name);
        return;
    }
    if (device->connected) {
        start_background_action("disconnect", address, name);
        return;
    }
    if (!device->paired) {
        int status = run_interactive_pair(device, error, sizeof(error));
        if (status != 0) {
            load_devices();
            set_message(true, "Could not pair with %s: %s.", name, error);
            return;
        }
        /*
         * Trust the device before doing a full refresh.  Some audio devices
         * disconnect immediately after exchanging keys and discard an
         * untrusted bond while load_devices() is querying every device.
         */
        if (!run_action("trust", address, error, sizeof(error))) {
            load_devices();
            device = find_device(address);
            if (!device || !device->paired)
                set_message(true, "BlueZ did not save a pairing for %s.", name);
            else
                set_message(true, "Paired with %s, but could not trust it: %s",
                            name, error);
            return;
        }
        if (!run_action("connect", address, error, sizeof(error))) {
            load_devices();
            set_message(true, "Paired and trusted %s, but could not connect: %s",
                        name, error);
            return;
        }
        load_devices();
        set_message(false, "Paired, trusted, and connected %s.", name);
        return;
    }
    start_background_action("connect", address, name);
}

static void toggle_trust_selected(void)
{
    Device *device;
    char address[18];
    char name[MAX_DEVICE_NAME];
    char error[256];
    bool trusting;

    if (!visible_device_count()) return;
    device = &app.devices[app.selected];
    copy_text(address, sizeof(address), device->address);
    format_device_name(device, name, sizeof(name));
    trusting = !device->trusted;
    if (!run_action(trusting ? "trust" : "untrust", address, error,
                    sizeof(error)))
        set_message(true, "Could not %s %s: %s",
                    trusting ? "trust" : "untrust", name, error);
    else
        set_message(false, "%s is now %s.", name,
                    trusting ? "trusted" : "untrusted");
    load_devices();
}

static void toggle_block_selected(void)
{
    Device *device;
    char address[18];
    char name[MAX_DEVICE_NAME];
    char error[256];
    bool blocking;

    if (!visible_device_count()) return;
    device = &app.devices[app.selected];
    copy_text(address, sizeof(address), device->address);
    format_device_name(device, name, sizeof(name));
    blocking = !device->blocked;
    if (!run_action(blocking ? "block" : "unblock", address, error,
                    sizeof(error)))
        set_message(true, "Could not %s %s: %s",
                    blocking ? "block" : "unblock", name, error);
    else
        set_message(false, "%s is now %s.", name,
                    blocking ? "blocked" : "unblocked");
    load_devices();
}

static bool confirm_forget(const Device *device)
{
    int key;
    char name[MAX_DEVICE_NAME];

    format_device_name(device, name, sizeof(name));
    set_message(false, "Forget %s and remove its pairing? y yes · n cancel",
                name);
    draw();
    for (;;) {
        key = getch();
        if (key == 'y' || key == 'Y') return true;
        if (key == 'n' || key == 'N' || key == 27) {
            set_message(false, "Forget cancelled.");
            return false;
        }
    }
}

static void forget_selected(void)
{
    Device *device;
    char address[18];
    char name[MAX_DEVICE_NAME];
    char error[256];

    if (!visible_device_count()) return;
    device = &app.devices[app.selected];
    if (!confirm_forget(device)) return;
    copy_text(address, sizeof(address), device->address);
    format_device_name(device, name, sizeof(name));
    if (!run_action("remove", address, error, sizeof(error)))
        set_message(true, "Could not forget %s: %s", name, error);
    else
        set_message(false, "Forgot %s.", name);
    load_devices();
}

static void toggle_power(void)
{
    char output[8192];
    char error[256];
    const char *state = app.adapter.powered ? "off" : "on";
    char *argv[] = {"bluetoothctl", "power", (char *)state, NULL};
    int status;

    set_message(false, "Turning Bluetooth %s...", state);
    draw();
    status = run_program(argv, NULL, output, sizeof(output),
                         COMMAND_TIMEOUT_MS);
    if (status != 0 || output_failed(output)) {
        output_summary(output, error, sizeof(error));
        set_message(true, "Could not turn Bluetooth %s: %s", state,
                    error[0] ? error : "BlueZ returned an error");
        return;
    }
    pause_ms(250);
    refresh_adapter();
    if (app.adapter.powered) start_live_discovery();
    else stop_live_discovery();
    load_devices();
    set_message(false, "Bluetooth is powered %s%s.", state,
                app.adapter.powered ? "; scanning for nearby devices" : "");
}

static int rssi_percent(int rssi)
{
    int percent = 2 * (rssi + 100);
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    return percent;
}

static const char *device_type(const Device *device)
{
    if (strstr(device->icon, "audio")) return "audio";
    if (strstr(device->icon, "keyboard")) return "keyboard";
    if (strstr(device->icon, "mouse")) return "mouse";
    if (strstr(device->icon, "joystick")) return "gamepad";
    if (strstr(device->icon, "phone")) return "phone";
    if (strstr(device->icon, "computer")) return "computer";
    if (strstr(device->icon, "display")) return "display";
    if (strstr(device->icon, "printer")) return "printer";
    return "device";
}

static const char *device_state(const Device *device)
{
    if (device->blocked) return "blocked";
    if (device->connected) return "connected";
    if (device->paired && device->trusted) return "paired+trusted";
    if (device->paired) return "paired";
    if (device->trusted) return "trusted";
    return "available";
}

static void draw(void)
{
    int visible = LINES - 8;
    int device_rows = visible_device_count();
    int end;
    int name_width;

    erase();
    if (LINES < 10 || COLS < 48) {
        mvprintw(0, 0, "simpleblue: terminal too small");
        refresh();
        return;
    }
    if (visible < 1) visible = 1;
    if (app.selected < app.top) app.top = app.selected;
    if (app.selected >= app.top + visible)
        app.top = app.selected - visible + 1;
    end = app.top + visible;
    if (end > device_rows) end = device_rows;
    name_width = COLS - 43;
    if (name_width < 12) name_width = 12;

    attron(A_BOLD);
    mvprintw(1, 2, "simpleblue");
    attroff(A_BOLD);
    mvprintw(1, 14, "BlueZ · %.*s · %s", COLS - 32,
             app.adapter.name[0] ? app.adapter.name : app.adapter.address,
             app.adapter.powered ? "on" : "off");
    mvprintw(3, 2, "   %-*s %6s  %-10s %-14s", name_width,
             "device", "signal", "type", "state");
    for (int i = app.top, row = 4; i < end; i++, row++) {
        Device *device = &app.devices[i];
        char name[MAX_DEVICE_NAME];
        char signal[16] = "--";
        format_device_name(device, name, sizeof(name));
        if (device->rssi > -127)
            snprintf(signal, sizeof(signal), "%d%%", rssi_percent(device->rssi));
        if (i == app.selected) attron(A_REVERSE);
        mvprintw(row, 2, "%s%s %-*.*s %6s  %-10.10s %-14.14s",
                 device->connected ? "●" : " ",
                 device->trusted ? "★" : " ",
                 name_width, name_width, name, signal,
                 device_type(device), device_state(device));
        if (i == app.selected) attroff(A_REVERSE);
    }
    if (!device_rows)
        mvprintw(5, 4, app.adapter.powered
                 ? "Looking for device names. Press u to show unnamed devices."
                 : "Bluetooth is off. Press p to power it on.");
    if (app.message_error) attron(A_BOLD);
    mvaddnstr(LINES - 3, 2, app.message, COLS - 4);
    if (app.message_error) attroff(A_BOLD);
    mvhline(LINES - 2, 0, ' ', COLS);
    mvaddnstr(LINES - 2, 2,
              "Enter connect/disconnect   r scan   t trust   x forget",
              COLS - 4);
    mvhline(LINES - 1, 0, ' ', COLS);
    mvaddnstr(LINES - 1, 2,
              show_unnamed
              ? "b block   p power   u hide unnamed   ? help   ↑/↓ choose   q quit"
              : "b block   p power   u show unnamed   ? help   ↑/↓ choose   q quit",
              COLS - 4);
    refresh();
}

static void show_help(void)
{
    erase();
    attron(A_BOLD);
    mvprintw(1, 2, "simpleblue help");
    attroff(A_BOLD);
    mvprintw(3, 2, "Enter   connect or disconnect; unpaired devices pair first");
    mvprintw(4, 2, "r       refresh nearby devices; discovery continues while open");
    mvprintw(5, 2, "t       trust or untrust the selected device");
    mvprintw(6, 2, "x       forget the device and remove its saved pairing");
    mvprintw(7, 2, "b       block or unblock the selected device");
    mvprintw(8, 2, "p       turn the Bluetooth adapter on or off");
    mvprintw(9, 2, "↑/↓     choose a device; Page Up/Page Down jump through the list");
    mvprintw(11, 2, "u       show/hide devices that have no Bluetooth name yet");
    mvprintw(12, 2, "● connected   ★ trusted");
    mvprintw(LINES - 2, 2, "Press any key to return");
    refresh();
    getch();
}

static void usage(const char *program)
{
    printf("Usage: %s [--setup-help]\n", program);
    puts("Scan, pair, trust, connect, disconnect, block, and forget Bluetooth devices.");
}

#ifndef SIMPLEBLUE_TEST
int main(int argc, char **argv)
{
    struct sigaction stop_action = {0};
    SetupReason setup;
    int key;

    if (argc == 2 && (!strcmp(argv[1], "--help") ||
                      !strcmp(argv[1], "-h"))) {
        usage(argv[0]);
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--setup-help")) {
        print_setup_help(SETUP_GENERAL);
        return 0;
    }
    if (argc != 1) {
        usage(argv[0]);
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    stop_action.sa_handler = request_stop;
    sigemptyset(&stop_action.sa_mask);
    sigaction(SIGINT, &stop_action, NULL);
    sigaction(SIGTERM, &stop_action, NULL);
    setup = detect_adapter();
    if (setup != SETUP_GENERAL) {
        print_setup_help(setup);
        return 1;
    }
    setlocale(LC_ALL, "");
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    timeout(100);
    curs_set(0);
    set_message(false, app.adapter.powered
                ? "Scanning for nearby Bluetooth devices..."
                : "Bluetooth is powered off; press p to turn it on.");
    draw();
    if (app.adapter.powered) start_background_scan();
    else load_devices();
    while (!stop_requested) {
        int device_rows;

        poll_background_action();
        poll_device_discovery();
        draw();
        device_rows = visible_device_count();
        key = getch();
        if (key == ERR) continue;
        if (key == 'q' || key == 'Q') break;
        if ((key == KEY_UP || key == 'k') && app.selected > 0)
            app.selected--;
        else if ((key == KEY_DOWN || key == 'j') &&
                 app.selected + 1 < device_rows) app.selected++;
        else if (key == KEY_PPAGE) {
            app.selected -= 10;
            if (app.selected < 0) app.selected = 0;
        } else if (key == KEY_NPAGE) {
            app.selected += 10;
            if (app.selected >= device_rows)
                app.selected = device_rows ? device_rows - 1 : 0;
        } else if (key == 'r' || key == 'R') {
            start_background_scan();
        } else if (key == '\n' || key == KEY_ENTER) connect_selected();
        else if (key == 't' || key == 'T') toggle_trust_selected();
        else if (key == 'x' || key == 'X') forget_selected();
        else if (key == 'b' || key == 'B') toggle_block_selected();
        else if (key == 'p' || key == 'P') toggle_power();
        else if (key == 'u' || key == 'U') toggle_unnamed_devices();
        else if (key == '?') show_help();
    }
    stop_background_action();
    stop_live_discovery();
    endwin();
    return 0;
}
#endif
