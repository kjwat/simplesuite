#include <stdio.h>
#include <string.h>

static int has_words(int argc, char **argv, const char *first,
                     const char *second)
{
    for (int i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], first) && !strcmp(argv[i + 1], second)) return 1;
    return 0;
}

int main(int argc, char **argv)
{
    if (has_words(argc, argv, "device", "status")) {
        puts("wlan-test:wifi:disconnected");
        return 0;
    }
    if (has_words(argc, argv, "wifi", "list")) {
        puts("*:home\\: east:AA\\:BB\\:CC\\:DD\\:EE\\:01:54:WPA2");
        puts(":home\\: east:AA\\:BB\\:CC\\:DD\\:EE\\:02:81:WPA2");
        puts(":coffee:12\\:34\\:56\\:78\\:90\\:AB:42:--");
        puts(":office:12\\:34\\:56\\:78\\:90\\:AC:36:WPA2 802.1X");
        return 0;
    }
    if (has_words(argc, argv, "wifi", "connect")) {
        char password[128] = "";

        for (int i = 1; i < argc; i++)
            if (strstr(argv[i], "correct horse")) return 9;
        if (fgets(password, sizeof(password), stdin) &&
            !strcmp(password, "correct horse\n")) {
            puts("Device 'wlan-test' successfully activated.");
            return 0;
        }
        return 8;
    }
    return 2;
}
