#define _GNU_SOURCE

#include "../simpleserve.h"

#include <sys/socket.h>
#include <sys/stat.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void fail(const char *message)
{
    fprintf(stderr, "simpleserve-check: %s\n", message);
    exit(1);
}

static void require(int condition, const char *message)
{
    if (!condition)
        fail(message);
}

static void test_names_and_sizes(void)
{
    char output[64];

    require(ss_valid_name("T7"), "ordinary share name was rejected");
    require(ss_valid_name("writing-2026_08.01"), "portable name was rejected");
    require(!ss_valid_name("../T7"), "traversal name was accepted");
    require(!ss_valid_name("T7 on box"), "space in internal name was accepted");
    require(ss_valid_absolute_path("/media/My Drive"),
            "path containing a space was rejected");
    require(!ss_valid_absolute_path("relative/path"), "relative path was accepted");
    ss_human_size(1800000000000ULL, output, sizeof(output));
    require(strcmp(output, "1.8 TB") == 0, "human size formatting changed");
}

static void sample_config(SSServerConfig *config)
{
    SSLocalShare *share;

    ss_server_config_defaults(config);
    require(ss_copy_string(config->server_name, sizeof(config->server_name),
                           "thetyper"), "server name copy failed");
    config->port = 7337;
    require(ss_copy_string(config->allowed_networks[0],
                           sizeof(config->allowed_networks[0]),
                           "192.168.1.0/24"), "network copy failed");
    config->network_count = 1;
    share = &config->shares[0];
    memset(share, 0, sizeof(*share));
    require(ss_copy_string(share->name, sizeof(share->name), "T7"),
            "share name copy failed");
    require(ss_copy_string(share->configured_path,
                           sizeof(share->configured_path), "/media/T7"),
            "configured path copy failed");
    require(ss_copy_string(share->current_path, sizeof(share->current_path),
                           "/media/T7"), "current path copy failed");
    require(ss_copy_string(share->filesystem_id,
                           sizeof(share->filesystem_id),
                           "8235f8b3-b565-43ab-9718-f18cc10a1fba"),
            "UUID copy failed");
    require(ss_copy_string(share->source, sizeof(share->source), "/dev/da0p1"),
            "source copy failed");
    require(ss_copy_string(share->fstype, sizeof(share->fstype), "ext2fs"),
            "filesystem type copy failed");
    share->access = SS_ACCESS_READ_WRITE;
    share->owner_uid = 1001;
    share->owner_gid = 1001;
    share->total_bytes = 1800000000000ULL;
    share->free_bytes = 1100000000000ULL;
    share->active = 1;
    config->share_count = 1;
}

static void test_config_round_trip(void)
{
    char temporary[] = "/tmp/simpleserve-config-check.XXXXXX";
    char path[4096];
    char error[512];
    SSServerConfig original;
    SSServerConfig loaded;

    require(mkdtemp(temporary) != NULL, "mkdtemp failed");
    require(snprintf(path, sizeof(path), "%s/server.conf", temporary) <
                (int)sizeof(path), "temporary path is too long");
    sample_config(&original);
    require(ss_save_server_config(path, &original, error, sizeof(error)), error);
    require(ss_load_server_config(path, &loaded, error, sizeof(error)), error);
    require(strcmp(loaded.server_name, "thetyper") == 0,
            "server name did not round-trip");
    require(loaded.network_count == 1 &&
                strcmp(loaded.allowed_networks[0], "192.168.1.0/24") == 0,
            "allowed network did not round-trip");
    require(loaded.share_count == 1 &&
                strcmp(loaded.shares[0].name, "T7") == 0 &&
                strcmp(loaded.shares[0].filesystem_id,
                       "8235f8b3-b565-43ab-9718-f18cc10a1fba") == 0 &&
                loaded.shares[0].access == SS_ACCESS_READ_WRITE,
            "share did not round-trip");
    unlink(path);
    rmdir(temporary);
}

static void test_exports(void)
{
    SSServerConfig config;
    SSBuffer freebsd;
    SSBuffer linux;
    SSBuffer replaced;
    char error[512];
    const char *existing =
        "# personal export\n/home -ro trusted\n"
        "# BEGIN SimpleServe managed exports\nold\n"
        "# END SimpleServe managed exports\n";

    sample_config(&config);
    ss_buffer_init(&freebsd);
    ss_buffer_init(&linux);
    ss_buffer_init(&replaced);
    require(ss_render_exports(SS_PLATFORM_FREEBSD, &config, &freebsd,
                              error, sizeof(error)), error);
    require(strstr(freebsd.data,
                   "/media/T7 -mapall=1001:1001 -network=192.168.1.0/24") != NULL,
            "FreeBSD export recipe is wrong");
    require(ss_render_exports(SS_PLATFORM_LINUX, &config, &linux,
                              error, sizeof(error)), error);
    require(strstr(linux.data,
                   "/media/T7 192.168.1.0/24(rw,sync,no_subtree_check,all_squash,anonuid=1001,anongid=1001)") != NULL,
            "Linux export recipe is wrong");
    require(ss_replace_managed_exports(existing, freebsd.data, &replaced,
                                       error, sizeof(error)), error);
    require(strstr(replaced.data, "# personal export") != NULL,
            "managed-block replacement lost user exports");
    require(strstr(replaced.data, "\nold\n") == NULL,
            "managed-block replacement retained stale export");
    require(strstr(replaced.data, "/media/T7") != NULL,
            "managed-block replacement omitted current export");
    ss_buffer_free(&freebsd);
    ss_buffer_free(&linux);
    ss_buffer_free(&replaced);
}

