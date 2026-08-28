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
    memset(&app, 0, sizeof(app));
    app.backend = BACKEND_AUTO;
    app.wpa_fd = -1;
}

static Network *network_named(const char *ssid)
{
    for (int i = 0; i < app.network_count; i++)
        if (!strcmp(app.networks[i].ssid, ssid)) return &app.networks[i];
    return NULL;
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
                    ? "bssid=12:34:56:78:90:AC\nssid=future\nwpa_state=COMPLETED\n"
                    : "bssid=AA:BB:CC:DD:EE:02\nssid=home\\x20mesh\nwpa_state=COMPLETED\n")
                : "wpa_state=DISCONNECTED\n";
        else if (!strcmp(command, "LIST_NETWORKS"))
            reply = "network id / ssid / bssid / flags\n"
                    "3\thome\\x20mesh\tany\t[CURRENT]\n"
                    "4\tother\tany\t\n"
                    "5\tdisabled\tany\t[DISABLED]\n";
        else if (!strcmp(command, "ADD_NETWORK")) reply = "7\n";
        else if (strstr(command, "SET_NETWORK 7 ssid 667574757265"))
            future = true;
        else if (!strncmp(command, "SELECT_NETWORK ", 15)) {
            selected = true;
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
    char row[] = "*:home\\: east:AA\\:BB\\:CC\\:DD\\:EE\\:FF:77:WPA2";
    char *fields[5];
    char decoded[64];
    char quoted[128];
    bool sae;
    Network personal = {.security = SECURITY_PERSONAL};
    Network wpa3 = {.security = SECURITY_PERSONAL, .sae = true};

    assert(split_escaped(row, fields, 5, ':') == 5);
    assert(!strcmp(fields[0], "*"));
    assert(!strcmp(fields[1], "home: east"));
    assert(!strcmp(fields[2], "AA:BB:CC:DD:EE:FF"));
    assert(!strcmp(fields[3], "77"));
    assert(!strcmp(fields[4], "WPA2"));

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
}

static void check_networkmanager(const char *mock_directory)
{
    char path[PATH_MAX * 2];
    char log_path[] = "/tmp/simplenet-nm-check.XXXXXX";
    char log[32768];
    NmProfile profile;
    Network *home;
    Network *coffee;
    Network *office;
    int file;

    snprintf(path, sizeof(path), "%s:%s", mock_directory, getenv("PATH"));
    assert(setenv("PATH", path, 1) == 0);
    file = mkstemp(log_path);
    assert(file >= 0);
    close(file);
    assert(setenv("SIMPLENET_MOCK_LOG", log_path, 1) == 0);
    reset_app();
    assert(nm_detect());
    assert(!strcmp(app.interface_name, "wlan-test"));
    app.backend = BACKEND_NETWORKMANAGER;
    assert(nm_scan());
    assert(app.network_count == 3);
    home = network_named("home: east");
    coffee = network_named("coffee");
    office = network_named("office");
    assert(home);
    assert(!home->active);
    assert(home->signal == 81);
    assert(home->security == SECURITY_PERSONAL);
    assert(coffee && coffee->security == SECURITY_OPEN);
    assert(office && office->security == SECURITY_ENTERPRISE);

    assert(nm_find_saved_profile(home, &profile) == NM_PROFILE_FOUND);
    assert(!strcmp(profile.uuid,
                   "11111111-1111-4111-8111-111111111111"));
    assert(nm_connect_saved(home) == NM_SAVED_CONNECTED);
    assert(nm_find_saved_profile(coffee, &profile) == NM_PROFILE_NOT_FOUND);
    assert(nm_connect_saved(coffee) == NM_SAVED_NOT_FOUND);
    assert(nm_connect_new(coffee, ""));
    assert(nm_connect_saved(office) == NM_SAVED_CONNECTED);

    assert(setenv("SIMPLENET_MOCK_MODE", "saved-fail", 1) == 0);
    assert(nm_connect_saved(home) == NM_SAVED_FAILED);
    assert(strstr(app.message, "stored secret was rejected"));
    assert(nm_connect_new(home, "correct horse"));
    assert(setenv("SIMPLENET_MOCK_MODE", "profile-list-fail", 1) == 0);
    assert(nm_connect_saved(home) == NM_SAVED_LOOKUP_FAILED);
    assert(strstr(app.message, "Could not inspect saved"));
    unsetenv("SIMPLENET_MOCK_MODE");

    home = network_named("home: east");
    assert(home);
    app.selected = (int)(home - app.networks);
    connect_selected();
    home = network_named("home: east");
    assert(home && home->active);
    assert(strstr(app.message, "saved profile"));

    read_file(log_path, log, sizeof(log));
    assert(strstr(log, "connection\tup\tuuid\t"
                       "11111111-1111-4111-8111-111111111111\t"
                       "ifname\twlan-test\n"));
    assert(strstr(log, "connection\tup\tuuid\t"
                       "44444444-4444-4444-8444-444444444444\t"
                       "ifname\twlan-test\n"));
    assert(strstr(log, "wifi\tconnect\tcoffee\tifname\twlan-test\n"));
    assert(strstr(log, "--ask\tdevice\twifi\tconnect\thome: east\t"
                       "ifname\twlan-test\n"));
    assert(!strstr(log, "correct horse"));
    unsetenv("SIMPLENET_MOCK_LOG");
    unlink(log_path);
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
    assert(app.network_count == 3);
    home = network_named("home mesh");
    future = network_named("future");
    assert(home && home->signal == 100 && home->security == SECURITY_PERSONAL);
    assert(future && future->sae);
    assert(network_named("coffee")->security == SECURITY_OPEN);
    assert(wpa_connect(home, "correct horse"));
    assert(wpa_connect(future, "x"));
    close(app.wpa_fd);
    app.wpa_fd = -1;
    kill(server, SIGTERM);
    waitpid(server, NULL, 0);
    read_file(log_path, log, sizeof(log));
    assert(strstr(log, "SCAN\n"));
    assert(strstr(log, "SET_NETWORK 7 ssid 686f6d65206d657368\n"));
    assert(strstr(log, "SET_NETWORK 7 psk \"correct horse\"\n"));
    assert(strstr(log, "SELECT_NETWORK 7\n"));
    assert(strstr(log, "REMOVE_NETWORK 3\n"));
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

int main(int argc, char **argv)
{
    assert(argc == 2);
    check_parsers();
    check_networkmanager(argv[1]);
    check_wpa_supplicant();
    puts("simplenet checks passed");
    return 0;
}
