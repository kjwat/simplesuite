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

static const char *after_word(int argc, char **argv, const char *word)
{
    for (int i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], word)) return argv[i + 1];
    return NULL;
}

static void log_arguments(int argc, char **argv)
{
    const char *path = getenv("SIMPLENET_MOCK_LOG");
    FILE *log;

    if (!path || !(log = fopen(path, "a"))) return;
    for (int i = 1; i < argc; i++)
        fprintf(log, "%s%s", i == 1 ? "" : "\t", argv[i]);
    fputc('\n', log);
    fclose(log);
}

int main(int argc, char **argv)
{
    const char *mode = getenv("SIMPLENET_MOCK_MODE");
    const char *uuid;

    log_arguments(argc, argv);
    if (has_words(argc, argv, "device", "status")) {
        puts("wlan-test:wifi:disconnected");
        return 0;
    }
    if (has_words(argc, argv, "wifi", "list")) {
        puts(":home\\: east:AA\\:BB\\:CC\\:DD\\:EE\\:01:54:WPA2");
        puts(":home\\: east:AA\\:BB\\:CC\\:DD\\:EE\\:02:81:WPA2");
        puts(":coffee:12\\:34\\:56\\:78\\:90\\:AB:42:--");
        puts(":office:12\\:34\\:56\\:78\\:90\\:AC:36:WPA2 802.1X");
        return 0;
    }
    if (has_words(argc, argv, "show", "uuid")) {
        uuid = after_word(argc, argv, "uuid");
        if (!uuid) return 3;
        if (!strcmp(uuid, "11111111-1111-4111-8111-111111111111") ||
            !strcmp(uuid, "22222222-2222-4222-8222-222222222222")) {
            puts("home\\: east");
            return 0;
        }
        if (!strcmp(uuid, "44444444-4444-4444-8444-444444444444")) {
            puts("office");
            return 0;
        }
        return 4;
    }
    if (has_words(argc, argv, "connection", "show")) {
        if (mode && !strcmp(mode, "profile-list-fail")) {
            fputs("NetworkManager is unavailable\n", stderr);
            return 7;
        }
        puts("11111111-1111-4111-8111-111111111111:802-11-wireless::200");
        puts("22222222-2222-4222-8222-222222222222:802-11-wireless::100");
        puts("33333333-3333-4333-8333-333333333333:802-3-ethernet::300");
        puts("44444444-4444-4444-8444-444444444444:802-11-wireless::50");
        return 0;
    }
    if (has_words(argc, argv, "connection", "up")) {
        char unexpected[8];

        uuid = after_word(argc, argv, "uuid");
        if (fgets(unexpected, sizeof(unexpected), stdin)) return 11;
        if (mode && !strcmp(mode, "saved-fail") && uuid &&
            !strcmp(uuid, "11111111-1111-4111-8111-111111111111")) {
            fputs("stored secret was rejected\n", stderr);
            return 10;
        }
        if (uuid &&
            (!strcmp(uuid, "11111111-1111-4111-8111-111111111111") ||
             !strcmp(uuid, "44444444-4444-4444-8444-444444444444"))) {
            puts("Connection successfully activated.");
            return 0;
        }
        return 12;
    }
    if (has_words(argc, argv, "wifi", "connect")) {
        char password[128] = "";

        for (int i = 1; i < argc; i++)
            if (strstr(argv[i], "correct horse")) return 9;
        if (after_word(argc, argv, "connect") &&
            !strcmp(after_word(argc, argv, "connect"), "coffee") &&
            !fgets(password, sizeof(password), stdin)) {
            puts("Device 'wlan-test' successfully activated.");
            return 0;
        }
        if (fgets(password, sizeof(password), stdin) &&
            !strcmp(password, "correct horse\n")) {
            puts("Device 'wlan-test' successfully activated.");
            return 0;
        }
        return 8;
    }
    return 2;
}
