#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SIMPLEBLUE_TEST 1
#include "../simpleblue.c"

static void reset_app(void)
{
    stop_background_action();
    memset(&app, 0, sizeof(app));
}

static void read_file(const char *path, char *contents, size_t size)
{
    FILE *file = fopen(path, "r");
    size_t count;

    assert(file);
    count = fread(contents, 1, size - 1, file);
    assert(!ferror(file));
    fclose(file);
    contents[count] = '\0';
}

static void check_parsers(void)
{
    char controllers[] =
        "Controller AA:BB:CC:DD:EE:01 Spare Adapter\n"
        "Controller 11:22:33:44:55:66 Main Adapter [default]\n";
    char listing[] =
        "Device 00:00:00:00:00:00 invalid\n"
        "Device 10:20:30:40:50:60 Initial name\n"
        "Device 00:12:6F:E3:0E:10 Speaker\n";
    char details[] =
        "\033[0;94m[prompt]>\033[0m\r\n"
        "Device 10:20:30:40:50:60 (public)\n"
        "\tAlias: Desk Keyboard\n"
        "\tIcon: input-keyboard\n"
        "\tBonded: yes\n"
        "\tTrusted: no\n"
        "\tConnected: no\n"
        "\tRSSI: -71\n"
        "Device 00:12:6F:E3:0E:10 (public)\n"
        "\tName: Studio Speaker\n"
        "\tAlias: 00-12-6f-e3-0e-10\n"
        "\tIcon: audio-card\n"
        "\tPaired: yes\n"
        "\tTrusted: yes\n"
        "\tConnected: yes\n"
        "\tBattery Percentage: 0x50 (80)\n";
    char scan[] =
        "[\033[0;93mCHG\033[0m] Device 10:20:30:40:50:60 RSSI: -40\n"
        "[NEW] Device C3:54:FE:51:2D:DB Ring f2\n"
        "[CHG] Device C3:54:FE:51:2D:DB RSSI: 0xffffffc2 (-62)\n"
        "[CHG] Device 10:20:30:40:50:60 Name: 10:20:30:40:50:60\n"
        "[CHG] Device 22:33:44:55:66:77 Alias: Pocket Sensor\n";
    Adapter adapter = {0};
    Device *keyboard;
    Device *speaker;
    Device *ring;

    assert(valid_address("11:22:33:44:55:66"));
    assert(!valid_address("00:00:00:00:00:00"));
    assert(!valid_address("FF:FF:FF:FF:FF:FF"));
    assert(!valid_address("not-an-address"));
    assert(parse_controller_list(controllers, &adapter));
    assert(!strcmp(adapter.address, "11:22:33:44:55:66"));
    assert(!strcmp(adapter.name, "Main Adapter"));

    reset_app();
    parse_device_listing(listing);
    assert(app.device_count == 2);
    parse_info_output(details);
    keyboard = find_device("10:20:30:40:50:60");
    speaker = find_device("00:12:6F:E3:0E:10");
    assert(keyboard && keyboard->paired && !keyboard->trusted);
    assert(keyboard->rssi == -71);
    assert(!strcmp(device_type(keyboard), "keyboard"));
    assert(speaker && speaker->connected && speaker->trusted);
    assert(speaker->battery == 80);
    assert(!strcmp(device_type(speaker), "audio"));
    parse_scan_output(scan);
    assert(keyboard->rssi == -40);
    assert(!strcmp(keyboard->name, "Desk Keyboard"));
    assert(!strcmp(find_device("22:33:44:55:66:77")->name, "Pocket Sensor"));
    ring = find_device("C3:54:FE:51:2D:DB");
    assert(ring && !strcmp(ring->name, "Ring f2") && ring->rssi == -62);
    sort_devices(NULL);
    assert(!strcmp(app.devices[0].name, "Studio Speaker"));
    assert(rssi_percent(-40) == 100);
    assert(rssi_percent(-75) == 50);
    assert(parse_first_integer("0xffffffae (-82)", -127) == -82);
    assert(parse_first_integer("0x4b (75)", -1) == 75);
    assert(parse_first_integer("invalid", -127) == -127);
    assert(output_failed("Failed to connect: org.bluez.Error.Failed"));
    assert(!output_failed("Connection successful"));
}

static void finish_background(void)
{
    long long deadline = monotonic_ms() + 3000;

    while (background_action_pid > 0 && monotonic_ms() < deadline) {
        poll_background_action();
        pause_ms(10);
    }
    assert(background_action_pid <= 0);
}

