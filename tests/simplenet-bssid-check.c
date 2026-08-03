#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SIMPLENET_TEST_SHARED_BACKENDS 1
#define main simplenet_program_main
#include "../simplenet.c"
#undef main

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

static const char *function_text(const char *source, const char *marker,
                                 const char *after, char *function,
                                 size_t function_size)
{
    const char *start = strstr(after ? after : source, marker);
    const char *end;
    size_t length;

    assert(start);
    end = strstr(start, "\n}\n");
    assert(end);
    end += 3;
    length = (size_t)(end - start);
    assert(length < function_size);
    memcpy(function, start, length);
    function[length] = '\0';
    return end;
}

static void check_source_contract(void)
{
    char source[131072];
    char function[32768];
    const char *next;

    read_file("simplenet.c", source, sizeof(source));

    function_text(source, "static void connect_selected_networkmanager", NULL,
                  function, sizeof(function));
    assert(strstr(function, "802-11-wireless.bssid ''"));
    assert(!strstr(function, "ap->bssid"));

    function_text(source, "static int nmcli_connect_password", NULL,
                  function, sizeof(function));
    assert(!strstr(function, "\"bssid\""));

    function_text(source, "static void connect_selected_iwd", NULL,
                  function, sizeof(function));
    assert(!strstr(function, " roam "));
    assert(!strstr(function, "ap->bssid"));

    next = function_text(source, "static void connect_selected_wpa", NULL,
                         function, sizeof(function));
    assert(strstr(function, "set_network %s bssid any"));
    assert(!strstr(function, "ap->bssid"));
    function_text(source, "static void connect_selected_wpa", next,
                  function, sizeof(function));
    assert(strstr(function, "set_network %s bssid any"));
    assert(!strstr(function, "ap->bssid"));

    function_text(source, "static int pin_bssid(const char *bssid)\n{", NULL,
                  function, sizeof(function));
    assert(strstr(function, "iwctl debug %s roam %s"));
    assert(strstr(function, "wpa_cli -i %s bssid %s %s"));
}

int main(int argc, char **argv)
{
    char temp[] = "/tmp/simplenet-bssid-check.XXXXXX";
    char args_path[PATH_MAX];
    char stdin_path[PATH_MAX];
    char new_path[PATH_MAX * 2];
    char contents[32768];
    char output[MAX_TEXT];
    char uuid[128];
    char password[64] = "mock secret";
    AccessPoint ap = {0};

    assert(argc == 2);
    assert(mkdtemp(temp));
    snprintf(args_path, sizeof(args_path), "%s/args", temp);
    snprintf(stdin_path, sizeof(stdin_path), "%s/stdin", temp);
    snprintf(new_path, sizeof(new_path), "%s:%s", argv[1], getenv("PATH"));
    assert(setenv("PATH", new_path, 1) == 0);
    assert(setenv("SIMPLENET_MOCK_ARGS", args_path, 1) == 0);
    assert(setenv("SIMPLENET_MOCK_STDIN", stdin_path, 1) == 0);
    assert(setenv("SIMPLENET_MOCK_BACKEND", "wpa", 1) == 0);

    snprintf(wifi_device, sizeof(wifi_device), "wlan-test");
    backend = BACKEND_WPA_SUPPLICANT;
    assert(wpa_select_network("7"));
    read_file(args_path, contents, sizeof(contents));
    assert(strstr(contents, "set_network\n7\nbssid\nany\n"));
    assert(strstr(contents, "select_network\n7\n"));
    assert(strstr(contents, "save_config\n"));
#if defined(SIMPLENET_TEST_FREEBSD_WPA_PATH) || \
    (defined(__FreeBSD__) && !defined(SIMPLENET_TEST_LINUX_WPA_PATH))
    assert(strstr(contents, "reassociate\n"));
#else
    assert(!strstr(contents, "reassociate\n"));
#endif

    assert(unlink(args_path) == 0);
    snprintf(connection_uuid, sizeof(connection_uuid), "7");
    assert(pin_bssid("aa:bb:cc:dd:ee:ff"));
    read_file(args_path, contents, sizeof(contents));
    assert(strstr(contents, "bssid\n7\naa:bb:cc:dd:ee:ff\n"));
    assert(strstr(contents, "reassociate\n"));

    assert(unlink(args_path) == 0);
    assert(setenv("SIMPLENET_MOCK_BACKEND", "nm", 1) == 0);
    assert(networkmanager_network_uuid("mesh with spaces", uuid,
                                       sizeof(uuid)));
    assert(!strcmp(uuid, "uuid-mesh"));
    snprintf(ap.ssid, sizeof(ap.ssid), "mesh with spaces");
    snprintf(ap.bssid, sizeof(ap.bssid), "aa:bb:cc:dd:ee:ff");
    assert(nmcli_connect_password(&ap, password, sizeof(password), output,
                                  sizeof(output)));
    read_file(args_path, contents, sizeof(contents));
    assert(strstr(contents, "mesh with spaces\n"));
    assert(strstr(contents, "ifname\nwlan-test\n"));
    assert(!strstr(contents, "bssid\n"));

    check_source_contract();

    unlink(args_path);
    unlink(stdin_path);
    rmdir(temp);
    puts("simplenet ordinary-connect BSSID checks passed");
    return 0;
}
