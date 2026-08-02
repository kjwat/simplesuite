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

#ifdef __FreeBSD__
static const char *argument_after(int argc, char **argv, const char *option)
{
    for (int i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], option)) return argv[i + 1];
    return NULL;
}

static void print_mock_lease(void)
{
    puts("lease {");
    printf("  fixed-address %s;\n",
           getenv("SIMPLENET_MOCK_DHCP_LEASE_MATCH")
               ? "192.168.1.151" : "192.168.1.199");
    puts("}");
}
#endif
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
    const char *freebsd_layout = getenv("SIMPLENET_MOCK_FREEBSD_LAYOUT");

    if (!strcmp(program, "sleepy")) {
        sleep(5);
        puts("mock sleepy action finished");
        return 0;
    }
    if (!strcmp(program, "sudo")) {
        append_args(args_path, argc, argv);
        if (getenv("SIMPLENET_MOCK_SUDO_OK")) {
            for (int i = 1; i < argc; i++)
                if (!strcmp(argv[i], "cat")) {
                    print_mock_lease();
                    return 0;
                }
            puts("mock sudo action activated");
            return 0;
        }
        fputs("sudo: a password is required\n", stderr);
        return 1;
    }
    if (!strcmp(program, "service")) {
        append_args(args_path, argc, argv);
        if (getenv("SIMPLENET_MOCK_STALE_ROUTE") && argc > 2 &&
            !strcmp(argv[2], "onestatus")) {
            if (getenv("SIMPLENET_MOCK_DHCLIENT_RUNNING")) {
                puts("mock dhclient is running");
                return 0;
            }
            puts("mock dhclient is not running");
            return 1;
        }
        puts("mock service action activated");
        return 0;
    }
    if (!strcmp(program, "cat")) {
        print_mock_lease();
        return 0;
    }
    if (!strcmp(program, "sysctl")) {
        if (argc > 2 && !strcmp(argv[1], "-n") &&
            !strcmp(argv[2], "net.wlan.devices")) {
            puts((getenv("SIMPLENET_MOCK_STALE_ROUTE") || freebsd_layout)
                     ? "run0 iwlwifi0" : "mockwifi0");
            return 0;
        }
        return 1;
    }
    if (!strcmp(program, "pciconf")) return 1;
    if (!strcmp(program, "route")) {
        const char *destination = argc > 1 ? argv[argc - 1] : "";
        int stale_route = getenv("SIMPLENET_MOCK_STALE_ROUTE") != NULL;
        int stale_removed = file_contains(args_path,
            "ifconfig\nusb-backup\ninet\n192.168.1.151\ndelete\n");
        const char *interface_name = "wlan-test";

        if (stale_route)
            interface_name = !strcmp(destination, "default") || stale_removed
                ? "radio-main" : "usb-backup";
        else if (freebsd_layout)
            interface_name = !strcmp(freebsd_layout, "stale-default") &&
                             !strcmp(destination, "default")
                ? "wlan0" : "radio-main";

        puts("   route to: 0.0.0.0");
        puts("destination: 0.0.0.0");
        puts("       mask: 0.0.0.0");
        if (!strcmp(destination, "default"))
            puts("    gateway: 192.168.1.1");
        printf("  interface: %s\n", interface_name);
        return 0;
    }
