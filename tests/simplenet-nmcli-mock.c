#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
        if (backend && !strcmp(backend, "iwd")) puts("Stations\nwlan-test");
        return 0;
    }
    if (!strcmp(program, "wpa_cli") && backend &&
        !strcmp(backend, "wpa") && argc > 1) {
        if (!strcmp(argv[argc - 1], "ping")) {
            puts("PONG");
            return 0;
        }
        if (!strcmp(argv[argc - 1], "list_networks")) {
            puts("network id / ssid / bssid / flags");
            puts("7\tmesh with spaces\tany\t[CURRENT]");
            return 0;
        }
        if (!strcmp(argv[argc - 1], "add_network")) {
            puts("7");
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
