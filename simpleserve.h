#ifndef SIMPLESERVE_H
#define SIMPLESERVE_H

#include <sys/types.h>

#include <limits.h>
#include <stddef.h>
#include <stdint.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define SS_PROTOCOL_VERSION 1
#define SS_DEFAULT_PORT 7337
#define SS_SERVICE_TYPE "_simpleserve._tcp"
#define SS_TAILSCALE_NETWORK "100.64.0.0/10"

#define SS_MAX_NAME 63
#define SS_MAX_ID 255
#define SS_MAX_SHARES 64
#define SS_MAX_SERVERS 32
#define SS_MAX_REMOTE_SHARES 64
#define SS_MAX_MOUNTS 128
#define SS_MAX_NETWORKS 16
#define SS_MAX_FRAME (1024U * 1024U)

typedef enum {
    SS_PLATFORM_UNSUPPORTED = 0,
    SS_PLATFORM_FREEBSD,
    SS_PLATFORM_LINUX,
    SS_PLATFORM_MACOS
} SSPlatform;

typedef enum {
    SS_ACCESS_READ_ONLY = 0,
    SS_ACCESS_READ_WRITE = 1
} SSAccess;

typedef enum {
    SS_ROUTE_NONE = 0,
    SS_ROUTE_LAN,
    SS_ROUTE_TAILSCALE
} SSRoute;

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} SSBuffer;

typedef struct {
    char target[PATH_MAX];
    char source[PATH_MAX];
    char fstype[64];
    char identity[SS_MAX_ID + 1];
    unsigned long long total_bytes;
    unsigned long long free_bytes;
    int read_only;
} SSMountInfo;

typedef struct {
    char name[SS_MAX_NAME + 1];
    char configured_path[PATH_MAX];
    char current_path[PATH_MAX];
    char filesystem_id[SS_MAX_ID + 1];
    char source[PATH_MAX];
    char fstype[64];
    SSAccess access;
    uid_t owner_uid;
    gid_t owner_gid;
    unsigned long long total_bytes;
    unsigned long long free_bytes;
    int active;
} SSLocalShare;

typedef struct {
    char server_name[SS_MAX_NAME + 1];
    unsigned int port;
    char allowed_networks[SS_MAX_NETWORKS][64];
    size_t network_count;
    SSLocalShare shares[SS_MAX_SHARES];
    size_t share_count;
} SSServerConfig;

typedef struct {
    char name[SS_MAX_NAME + 1];
    char export_path[PATH_MAX];
    char filesystem_id[SS_MAX_ID + 1];
    SSAccess access;
    unsigned long long total_bytes;
    unsigned long long free_bytes;
} SSRemoteShare;

typedef struct {
    char name[SS_MAX_NAME + 1];
    char hostname[256];
    char address[64];
    char tailscale_name[256];
    char tailscale_address[64];
    unsigned int port;
    SSRemoteShare shares[SS_MAX_REMOTE_SHARES];
    size_t share_count;
    int reachable;
} SSRemoteServer;

typedef struct {
    uid_t uid;
    gid_t gid;
    char server[SS_MAX_NAME + 1];
    char share[SS_MAX_NAME + 1];
    char hostname[256];
    char tailscale_name[256];
    char lan_address[64];
    char tailscale_address[64];
    unsigned int port;
    char address[64];
    char export_path[PATH_MAX];
    char filesystem_id[SS_MAX_ID + 1];
    char target[PATH_MAX];
    SSAccess access;
    int remembered;
    int mounted;
    int available;
    unsigned int misses;
    SSRoute route;
} SSClientMount;

typedef struct {
    SSClientMount mounts[SS_MAX_MOUNTS];
    size_t mount_count;
} SSMountConfig;

#define SS_COMMAND_MAX_ARGS 20
typedef struct {
    char args[SS_COMMAND_MAX_ARGS][PATH_MAX];
    char *argv[SS_COMMAND_MAX_ARGS + 1];
    size_t argc;
} SSCommand;

SSPlatform ss_platform_detect(void);
const char *ss_platform_name(SSPlatform platform);
SSPlatform ss_platform_from_name(const char *name);

void ss_buffer_init(SSBuffer *buffer);
void ss_buffer_free(SSBuffer *buffer);
int ss_buffer_append(SSBuffer *buffer, const char *text);
int ss_buffer_append_n(SSBuffer *buffer, const char *text, size_t length);
int ss_buffer_appendf(SSBuffer *buffer, const char *format, ...)
    __attribute__((format(printf, 2, 3)));