#endif
    if (!strcmp(program, "ifconfig")) {
#ifdef __FreeBSD__
        if (getenv("SIMPLENET_MOCK_STALE_ROUTE")) {
            int stale_removed = file_contains(
                args_path,
                "ifconfig\nusb-backup\ninet\n192.168.1.151\ndelete\n");
            if ((argc > 2 && !strcmp(argv[1], "-g") &&
                 !strcmp(argv[2], "wlan")) ||
                (argc > 1 && !strcmp(argv[1], "-l"))) {
                puts("radio-main usb-backup");
                return 0;
            }
            if (argc > 1 && !strcmp(argv[1], "radio-main")) {
                puts("radio-main: flags=8843<UP,BROADCAST,RUNNING,SIMPLEX,MULTICAST>");
                puts("\tinet 192.168.1.102 netmask 0xffffff00 broadcast 192.168.1.255");
                puts("\tgroups: wlan");
                puts("\tssid test channel 11 bssid aa:bb:cc:dd:ee:ff");
                puts("\tparent interface: run0");
                puts("\tstatus: associated");
                return 0;
            }
            if (argc > 1 && !strcmp(argv[1], "usb-backup")) {
                puts("usb-backup: flags=8843<UP,BROADCAST,RUNNING,SIMPLEX,MULTICAST>");
                if (!stale_removed)
                    puts("\tinet 192.168.1.151 netmask 0xffffff00 broadcast 192.168.1.255");
                puts("\tgroups: wlan");
                puts("\tssid \"\" channel 36");
                puts("\tparent interface: iwlwifi0");
                puts("\tstatus: no carrier");
                return 0;
            }
        }
        if (freebsd_layout) {
            if ((argc > 2 && !strcmp(argv[1], "-g") &&
                 !strcmp(argv[2], "wlan"))) {
                if (!strcmp(freebsd_layout, "fallback")) return 1;
                puts("wlan0 radio-main");
                return 0;
            }
            if (argc > 1 && !strcmp(argv[1], "-l")) {
                puts(!strcmp(freebsd_layout, "fallback")
                         ? "re0 radio-main" : "wlan0 radio-main");
                return 0;
            }
            if (argc > 1 && !strcmp(argv[1], "re0")) {
                puts("re0: flags=8843<UP,BROADCAST,RUNNING,SIMPLEX,MULTICAST>");
                puts("\tgroups: egress");
                return 0;
            }
            if (argc > 1 && !strcmp(argv[1], "wlan0")) {
                puts("wlan0: flags=8802<BROADCAST,SIMPLEX,MULTICAST>");
                puts("\tgroups: wlan");
                puts("\tparent interface: run0");
                puts("\tstatus: not associated");
                return 0;
            }
            if (argc > 1 && !strcmp(argv[1], "radio-main")) {
                puts("radio-main: flags=8843<UP,BROADCAST,RUNNING,SIMPLEX,MULTICAST>");
                puts("\tinet 192.168.1.102 netmask 0xffffff00 broadcast 192.168.1.255");
                puts("\tgroups: wlan");
                puts("\tssid test channel 11 bssid aa:bb:cc:dd:ee:ff");
                puts("\tparent interface: iwlwifi0");
                puts("\tstatus: associated");
                return 0;
            }
        }
        if (argc > 2 && !strcmp(argv[1], "-g") &&
            !strcmp(argv[2], "wlan")) {
            puts("wlan-test");
            return 0;
        }
#else
        if (getenv("SIMPLENET_MOCK_STALE_ROUTE")) {
            int stale_removed = file_contains(
                args_path,
                "ifconfig\nwlan2\ninet\n192.168.1.151\ndelete\n");
            if (argc > 1 && !strcmp(argv[1], "-l")) {
                puts("wlan0 wlan2");
                return 0;
            }
            if (argc > 1 && !strcmp(argv[1], "wlan0")) {
                puts("wlan0: flags=8843<UP,BROADCAST,RUNNING,SIMPLEX,MULTICAST>");
                puts("\tinet 192.168.1.102 netmask 0xffffff00 broadcast 192.168.1.255");
                puts("\tssid test channel 11 bssid aa:bb:cc:dd:ee:ff");
                puts("\tparent interface: run0");
                puts("\tstatus: associated");
                return 0;
            }
            if (argc > 1 && !strcmp(argv[1], "wlan2")) {
                puts("wlan2: flags=8843<UP,BROADCAST,RUNNING,SIMPLEX,MULTICAST>");
                if (!stale_removed)
                    puts("\tinet 192.168.1.151 netmask 0xffffff00 broadcast 192.168.1.255");
                puts("\tssid \"\" channel 36");
                puts("\tparent interface: iwlwifi0");
                puts("\tstatus: no carrier");
                return 0;
            }
        }
#endif
        if (argc > 1 && !strcmp(argv[1], "-l")) {
            puts("wlan-test");
            return 0;
        }
        puts("wlan-test: flags=8843<UP,BROADCAST,RUNNING,SIMPLEX,MULTICAST>");
        puts("\tgroups: wlan");
#ifdef __FreeBSD__
        puts("\tparent interface: mockwifi0");
        puts("\tstatus: associated");
#endif
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
#ifdef __FreeBSD__
        const char *requested_interface = argument_after(argc, argv, "-i");
#endif
        append_args(args_path, argc, argv);
        if (!strcmp(argv[argc - 1], "ping")) {
#ifdef __FreeBSD__
            if (freebsd_layout && requested_interface) {
                if (strcmp(requested_interface, "wlan0") &&
                    strcmp(requested_interface, "radio-main"))
                    return 1;
                if (strcmp(freebsd_layout, "stale-default") &&
                    strcmp(requested_interface, "radio-main"))
                    return 1;
            }
#endif
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
#ifdef __FreeBSD__
        for (int i = 1; i + 1 < argc; i++) {
            if (!strcmp(argv[i], "-g") &&
                !strcmp(argv[i + 1], "GENERAL.TYPE")) {
                puts("wifi");
                return 0;
            }
        }
#endif
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
