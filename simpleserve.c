#define _GNU_SOURCE

#include "simpleserve.h"

#include <sys/socket.h>
#include <sys/un.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void usage(FILE *stream)
{
    fprintf(stream,
            "SimpleServe discovers NFS shares and mounts them as real folders.\n"
            "\n"
            "Usage:\n"
            "  simpleserve share PATH [--name NAME] [--read-only]\n"
            "  simpleserve unshare NAME\n"
            "  simpleserve discover\n"
            "  simpleserve configure\n"
            "  simpleserve mount SERVER:SHARE [--remember]\n"
            "  simpleserve unmount SERVER:SHARE\n"
            "  simpleserve status\n"
            "\n"
            "Mounts appear below ~/SimpleServe/SERVER/SHARE and are ordinary\n"
            "kernel VFS mounts usable by every local application.\n");
}

static int append_field(SSBuffer *request, const char *field)
{
    if (!field || strchr(field, '\t') || strchr(field, '\n') ||
        strchr(field, '\r'))
        return 0;
    return ss_buffer_append(request, "\t") && ss_buffer_append(request, field);
}

static int split_share_spec(const char *spec, char *server, size_t server_size,
                            char *share, size_t share_size)
{
    const char *colon;
    size_t server_length;

    if (!spec || !(colon = strchr(spec, ':')) || colon == spec || !colon[1] ||
        strchr(colon + 1, ':'))
        return 0;
    server_length = (size_t)(colon - spec);
    if (server_length >= server_size || strlen(colon + 1) >= share_size)
        return 0;
    memcpy(server, spec, server_length);
    server[server_length] = '\0';
    ss_copy_string(share, share_size, colon + 1);
    return ss_valid_name(server) && ss_valid_name(share);
}

static int connect_daemon(char *error, size_t error_size)
{
    SSPlatform platform = ss_platform_detect();
    const char *socket_path = ss_default_socket_path(platform);
    struct sockaddr_un address;
    int descriptor;

    if (platform == SS_PLATFORM_UNSUPPORTED) {
        snprintf(error, error_size, "SimpleServe supports FreeBSD and Linux");
        return -1;
    }
    if (strlen(socket_path) >= sizeof(address.sun_path)) {
        snprintf(error, error_size, "daemon socket path is too long");
        return -1;
    }
    descriptor = socket(AF_UNIX, SOCK_STREAM, 0);
    if (descriptor < 0) {
        snprintf(error, error_size, "cannot create daemon socket: %s",
                 strerror(errno));
        return -1;
    }
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    ss_copy_string(address.sun_path, sizeof(address.sun_path), socket_path);
    if (connect(descriptor, (struct sockaddr *)&address, sizeof(address)) != 0) {
        snprintf(error, error_size,
                 "cannot reach simpleserved at %s: %s\n"
                 "Install and start it with the SimpleServe system setup described in README.md.",
                 socket_path, strerror(errno));
        close(descriptor);
        return -1;
    }
    return descriptor;
}

static int send_request(const SSBuffer *request)
{
    char error[512];
    char *response = NULL;
    size_t response_length = 0;
    int descriptor;
    int result = 1;

    descriptor = connect_daemon(error, sizeof(error));
    if (descriptor < 0) {
        fprintf(stderr, "simpleserve: %s\n", error);
        return 1;
    }
    if (!ss_send_frame(descriptor, request->data, request->length,
                       error, sizeof(error)) ||
        !ss_receive_frame(descriptor, &response, &response_length,
                          error, sizeof(error))) {
        fprintf(stderr, "simpleserve: %s\n", error);
        close(descriptor);
        free(response);
        return 1;
    }
    close(descriptor);
    if (response_length >= 3 && memcmp(response, "OK\n", 3) == 0) {
        if (response_length > 3)
            fputs(response + 3, stdout);
        result = 0;
    } else if (response_length >= 4 && memcmp(response, "ERR\n", 4) == 0) {
        fprintf(stderr, "simpleserve: %s", response + 4);
        if (!response_length || response[response_length - 1] != '\n')
            fputc('\n', stderr);
    } else {
        fprintf(stderr, "simpleserve: malformed response from simpleserved\n");
    }
    free(response);
    return result;
}