int ss_valid_name(const char *name);
int ss_valid_absolute_path(const char *path);
const char *ss_access_name(SSAccess access);
int ss_access_parse(const char *text, SSAccess *access);
const char *ss_route_name(SSRoute route);
int ss_tailscale_ipv4_address(const char *text);
int ss_choose_route(const char *lan_address, int lan_usable,
                    const char *tailscale_address, int tailscale_usable,
                    SSRoute *route, char *address, size_t address_size);
int ss_copy_string(char *destination, size_t size, const char *source);
void ss_human_size(unsigned long long bytes, char *output, size_t output_size);

const char *ss_default_socket_path(SSPlatform platform);
const char *ss_default_config_path(void);
const char *ss_default_state_path(SSPlatform platform);
const char *ss_default_exports_path(SSPlatform platform);
const char *ss_default_fstab_path(void);
const char *ss_default_smb_conf_path(void);
const char *ss_default_samba_path(void);
const char *ss_default_macos_smb_state_path(void);

void ss_server_config_defaults(SSServerConfig *config);
int ss_load_server_config(const char *path, SSServerConfig *config,
                          char *error, size_t error_size);
int ss_save_server_config(const char *path, const SSServerConfig *config,
                          char *error, size_t error_size);
int ss_load_mount_config(const char *path, SSMountConfig *config,
                         char *error, size_t error_size);
int ss_save_mount_config(const char *path, const SSMountConfig *config,
                         char *error, size_t error_size);

int ss_mount_info_exact(const char *path, SSMountInfo *info,
                        char *error, size_t error_size);
int ss_find_mount_by_identity(const char *identity, SSMountInfo *info,
                              char *error, size_t error_size);
int ss_user_can_access(uid_t uid, gid_t gid, const char *path,
                       SSAccess access, char *error, size_t error_size);

int ss_collect_private_networks(char networks[][64], size_t maximum,
                                size_t *count, char *error,
                                size_t error_size);
int ss_private_ipv4_address(const char *address);
int ss_render_exports(SSPlatform platform, const SSServerConfig *config,
                      int tailscale_active, SSBuffer *output, char *error,
                      size_t error_size);
int ss_replace_managed_exports(const char *existing, const char *managed,
                               SSBuffer *output, char *error,
                               size_t error_size);
int ss_render_fstab(const SSServerConfig *config, SSBuffer *output,
                    char *error, size_t error_size);
int ss_replace_managed_fstab(const char *existing, const char *managed,
                             SSBuffer *output, char *error,
                             size_t error_size);
int ss_render_samba_config(const SSServerConfig *config, SSBuffer *output,
                           char *error, size_t error_size);
int ss_replace_managed_samba_include(const char *existing,
                                     const char *include_path,
                                     SSBuffer *output, char *error,
                                     size_t error_size);

int ss_render_manifest(const SSServerConfig *config, SSBuffer *output,
                       char *error, size_t error_size);
int ss_parse_manifest(const char *manifest, const char *address,
                      unsigned int port, SSRemoteServer *server,
                      char *error, size_t error_size);
void ss_command_init(SSCommand *command);
int ss_command_add(SSCommand *command, const char *argument);
int ss_build_mount_command(SSPlatform platform, const char *address,
                           const char *export_path, const char *target,
                           SSAccess access, SSCommand *command,
                           char *error, size_t error_size);
int ss_build_unmount_command(SSPlatform platform, const char *target,
                             int force, SSCommand *command,
                             char *error, size_t error_size);
int ss_build_lazy_unmount_command(SSPlatform platform, const char *target,
                                  SSCommand *command, char *error,
                                  size_t error_size);

int ss_send_frame(int fd, const void *data, size_t length,
                  char *error, size_t error_size);
int ss_receive_frame(int fd, char **data, size_t *length,
                     char *error, size_t error_size);

int ss_atomic_write(const char *path, const void *data, size_t length,
                    mode_t mode, char *error, size_t error_size);
int ss_read_file(const char *path, size_t maximum, char **data,
                 size_t *length, char *error, size_t error_size);
int ss_mkdir_parents(const char *path, mode_t mode, uid_t uid, gid_t gid,
                     char *error, size_t error_size);

#endif
