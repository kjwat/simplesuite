#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int has_words(int argc, char **argv, const char *first,
                     const char *second)
{
    for (int i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], first) && !strcmp(argv[i + 1], second)) return 1;
    return 0;
}

static int has_word(int argc, char **argv, const char *word)
{
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], word)) return 1;
    return 0;
}

static void log_invocation(int argc, char **argv)
{
    const char *path = getenv("SIMPLEBLUE_MOCK_LOG");
    FILE *log;

    if (!path) return;
    log = fopen(path, "a");
    if (!log) return;
    for (int i = 1; i < argc; i++)
        fprintf(log, "%s%s", i == 1 ? "" : " ", argv[i]);
    fputc('\n', log);
    fclose(log);
}

static void print_info(const char *address)
{
    if (!strcmp(address, "00:12:6F:E3:0E:10")) {
        puts("Device 00:12:6F:E3:0E:10 (public)");
        puts("\tName: Raw speaker name");
        puts("\tAlias: Studio Speaker");
        puts("\tIcon: audio-card");
        puts("\tPaired: yes");
        puts("\tBonded: yes");
        puts("\tTrusted: yes");
        puts("\tBlocked: no");
        puts("\tConnected: yes");
        puts("\tRSSI: -43");
        puts("\tBattery Percentage: 0x4b (75)");
    } else if (!strcmp(address, "10:20:30:40:50:60")) {
        puts("Device 10:20:30:40:50:60 (public)");
        puts("\tAlias: Desk Keyboard");
        puts("\tIcon: input-keyboard");
        puts("\tPaired: yes");
        puts("\tTrusted: no");
        puts("\tBlocked: no");
        puts("\tConnected: no");
        puts("\tRSSI: -70");
    } else if (!strcmp(address, "22:33:44:55:66:77")) {
        puts("Device 22:33:44:55:66:77 (random)");
        puts("\tAlias: Pocket Sensor");
        puts("\tPaired: no");
        puts("\tTrusted: no");
        puts("\tBlocked: no");
        puts("\tConnected: no");
        puts("\tRSSI: -82");
    }
}

static void interactive_commands(void)
{
    char line[256];

    puts("Waiting to connect to bluetoothd...\r\033[0;94m[bluetoothctl]> \033[0m");
    while (fgets(line, sizeof(line), stdin)) {
        char address[18];
        if (sscanf(line, "info %17s", address) == 1) print_info(address);
        else if (!strncmp(line, "quit", 4)) break;
    }
}

int main(int argc, char **argv)
{
    log_invocation(argc, argv);
    if (argc == 1) {
        interactive_commands();
        return 0;
    }
    if (has_word(argc, argv, "list")) {
        puts("Controller AA:BB:CC:DD:EE:01 Spare Adapter");
        puts("Controller 11:22:33:44:55:66 Workstation [default]");
        return 0;
    }
    if (has_word(argc, argv, "show")) {
        puts("Controller 11:22:33:44:55:66 (public)");
        puts("\tName: workstation");
        puts("\tAlias: Workstation Bluetooth");
        puts("\tPowered: yes");
        puts("\tDiscovering: no");
        return 0;
    }
    if (has_word(argc, argv, "devices")) {
        puts("Device 00:00:00:00:00:00 invalid");
        puts("Device 22:33:44:55:66:77 Pocket Sensor");
        puts("Device 10:20:30:40:50:60 Desk Keyboard");
        puts("Device 00:12:6F:E3:0E:10 Raw speaker name");
        return 0;
    }
    if (has_words(argc, argv, "scan", "on")) {
        puts("SetDiscoveryFilter success");
        puts("Discovery started");
        puts("[\033[0;93mCHG\033[0m] Device 00:12:6F:E3:0E:10 RSSI: -38");
        puts("[\033[0;92mNEW\033[0m] Device C3:54:FE:51:2D:DB Ring f2");
        puts("[\033[0;93mCHG\033[0m] Device C3:54:FE:51:2D:DB RSSI: -61");
        puts("[NEW] Device 00:00:00:00:00:00 invalid");
        return 0;
    }
    if (has_words(argc, argv, "scan", "off")) {
        puts("Discovery stopped");
        return 0;
    }
    if (has_word(argc, argv, "connect")) {
        if (getenv("SIMPLEBLUE_MOCK_CONNECT_FAIL")) {
            puts("Failed to connect: org.bluez.Error.NotAvailable");
            return 1;
        }
        puts("Connection successful");
        return 0;
    }
    if (has_word(argc, argv, "disconnect")) {
        puts("Successful disconnected");
        return 0;
    }
    if (has_word(argc, argv, "trust") || has_word(argc, argv, "untrust")) {
        puts("Changing trust succeeded");
        return 0;
    }
    if (has_word(argc, argv, "block") || has_word(argc, argv, "unblock")) {
        puts("Changing block succeeded");
        return 0;
    }
    if (has_word(argc, argv, "remove")) {
        puts("Device has been removed");
        return 0;
    }
    if (has_word(argc, argv, "power")) {
        puts("Changing power succeeded");
        return 0;
    }
    return 2;
}