static void check_mock_backend(const char *mock_directory)
{
    char path[PATH_MAX * 2];
    char log_path[] = "/tmp/simpleblue-check.XXXXXX";
    char log[16384];
    char error[256];
    Device *speaker;
    Device *ring;
    int file;

    snprintf(path, sizeof(path), "%s:%s", mock_directory, getenv("PATH"));
    assert(setenv("PATH", path, 1) == 0);
    file = mkstemp(log_path);
    assert(file >= 0);
    close(file);
    assert(setenv("SIMPLEBLUE_MOCK_LOG", log_path, 1) == 0);

    reset_app();
    assert(detect_adapter() == SETUP_GENERAL);
    assert(!strcmp(app.adapter.address, "11:22:33:44:55:66"));
    assert(!strcmp(app.adapter.name, "Workstation Bluetooth"));
    assert(app.adapter.powered);
    assert(load_devices());
    assert(app.device_count == 3);
    speaker = find_device("00:12:6F:E3:0E:10");
    assert(speaker && speaker->connected && speaker->paired && speaker->trusted);
    assert(speaker->rssi == -43 && speaker->battery == 75);
    assert(find_device("10:20:30:40:50:60")->paired);
    assert(scan_devices());
    assert(app.device_count == 4);
    speaker = find_device("00:12:6F:E3:0E:10");
    ring = find_device("C3:54:FE:51:2D:DB");
    assert(speaker && speaker->rssi == -38);
    assert(ring && ring->rssi == -61 && !strcmp(ring->name, "Ring f2"));
    assert(!strcmp(find_device("22:33:44:55:66:77")->name, "Pocket Sensor"));
    assert(load_devices());
    /* A later query can still return BlueZ's address placeholder. */
    assert(!strcmp(find_device("22:33:44:55:66:77")->name, "Pocket Sensor"));
    assert(start_background_scan());
    app.selected = 1; /* Moving selection during a scan must survive it. */
    char selected_address[18];
    copy_text(selected_address, sizeof(selected_address),
              app.devices[app.selected].address);
    finish_background();
    assert(!app.message_error);
    assert(app.device_count == 4);
    assert(!strcmp(app.devices[app.selected].address, selected_address));
    ring = find_device("C3:54:FE:51:2D:DB");
    assert(ring && !strcmp(ring->name, "Ring f2") && ring->rssi == -61);
    assert(!strcmp(find_device("22:33:44:55:66:77")->name, "Pocket Sensor"));
    assert(!background_scan_result);

    app.adapter.powered = false;
    assert(start_background_scan());
    finish_background();
    assert(app.message_error && strstr(app.message, "powered off"));
    app.adapter.powered = true;

    assert(run_action("connect", "10:20:30:40:50:60", error,
                      sizeof(error)));
    assert(run_action("disconnect", "00:12:6F:E3:0E:10", error,
                      sizeof(error)));
    assert(run_action("trust", "10:20:30:40:50:60", error,
                      sizeof(error)));
    assert(run_action("block", "22:33:44:55:66:77", error,
                      sizeof(error)));
    assert(run_action("remove", "22:33:44:55:66:77", error,
                      sizeof(error)));

    assert(setenv("SIMPLEBLUE_MOCK_CONNECT_FAIL", "1", 1) == 0);
    assert(start_background_action("connect", "10:20:30:40:50:60",
                                   "Desk Keyboard"));
    finish_background();
    assert(app.message_error);
    assert(strstr(app.message, "org.bluez.Error.NotAvailable"));
    unsetenv("SIMPLEBLUE_MOCK_CONNECT_FAIL");

    read_file(log_path, log, sizeof(log));
    assert(strstr(log, "list\n"));
    assert(strstr(log, "show 11:22:33:44:55:66\n"));
    assert(strstr(log, "--timeout 36 scan on\n"));
    assert(strstr(log, "connect 10:20:30:40:50:60\n"));
    assert(strstr(log, "disconnect 00:12:6F:E3:0E:10\n"));
    assert(strstr(log, "trust 10:20:30:40:50:60\n"));
    assert(strstr(log, "block 22:33:44:55:66:77\n"));
    assert(strstr(log, "remove 22:33:44:55:66:77\n"));
    unlink(log_path);
    unsetenv("SIMPLEBLUE_MOCK_LOG");
}

