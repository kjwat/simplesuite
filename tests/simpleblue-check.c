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
        "\tAlias: Studio Speaker\n"
        "\tIcon: audio-card\n"
        "\tPaired: yes\n"
        "\tTrusted: yes\n"
        "\tConnected: yes\n"
        "\tBattery Percentage: 0x50 (80)\n";
    char scan[] =
        "[\033[0;93mCHG\033[0m] Device 10:20:30:40:50:60 RSSI: -40\n"
        "[NEW] Device C3:54:FE:51:2D:DB Ring f2\n"
        "[CHG] Device C3:54:FE:51:2D:DB RSSI: -62\n";
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
    ring = find_device("C3:54:FE:51:2D:DB");
    assert(ring && !strcmp(ring->name, "Ring f2") && ring->rssi == -62);
    sort_devices(NULL);
    assert(!strcmp(app.devices[0].name, "Studio Speaker"));
    assert(rssi_percent(-40) == 100);
    assert(rssi_percent(-75) == 50);
    assert(output_failed("Failed to connect: org.bluez.Error.Failed"));
    assert(!output_failed("Connection successful"));
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

    read_file(log_path, log, sizeof(log));
    assert(strstr(log, "list\n"));
    assert(strstr(log, "show 11:22:33:44:55:66\n"));
    assert(strstr(log, "--timeout 6 scan on\n"));
    assert(strstr(log, "scan off\n"));
    assert(strstr(log, "connect 10:20:30:40:50:60\n"));
    assert(strstr(log, "disconnect 00:12:6F:E3:0E:10\n"));
    assert(strstr(log, "trust 10:20:30:40:50:60\n"));
    assert(strstr(log, "block 22:33:44:55:66:77\n"));
    assert(strstr(log, "remove 22:33:44:55:66:77\n"));
    unlink(log_path);
    unsetenv("SIMPLEBLUE_MOCK_LOG");
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    check_parsers();
    check_mock_backend(argv[1]);
    puts("simpleblue checks passed");
    return 0;
}
