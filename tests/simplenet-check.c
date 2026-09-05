#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>

#define SIMPLENET_TEST 1
#include "../simplenet.c"

static void reset_app(void)
{
#ifdef HAVE_LIBNM
    g_clear_object(&app.nm_device);
    g_clear_object(&app.nm_client);
#endif
    memset(&app, 0, sizeof(app));
    app.backend = BACKEND_AUTO;
    app.wpa_fd = -1;
}

static Network *network_named(const char *ssid)
{
    Network *best = NULL;
    for (int i = 0; i < app.network_count; i++)
        if (!strcmp(app.networks[i].ssid, ssid) &&
            (!best || app.networks[i].signal > best->signal))
            best = &app.networks[i];
    return best;
}

static void fake_wpa_server(int descriptor, const char *log_path)
{
    char command[2048];
    bool selected = false;
    bool future = false;
    FILE *log = fopen(log_path, "a");

    if (!log) _exit(2);
    setvbuf(log, NULL, _IONBF, 0);
    for (;;) {
        const char *reply = "OK\n";
        ssize_t count = recv(descriptor, command, sizeof(command) - 1, 0);
        if (count <= 0) break;
        command[count] = '\0';
        fprintf(log, "%s\n", command);
        if (!strcmp(command, "PING")) reply = "PONG\n";
        else if (!strcmp(command, "SCAN_RESULTS"))
            reply =
                "bssid / frequency / signal level / flags / ssid\n"
                "AA:BB:CC:DD:EE:01\t2412\t-72\t[WPA2-PSK-CCMP][ESS]\thome\\x20mesh\n"
                "AA:BB:CC:DD:EE:02\t5180\t-41\t[WPA2-PSK-CCMP][ESS]\thome\\x20mesh\n"
                "12:34:56:78:90:AB\t2437\t-55\t[ESS]\tcoffee\n"
                "12:34:56:78:90:AC\t5955\t-48\t[RSN-SAE-CCMP][ESS]\tfuture\n";
        else if (!strcmp(command, "STATUS"))
            reply = selected
                ? (future
                    ? "id=7\nbssid=12:34:56:78:90:AC\nssid=future\nwpa_state=COMPLETED\n"
                    : "id=3\nbssid=AA:BB:CC:DD:EE:02\nssid=home\\x20mesh\nwpa_state=COMPLETED\n")
                : "wpa_state=DISCONNECTED\n";
        else if (!strcmp(command, "LIST_NETWORKS"))
            reply = "network id / ssid / bssid / flags\n"
                    "3\thome\\x20mesh\tany\t[CURRENT]\n"
                    "4\tother\tany\t\n"
                    "5\tdisabled\tany\t[DISABLED]\n"
                    "6\thome\\x20mesh\tAA:BB:CC:DD:EE:99\t[DISABLED]\n";
        else if (!strcmp(command, "ADD_NETWORK")) reply = "7\n";
        else if (!strcmp(command, "GET_NETWORK 3 key_mgmt")) reply = "WPA-PSK\n";
        else if (strstr(command, "SET_NETWORK 7 ssid 667574757265"))
            future = true;
        else if (!strncmp(command, "SELECT_NETWORK ", 15)) {
            selected = true;
            if (!strcmp(command, "SELECT_NETWORK 3")) future = false;
            reply = "OK\n";
        }
        if (send(descriptor, reply, strlen(reply), 0) < 0) break;
    }
    fclose(log);
    close(descriptor);
    _exit(0);
}

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

static void check_parsers(void)
{
    char decoded[64];
    char quoted[128];
    unsigned char bytes[32];
    char encoded[65];
    bool sae;
    Network personal = {.security = SECURITY_PERSONAL};
    Network wpa3 = {.security = SECURITY_PERSONAL, .sae = true};

    decode_wpa_text("home\\x20mesh\\\\guest", decoded, sizeof(decoded));
    assert(!strcmp(decoded, "home mesh\\guest"));
    assert(classify_security("[ESS]", &sae) == SECURITY_OPEN && !sae);
    assert(classify_security("[RSN-SAE-CCMP][ESS]", &sae) ==
           SECURITY_PERSONAL && sae);
    assert(classify_security("WPA2 802.1X", &sae) == SECURITY_ENTERPRISE);
    assert(classify_security("WEP", &sae) == SECURITY_WEP);
    assert(quote_wpa_secret("a \\\" b", quoted, sizeof(quoted)));
    assert(!strcmp(quoted, "\"a \\\\\\\" b\""));
    assert(password_valid(&personal, "eight888"));
    assert(!password_valid(&personal, "short"));
    assert(password_valid(&wpa3, "x"));
    assert(decode_wpa_ssid("a\\x00\\xFF\\n", bytes, sizeof(bytes)) == 4);
    assert(bytes[0] == 'a' && bytes[1] == 0 && bytes[2] == 255 && bytes[3] == '\n');
    hex_encode(bytes, 4, encoded, sizeof(encoded));
    assert(!strcmp(encoded, "6100ff0a"));
    decode_wpa_text("a\\x", decoded, sizeof(decoded));
    assert(!strcmp(decoded, "ax"));
}