static void check_live_scan_metadata(void)
{
    char state_path[] = "/tmp/simpleblue-discovery.XXXXXX";
    int state_file = mkstemp(state_path);
    char label[MAX_DEVICE_NAME];
    char listing[] =
        "Device 7B:B4:5F:F2:4C:E1 7B-B4-5F-F2-4C-E1\n"
        "Device 61:E3:4B:7D:14:63 61-E3-4B-7D-14-63\n";
    /* Captured shape of BlueZ's output for the two unnamed advertisements. */
    char details[] =
        "Device 7B:B4:5F:F2:4C:E1 (random)\n"
        "\tAlias: 7B-B4-5F-F2-4C-E1\n"
        "\tManufacturerData.Key: 0x004c (76)\n"
        "\tRSSI: 0xffffffa9 (-87)\n"
        "Device 61:E3:4B:7D:14:63 (random)\n"
        "\tAlias: 61-E3-4B-7D-14-63\n"
        "\tRSSI: 0xffffffa4 (-92)\n";
    Device *device;

    reset_app();
    parse_device_listing(listing);
    parse_info_output(details);
    device = find_device("7B:B4:5F:F2:4C:E1");
    assert(device && device->rssi == -87 && device->manufacturer == 0x004c);
    format_device_name(device, label, sizeof(label));
    assert(strstr(label, "Unnamed device (Apple data)"));
    assert(strstr(label, device->address));
    assert(address_name(device, device->name)); /* The hint is not a name. */
    update_device_name(device, "Kitchen Speaker");
    format_device_name(device, label, sizeof(label));
    assert(!strcmp(label, "Kitchen Speaker"));
    device = find_device("61:E3:4B:7D:14:63");
    format_device_name(device, label, sizeof(label));
    assert(!strcmp(label, "Unnamed device [61:E3:4B:7D:14:63]"));

    assert(state_file >= 0);
    close(state_file);
    assert(setenv("SIMPLEBLUE_MOCK_SCAN_STATE", state_path, 1) == 0);
    reset_app();
    assert(detect_adapter() == SETUP_GENERAL);
    assert(scan_devices_for(100));
    device = find_device("22:33:44:55:66:77");
    /* The mock only exposes these properties while its scanner is alive. */
    assert(device && !strcmp(device->name, "Discovery Name"));
    assert(device->manufacturer == 0x004c && device->rssi == -82);
    assert(!strcmp(device->icon, "audio-card"));
    unsetenv("SIMPLEBLUE_MOCK_SCAN_STATE");
    unlink(state_path);
}

static int pair_with_input(const char *mode, const char *answer, int timeout_ms,
                           char *output, size_t output_size,
                           char *error, size_t error_size)
{
    FILE *input_file = tmpfile();
    FILE *output_file = tmpfile();
    int saved_stdin = dup(STDIN_FILENO);
    int saved_stdout = dup(STDOUT_FILENO);
    int status;
    size_t count;
    long long started;

    assert(input_file && output_file && saved_stdin >= 0 && saved_stdout >= 0);
    assert(fputs(answer, input_file) >= 0);
    rewind(input_file);
    fflush(stdout);
    assert(dup2(fileno(input_file), STDIN_FILENO) >= 0);
    assert(dup2(fileno(output_file), STDOUT_FILENO) >= 0);
    assert(setenv("SIMPLEBLUE_MOCK_PAIR_MODE", mode, 1) == 0);
    started = monotonic_ms();
    status = run_pair_command("00:12:6F:E3:0E:10", error, error_size, timeout_ms);
    /* The mock stays alive for three seconds even after its successful reply.
     * Completion must follow the reply, and cancellation must also be bounded. */
    assert(monotonic_ms() - started < 1500);
    unsetenv("SIMPLEBLUE_MOCK_PAIR_MODE");
    fflush(stdout);
    assert(dup2(saved_stdin, STDIN_FILENO) >= 0);
    assert(dup2(saved_stdout, STDOUT_FILENO) >= 0);
    close(saved_stdin); close(saved_stdout);
    rewind(output_file);
    count = fread(output, 1, output_size - 1, output_file);
    assert(!ferror(output_file));
    output[count] = '\0';
    fclose(input_file); fclose(output_file);
    assert(!strstr(output, "[CHG]"));
    assert(!strstr(output, "Agent registered"));
    return status;
}

