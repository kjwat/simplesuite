#define main simpleserved_program_main
#include "../simpleserved.c"
#undef main

#include <assert.h>

int main(void)
{
    static const char tailscale_status[] =
        "{\"BackendState\":\"Running\",\"Self\":{"
        "\"DNSName\":\"generic-node.example.ts.net.\"}}";
    static const char manifest_response[] =
        "HTTP/1.0 200 OK\r\n"
        "X-SimpleServe-Tailscale-Name: generic-node.example.ts.net.\r\n"
        "X-SimpleServe-Tailscale-IPv4: 100.103.24.81\r\n\r\nbody";
    static const char manifest_request[] =
        "GET /v1/manifest HTTP/1.0\r\n\r\n";
    SSDaemon *daemon = calloc(1, sizeof(*daemon));
    SSRemoteServer *copy;
    SSBuffer first;
    SSBuffer second;
    uint64_t original_generation;
    AvahiServiceBrowser *browser_identity;
    char metadata[256];
    char served[4096];
    int manifest_sockets[2];
    ssize_t served_length;
    const char *headers_end = strstr(manifest_response, "\r\n\r\n");

    assert(daemon);
    assert(tailscale_name_from_status(tailscale_status, metadata,
                                      sizeof(metadata)));
    assert(strcmp(metadata, "generic-node.example.ts.net.") == 0);
    assert(http_header_value(manifest_response, headers_end,
                             "X-SimpleServe-Tailscale-Name", metadata,
                             sizeof(metadata)));
    assert(strcmp(metadata, "generic-node.example.ts.net.") == 0);
    assert(http_header_value(manifest_response, headers_end,
                             "X-SimpleServe-Tailscale-IPv4", metadata,
                             sizeof(metadata)));
    assert(strcmp(metadata, "100.103.24.81") == 0);
    assert(ss_copy_string(daemon->config.server_name,
                          sizeof(daemon->config.server_name),
                          "generic-server"));
    daemon->tailscale_active = 1;
    assert(ss_copy_string(daemon->tailscale_name,
                          sizeof(daemon->tailscale_name),
                          "generic-server.example.ts.net."));
    assert(ss_copy_string(daemon->tailscale_address,
                          sizeof(daemon->tailscale_address),
                          "100.99.45.12"));
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, manifest_sockets) == 0);
    assert(send(manifest_sockets[0], manifest_request,
                sizeof(manifest_request) - 1, 0) ==
           (ssize_t)(sizeof(manifest_request) - 1));
    assert(shutdown(manifest_sockets[0], SHUT_WR) == 0);
    serve_manifest(daemon, manifest_sockets[1]);
    close(manifest_sockets[1]);
    served_length = recv(manifest_sockets[0], served, sizeof(served) - 1, 0);
    assert(served_length > 0);
    served[served_length] = '\0';
    assert(strstr(served,
                  "X-SimpleServe-Tailscale-Name: "
                  "generic-server.example.ts.net.\r\n"));
    assert(strstr(served,
                  "X-SimpleServe-Tailscale-IPv4: 100.99.45.12\r\n"));
    close(manifest_sockets[0]);
    browser_identity = (AvahiServiceBrowser *)(void *)daemon;
    assert(pthread_mutex_init(&daemon->remote_mutex, NULL) == 0);
    assert(pthread_cond_init(&daemon->manifest_condition, NULL) == 0);
    daemon->remote_sync_initialized = 1;
    daemon->avahi_browser = browser_identity;

    service_browser_callback(NULL, 7, AVAHI_PROTO_INET,
                             AVAHI_BROWSER_NEW, "remotebox SimpleServe",
                             SS_SERVICE_TYPE, "local", 0, daemon);
    assert(daemon->service_count == 1);
    assert(daemon->services[0].resolve_requested);
    original_generation = daemon->services[0].generation;

    pthread_mutex_lock(&daemon->remote_mutex);
    assert(ss_copy_string(daemon->services[0].server_name,
                          sizeof(daemon->services[0].server_name),
                          "remotebox"));
    assert(ss_copy_string(daemon->remotes[0].name,
                          sizeof(daemon->remotes[0].name), "remotebox"));
    assert(ss_copy_string(daemon->remotes[0].address,
                          sizeof(daemon->remotes[0].address),
                          "192.168.1.50"));
    daemon->remotes[0].port = SS_DEFAULT_PORT;
    daemon->remotes[0].share_count = 1;
    assert(ss_copy_string(daemon->remotes[0].shares[0].name,
                          sizeof(daemon->remotes[0].shares[0].name), "T7"));
    daemon->remote_sources[0] = original_generation;
    daemon->remote_count = 1;
    daemon->remote_revision++;
    pthread_mutex_unlock(&daemon->remote_mutex);

    ss_buffer_init(&first);
    ss_buffer_init(&second);
    assert(format_discovery(daemon, getuid(), &first));
    assert(format_discovery(daemon, getuid(), &second));
    assert(first.length == second.length);
    assert(memcmp(first.data, second.data, first.length) == 0);
    assert(strstr(first.data, "remotebox") && strstr(first.data, "T7"));
    assert(daemon->avahi_browser == browser_identity);
    ss_buffer_free(&first);
    ss_buffer_free(&second);

    copy = copy_cached_remote(daemon, "remotebox");
    assert(copy);
    assert(strcmp(copy->address, "192.168.1.50") == 0);
    free(copy);
    assert(daemon->avahi_browser == browser_identity);

    invalidate_remote_and_refresh(daemon, "remotebox");
    assert(daemon->remote_count == 0);
    assert(daemon->service_count == 1);
    assert(daemon->services[0].generation != original_generation);
    assert(daemon->services[0].resolve_requested);
    assert(daemon->avahi_browser == browser_identity);

    pthread_mutex_lock(&daemon->remote_mutex);
    assert(ss_copy_string(daemon->remotes[0].name,
                          sizeof(daemon->remotes[0].name), "remotebox"));
    daemon->remote_sources[0] = daemon->services[0].generation;
    daemon->remote_count = 1;
    daemon->remote_revision++;
    pthread_mutex_unlock(&daemon->remote_mutex);

    service_browser_callback(NULL, 7, AVAHI_PROTO_INET,
                             AVAHI_BROWSER_ALL_FOR_NOW, "", "", "", 0,
                             daemon);
    assert(daemon->avahi_all_for_now);
    service_browser_callback(NULL, 7, AVAHI_PROTO_INET,
                             AVAHI_BROWSER_REMOVE,
                             "remotebox SimpleServe", SS_SERVICE_TYPE,
                             "local", 0, daemon);
    assert(daemon->service_count == 0);
    assert(daemon->remote_count == 0);

    daemon->avahi_browser = NULL;
    stop_remote_discovery(daemon);
    free(daemon);
    return 0;
}
