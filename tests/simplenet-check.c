#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef __FreeBSD__
#include <sys/sysctl.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#include <unistd.h>

#define SIMPLENET_TEST_SHARED_BACKENDS 1
#define main simplenet_program_main
#include "../simplenet.c"
#undef main

static int current_executable_path(char *out, size_t size)
{
    if (!out || size == 0)
        return 0;
#ifdef __FreeBSD__
    {
        size_t len = size;
        int mib[4] = {
            CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, (int)getpid()
        };

        if (sysctl(mib, 4, out, &len, NULL, 0) != 0 ||
            len == 0 || out[0] != '/')
            return 0;
        out[size - 1] = '\0';
        return 1;
    }
#elif defined(__APPLE__)
    {
        uint32_t length = (uint32_t)size;

        if (_NSGetExecutablePath(out, &length) != 0 || out[0] != '/')
            return 0;
        out[size - 1] = '\0';
        return 1;
    }
#else
    {
        ssize_t len = readlink("/proc/self/exe", out, size - 1);

        if (len <= 0 || (size_t)len >= size)
            return 0;
        out[len] = '\0';
        return 1;
    }
#endif
}

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
    unsigned int random_state = 0x51a7u;

    assert(!freebsd_dhcp_state_needs_refresh(
        "home wifi", "home wifi", "wlan0", "wlan0", 1));
    assert(freebsd_dhcp_state_needs_refresh(
        "", "home wifi", "wlan0", "wlan2", 1));
    assert(freebsd_dhcp_state_needs_refresh(
        "old wifi", "home wifi", "wlan0", "wlan0", 1));
    assert(freebsd_dhcp_state_needs_refresh(
        "home wifi", "home wifi", "wlan0", "wlan2", 1));
    assert(freebsd_dhcp_state_needs_refresh(
        "home wifi", "home wifi", "wlan0", "wlan0", 0));

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
#ifdef __FreeBSD__
    {
        const char scan_text[] =
            "bssid / frequency / signal level / flags / ssid\n"
            "aa:bb:cc:dd:ee:ff\t5180\t-48\t[WPA2-PSK-CCMP][ESS]\tmesh home\n"
            "11:22:33:44:55:66\t2412\t-78\t[ESS]\tcafe wifi\n";
        scan_file = tmpfile();
        assert(scan_file);
        assert(fwrite(scan_text, 1, sizeof(scan_text) - 1, scan_file) ==
               sizeof(scan_text) - 1);
        rewind(scan_file);
        ap_count = 0;
        assert(parse_wpa_scan_results(scan_file));
        fclose(scan_file);
        assert(ap_count == 2);
        assert(strcmp(aps[0].ssid, "mesh home") == 0);
        assert(strcmp(aps[0].security, "WPA2") == 0);
        assert(aps[0].channel == 36);
        assert(aps[0].signal == 70);
        assert(strcmp(aps[1].ssid, "cafe wifi") == 0);
        assert(strcmp(aps[1].security, "open") == 0);
    }