static void check_access_point_identity(void)
{
    Network first = {.signal = 40};
    Network second = {.signal = 80, .active = true};
    Network update = {.signal = 55, .active = true};

    reset_app();
    copy_text(first.ssid, sizeof(first.ssid), "mesh");
    copy_text(second.ssid, sizeof(second.ssid), "mesh");
    copy_text(update.ssid, sizeof(update.ssid), "mesh");
    copy_text(first.bssid, sizeof(first.bssid), "AA:BB:CC:DD:EE:01");
    copy_text(second.bssid, sizeof(second.bssid), "AA:BB:CC:DD:EE:02");
    copy_text(update.bssid, sizeof(update.bssid), "AA:BB:CC:DD:EE:01");
    add_network(&first);
    add_network(&second);
    assert(app.network_count == 2);
    add_network(&update);
    assert(app.network_count == 2);
    assert(app.networks[0].signal == 55);
    assert(app.networks[0].active);
    app.selected = 1;
    restore_selection(&second);
    assert(!strcmp(app.networks[app.selected].bssid, second.bssid));
    app.networks[app.selected].signal = 1;
    restore_selection(&second);
    assert(!strcmp(app.networks[app.selected].bssid, second.bssid));
}

static void check_wpa_supplicant(void)
{
    int sockets[2];
    char log_path[] = "/tmp/simplenet-wpa-check.XXXXXX";
    char log[16384];
    Network *home;
    Network *future;
    pid_t server;
    int file;

    file = mkstemp(log_path);
    assert(file >= 0);
    close(file);
    assert(socketpair(AF_UNIX, SOCK_DGRAM, 0, sockets) == 0);
    server = fork();
    assert(server >= 0);
    if (server == 0) {
        close(sockets[0]);
        fake_wpa_server(sockets[1], log_path);
    }
    close(sockets[1]);
    reset_app();
    app.backend = BACKEND_WPA_SUPPLICANT;
    app.wpa_fd = sockets[0];
    copy_text(app.interface_name, sizeof(app.interface_name), "wlan-test");
    assert(wpa_scan());
    assert(app.network_count == 4);
    home = network_named("home mesh");
    future = network_named("future");
    assert(home && home->signal == 100 && home->security == SECURITY_PERSONAL);
    assert(future && future->sae);
    assert(network_named("coffee")->security == SECURITY_OPEN);
    assert(wpa_connect(home, "correct horse"));
    assert(wpa_scan());
    int active_count = 0;
    for (int i = 0; i < app.network_count; i++) active_count += app.networks[i].active;
    assert(active_count == 1);
    future = network_named("future");
    /* Failed creation may remove only the just-created profile. */
    assert(!wpa_connect(future, "invalid\nsecret"));
    assert(wpa_active_id() == 3);
    assert(wpa_connect(future, "x"));
    close(app.wpa_fd);
    app.wpa_fd = -1;
    kill(server, SIGTERM);
    waitpid(server, NULL, 0);
    read_file(log_path, log, sizeof(log));
    assert(strstr(log, "SCAN\n"));
    assert(!strstr(log, "SET_NETWORK 7 ssid 686f6d65206d657368\n"));
    assert(!strstr(log, "correct horse"));
    assert(strstr(log, "SELECT_NETWORK 3\n"));
    assert(strstr(log, "SELECT_NETWORK 7\n"));
    assert(!strstr(log, "REMOVE_NETWORK 3\n"));
    assert(!strstr(log, "REMOVE_NETWORK 6\n"));
    assert(strstr(log, "REMOVE_NETWORK 7\n"));
    assert(!strstr(log, "REMOVE_NETWORK 4\n"));
    assert(strstr(log, "ENABLE_NETWORK 4\n"));
    assert(!strstr(log, "ENABLE_NETWORK 5\n"));
    assert(strstr(log, "SET_NETWORK 7 key_mgmt SAE\n"));
    assert(strstr(log, "SET_NETWORK 7 sae_password \"x\"\n"));
    assert(strstr(log, "SET_NETWORK 7 ieee80211w 2\n"));
    assert(!strstr(log, "SET_NETWORK 7 psk \"x\"\n"));
    assert(strstr(log, "SAVE_CONFIG\n"));
    unlink(log_path);
}