static int share_command(int argc, char **argv, SSBuffer *request)
{
    char resolved[PATH_MAX];
    char name[SS_MAX_NAME + 1] = "";
    SSAccess access = SS_ACCESS_READ_WRITE;
    const char *base;

    if (argc < 1 || !realpath(argv[0], resolved)) {
        fprintf(stderr, "simpleserve: share path is not an accessible directory: %s\n",
                argc > 0 ? argv[0] : "<missing>");
        return 0;
    }
    for (int index = 1; index < argc; index++) {
        if (strcmp(argv[index], "--name") == 0 && index + 1 < argc) {
            if (!ss_copy_string(name, sizeof(name), argv[++index])) {
                fprintf(stderr, "simpleserve: share name is too long\n");
                return 0;
            }
        } else if (strcmp(argv[index], "--read-only") == 0) {
            access = SS_ACCESS_READ_ONLY;
        } else if (strcmp(argv[index], "--read-write") == 0) {
            access = SS_ACCESS_READ_WRITE;
        } else {
            fprintf(stderr, "simpleserve: unknown share option: %s\n", argv[index]);
            return 0;
        }
    }
    if (!name[0]) {
        base = strrchr(resolved, '/');
        base = base ? base + 1 : resolved;
        if (!ss_copy_string(name, sizeof(name), base)) {
            fprintf(stderr, "simpleserve: cannot derive a share name; use --name\n");
            return 0;
        }
    }
    if (!ss_valid_name(name)) {
        fprintf(stderr,
                "simpleserve: share names use letters, numbers, '.', '_' or '-'\n");
        return 0;
    }
    return ss_buffer_append(request, "SHARE") && append_field(request, name) &&
           append_field(request, ss_access_name(access)) &&
           append_field(request, resolved);
}

int main(int argc, char **argv)
{
    SSBuffer request;
    int valid = 0;
    int result;

    if (argc < 2) {
        usage(stderr);
        return 2;
    }
    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        usage(stdout);
        return 0;
    }
    if (strcmp(argv[1], "--version") == 0) {
        printf("simpleserve protocol %d\n", SS_PROTOCOL_VERSION);
        return 0;
    }
    ss_buffer_init(&request);
    if (strcmp(argv[1], "share") == 0) {
        valid = share_command(argc - 2, argv + 2, &request);
    } else if (strcmp(argv[1], "unshare") == 0) {
        valid = argc == 3 && ss_valid_name(argv[2]) &&
                ss_buffer_append(&request, "UNSHARE") &&
                append_field(&request, argv[2]);
    } else if (strcmp(argv[1], "discover") == 0) {
        valid = argc == 2 && ss_buffer_append(&request, "DISCOVER");
    } else if (strcmp(argv[1], "configure") == 0 ||
               strcmp(argv[1], "refresh") == 0) {
        valid = argc == 2 && ss_buffer_append(&request, "CONFIGURE");
    } else if (strcmp(argv[1], "status") == 0) {
        valid = argc == 2 && ss_buffer_append(&request, "STATUS");
    } else if (strcmp(argv[1], "mount") == 0 ||
               strcmp(argv[1], "unmount") == 0) {
        char server[SS_MAX_NAME + 1];
        char share[SS_MAX_NAME + 1];
        int remember = 0;

        if (argc >= 3 && split_share_spec(argv[2], server, sizeof(server),
                                          share, sizeof(share))) {
            if (strcmp(argv[1], "mount") == 0) {
                if (argc == 4 && strcmp(argv[3], "--remember") == 0)
                    remember = 1;
                else if (argc != 3)
                    goto invalid_usage;
                valid = ss_buffer_append(&request, "MOUNT") &&
                        append_field(&request, server) &&
                        append_field(&request, share) &&
                        append_field(&request, remember ? "remember" : "once");
            } else if (argc == 3) {
                valid = ss_buffer_append(&request, "UNMOUNT") &&
                        append_field(&request, server) &&
                        append_field(&request, share);
            }
        }
    } else {
        fprintf(stderr, "simpleserve: unknown command: %s\n", argv[1]);
        goto invalid_usage;
    }
    if (!valid)
        goto invalid_usage;
    result = send_request(&request);
    ss_buffer_free(&request);
    return result;

invalid_usage:
    ss_buffer_free(&request);
    usage(stderr);
    return 2;
}