#endif
#ifdef __FreeBSD__
    {
        const char scan_text[] =
            "SSID/MESH ID                      BSSID              CHAN RATE    S:N     INT CAPS\n"
            "Fios-kachala                      74:90:bc:fa:1f:ac    1   54M  -42:-71   100 EP   RSN BSSLOAD HTCAP WPS WME\n"
            "149b7ce                           7c:7e:f9:57:08:72    6   54M  -38:-63   100 P    RSN HTCAP MESHCONF VHTCAP\n"
            "bayshore house                    7c:7e:f9:57:90:64    6   54M  -26:-39   100 EPS  RSN BSSLOAD HTCAP WME\n"
            "                                  7c:7e:f9:57:90:66    6   54M  -26:-39   100 ES   HTCAP WME\n"
            "Ring Setup f2                     9c:76:13:b6:90:f2    6   54M  -43:-73   100 ES   HTCAP WME\n";
        scan_file = tmpfile();
        assert(scan_file);
        assert(fwrite(scan_text, 1, sizeof(scan_text) - 1, scan_file) ==
               sizeof(scan_text) - 1);
        rewind(scan_file);
        ap_count = 0;
        assert(parse_freebsd_scan(scan_file));
        fclose(scan_file);
        assert(ap_count == 5);
        assert(strcmp(aps[0].ssid, "Fios-kachala") == 0);
        assert(strcmp(aps[0].bssid, "74:90:bc:fa:1f:ac") == 0);
        assert(aps[0].channel == 1);
        assert(aps[0].frequency == 2412);
        assert(aps[0].signal == 80);
        assert(strcmp(aps[0].security, "WPA2") == 0);
        assert(strcmp(aps[1].ssid, "149b7ce") == 0);
        assert(strcmp(aps[1].bssid, "7c:7e:f9:57:08:72") == 0);
        assert(aps[1].channel == 6);
        assert(aps[1].frequency == 2437);
        assert(strcmp(aps[1].security, "WPA2") == 0);
        assert(strcmp(aps[2].ssid, "bayshore house") == 0);
        assert(strcmp(aps[2].bssid, "7c:7e:f9:57:90:64") == 0);
        assert(aps[2].channel == 6);
        assert(aps[2].frequency == 2437);
        assert(aps[2].signal == 100);
        assert(strcmp(aps[2].security, "WPA2") == 0);
        assert(aps[3].hidden_ssid);
        assert(strcmp(ap_ssid_label(&aps[3]), "(hidden SSID)") == 0);
        assert(strcmp(aps[3].bssid, "7c:7e:f9:57:90:66") == 0);
        assert(strcmp(aps[3].security, "open") == 0);
        assert(strcmp(aps[4].ssid, "Ring Setup f2") == 0);
        assert(strcmp(aps[4].security, "open") == 0);
    }
#endif
#ifdef __FreeBSD__
    {
        char parents[] = "iwlwifi0 run0\n";
        char intel_ifconfig[] =
            "radio-main: flags=8843<UP,BROADCAST,RUNNING,SIMPLEX,MULTICAST>\n"
            "\tinet 192.168.4.207 netmask 0xfffffc00 broadcast 192.168.7.255\n"
            "\tgroups: wlan\n"
            "\tssid \"bayshore house\" channel 6 (2437 MHz 11g)\n"
            "\tparent interface: iwlwifi0\n"
            "\tstatus: associated\n";
        char usb_ifconfig[] =
            "usb-backup: flags=8802<BROADCAST,SIMPLEX,MULTICAST>\n"
            "\tgroups: wlan\n"
            "\tparent interface: run0\n"
            "\tstatus: not associated\n";
        const char pci_text[] =
            "iwlwifi0@pci0:2:0:0: class=0x028000 vendor=0x8086\n"
            "    vendor     = 'Intel Corporation'\n"
            "    device     = 'Wi-Fi 6E AX210'\n"
            "    class      = network\n";
        WifiCard *card;
        WifiCard stale = {0};
        char value[256];

        memset(wifi_cards, 0, sizeof(wifi_cards));
        wifi_card_count = 0;
        assert(parse_freebsd_card_parents(parents) == 2);
        assert(wifi_card_count == 2);
        assert(strcmp(wifi_cards[0].parent, "iwlwifi0") == 0);
        assert(strcmp(wifi_cards[0].driver, "iwlwifi") == 0);
        assert(strcmp(wifi_cards[1].parent, "run0") == 0);
        assert(strcmp(wifi_cards[1].driver, "run") == 0);
        assert(parse_freebsd_card_ifconfig("radio-main", intel_ifconfig));
        card = freebsd_card_by_interface("radio-main");
        assert(card);
        assert(card->associated);
        assert(strcmp(card->parent, "iwlwifi0") == 0);
        assert(strcmp(card->ssid, "bayshore house") == 0);
        assert(strcmp(card->address, "192.168.4.207") == 0);
        assert(parse_freebsd_card_ifconfig("usb-backup", usb_ifconfig));
        card = freebsd_card_by_interface("usb-backup");
        assert(card);
        assert(!card->associated);
        assert(freebsd_pciconf_value(pci_text, "vendor", value,
                                     sizeof(value)));
        assert(strcmp(value, "Intel Corporation") == 0);
        assert(freebsd_pciconf_value(pci_text, "device", value,
                                     sizeof(value)));
        assert(strcmp(value, "Wi-Fi 6E AX210") == 0);
        assert(freebsd_device_name_valid("iwlwifi0"));
        assert(!freebsd_device_name_valid("wlan0;reboot"));
        assert(freebsd_word_list_contains("egress wlan debug", "wlan"));
        assert(!freebsd_word_list_contains("egress wlan2", "wlan"));
        assert(freebsd_lease_text_has_address(
            "lease {\n  fixed-address 192.168.1.151;\n}\n",
            "192.168.1.151"));
        assert(!freebsd_lease_text_has_address(
            "lease { fixed-address 192.168.1.151; }\n",
            "192.168.1.15"));
        snprintf(stale.interface_name, sizeof(stale.interface_name),
                 "usb-backup");
        snprintf(stale.address, sizeof(stale.address), "192.168.1.151");
        assert(freebsd_card_has_disconnected_ipv4(&stale, "radio-main"));
        stale.associated = 1;
        assert(!freebsd_card_has_disconnected_ipv4(&stale, "radio-main"));
        stale.associated = 0;
        assert(!freebsd_card_has_disconnected_ipv4(&stale, "usb-backup"));
    }