static void check_pairing(void)
{
    char output[8192], error[256];
    PairOutput parsed = {0};
    const char *chatter =
        "[CHG] Device 00:12:6F:E3:0E:10 Connected: yes\n"
        "[CHG] Device 22:33:44:55:66:77 Name: Pairing successful\n";
    const char *success = "\033[0;94m[bluetoothctl]> \033[0mPairing successful\r\n";

    parse_pair_output(&parsed, chatter, strlen(chatter));
    assert(!parsed.finished);
    /* Pipe reads can split terminal escapes and completion messages anywhere. */
    for (size_t i = 0; i < strlen(success); i++)
        parse_pair_output(&parsed, success + i, 1);
    assert(parsed.finished && parsed.succeeded);

    assert(pair_with_input("success", "", 2000, output, sizeof(output),
                           error, sizeof(error)) == 0);
    assert(!error[0] && !output[0]);
    assert(pair_with_input("confirm", "yes\n", 2000, output, sizeof(output),
                           error, sizeof(error)) == 0);
    assert(strstr(output, "Confirm passkey 123456 (yes/no):"));
    assert(pair_with_input("confirm", "no\n", 2000, output, sizeof(output),
                           error, sizeof(error)) != 0);
    assert(strstr(error, "AuthenticationRejected"));
    assert(pair_with_input("pin", "0123\n", 2000, output, sizeof(output),
                           error, sizeof(error)) == 0);
    assert(strstr(output, "Enter PIN code:"));
    assert(pair_with_input("passkey", "123456\n", 2000, output, sizeof(output),
                           error, sizeof(error)) == 0);
    assert(pair_with_input("display", "", 2000, output, sizeof(output),
                           error, sizeof(error)) == 0);
    assert(strstr(output, "Passkey: 123456"));
    assert(pair_with_input("fail", "", 2000, output, sizeof(output),
                           error, sizeof(error)) != 0);
    assert(strstr(error, "AuthenticationRejected"));
    assert(pair_with_input("exit", "", 2000, output, sizeof(output),
                           error, sizeof(error)) != 0);
    assert(strstr(error, "before completion"));
    assert(pair_with_input("stall", "", 100, output, sizeof(output),
                           error, sizeof(error)) != 0);
    assert(strstr(error, "timed out"));
    stop_requested = 1;
    assert(pair_with_input("stall", "", 2000, output, sizeof(output),
                           error, sizeof(error)) != 0);
    assert(strstr(error, "cancelled"));
    stop_requested = 0;
    errno = 0;
    assert(waitpid(-1, NULL, WNOHANG) == -1 && errno == ECHILD);
}

static void check_missing_backend_message(void)
{
    char empty_path[] = "/tmp/simpleblue-empty.XXXXXX";
    char output_path[] = "/tmp/simpleblue-setup.XXXXXX";
    char output[8192];
    char *original_path = strdup(getenv("PATH") ? getenv("PATH") : "");
    int output_file;
    int saved_stdout;
    int saved_stderr;

    assert(original_path);
    assert(mkdtemp(empty_path));
    assert(setenv("PATH", empty_path, 1) == 0);
    reset_app();
    assert(detect_adapter() == SETUP_CLI_MISSING);

    output_file = mkstemp(output_path);
    assert(output_file >= 0);
    saved_stdout = dup(STDOUT_FILENO);
    saved_stderr = dup(STDERR_FILENO);
    assert(saved_stdout >= 0 && saved_stderr >= 0);
    fflush(NULL);
    assert(dup2(output_file, STDOUT_FILENO) >= 0);
    assert(dup2(output_file, STDERR_FILENO) >= 0);
    print_setup_help(SETUP_CLI_MISSING);
    fflush(NULL);
    assert(dup2(saved_stdout, STDOUT_FILENO) >= 0);
    assert(dup2(saved_stderr, STDERR_FILENO) >= 0);
    close(saved_stdout);
    close(saved_stderr);
    close(output_file);

    read_file(output_path, output, sizeof(output));
    assert(strstr(output, "Bluetooth support is not installed yet"));
    assert(strstr(output, "optional BlueZ stack"));
    assert(strstr(output, "To add Bluetooth support, run:"));
    assert(strstr(output, "Then run simpleblue again"));

    assert(setenv("PATH", original_path, 1) == 0);
    free(original_path);
    unlink(output_path);
    rmdir(empty_path);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    signal(SIGPIPE, SIG_IGN);
    check_parsers();
    check_mock_backend(argv[1]);
    check_live_scan_metadata();
    check_pairing();
    check_missing_backend_message();
    puts("simpleblue checks passed");
    return 0;
}
