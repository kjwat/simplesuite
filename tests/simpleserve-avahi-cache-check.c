#define main simpleserved_program_main
#include "../simpleserved.c"
#undef main

#include <assert.h>

int main(void)
{
    SSDaemon *daemon = calloc(1, sizeof(*daemon));
    SSRemoteServer *copy;
    SSBuffer first;
    SSBuffer second;
    uint64_t original_generation;
    AvahiServiceBrowser *browser_identity;

    assert(daemon);
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
    assert(format_discovery(daemon, &first));
    assert(format_discovery(daemon, &second));
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