#endif

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

    assert(current_executable_path(executable, sizeof(executable)));
    snprintf(build_dir, sizeof(build_dir), "%s", executable);
    *strrchr(build_dir, '/') = '\0';
    snprintf(new_path, sizeof(new_path), "%s:%s", build_dir, getenv("PATH"));
    assert(setenv("PATH", new_path, 1) == 0);

#ifdef __FreeBSD__
    {
        char timed_secret[32] = "timeout secret";
        char *const sleepy_argv[] = {"sleepy", NULL};
        assert(!command_argv_input_timeout(sleepy_argv, timed_secret,
                                           sizeof(timed_secret), output,
                                           sizeof(output), 50));
        assert(strstr(output, "Timed out."));
        for (size_t i = 0; i < sizeof(timed_secret); i++)
            assert(timed_secret[i] == '\0');
    }
    assert(setenv("SIMPLENET_MOCK_BACKEND", "wpa", 1) == 0);
    assert(setenv("SIMPLENET_MOCK_FREEBSD_LAYOUT", "ranked", 1) == 0);
    detect_backend();
    assert(backend == BACKEND_WPA_SUPPLICANT);
    assert(strcmp(wifi_device, "radio-main") == 0);
    assert(setenv("SIMPLENET_MOCK_FREEBSD_LAYOUT", "stale-default", 1) == 0);
    detect_backend();
    assert(backend == BACKEND_WPA_SUPPLICANT);
    assert(strcmp(wifi_device, "radio-main") == 0);
    assert(setenv("SIMPLENET_MOCK_FREEBSD_LAYOUT", "fallback", 1) == 0);
    detect_backend();
    assert(backend == BACKEND_WPA_SUPPLICANT);
    assert(strcmp(wifi_device, "radio-main") == 0);
    assert(unsetenv("SIMPLENET_MOCK_FREEBSD_LAYOUT") == 0);
