#define _GNU_SOURCE

#include "../simpleserve.h"

#include <sys/socket.h>
#include <sys/stat.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <grp.h>
#include <pwd.h>
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

static size_t count_text(const char *text, const char *wanted)
{
    size_t count = 0;
    size_t length = strlen(wanted);

    while (text && (text = strstr(text, wanted)) != NULL) {
        count++;
        text += length;
    }
    return count;
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

#ifdef __linux__
static void test_linux_mountinfo(void)
{
    SSMountInfo mount;
    char error[512];

    unsetenv("SIMPLESERVE_TEST_MOUNTS");
    require(ss_mount_info_exact("/", &mount, error, sizeof(error)), error);
    require(strcmp(mount.target, "/") == 0,
            "Linux mountinfo lookup returned the wrong root mount");
}
#endif

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

static void test_remembered_peer_compatibility(void)
{
    static const char legacy[] =
        "# SimpleServe remembered mounts\n"
        "[mounts]\nversion=1\n\n"
        "[mount]\nuid=1000\ngid=1000\nserver=oldpeer\n"
        "share=OldShare\nfilesystem_id=old-filesystem\n";
    char temporary[] = "/tmp/simpleserve-mount-config-check.XXXXXX";
    char path[4096];
    char error[512];
    SSMountConfig original;
    SSMountConfig loaded;
    SSClientMount *mount;

    require(mkdtemp(temporary) != NULL, "mount config mkdtemp failed");
    require(snprintf(path, sizeof(path), "%s/mounts.conf", temporary) <
                (int)sizeof(path),
            "mount config temporary path is too long");
    memset(&original, 0, sizeof(original));
    original.mount_count = 1;
    mount = &original.mounts[0];
    mount->uid = 1000;
    mount->gid = 1001;
    mount->remembered = 1;
    mount->port = 7337;
    mount->access = SS_ACCESS_READ_WRITE;
    mount->route = SS_ROUTE_TAILSCALE;
    require(ss_copy_string(mount->server, sizeof(mount->server),
                           "portable-peer") &&
                ss_copy_string(mount->share, sizeof(mount->share),
                               "Music_Elsewhere") &&
                ss_copy_string(mount->hostname, sizeof(mount->hostname),
                               "portable-peer.local") &&
                ss_copy_string(mount->tailscale_name,
                               sizeof(mount->tailscale_name),
                               "portable-peer.example.ts.net") &&
                ss_copy_string(mount->lan_address,
                               sizeof(mount->lan_address), "100.70.8.91") &&
                ss_copy_string(mount->tailscale_address,
                               sizeof(mount->tailscale_address),
                               "100.92.44.17") &&
                ss_copy_string(mount->address, sizeof(mount->address),
                               "100.92.44.17") &&
                ss_copy_string(mount->export_path,
                               sizeof(mount->export_path),
                               "/volumes/Music Elsewhere") &&
                ss_copy_string(mount->filesystem_id,
                               sizeof(mount->filesystem_id),
                               "portable-filesystem-id"),
            "remembered peer setup failed");
    require(ss_save_mount_config(path, &original, error, sizeof(error)), error);
    require(ss_load_mount_config(path, &loaded, error, sizeof(error)), error);
    require(loaded.mount_count == 1 &&
                strcmp(loaded.mounts[0].server, "portable-peer") == 0 &&
                strcmp(loaded.mounts[0].tailscale_name,
                       "portable-peer.example.ts.net") == 0 &&
                strcmp(loaded.mounts[0].lan_address, "100.70.8.91") == 0 &&
                strcmp(loaded.mounts[0].tailscale_address,
                       "100.92.44.17") == 0 &&
                strcmp(loaded.mounts[0].export_path,
                       "/volumes/Music Elsewhere") == 0 &&
                loaded.mounts[0].access == SS_ACCESS_READ_WRITE &&
                loaded.mounts[0].route == SS_ROUTE_TAILSCALE &&
                strcmp(loaded.mounts[0].address, "100.92.44.17") == 0,
            "remembered peer route metadata did not round-trip");
    require(ss_atomic_write(path, legacy, sizeof(legacy) - 1, 0600,
                            error, sizeof(error)), error);
    require(ss_load_mount_config(path, &loaded, error, sizeof(error)), error);
    require(loaded.mount_count == 1 && loaded.mounts[0].remembered &&
                strcmp(loaded.mounts[0].server, "oldpeer") == 0 &&
                loaded.mounts[0].hostname[0] == '\0' &&
                loaded.mounts[0].tailscale_name[0] == '\0' &&
                loaded.mounts[0].lan_address[0] == '\0' &&
                loaded.mounts[0].tailscale_address[0] == '\0' &&
                loaded.mounts[0].export_path[0] == '\0' &&
                loaded.mounts[0].route == SS_ROUTE_NONE &&
                loaded.mounts[0].address[0] == '\0',
            "legacy remembered mount did not load as LAN-discovery-only");
    unlink(path);
    rmdir(temporary);
}

static void test_exports(void)
{
    SSServerConfig config;
    SSBuffer freebsd;
    SSBuffer linux_exports;
    SSBuffer macos;
    SSBuffer replaced;
    char error[512];
    const char *existing =
        "# personal export\n/home -ro trusted\n"
        "# BEGIN SimpleServe managed exports\nold\n"
        "# END SimpleServe managed exports\n";

    sample_config(&config);
    ss_buffer_init(&freebsd);
    ss_buffer_init(&linux_exports);
    ss_buffer_init(&macos);
    ss_buffer_init(&replaced);
    require(ss_render_exports(SS_PLATFORM_FREEBSD, &config, 0, &freebsd,
                              error, sizeof(error)), error);
    require(strstr(freebsd.data,
                   "/media/T7 -mapall=1001:1001 -network=192.168.1.0/24") != NULL,
            "FreeBSD export recipe is wrong");
    require(ss_render_exports(SS_PLATFORM_LINUX, &config, 0, &linux_exports,
                              error, sizeof(error)), error);
    require(strstr(linux_exports.data,
                   "/media/T7 192.168.1.0/24(rw,sync,no_subtree_check,all_squash,anonuid=1001,anongid=1001)") != NULL,
            "Linux export recipe is wrong");
    require(ss_render_exports(SS_PLATFORM_MACOS, &config, 0, &macos,
                              error, sizeof(error)), error);
    require(strstr(macos.data,
                   "/media/T7 -mapall=1001:1001 -fspath=/media/T7 -network=192.168.1.0 -mask=255.255.255.0") != NULL,
            "macOS export recipe is wrong");
    require(ss_replace_managed_exports(existing, freebsd.data, &replaced,
                                       error, sizeof(error)), error);
    require(strstr(replaced.data, "# personal export") != NULL,
            "managed-block replacement lost user exports");
    require(strstr(replaced.data, "\nold\n") == NULL,
            "managed-block replacement retained stale export");
    require(strstr(replaced.data, "/media/T7") != NULL,
            "managed-block replacement omitted current export");
    ss_buffer_free(&freebsd);
    ss_buffer_free(&linux_exports);
    ss_buffer_free(&macos);
    ss_buffer_free(&replaced);
}

static void test_tailscale_exports(void)
{
    SSServerConfig config;
    SSBuffer lan_only;
    SSBuffer roaming;
    SSBuffer freebsd_roaming;
    SSBuffer macos_roaming;
    char error[512];

    sample_config(&config);
    require(ss_copy_string(config.allowed_networks[0],
                           sizeof(config.allowed_networks[0]),
                           "10.42.16.0/20"),
            "arbitrary LAN network copy failed");
    config.share_count = 3;
    config.shares[1] = config.shares[0];
    config.shares[2] = config.shares[0];
    require(ss_copy_string(config.shares[1].name,
                           sizeof(config.shares[1].name), "Writing") &&
                ss_copy_string(config.shares[1].current_path,
                               sizeof(config.shares[1].current_path),
                               "/srv/Writing") &&
                ss_copy_string(config.shares[1].filesystem_id,
                               sizeof(config.shares[1].filesystem_id),
                               "writing-filesystem") &&
                ss_copy_string(config.shares[2].name,
                               sizeof(config.shares[2].name), "Archive_2030") &&
                ss_copy_string(config.shares[2].current_path,
                               sizeof(config.shares[2].current_path),
                               "/data/arbitrary") &&
                ss_copy_string(config.shares[2].filesystem_id,
                               sizeof(config.shares[2].filesystem_id),
                               "archive-filesystem"),
            "arbitrary share setup failed");
    ss_buffer_init(&lan_only);
    ss_buffer_init(&roaming);
    ss_buffer_init(&freebsd_roaming);
    ss_buffer_init(&macos_roaming);
    require(ss_render_exports(SS_PLATFORM_LINUX, &config, 0, &lan_only,
                              error, sizeof(error)), error);
    require(strstr(lan_only.data, SS_TAILSCALE_NETWORK) == NULL,
            "inactive Tailscale broadened LAN exports");
    require(count_text(lan_only.data, "10.42.16.0/20(") == 3,
            "LAN export was not applied to every arbitrary share");
    require(ss_render_exports(SS_PLATFORM_LINUX, &config, 1, &roaming,
                              error, sizeof(error)), error);
    require(count_text(roaming.data, "10.42.16.0/20(") == 3 &&
                count_text(roaming.data, SS_TAILSCALE_NETWORK) == 3,
            "LAN and Tailscale permissions were not applied to every share");
    require(strstr(roaming.data, "/srv/Writing ") != NULL &&
                strstr(roaming.data, "/data/arbitrary ") != NULL,
            "generic export handling omitted an arbitrary share");
    require(ss_render_exports(SS_PLATFORM_FREEBSD, &config, 1,
                              &freebsd_roaming, error, sizeof(error)), error);
    require(count_text(freebsd_roaming.data, "10.42.16.0/20") == 3 &&
                count_text(freebsd_roaming.data, SS_TAILSCALE_NETWORK) == 3,
            "FreeBSD did not apply both routes to every arbitrary share");
    require(ss_render_exports(SS_PLATFORM_MACOS, &config, 1,
                              &macos_roaming, error, sizeof(error)), error);
    require(count_text(macos_roaming.data,
                       "-network=10.42.16.0 -mask=255.255.240.0") == 3 &&
                count_text(macos_roaming.data,
                           "-network=100.64.0.0 -mask=255.192.0.0") == 3,
            "macOS did not apply LAN and Tailscale routes to every share");
    ss_buffer_free(&lan_only);
    ss_buffer_free(&roaming);
    ss_buffer_free(&freebsd_roaming);
    ss_buffer_free(&macos_roaming);
}

static void test_fstab(void)
{
    SSServerConfig config;
    SSBuffer generated;
    SSBuffer replaced;
    SSBuffer removed;
    SSBuffer malformed;
    char error[512];
    const char *existing =
        "# local root\nUUID=root / ext4 defaults 0 1\n"
        "# BEGIN SimpleServe managed mounts\nold entry\n"
        "# END SimpleServe managed mounts\n";

    sample_config(&config);
    require(ss_copy_string(config.shares[0].configured_path,
                           sizeof(config.shares[0].configured_path),
                           "/media/My Drive"), "fstab path copy failed");
    require(ss_copy_string(config.shares[0].fstype,
                           sizeof(config.shares[0].fstype), "ext4"),
            "fstab type copy failed");
    config.share_count = 2;
    config.shares[1] = config.shares[0];
    require(ss_copy_string(config.shares[1].name,
                           sizeof(config.shares[1].name), "fallback"),
            "fallback name copy failed");
    require(ss_copy_string(config.shares[1].filesystem_id,
                           sizeof(config.shares[1].filesystem_id),
                           "ext4:/dev/test:8:1"),
            "fallback identity copy failed");

    ss_buffer_init(&generated);
    ss_buffer_init(&replaced);
    ss_buffer_init(&removed);
    ss_buffer_init(&malformed);
    require(ss_render_fstab(&config, &generated, error, sizeof(error)), error);
    require(strstr(generated.data,
                   "UUID=8235f8b3-b565-43ab-9718-f18cc10a1fba "
                   "/media/My\\040Drive ext4 defaults,nofail,nosuid,nodev,"
                   "x-systemd.device-timeout=10s 0 2") != NULL,
            "Linux persistent mount recipe is wrong");
    require(strstr(generated.data, "ext4:/dev/test") == NULL,
            "non-UUID fallback identity was written to fstab");
    require(ss_replace_managed_fstab(existing, generated.data, &replaced,
                                     error, sizeof(error)), error);
    require(strstr(replaced.data, "UUID=root / ext4 defaults 0 1") != NULL,
            "fstab replacement lost an unrelated mount");
    require(strstr(replaced.data, "old entry") == NULL,
            "fstab replacement retained a stale managed mount");
    require(ss_replace_managed_fstab(replaced.data, "", &removed,
                                     error, sizeof(error)), error);
    require(strstr(removed.data, "SimpleServe managed mounts") == NULL &&
                strstr(removed.data, "UUID=root / ext4 defaults 0 1") != NULL,
            "fstab managed-block removal was not surgical");
    require(!ss_replace_managed_fstab(
                "# BEGIN SimpleServe managed mounts\nunterminated\n", "",
                &malformed, error, sizeof(error)) &&
                strstr(error, "unterminated") != NULL,
            "malformed fstab markers were accepted");
    ss_buffer_free(&generated);
    ss_buffer_free(&replaced);
    ss_buffer_free(&removed);
    ss_buffer_free(&malformed);
}

static void test_samba(void)
{
    const char *existing =
        "[global]\nworkgroup=KEEP\n\n"
        "[archive]\npath=/srv/archive\nread only=yes\n";
    struct passwd *account = getpwuid(getuid());
    struct group *group = getgrgid(getgid());
    SSServerConfig config;
    SSBuffer generated;
    SSBuffer included;
    SSBuffer removed;
    char error[512];
    char force_user[512];
    char force_group[512];

    require(account != NULL && group != NULL,
            "test account cannot be resolved for Samba");
    sample_config(&config);
    config.shares[0].owner_uid = getuid();
    config.shares[0].owner_gid = getgid();
    config.share_count = 2;
    config.shares[1] = config.shares[0];
    require(ss_copy_string(config.shares[1].name,
                           sizeof(config.shares[1].name), "Music"),
            "Samba read-only name copy failed");
    require(ss_copy_string(config.shares[1].current_path,
                           sizeof(config.shares[1].current_path),
                           "/media/My Music"),
            "Samba read-only path copy failed");
    config.shares[1].access = SS_ACCESS_READ_ONLY;

    ss_buffer_init(&generated);
    ss_buffer_init(&included);
    ss_buffer_init(&removed);
    require(ss_render_samba_config(&config, &generated,
                                   error, sizeof(error)), error);
    require(strstr(generated.data,
                   "[T7]\npath=/media/T7\nbrowseable=yes\nread only=no\n"
                   "guest ok=yes\n") != NULL,
            "Samba read-write recipe is wrong");
    require(strstr(generated.data,
                   "[Music]\npath=/media/My Music\nbrowseable=yes\n"
                   "read only=yes\nguest ok=yes\n") != NULL,
            "Samba read-only recipe is wrong");
    require(snprintf(force_user, sizeof(force_user), "force user=%s\n",
                     account->pw_name) < (int)sizeof(force_user),
            "Samba owner expectation is too long");
    require(snprintf(force_group, sizeof(force_group), "force group=%s\n",
                     group->gr_name) < (int)sizeof(force_group),
            "Samba group expectation is too long");
    require(strstr(generated.data, force_user) != NULL &&
                strstr(generated.data, force_group) != NULL,
            "Samba ownership mapping is wrong");
    require(strstr(generated.data,
                   "create mask=0664\ndirectory mask=0775\n") != NULL,
            "Samba creation masks are wrong");
    require(ss_replace_managed_samba_include(
                existing, "/etc/samba/simpleserve.conf", &included,
                error, sizeof(error)), error);
    require(strstr(included.data, "workgroup=KEEP") != NULL &&
                strstr(included.data,
                       "[archive]\npath=/srv/archive\nread only=yes") != NULL,
            "Samba include registration changed unrelated smb.conf content");
    require(strstr(included.data,
                   "# BEGIN SimpleServe managed Samba include\n"
                   "[global]\ninclude=/etc/samba/simpleserve.conf\n"
                   "# END SimpleServe managed Samba include\n") != NULL,
            "Samba include registration is wrong");
    require(ss_replace_managed_samba_include(
                included.data, "", &removed, error, sizeof(error)), error);
    require(strstr(removed.data, "SimpleServe managed Samba include") == NULL &&
                strstr(removed.data, "workgroup=KEEP") != NULL &&
                strstr(removed.data, "[archive]") != NULL,
            "Samba include removal changed unrelated smb.conf content");
    ss_buffer_free(&generated);
    ss_buffer_free(&included);
    ss_buffer_free(&removed);
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

static void test_mount_commands(void)
{
    SSCommand command;
    char error[512];

    require(ss_private_ipv4_address("192.168.1.149"),
            "private Avahi address was rejected");
    require(!ss_private_ipv4_address("203.0.113.9"),
            "public Avahi address was accepted");
    require(ss_build_mount_command(SS_PLATFORM_FREEBSD, "192.168.1.149",
                                   "/media/T7",
                                   "/home/keelan/SimpleServe/thetyper/T7",
                                   SS_ACCESS_READ_WRITE, &command,
                                   error, sizeof(error)), error);
    require(command.argc == 5 && strcmp(command.argv[0], "/sbin/mount_nfs") == 0 &&
                strcmp(command.argv[2],
                       "nfsv3,tcp,nosuid,rdirplus,readahead=4") == 0 &&
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
    require(ss_build_mount_command(SS_PLATFORM_MACOS, "192.168.1.149",
                                   "/media/T7", "/Users/k/SimpleServe/b/T7",
                                   SS_ACCESS_READ_ONLY, &command,
                                   error, sizeof(error)), error);
    require(command.argc == 5 &&
                strcmp(command.argv[0], "/sbin/mount_nfs") == 0 &&
                strstr(command.argv[2], "vers=3,proto=tcp,inet") != NULL &&
                strstr(command.argv[2], "readahead=16") != NULL &&
                strstr(command.argv[2], ",ro") != NULL,
            "macOS mount command is wrong");
    require(ss_build_lazy_unmount_command(
                SS_PLATFORM_LINUX, "/home/k/SimpleServe/b/T7", &command,
                error, sizeof(error)), error);
    require(command.argc == 3 &&
                strcmp(command.argv[0], "/bin/umount") == 0 &&
                strcmp(command.argv[1], "-l") == 0 &&
                strcmp(command.argv[2], "/home/k/SimpleServe/b/T7") == 0,
            "Linux lazy unmount command is wrong");
    require(!ss_build_lazy_unmount_command(
                SS_PLATFORM_FREEBSD, "/home/k/SimpleServe/b/T7", &command,
                error, sizeof(error)),
            "FreeBSD unexpectedly accepted a Linux lazy unmount");
}

static void test_route_selection(void)
{
    char address[64];
    SSRoute route;

    require(ss_tailscale_ipv4_address("100.64.0.1") &&
                ss_tailscale_ipv4_address("100.127.255.254") &&
                !ss_tailscale_ipv4_address("100.128.0.1") &&
                !ss_tailscale_ipv4_address("192.168.50.2"),
            "Tailscale IPv4 range validation is wrong");
    require(ss_choose_route("10.77.5.9", 1, NULL, 0, &route, address,
                            sizeof(address)) &&
                route == SS_ROUTE_LAN &&
                strcmp(address, "10.77.5.9") == 0,
            "usable LAN route was not selected");
    require(ss_choose_route("172.20.4.8", 1, "100.101.22.33", 1,
                            &route, address, sizeof(address)) &&
                route == SS_ROUTE_LAN &&
                strcmp(address, "172.20.4.8") == 0,
            "LAN was not preferred over Tailscale");
    require(ss_choose_route("100.70.8.9", 1, "100.101.22.33", 1,
                            &route, address, sizeof(address)) &&
                route == SS_ROUTE_LAN &&
                strcmp(address, "100.70.8.9") == 0,
            "a LAN route in CGNAT space was confused with Tailscale");
    require(ss_choose_route("192.168.90.4", 0, "100.101.22.33", 1,
                            &route, address, sizeof(address)) &&
                route == SS_ROUTE_TAILSCALE &&
                strcmp(address, "100.101.22.33") == 0,
            "Tailscale was not selected after LAN failure");
    require(!ss_choose_route("192.168.90.4", 0, "100.101.22.33", 0,
                             &route, address, sizeof(address)) &&
                route == SS_ROUTE_NONE && address[0] == '\0',
            "unavailable routes did not fail cleanly");
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
#ifdef __linux__
    test_linux_mountinfo();
#endif
    test_config_round_trip();
    test_remembered_peer_compatibility();
    test_exports();
    test_tailscale_exports();
    test_fstab();
    test_samba();
    test_manifest();
    test_mount_commands();
    test_route_selection();
    test_frames();
    puts("OK SimpleServe protocol, config, discovery, and NFS/SMB platform adapters");
    return 0;
}
