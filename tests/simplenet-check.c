#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define main simplenet_program_main
#include "../simplenet.c"
#undef main

int main(void)
{
    char row[] = "*:mesh\\: west:A8\\:13\\:0B\\:EC\\:50\\:83:161:5805 MHz:72:WPA2";
    char *field[7];
    char quoted[128];
    char encoded[128];
    char temp[] = "/tmp/simplenet-check.XXXXXX";
    char args_path[PATH_MAX];
    char stdin_path[PATH_MAX];
    char executable[PATH_MAX];
    char build_dir[PATH_MAX];
    char new_path[PATH_MAX * 2];
    char contents[2048];
    char password[256] = "secret ! '$ with spaces";
    char output[512];
    double ping_loss;
    FILE *file;
    FILE *scan_file;
    ssize_t executable_length;
    unsigned int random_state = 0x51a7u;

    assert(split_nmcli(row, field, 7) == 7);
    assert(strcmp(field[0], "*") == 0);
    assert(strcmp(field[1], "mesh: west") == 0);
    assert(strcmp(field[2], "A8:13:0B:EC:50:83") == 0);
    assert(strcmp(field[4], "5805 MHz") == 0);
    assert(strcmp(field[6], "WPA2") == 0);
    assert(strcmp(band_name(2412), "2.4") == 0);
    assert(strcmp(band_name(5180), "5") == 0);
    assert(strcmp(band_name(5975), "6") == 0);
    assert(signal_percent(-90) == 0);
    assert(signal_percent(-60) == 50);
    assert(signal_percent(-30) == 100);
    {
        const char scan_text[] =
            "BSS aa:bb:cc:dd:ee:ff(on wlan0) -- associated\n"
            "\tfreq: 5180.0\n"
            "\tsignal: -48.00 dBm\n"
            "\tSSID: mesh home\n"
            "\tRSN:\n"
            "\t * Authentication suites: SAE\n"
            "BSS 11:22:33:44:55:66(on wlan0)\n"
            "\tfreq: 2412.0\n"
            "\tsignal: -78.00 dBm\n"
            "\tSSID: cafe\n";
        scan_file = tmpfile();
        assert(scan_file);
        assert(fwrite(scan_text, 1, sizeof(scan_text) - 1, scan_file) ==
               sizeof(scan_text) - 1);
        rewind(scan_file);
        ap_count = 0;
        assert(parse_iw_scan(scan_file));
        fclose(scan_file);
        assert(ap_count == 2);
        assert(aps[0].active);
        assert(strcmp(aps[0].ssid, "mesh home") == 0);
        assert(strcmp(aps[0].security, "WPA3") == 0);
        assert(aps[0].channel == 36);
        assert(aps[0].signal == 70);
        assert(strcmp(aps[1].security, "open") == 0);
        assert(aps[1].channel == 1);
        assert(aps[1].signal == 20);
    }

    shell_quote("house's mesh", quoted, sizeof(quoted));
    assert(strcmp(quoted, "'house'\\''s mesh'") == 0);
    hex_encode("mesh home", encoded, sizeof(encoded));
    assert(strcmp(encoded, "6d65736820686f6d65") == 0);
    wpa_config_quote("p\"a\\ss", encoded, sizeof(encoded));
    assert(strcmp(encoded, "\"p\\\"a\\\\ss\"") == 0);

    for (int round = 0; round < 5000; round++) {
        char fuzz[256];
        char *fuzz_fields[7];
        size_t length = (size_t)(round % 255);
        for (size_t i = 0; i < length; i++) {
            random_state = random_state * 1103515245u + 12345u;
            fuzz[i] = (char)(1 + random_state % 126);
        }
        fuzz[length] = '\0';
        int fuzz_count = split_nmcli(fuzz, fuzz_fields, 7);
        assert(fuzz_count >= 1 && fuzz_count <= 7);
        for (int i = 0; i < fuzz_count; i++)
            assert(fuzz_fields[i] >= fuzz && fuzz_fields[i] <= fuzz + length);
    }

    assert(command_output("yes x | head -c 131072", contents, sizeof(contents)));
    assert(strlen(contents) == sizeof(contents) - 1);
    assert(ping_average("127.0.0.1", 2, &ping_loss) >= 0);
    assert(ping_loss == 0);

    assert(mkdtemp(temp));
    snprintf(args_path, sizeof(args_path), "%s/args", temp);
    snprintf(stdin_path, sizeof(stdin_path), "%s/stdin", temp);
    assert(setenv("SIMPLENET_MOCK_ARGS", args_path, 1) == 0);
    assert(setenv("SIMPLENET_MOCK_STDIN", stdin_path, 1) == 0);

    executable_length = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    assert(executable_length > 0);
    executable[executable_length] = '\0';
    snprintf(build_dir, sizeof(build_dir), "%s", executable);
    *strrchr(build_dir, '/') = '\0';
    snprintf(new_path, sizeof(new_path), "%s:%s", build_dir, getenv("PATH"));
    assert(setenv("PATH", new_path, 1) == 0);

    assert(setenv("SIMPLENET_MOCK_BACKEND", "nm", 1) == 0);
    detect_backend();
    assert(backend == BACKEND_NETWORKMANAGER);
    assert(strcmp(wifi_device, "wlan-test") == 0);
    assert(setenv("SIMPLENET_MOCK_BACKEND", "iwd", 1) == 0);
    detect_backend();
    assert(backend == BACKEND_IWD);
    assert(strcmp(wifi_device, "wlan-test") == 0);
    assert(setenv("SIMPLENET_MOCK_BACKEND", "wpa", 1) == 0);
    detect_backend();
    assert(backend == BACKEND_WPA_SUPPLICANT);
    assert(strcmp(wifi_device, "wlan-test") == 0);
    {
        char network_id[32];
        assert(wpa_network_id("mesh with spaces", network_id,
                              sizeof(network_id)));
        assert(strcmp(network_id, "7") == 0);
        snprintf(connection_uuid, sizeof(connection_uuid), "%s", network_id);
        assert(pin_bssid("aa:bb:cc:dd:ee:ff"));
    }
    assert(setenv("SIMPLENET_MOCK_BACKEND", "iwd", 1) == 0);
    backend = BACKEND_IWD;
    assert(pin_bssid("aa:bb:cc:dd:ee:ff"));
    assert(unsetenv("SIMPLENET_MOCK_BACKEND") == 0);

    snprintf(wifi_device, sizeof(wifi_device), "wlan-test");
    AccessPoint ap = {0};
    snprintf(ap.ssid, sizeof(ap.ssid), "mesh with spaces");
    snprintf(ap.bssid, sizeof(ap.bssid), "AA:BB:CC:DD:EE:FF");
    assert(nmcli_connect_password(&ap, password, sizeof(password),
                                  output, sizeof(output)));
    for (size_t i = 0; i < sizeof(password); i++) assert(password[i] == '\0');
    assert(strstr(output, "mock connection activated"));

    file = fopen(args_path, "r");
    assert(file);
    size_t read_count = fread(contents, 1, sizeof(contents) - 1, file);
    contents[read_count] = '\0';
    fclose(file);
    assert(!strstr(contents, "secret"));
    assert(strstr(contents, "mesh with spaces"));
    assert(strstr(contents, "AA:BB:CC:DD:EE:FF"));

    file = fopen(stdin_path, "r");
    assert(file);
    assert(fgets(contents, sizeof(contents), file));
    fclose(file);
    assert(strcmp(contents, "secret ! '$ with spaces\n") == 0);

    snprintf(password, sizeof(password), "another secret");
    assert(setenv("SIMPLENET_MOCK_FAIL", "1", 1) == 0);
    assert(!nmcli_connect_password(&ap, password, sizeof(password),
                                   output, sizeof(output)));
    for (size_t i = 0; i < sizeof(password); i++) assert(password[i] == '\0');
    assert(unsetenv("SIMPLENET_MOCK_FAIL") == 0);

    snprintf(connection_uuid, sizeof(connection_uuid), "uuid-test");
    backend = BACKEND_NETWORKMANAGER;
    assert(restore_bssid("11:22:33:44:55:66"));
    {
        AccessPoint target = {0};
        snprintf(target.ssid, sizeof(target.ssid), "mesh with spaces");
        snprintf(target.bssid, sizeof(target.bssid), "11:22:33:44:55:66");
        assert(setenv("SIMPLENET_MOCK_CURRENT_BSSID",
                      "11:22:33:44:55:66", 1) == 0);
        assert(enforce_selected_bssid(&target));
        assert(setenv("SIMPLENET_MOCK_CURRENT_BSSID",
                      "aa:bb:cc:dd:ee:ff", 1) == 0);
        assert(!enforce_selected_bssid(&target));
        assert(unsetenv("SIMPLENET_MOCK_CURRENT_BSSID") == 0);
    }
    file = fopen(args_path, "r");
    assert(file);
    read_count = fread(contents, 1, sizeof(contents) - 1, file);
    contents[read_count] = '\0';
    fclose(file);
    assert(strstr(contents, "802-11-wireless.bssid"));
    assert(strstr(contents, "11:22:33:44:55:66"));
    assert(strstr(contents, "uuid-test"));

    unlink(args_path);
    unlink(stdin_path);
    rmdir(temp);

    puts("simplenet checks passed");
    return 0;
}