#endif
    assert(setenv("SIMPLENET_MOCK_BACKEND", "nm", 1) == 0);
    detect_backend();
    assert(backend == BACKEND_NETWORKMANAGER);
    assert(strcmp(wifi_device, "wlan-test") == 0);
    unlink(args_path);
    snprintf(connection_uuid, sizeof(connection_uuid), "uuid-preferred");
    assert(networkmanager_prefer_current(output, sizeof(output)));
    file = fopen(args_path, "r");
    assert(file);
    {
        size_t read_count = fread(contents, 1, sizeof(contents) - 1, file);
        contents[read_count] = '\0';
    }
    fclose(file);
    assert(strstr(contents, "connection.autoconnect\nyes\n"));
    assert(strstr(contents,
                  "connection.autoconnect-priority\n999\n"));
    assert(setenv("SIMPLENET_MOCK_BACKEND", "iwd", 1) == 0);
    detect_backend();
    assert(backend == BACKEND_IWD);
    assert(strcmp(wifi_device, "wlan-test") == 0);
    unlink(args_path);
    assert(iwd_prefer_network("mesh with spaces", output, sizeof(output)));
    file = fopen(args_path, "r");
    assert(file);
    {
        size_t read_count = fread(contents, 1, sizeof(contents) - 1, file);
        contents[read_count] = '\0';
    }
    fclose(file);
    assert(strstr(contents, "known-networks\nmesh with spaces\n"));
    assert(strstr(contents, "set-property\nAutoConnect\nyes\n"));
    assert(setenv("SIMPLENET_MOCK_BACKEND", "wpa", 1) == 0);
    detect_backend();
    assert(backend == BACKEND_WPA_SUPPLICANT);
    assert(strcmp(wifi_device, "wlan-test") == 0);
    unlink(args_path);
    assert(setenv("SIMPLENET_MOCK_UPDATE_CONFIG_ZERO", "1", 1) == 0);
    assert(wpa_prepare_persistence(output, sizeof(output)));
    assert(unsetenv("SIMPLENET_MOCK_UPDATE_CONFIG_ZERO") == 0);
    file = fopen(args_path, "r");
    assert(file);
    {
        size_t read_count = fread(contents, 1, sizeof(contents) - 1, file);
        contents[read_count] = '\0';
    }
    fclose(file);
    assert(strstr(contents, "get\nupdate_config\n"));
    assert(strstr(contents, "set\nupdate_config\n1\n"));
    assert(strstr(contents, "save_config\n"));
    unlink(args_path);
    assert(setenv("SIMPLENET_MOCK_UPDATE_CONFIG_ZERO", "1", 1) == 0);
    assert(setenv("SIMPLENET_MOCK_SAVE_FAIL", "1", 1) == 0);
    assert(!wpa_prepare_persistence(output, sizeof(output)));
    assert(strstr(output, "could not write"));
    assert(unsetenv("SIMPLENET_MOCK_UPDATE_CONFIG_ZERO") == 0);
    assert(unsetenv("SIMPLENET_MOCK_SAVE_FAIL") == 0);
    file = fopen(args_path, "r");
    assert(file);
    {
        size_t read_count = fread(contents, 1, sizeof(contents) - 1, file);
        contents[read_count] = '\0';
    }
    fclose(file);
    assert(strstr(contents, "set\nupdate_config\n1\n"));
    assert(strstr(contents, "set\nupdate_config\n0\n"));
    unlink(args_path);
    assert(wpa_prefer_network("7", output, sizeof(output)));
    file = fopen(args_path, "r");
    assert(file);
    {
        size_t read_count = fread(contents, 1, sizeof(contents) - 1, file);
        contents[read_count] = '\0';
    }
    fclose(file);
    assert(strstr(contents, "set_network\n7\npriority\n10\n"));
    assert(strstr(contents, "enable_network\nall\n"));
    assert(strstr(contents, "save_config\n"));
    assert(setenv("SIMPLENET_MOCK_SAVE_FAIL", "1", 1) == 0);
    assert(!wpa_prefer_network("7", output, sizeof(output)));
    assert(strstr(output, "refused to save"));
    assert(unsetenv("SIMPLENET_MOCK_SAVE_FAIL") == 0);
#ifdef __FreeBSD__
    assert(freebsd_backend_for_device("wlan-test") ==
           BACKEND_WPA_SUPPLICANT);
    assert(freebsd_backend_for_device("wlan-test;false") == BACKEND_NONE);
#endif
    assert(setenv("SIMPLENET_MOCK_CURRENT_BSSID",
                  "aa:bb:cc:dd:ee:ff", 1) == 0);
    assert(current_bssid(contents, sizeof(contents)));
    assert(strcmp(contents, "aa:bb:cc:dd:ee:ff") == 0);
#ifdef __FreeBSD__
    assert(active_ssid(contents, sizeof(contents)));
    assert(strcmp(contents, "mesh with spaces") == 0);
