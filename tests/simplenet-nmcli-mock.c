#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void append_args(const char *path, int argc, char **argv)
{
    FILE *args;
    if (!path) return;
    args = fopen(path, "a");
    if (!args) return;
    for (int i = 1; i < argc; i++)
        fprintf(args, "%s\n", argv[i]);
    fclose(args);
}

static int file_contains(const char *path, const char *needle)
{
    FILE *file;
    char contents[8192];
    size_t count;

    if (!path || !needle) return 0;
    file = fopen(path, "r");
    if (!file) return 0;
    count = fread(contents, 1, sizeof(contents) - 1, file);
    fclose(file);
    contents[count] = '\0';
    return strstr(contents, needle) != NULL;
}

static int last_update_config_value(const char *path, int initial)
{
    FILE *file;
    char contents[8192];
    const char *cursor;
    const char marker[] = "set\nupdate_config\n";
    size_t count;
    int value = initial;

    if (!path) return value;
    file = fopen(path, "r");
    if (!file) return value;
    count = fread(contents, 1, sizeof(contents) - 1, file);
    fclose(file);
    contents[count] = '\0';
    cursor = contents;
    while ((cursor = strstr(cursor, marker)) != NULL) {
        cursor += sizeof(marker) - 1;
        if (*cursor == '0') value = 0;
        if (*cursor == '1') value = 1;
    }
    return value;
}

int main(int argc, char **argv)
{
    const char *program = strrchr(argv[0], '/');
    const char *backend = getenv("SIMPLENET_MOCK_BACKEND");
    const char *args_path = getenv("SIMPLENET_MOCK_ARGS");
    const char *stdin_path = getenv("SIMPLENET_MOCK_STDIN");
    FILE *args;
    FILE *input;
    char line[512] = "";
    int asks = 0;

    program = program ? program + 1 : argv[0];
#ifdef __FreeBSD__
    if (!strcmp(program, "sleepy")) {
        sleep(5);
        puts("mock sleepy action finished");
        return 0;
    }
    if (!strcmp(program, "sudo")) {
        append_args(args_path, argc, argv);
        if (getenv("SIMPLENET_MOCK_SUDO_OK")) {
            puts("mock sudo action activated");
            return 0;
        }
        fputs("sudo: a password is required\n", stderr);
        return 1;
    }
    if (!strcmp(program, "service")) {
        append_args(args_path, argc, argv);
        puts("mock service action activated");
        return 0;
    }
#endif
    if (!strcmp(program, "ifconfig")) {
        if (argc > 1 && !strcmp(argv[1], "-l")) {
            puts("wlan-test");
            return 0;
        }
        puts("wlan-test: flags=8843<UP,BROADCAST,RUNNING,SIMPLEX,MULTICAST>");
        puts("\tgroups: wlan");
        return 0;
    }
    if (!strcmp(program, "iw")) {
        const char *current = getenv("SIMPLENET_MOCK_CURRENT_BSSID");
        if (argc > 1 && !strcmp(argv[argc - 1], "link")) {
            if (current) printf("Connected to %s (on wlan-test)\n", current);
            else puts("Not connected.");
            return 0;
        }
        if (backend) puts("phy#0\n\tInterface wlan-test");
        return 0;
    }
    if (!strcmp(program, "iwctl")) {
        if (backend && !strcmp(backend, "iwd") && argc > 2 &&
            !strcmp(argv[1], "station") && !strcmp(argv[2], "list")) {
            puts("Stations\nwlan-test");
            return 0;
        }
        if (backend && !strcmp(backend, "iwd") && argc > 2 &&
            !strcmp(argv[1], "known-networks")) {
            append_args(args_path, argc, argv);
            if (argc > 3 && !strcmp(argv[3], "show")) {
                puts("Known Network");
                puts("  AutoConnect  yes");
            } else {
                puts("mock iwd preference saved");
            }
            return 0;
        }
    }
    if (!strcmp(program, "wpa_cli") && backend &&
        !strcmp(backend, "wpa") && argc > 1) {
        append_args(args_path, argc, argv);
        if (!strcmp(argv[argc - 1], "ping")) {
            puts("PONG");
            return 0;
        }
        if (!strcmp(argv[argc - 1], "list_networks")) {
            puts("network id / ssid / bssid / flags");
            puts("7\tmesh with spaces\tany\t[CURRENT]");
            puts("8\tcafe wifi\tany\t");
            return 0;
        }
        for (int i = 1; i + 2 < argc; i++) {
            if (!strcmp(argv[i], "get_network") &&
                !strcmp(argv[i + 2], "priority")) {
                if (!strcmp(argv[i + 1], "7"))
                    puts(file_contains(args_path,
                                       "set_network\n7\npriority\n10\n") ?
                         "10" : "4");
                else
                    puts("9");
                return 0;
            }
        }
        for (int i = 1; i + 1 < argc; i++) {
            if (!strcmp(argv[i], "get") &&
                !strcmp(argv[i + 1], "update_config")) {
                printf("%d\n", last_update_config_value(
                    args_path,
                    getenv("SIMPLENET_MOCK_UPDATE_CONFIG_ZERO") ? 0 : 1));
                return 0;
            }
        }
        if (!strcmp(argv[argc - 1], "status")) {
            const char *current = getenv("SIMPLENET_MOCK_CURRENT_BSSID");
            puts("wpa_state=COMPLETED");
            if (current) printf("bssid=%s\n", current);
#ifdef __FreeBSD__
            puts("ssid=mesh with spaces");
#endif
            puts("id=7");
            return 0;
        }
        if (!strcmp(argv[argc - 1], "add_network")) {
            puts("7");
            return 0;
        }
        if (!strcmp(argv[argc - 1], "save_config")) {
            if (getenv("SIMPLENET_MOCK_SAVE_FAIL") ||
                (getenv("SIMPLENET_MOCK_UPDATE_CONFIG_ZERO") &&
                 !last_update_config_value(args_path, 0))) {
                puts("FAIL");
                return 0;
            }
            puts("OK");
            return 0;
        }
        puts("OK");
        return 0;
    }
    if (!strcmp(program, "nmcli") && backend && !strcmp(backend, "nm")) {
        for (int i = 1; i < argc; i++) {
            if (!strcmp(argv[i], "status")) {
                puts("wlan-test:wifi:connected");
                return 0;
            }
        }
        for (int i = 1; i + 1 < argc; i++) {
            if (!strcmp(argv[i], "-g") &&
                !strcmp(argv[i + 1], "connection.autoconnect")) {
                puts("yes");
                return 0;
            }
            if (!strcmp(argv[i], "-g") &&
                !strcmp(argv[i + 1],
                        "connection.autoconnect-priority")) {
                puts("999");
                return 0;
            }
        }
    }

    if (!args_path || !stdin_path) return 2;
    args = fopen(args_path, "a");
    if (!args) return 3;
    for (int i = 1; i < argc; i++) {
        fprintf(args, "%s\n", argv[i]);
        if (!strcmp(argv[i], "--ask")) asks = 1;
    }
    if (fclose(args) != 0) return 4;

    if (!asks) {
        puts("mock action activated");
        return 0;
    }
    if (!fgets(line, sizeof(line), stdin)) return 5;
    input = fopen(stdin_path, "w");
    if (!input) return 6;
    fputs(line, input);
    if (fclose(input) != 0) return 7;

    puts("mock connection activated");
    if (getenv("SIMPLENET_MOCK_FAIL")) return 9;
    return 0;
}
