#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

static int scan_active(void)
{
    const char *path = getenv("SIMPLEBLUE_MOCK_SCAN_STATE");
    FILE *file = path ? fopen(path, "r") : NULL;
    long pid = -1;

    if (!file) return 0;
    (void)fscanf(file, "%ld", &pid);
    fclose(file);
    return pid > 0 && kill((pid_t)pid, 0) == 0;
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
        if (scan_active()) {
            puts("\tName: Discovery Name");
            puts("\tIcon: audio-card");
            puts("\tManufacturerData.Key: 0x004c (76)");
        }
        puts("\tAlias: 22-33-44-55-66-77");
        puts("\tPaired: no");
        puts("\tTrusted: no");
        puts("\tBlocked: no");
        puts("\tConnected: no");
        puts("\tRSSI: 0xffffffae (-82)");
    }
}

static void pair_command(void)
{
    const char *mode = getenv("SIMPLEBLUE_MOCK_PAIR_MODE");
    const char *answer = NULL;
    char response[256];

    if (mode && !strcmp(mode, "exit")) exit(0);
    if (mode && !strcmp(mode, "stall")) {
        signal(SIGTERM, SIG_IGN);
        sleep(3);
    }
    puts("Agent registered");
    for (int i = 0; i < 100; i++)
        puts("[CHG] Device 00:12:6F:E3:0E:10 RSSI: -43");
    puts("[CHG] Device 22:33:44:55:66:77 Name: Pairing successful");
    if (mode && !strcmp(mode, "fail")) {
        puts("Failed to pair: org.bluez.Error.AuthenticationRejected");
    } else {
        if (mode && !strcmp(mode, "confirm")) {
            fputs("\033[0;93m[agent] Confirm passkey 123456 (yes/no): \033[0m", stdout);
            answer = "yes\n";
        } else if (mode && !strcmp(mode, "pin")) {
            fputs("[agent] Enter PIN code: ", stdout);
            answer = "0123\n";
        } else if (mode && !strcmp(mode, "passkey")) {
            fputs("[agent] Enter passkey (number in 0-999999): ", stdout);
            answer = "123456\n";
        } else if (mode && !strcmp(mode, "display")) {
            puts("[agent] Passkey: 123456");
        }
        fflush(stdout);
        if (answer && (!fgets(response, sizeof(response), stdin) ||
                       strcmp(response, answer)))
            puts("\nFailed to pair: org.bluez.Error.AuthenticationRejected");
        else
            puts("\nPairing successful");
    }
    fflush(stdout);
    /* An interactive bluetoothctl remains open after success or failure. */
    sleep(3);
}

static void interactive_commands(void)
{
    char line[256];

    puts("Waiting to connect to bluetoothd...\r\033[0;94m[bluetoothctl]> \033[0m");
    while (fgets(line, sizeof(line), stdin)) {
        char address[18];
        if (sscanf(line, "info %17s", address) == 1) print_info(address);
        else if (sscanf(line, "pair %17s", address) == 1) {
            char *command[] = {"bluetoothctl", "pair", address};
            log_invocation(3, command);
            pair_command();
        } else if (!strncmp(line, "quit", 4)) break;
    }
}

int main(int argc, char **argv)
{
    log_invocation(argc, argv);
    if (argc == 1 || has_word(argc, argv, "--agent")) {
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
        puts("Device 22:33:44:55:66:77 22-33-44-55-66-77");
        puts("Device 10:20:30:40:50:60 Desk Keyboard");
        puts("Device 00:12:6F:E3:0E:10 Raw speaker name");
        return 0;
    }
    if (has_words(argc, argv, "scan", "on")) {
        const char *state_path = getenv("SIMPLEBLUE_MOCK_SCAN_STATE");
        puts("SetDiscoveryFilter success");
        puts("Discovery started");
        puts("[\033[0;93mCHG\033[0m] Device 00:12:6F:E3:0E:10 RSSI: -38");
        puts("[\033[0;92mNEW\033[0m] Device C3:54:FE:51:2D:DB Ring f2");
        puts("[\033[0;93mCHG\033[0m] Device C3:54:FE:51:2D:DB RSSI: -61");
        if (!state_path)
            puts("[CHG] Device 22:33:44:55:66:77 Name: Pocket Sensor");
        puts("[NEW] Device 00:00:00:00:00:00 invalid");
        if (state_path) {
            FILE *state = fopen(state_path, "w");
            if (!state) return 2;
            fprintf(state, "%ld\n", (long)getpid());
            fclose(state);
            fflush(stdout);
            sleep(3);
        }
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