#endif
#ifdef __FreeBSD__
    {
        FreebsdDhcpAuth dhcp_auth = {0};
        assert(unsetenv("SIMPLENET_MOCK_SUDO_OK") == 0);
        if (geteuid() != 0) {
            assert(!freebsd_prepare_dhcp_auth(&dhcp_auth, 1, 0));
            assert(strstr(message, "needs root"));
        }
        assert(setenv("SIMPLENET_MOCK_SUDO_OK", "1", 1) == 0);
        assert(freebsd_prepare_dhcp_auth(&dhcp_auth, 1, 0));
        freebsd_clear_dhcp_auth(&dhcp_auth);
        assert(unsetenv("SIMPLENET_MOCK_SUDO_OK") == 0);
    }
    {
        FreebsdDhcpAuth dhcp_auth = {0};
        size_t read_count;

        unlink(args_path);
        snprintf(wifi_device, sizeof(wifi_device), "radio-main");
        assert(setenv("SIMPLENET_MOCK_STALE_ROUTE", "1", 1) == 0);
        assert(setenv("SIMPLENET_MOCK_SUDO_OK", "1", 1) == 0);
        assert(freebsd_interface_has_ipv4("usb-backup", "192.168.1.151"));
        assert(!freebsd_selected_route_ready());
        assert(freebsd_prepare_dhcp_auth(&dhcp_auth, 1, 0));
        assert(freebsd_remove_disconnected_ipv4("192.168.1.1", &dhcp_auth));
        assert(freebsd_interface_has_ipv4("usb-backup", "192.168.1.151"));
        file = fopen(args_path, "r");
        assert(file);
        read_count = fread(contents, 1, sizeof(contents) - 1, file);
        contents[read_count] = '\0';
        fclose(file);
        assert(!strstr(contents, "onestop\nusb-backup\n"));
        assert(!strstr(contents,
                       "ifconfig\nusb-backup\ninet\n192.168.1.151\ndelete\n"));

        unlink(args_path);
        assert(setenv("SIMPLENET_MOCK_DHCLIENT_RUNNING", "1", 1) == 0);
        assert(freebsd_remove_disconnected_ipv4("192.168.1.1", &dhcp_auth));
        assert(freebsd_interface_has_ipv4("usb-backup", "192.168.1.151"));
        file = fopen(args_path, "r");
        assert(file);
        read_count = fread(contents, 1, sizeof(contents) - 1, file);
        contents[read_count] = '\0';
        fclose(file);
        assert(strstr(contents, "cat\n/var/db/dhclient.leases.usb-backup\n"));
        assert(!strstr(contents, "onestop\nusb-backup\n"));
        assert(!strstr(contents,
                       "ifconfig\nusb-backup\ninet\n192.168.1.151\ndelete\n"));

        unlink(args_path);
        assert(setenv("SIMPLENET_MOCK_DHCP_LEASE_MATCH", "1", 1) == 0);
        assert(freebsd_remove_disconnected_ipv4("192.168.1.1", &dhcp_auth));
        assert(!freebsd_interface_has_ipv4("usb-backup", "192.168.1.151"));
        assert(freebsd_selected_route_ready());
        file = fopen(args_path, "r");
        assert(file);
        read_count = fread(contents, 1, sizeof(contents) - 1, file);
        contents[read_count] = '\0';
        fclose(file);
        assert(strstr(contents,
                      "service\ndhclient\nonestop\nusb-backup\n"));
        assert(strstr(contents,
                      "ifconfig\nusb-backup\ninet\n192.168.1.151\ndelete\n"));
        assert(!strstr(contents,
                       "ifconfig\nradio-main\ninet\n192.168.1.102\ndelete\n"));
        freebsd_clear_dhcp_auth(&dhcp_auth);
        assert(unsetenv("SIMPLENET_MOCK_SUDO_OK") == 0);
        assert(unsetenv("SIMPLENET_MOCK_DHCLIENT_RUNNING") == 0);
        assert(unsetenv("SIMPLENET_MOCK_DHCP_LEASE_MATCH") == 0);
        assert(unsetenv("SIMPLENET_MOCK_STALE_ROUTE") == 0);
        snprintf(wifi_device, sizeof(wifi_device), "wlan-test");
    }
#endif
    assert(unsetenv("SIMPLENET_MOCK_CURRENT_BSSID") == 0);
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
#ifdef __FreeBSD__
    {
        size_t read_count;
        unlink(args_path);
        assert(setenv("SIMPLENET_MOCK_BACKEND", "wpa", 1) == 0);
        backend = BACKEND_WPA_SUPPLICANT;
        snprintf(wifi_device, sizeof(wifi_device), "wlan-test");
        assert(wpa_select_network("7", "aa:bb:cc:dd:ee:ff"));
        file = fopen(args_path, "r");
        assert(file);
        read_count = fread(contents, 1, sizeof(contents) - 1, file);
        contents[read_count] = '\0';
        fclose(file);
        assert(strstr(contents, "bssid\n7\naa:bb:cc:dd:ee:ff\n"));
        assert(strstr(contents, "select_network\n7\n"));
        assert(strstr(contents, "reassociate\n"));
        unlink(args_path);
        assert(unsetenv("SIMPLENET_MOCK_BACKEND") == 0);
    }
#endif

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