static void check_wpa_transport_failure(void)
{
    int sockets[2];
    char reply[8];
    reset_app();
    copy_text(app.interface_name, sizeof(app.interface_name), "wlan-test");
    assert(socketpair(AF_UNIX, SOCK_DGRAM, 0, sockets) == 0);
    app.wpa_fd = sockets[0];
    assert(send(sockets[1], "a truncated response", 20, 0) == 20);
    assert(!wpa_request("PING", reply, sizeof(reply), 50));
    assert(app.wpa_fd == -1);
    close(sockets[1]);
    assert(socketpair(AF_UNIX, SOCK_DGRAM, 0, sockets) == 0);
    app.wpa_fd = sockets[0];
    assert(!wpa_request("PING", reply, sizeof(reply), 10));
    assert(app.wpa_fd == -1);
    assert(!wpa_request("ADD_NETWORK", reply, sizeof(reply), 10));
    close(sockets[1]);
}

#ifdef HAVE_LIBNM
static void check_nm_ownership_failure(void)
{
    int sockets[2];
    char reply[32];
    GError *error = NULL;

    reset_app();
    copy_text(app.interface_name, sizeof(app.interface_name), "wlan-test");
    assert(!nm_owns_interface(app.interface_name));
    assert(app.nm_client);
    assert(g_dbus_connection_close_sync(nm_client_get_dbus_connection(app.nm_client),
                                        NULL, &error));
    assert(!error);
    assert(socketpair(AF_UNIX, SOCK_DGRAM, 0, sockets) == 0);
    app.wpa_fd = sockets[0];
    assert(!wpa_request("ADD_NETWORK", reply, sizeof(reply), 50));
    assert(strstr(app.message, "ownership"));
    assert(recv(sockets[1], reply, sizeof(reply), MSG_DONTWAIT) < 0 &&
           (errno == EAGAIN || errno == EWOULDBLOCK));
    close_wpa();
    close(sockets[1]);
}

static void check_nm_authentication_fields(void)
{
    NMConnection *connection = nm_simple_connection_new();
    NMSetting *security = nm_setting_wireless_security_new();
    NMSetting8021x *eap = NM_SETTING_802_1X(nm_setting_802_1x_new());
    NmSecretField fields[16] = {0};
    const char *hints[] = {"phase2-private-key-password", NULL};
    const char *bad_hints[] = {"ca-cert", NULL};
    nm_connection_add_setting(connection, security);
    g_object_set(security, NM_SETTING_WIRELESS_SECURITY_KEY_MGMT, "sae", NULL);
    assert(nm_agent_fields(connection, NULL, fields) == 1);
    assert(fields[0].hidden && !strcmp(fields[0].property, "psk"));
    g_object_set(security, NM_SETTING_WIRELESS_SECURITY_KEY_MGMT, "none",
                 NM_SETTING_WIRELESS_SECURITY_WEP_TX_KEYIDX, 2u, NULL);
    assert(nm_agent_fields(connection, NULL, fields) == 1);
    assert(!strcmp(fields[0].property, "wep-key2"));
    g_object_set(security, NM_SETTING_WIRELESS_SECURITY_KEY_MGMT, "wpa-eap", NULL);
    nm_connection_add_setting(connection, NM_SETTING(eap));
    nm_setting_802_1x_add_eap_method(eap, "peap");
    assert(nm_agent_fields(connection, NULL, fields) == 2);
    assert(!fields[0].hidden && !strcmp(fields[0].property, "identity"));
    assert(fields[1].hidden && !strcmp(fields[1].property, "password"));
    assert(nm_agent_fields(connection, hints, fields) == 1);
    assert(fields[0].hidden && !strcmp(fields[0].property, hints[0]));
    /* Never turn certificate/configuration hints into a secret prompt. */
    assert(nm_agent_fields(connection, bad_hints, fields) == 0);
    g_object_unref(connection);
}
#endif

int main(int argc, char **argv)
{
    assert(argc == 2);
    (void)&request_stop;
    (void)&detect_backend;
    (void)&scan_networks;
    (void)&connect_selected;
    (void)&usage;
    check_parsers();
    (void)argv;
    check_access_point_identity();
    check_wpa_supplicant();
    check_wpa_transport_failure();
#ifdef HAVE_LIBNM
    check_nm_authentication_fields();
    check_nm_ownership_failure();
#endif
    reset_app();
    puts("simplenet checks passed");
    return 0;
}