static void test_manifest(void)
{
    const char *incomplete =
        "[server]\nversion=1\nname=thetyper\nhostname=thetyper.local\n"
        "protocol=nfs\n\n[share T7]\nprotocol=nfs\nexport=/media/T7\n"
        "uuid=8235f8b3-b565-43ab-9718-f18cc10a1fba\nsize=1\nfree=1\n";
    SSServerConfig config;
    SSBuffer manifest;
    SSRemoteServer remote;
    char error[512];

    sample_config(&config);
    ss_buffer_init(&manifest);
    require(ss_render_manifest(&config, &manifest, error, sizeof(error)), error);
    require(ss_parse_manifest(manifest.data, "192.168.1.149", 7337, &remote,
                              error, sizeof(error)), error);
    require(strcmp(remote.name, "thetyper") == 0 && remote.share_count == 1,
            "manifest server did not round-trip");
    require(strcmp(remote.shares[0].name, "T7") == 0 &&
                strcmp(remote.shares[0].export_path, "/media/T7") == 0 &&
                remote.shares[0].access == SS_ACCESS_READ_WRITE,
            "manifest share did not round-trip");
    require(!ss_parse_manifest(incomplete, "192.168.1.149", 7337, &remote,
                               error, sizeof(error)) &&
                strstr(error, "incomplete") != NULL,
            "manifest parser accepted a share without access metadata");
    ss_buffer_free(&manifest);
}

static void test_avahi_and_commands(void)
{
    const char *line =
        "=;wlan2;IPv4;thetyper\\032SimpleServe;_simpleserve._tcp;local;"
        "thetyper.local;192.168.1.149;7337;\"version=1\"\\032\"server=thetyper\"";
    char hostname[256];
    char address[64];
    char server[SS_MAX_NAME + 1];
    unsigned int port;
    SSCommand command;
    char error[512];

    require(ss_parse_avahi_resolved(line, hostname, sizeof(hostname), address,
                                    sizeof(address), &port, server,
                                    sizeof(server)),
            "Avahi resolved record was not parsed");
    require(strcmp(hostname, "thetyper.local") == 0 &&
                strcmp(address, "192.168.1.149") == 0 && port == 7337 &&
                strcmp(server, "thetyper") == 0,
            "Avahi fields were parsed incorrectly");
    require(!ss_parse_avahi_resolved(
                "=;wlan2;IPv4;bad\\032SimpleServe;_simpleserve._tcp;local;"
                "bad.local;203.0.113.9;7337;\"server=bad\"",
                hostname, sizeof(hostname), address, sizeof(address), &port,
                server, sizeof(server)),
            "Avahi parser accepted a non-LAN address");
    require(ss_build_mount_command(SS_PLATFORM_FREEBSD, "192.168.1.149",
                                   "/media/T7",
                                   "/home/keelan/SimpleServe/thetyper/T7",
                                   SS_ACCESS_READ_WRITE, &command,
                                   error, sizeof(error)), error);
    require(command.argc == 5 && strcmp(command.argv[0], "/sbin/mount_nfs") == 0 &&
                strcmp(command.argv[2], "nfsv3,tcp,nosuid") == 0 &&
                strcmp(command.argv[3], "192.168.1.149:/media/T7") == 0,
            "FreeBSD mount command is wrong");
    require(ss_build_mount_command(SS_PLATFORM_LINUX, "192.168.1.149",
                                   "/media/T7", "/home/k/SimpleServe/b/T7",
                                   SS_ACCESS_READ_ONLY, &command,
                                   error, sizeof(error)), error);
    require(command.argc == 7 && strcmp(command.argv[0], "/bin/mount") == 0 &&
                strstr(command.argv[4], "vers=3,proto=tcp") != NULL &&
                strstr(command.argv[4], ",ro") != NULL,
            "Linux mount command is wrong");
}

static void test_frames(void)
{
    int sockets[2];
    char *received = NULL;
    size_t length = 0;
    char error[512];
    const char payload[] = "MOUNT\tthetyper\tT7\tremember";

    require(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0,
            "socketpair failed");
    require(ss_send_frame(sockets[0], payload, sizeof(payload) - 1,
                          error, sizeof(error)), error);
    require(ss_receive_frame(sockets[1], &received, &length,
                             error, sizeof(error)), error);
    require(length == sizeof(payload) - 1 && strcmp(received, payload) == 0,
            "protocol frame did not round-trip");
    free(received);
    close(sockets[0]);
    close(sockets[1]);
}

int main(void)
{
    test_names_and_sizes();
    test_config_round_trip();
    test_exports();
    test_manifest();
    test_avahi_and_commands();
    test_frames();
    puts("OK SimpleServe protocol, config, discovery, and platform adapters");
    return 0;
}
