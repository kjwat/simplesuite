#define _GNU_SOURCE

#include "simpleserve.h"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>

#ifdef __FreeBSD__
#include <sys/mount.h>
#endif

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pwd.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#define SS_CONFIG_MAX (1024U * 1024U)
#define SS_EXPORTS_BEGIN "# BEGIN SimpleServe managed exports"
#define SS_EXPORTS_END "# END SimpleServe managed exports"
#define SS_FSTAB_BEGIN "# BEGIN SimpleServe managed mounts"
#define SS_FSTAB_END "# END SimpleServe managed mounts"

static void ss_error(char *error, size_t error_size, const char *format, ...)
{
    va_list arguments;

    if (!error || error_size == 0)
        return;
    va_start(arguments, format);
    (void)vsnprintf(error, error_size, format, arguments);
    va_end(arguments);
}

int ss_copy_string(char *destination, size_t size, const char *source)
{
    size_t length;

    if (!destination || size == 0 || !source)
        return 0;
    length = strlen(source);
    if (length >= size)
        return 0;
    memcpy(destination, source, length + 1);
    return 1;
}

SSPlatform ss_platform_detect(void)
{
    const char *override = getenv("SIMPLESERVE_TEST_PLATFORM");

    if (override && *override)
        return ss_platform_from_name(override);
#ifdef __FreeBSD__
    return SS_PLATFORM_FREEBSD;
#elif defined(__linux__)
    return SS_PLATFORM_LINUX;
#else
    return SS_PLATFORM_UNSUPPORTED;
#endif
}

const char *ss_platform_name(SSPlatform platform)
{
    switch (platform) {
    case SS_PLATFORM_FREEBSD:
        return "FreeBSD";
    case SS_PLATFORM_LINUX:
        return "Linux";
    default:
        return "unsupported";
    }
}

SSPlatform ss_platform_from_name(const char *name)
{
    if (name && strcasecmp(name, "FreeBSD") == 0)
        return SS_PLATFORM_FREEBSD;
    if (name && strcasecmp(name, "Linux") == 0)
        return SS_PLATFORM_LINUX;
    return SS_PLATFORM_UNSUPPORTED;
}

void ss_buffer_init(SSBuffer *buffer)
{
    if (!buffer)
        return;
    memset(buffer, 0, sizeof(*buffer));
}

void ss_buffer_free(SSBuffer *buffer)
{
    if (!buffer)
        return;
    free(buffer->data);
    memset(buffer, 0, sizeof(*buffer));
}

static int ss_buffer_reserve(SSBuffer *buffer, size_t extra)
{
    size_t needed;
    size_t capacity;
    char *grown;

    if (!buffer || extra > SIZE_MAX - buffer->length - 1)
        return 0;
    needed = buffer->length + extra + 1;
    if (needed <= buffer->capacity)
        return 1;
    capacity = buffer->capacity ? buffer->capacity : 256;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2)
            return 0;
        capacity *= 2;
    }
    grown = realloc(buffer->data, capacity);
    if (!grown)
        return 0;
    buffer->data = grown;
    buffer->capacity = capacity;
    return 1;
}

int ss_buffer_append_n(SSBuffer *buffer, const char *text, size_t length)
{
    if (!buffer || (!text && length != 0) ||
        !ss_buffer_reserve(buffer, length))
        return 0;
    if (length)
        memcpy(buffer->data + buffer->length, text, length);
    buffer->length += length;
    buffer->data[buffer->length] = '\0';
    return 1;
}

int ss_buffer_append(SSBuffer *buffer, const char *text)
{
    return text && ss_buffer_append_n(buffer, text, strlen(text));
}

int ss_buffer_appendf(SSBuffer *buffer, const char *format, ...)
{
    va_list arguments;
    va_list copy;
    int needed;

    if (!buffer || !format)
        return 0;
    va_start(arguments, format);
    va_copy(copy, arguments);
    needed = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (needed < 0 || !ss_buffer_reserve(buffer, (size_t)needed)) {
        va_end(arguments);
        return 0;
    }
    (void)vsnprintf(buffer->data + buffer->length,
                    buffer->capacity - buffer->length, format, arguments);
    va_end(arguments);
    buffer->length += (size_t)needed;
    return 1;
}

int ss_valid_name(const char *name)
{
    size_t length;

    if (!name || (length = strlen(name)) == 0 || length > SS_MAX_NAME ||
        name[0] == '.')
        return 0;
    for (size_t index = 0; index < length; index++) {
        unsigned char character = (unsigned char)name[index];

        if (isalnum(character) || character == '-' || character == '_' ||
            character == '.')
            continue;
        return 0;
    }
    return 1;
}

int ss_valid_absolute_path(const char *path)
{
    const unsigned char *cursor;

    if (!path || path[0] != '/' || strlen(path) >= PATH_MAX)
        return 0;
    for (cursor = (const unsigned char *)path; *cursor; cursor++) {
        if (*cursor < 32 || *cursor == 127)
            return 0;
    }
    return 1;
}

const char *ss_access_name(SSAccess access)
{
    return access == SS_ACCESS_READ_WRITE ? "read-write" : "read-only";
}

int ss_access_parse(const char *text, SSAccess *access)
{
    if (!text || !access)
        return 0;
    if (strcmp(text, "read-write") == 0 || strcmp(text, "rw") == 0) {
        *access = SS_ACCESS_READ_WRITE;
        return 1;
    }
    if (strcmp(text, "read-only") == 0 || strcmp(text, "ro") == 0) {
        *access = SS_ACCESS_READ_ONLY;
        return 1;
    }
    return 0;
}

void ss_human_size(unsigned long long bytes, char *output, size_t output_size)
{
    static const char *units[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    double value = (double)bytes;
    size_t unit = 0;

    while (value >= 1000.0 && unit + 1 < sizeof(units) / sizeof(units[0])) {
        value /= 1000.0;
        unit++;
    }
    if (unit == 0)
        (void)snprintf(output, output_size, "%llu %s", bytes, units[unit]);
    else if (value >= 100.0)
        (void)snprintf(output, output_size, "%.0f %s", value, units[unit]);
    else
        (void)snprintf(output, output_size, "%.1f %s", value, units[unit]);
}

const char *ss_default_socket_path(SSPlatform platform)
{
    const char *override = getenv("SIMPLESERVE_SOCKET");

    if (override && ss_valid_absolute_path(override))
        return override;
    return platform == SS_PLATFORM_LINUX ? "/run/simpleserve.sock" :
                                           "/var/run/simpleserve.sock";
}

const char *ss_default_config_path(void)
{
    const char *override = getenv("SIMPLESERVE_CONFIG");

    if (override && ss_valid_absolute_path(override))
        return override;
    return "/etc/simpleserve.conf";
}

const char *ss_default_state_path(SSPlatform platform)
{
    const char *override = getenv("SIMPLESERVE_STATE");

    if (override && ss_valid_absolute_path(override))
        return override;
    return platform == SS_PLATFORM_LINUX ?
        "/var/lib/simpleserve/mounts.conf" :
        "/var/db/simpleserve/mounts.conf";
}

const char *ss_default_exports_path(SSPlatform platform)
{
    const char *override = getenv("SIMPLESERVE_EXPORTS");

    if (override && ss_valid_absolute_path(override))
        return override;
    return platform == SS_PLATFORM_LINUX ?
        "/etc/exports.d/simpleserve.exports" : "/etc/exports";
}

const char *ss_default_fstab_path(void)
{
    const char *override = getenv("SIMPLESERVE_FSTAB");

    if (override && ss_valid_absolute_path(override))
        return override;
    return "/etc/fstab";
}

static int ss_write_all(int fd, const void *data, size_t length)
{
    const unsigned char *cursor = data;

    while (length > 0) {
        ssize_t written = write(fd, cursor, length);

        if (written < 0) {
            if (errno == EINTR)
                continue;
            return 0;
        }
        if (written == 0) {
            errno = EIO;
            return 0;
        }
        cursor += (size_t)written;
        length -= (size_t)written;
    }
    return 1;
}

int ss_mkdir_parents(const char *path, mode_t mode, uid_t uid, gid_t gid,
                     char *error, size_t error_size)
{
    char copy[PATH_MAX];
    char *cursor;

    if (!ss_valid_absolute_path(path) || !ss_copy_string(copy, sizeof(copy), path)) {
        ss_error(error, error_size, "invalid directory path");
        return 0;
    }
    for (cursor = copy + 1;; cursor++) {
        struct stat status;
        char saved;

        while (*cursor && *cursor != '/')
            cursor++;
        saved = *cursor;
        *cursor = '\0';
        if (lstat(copy, &status) != 0) {
            if (errno != ENOENT || mkdir(copy, mode) != 0) {
                ss_error(error, error_size, "cannot create %s: %s", copy,
                         strerror(errno));
                return 0;
            }
            if (chown(copy, uid, gid) != 0) {
                ss_error(error, error_size, "cannot set owner on %s: %s", copy,
                         strerror(errno));
                return 0;
            }
        } else if (!S_ISDIR(status.st_mode) || S_ISLNK(status.st_mode)) {
            ss_error(error, error_size, "%s is not a real directory", copy);
            return 0;
        }
        *cursor = saved;
        if (!saved)
            break;
    }
    return 1;
}

int ss_atomic_write(const char *path, const void *data, size_t length,
                    mode_t mode, char *error, size_t error_size)
{
    char directory[PATH_MAX];
    char temporary[PATH_MAX];
    char *slash;
    int fd = -1;
    int directory_fd = -1;
    int ok = 0;

    if (!ss_valid_absolute_path(path) || (!data && length != 0) ||
        !ss_copy_string(directory, sizeof(directory), path)) {
        ss_error(error, error_size, "invalid file path");
        return 0;
    }
    slash = strrchr(directory, '/');
    if (!slash) {
        ss_error(error, error_size, "invalid file path");
        return 0;
    }
    if (slash == directory)
        slash[1] = '\0';
    else
        *slash = '\0';
    if (!ss_mkdir_parents(directory, 0755, 0, 0, error, error_size))
        return 0;
    if (snprintf(temporary, sizeof(temporary), "%s.tmp.%ld.%lld", path,
                 (long)getpid(), (long long)time(NULL)) >=
        (int)sizeof(temporary)) {
        ss_error(error, error_size, "temporary file path is too long");
        return 0;
    }
    fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, mode);
    if (fd < 0) {
        ss_error(error, error_size, "cannot create %s: %s", temporary,
                 strerror(errno));
        return 0;
    }
    if (fchmod(fd, mode) != 0 || !ss_write_all(fd, data, length) ||
        fsync(fd) != 0) {
        ss_error(error, error_size, "cannot write %s: %s", temporary,
                 strerror(errno));
        goto done;
    }
    if (close(fd) != 0) {
        fd = -1;
        ss_error(error, error_size, "cannot close %s: %s", temporary,
                 strerror(errno));
        goto done;
    }
    fd = -1;
    if (rename(temporary, path) != 0) {
        ss_error(error, error_size, "cannot replace %s: %s", path,
                 strerror(errno));
        goto done;
    }
    directory_fd = open(directory, O_RDONLY | O_DIRECTORY);
    if (directory_fd >= 0)
        (void)fsync(directory_fd);
    ok = 1;

done:
    if (fd >= 0)
        close(fd);
    if (directory_fd >= 0)
        close(directory_fd);
    if (!ok)
        unlink(temporary);
    return ok;
}

int ss_read_file(const char *path, size_t maximum, char **data,
                 size_t *length, char *error, size_t error_size)
{
    struct stat status;
    char *buffer;
    size_t capacity;
    size_t used = 0;
    int fd;

    if (!data || !length || !path || maximum == 0 || maximum == SIZE_MAX) {
        ss_error(error, error_size, "invalid read request");
        return 0;
    }
    *data = NULL;
    *length = 0;
    fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) {
        ss_error(error, error_size, "cannot open %s: %s", path,
                 strerror(errno));
        return 0;
    }
    if (fstat(fd, &status) != 0 || !S_ISREG(status.st_mode) ||
        status.st_size < 0 || (uintmax_t)status.st_size > maximum) {
        ss_error(error, error_size, "%s is not a valid small regular file", path);
        close(fd);
        return 0;
    }
    capacity = status.st_size > 0 ? (size_t)status.st_size : 4096;
    if (capacity > maximum)
        capacity = maximum;
    buffer = malloc(capacity + 1);
    if (!buffer) {
        ss_error(error, error_size, "out of memory");
        close(fd);
        return 0;
    }
    for (;;) {
        ssize_t result;

        if (used == capacity) {
            if (capacity == maximum) {
                char extra;

                result = read(fd, &extra, 1);
                if (result < 0 && errno == EINTR)
                    continue;
                if (result > 0) {
                    ss_error(error, error_size,
                             "%s is not a valid small regular file", path);
                    free(buffer);
                    close(fd);
                    return 0;
                }
                if (result < 0) {
                    ss_error(error, error_size, "cannot read %s: %s", path,
                             strerror(errno));
                    free(buffer);
                    close(fd);
                    return 0;
                }
                break;
            }
            {
                size_t grown = capacity <= maximum / 2 ?
                    capacity * 2 : maximum;
                char *resized = realloc(buffer, grown + 1);

                if (!resized) {
                    ss_error(error, error_size, "out of memory");
                    free(buffer);
                    close(fd);
                    return 0;
                }
                buffer = resized;
                capacity = grown;
            }
        }
        result = read(fd, buffer + used, capacity - used);

        if (result < 0) {
            if (errno == EINTR)
                continue;
            ss_error(error, error_size, "cannot read %s: %s", path,
                     strerror(errno));
            free(buffer);
            close(fd);
            return 0;
        }
        if (result == 0)
            break;
        used += (size_t)result;
    }
    close(fd);
    buffer[used] = '\0';
    *data = buffer;
    *length = used;
    return 1;
}

static char *ss_line_value(char *line)
{
    char *equals = strchr(line, '=');
    char *key_end;

    if (!equals)
        return NULL;
    key_end = equals;
    while (key_end > line && isspace((unsigned char)key_end[-1]))
        key_end--;
    *key_end = '\0';
    equals++;
    while (*equals == ' ' || *equals == '\t')
        equals++;
    return equals;
}

static void ss_strip_line_end(char *line)
{
    size_t length = strlen(line);

    while (length > 0 && (line[length - 1] == '\r' || line[length - 1] == '\n'))
        line[--length] = '\0';
}

static int ss_parse_unsigned(const char *text, unsigned long long maximum,
                             unsigned long long *value)
{
    char *end = NULL;
    unsigned long long parsed;

    if (!text || !*text || !value || *text == '-')
        return 0;
    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno || !end || *end || parsed > maximum)
        return 0;
    *value = parsed;
    return 1;
}

void ss_server_config_defaults(SSServerConfig *config)
{
    char hostname[256] = "simpleserve";
    char *dot;

    if (!config)
        return;
    memset(config, 0, sizeof(*config));
    if (gethostname(hostname, sizeof(hostname)) != 0)
        ss_copy_string(hostname, sizeof(hostname), "simpleserve");
    hostname[sizeof(hostname) - 1] = '\0';
    dot = strchr(hostname, '.');
    if (dot)
        *dot = '\0';
    if (!ss_valid_name(hostname))
        ss_copy_string(hostname, sizeof(hostname), "simpleserve");
    ss_copy_string(config->server_name, sizeof(config->server_name), hostname);
    config->port = SS_DEFAULT_PORT;
}

int ss_load_server_config(const char *path, SSServerConfig *config,
                          char *error, size_t error_size)
{
    char *contents = NULL;
    char *save = NULL;
    char *line;
    size_t length = 0;
    SSLocalShare *share = NULL;
    unsigned long long version = 0;
    int saw_version = 0;

    if (!path || !config) {
        ss_error(error, error_size, "invalid server configuration request");
        return 0;
    }
    ss_server_config_defaults(config);
    if (access(path, F_OK) != 0 && errno == ENOENT)
        return 1;
    if (!ss_read_file(path, SS_CONFIG_MAX, &contents, &length, error, error_size))
        return 0;
    (void)length;
    for (line = strtok_r(contents, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        char *value;

        ss_strip_line_end(line);
        while (*line == ' ' || *line == '\t')
            line++;
        if (!*line || *line == '#' || *line == ';')
            continue;
        if (*line == '[') {
            char *end = strrchr(line, ']');

            share = NULL;
            if (!end || end[1]) {
                ss_error(error, error_size, "malformed section in %s", path);
                free(contents);
                return 0;
            }
            *end = '\0';
            if (strcmp(line + 1, "server") == 0)
                continue;
            if (strncmp(line + 1, "share ", 6) == 0 &&
                ss_valid_name(line + 7)) {
                if (config->share_count >= SS_MAX_SHARES) {
                    ss_error(error, error_size, "too many shares in %s", path);
                    free(contents);
                    return 0;
                }
                share = &config->shares[config->share_count++];
                memset(share, 0, sizeof(*share));
                ss_copy_string(share->name, sizeof(share->name), line + 7);
                share->access = SS_ACCESS_READ_WRITE;
                continue;
            }
            ss_error(error, error_size, "unknown section [%s] in %s", line + 1,
                     path);
            free(contents);
            return 0;
        }
        value = ss_line_value(line);
        if (!value) {
            ss_error(error, error_size, "malformed setting in %s", path);
            free(contents);
            return 0;
        }
        if (!share) {
            unsigned long long parsed;

            if (strcmp(line, "version") == 0) {
                if (!ss_parse_unsigned(value, UINT_MAX, &version))
                    goto invalid_value;
                saw_version = 1;
            } else if (strcmp(line, "name") == 0) {
                if (!ss_valid_name(value) ||
                    !ss_copy_string(config->server_name,
                                    sizeof(config->server_name), value))
                    goto invalid_value;
            } else if (strcmp(line, "port") == 0) {
                if (!ss_parse_unsigned(value, 65535, &parsed) || parsed == 0)
                    goto invalid_value;
                config->port = (unsigned int)parsed;
            } else if (strcmp(line, "allowed_network") == 0) {
                if (config->network_count >= SS_MAX_NETWORKS ||
                    strlen(value) >= sizeof(config->allowed_networks[0]))
                    goto invalid_value;
                ss_copy_string(config->allowed_networks[config->network_count++],
                               sizeof(config->allowed_networks[0]), value);
            } else {
                goto invalid_key;
            }
        } else {
            unsigned long long parsed;

            if (strcmp(line, "path") == 0) {
                if (!ss_valid_absolute_path(value) ||
                    !ss_copy_string(share->configured_path,
                                    sizeof(share->configured_path), value))
                    goto invalid_value;
            } else if (strcmp(line, "filesystem_id") == 0) {
                if (!*value || strlen(value) > SS_MAX_ID ||
                    !ss_copy_string(share->filesystem_id,
                                    sizeof(share->filesystem_id), value))
                    goto invalid_value;
            } else if (strcmp(line, "source") == 0) {
                if (!ss_copy_string(share->source, sizeof(share->source), value))
                    goto invalid_value;
            } else if (strcmp(line, "fstype") == 0) {
                if (!ss_copy_string(share->fstype, sizeof(share->fstype), value))
                    goto invalid_value;
            } else if (strcmp(line, "access") == 0) {
                if (!ss_access_parse(value, &share->access))
                    goto invalid_value;
            } else if (strcmp(line, "owner_uid") == 0) {
                if (!ss_parse_unsigned(value, UINT_MAX, &parsed))
                    goto invalid_value;
                share->owner_uid = (uid_t)parsed;
            } else if (strcmp(line, "owner_gid") == 0) {
                if (!ss_parse_unsigned(value, UINT_MAX, &parsed))
                    goto invalid_value;
                share->owner_gid = (gid_t)parsed;
            } else {
                goto invalid_key;
            }
        }
        continue;

invalid_key:
        ss_error(error, error_size, "unknown setting %s in %s", line, path);
        free(contents);
        return 0;
invalid_value:
        ss_error(error, error_size, "invalid value for %s in %s", line, path);
        free(contents);
        return 0;
    }
    free(contents);
    if (!saw_version || version != SS_PROTOCOL_VERSION) {
        ss_error(error, error_size, "%s has unsupported or missing version", path);
        return 0;
    }
    for (size_t index = 0; index < config->share_count; index++) {
        if (!config->shares[index].configured_path[0] ||
            !config->shares[index].filesystem_id[0]) {
            ss_error(error, error_size, "share %s is incomplete in %s",
                     config->shares[index].name, path);
            return 0;
        }
    }
    return 1;
}

int ss_save_server_config(const char *path, const SSServerConfig *config,
                          char *error, size_t error_size)
{
    SSBuffer output;
    int ok = 0;

    if (!path || !config || !ss_valid_name(config->server_name) ||
        config->port == 0 || config->port > 65535 ||
        config->share_count > SS_MAX_SHARES) {
        ss_error(error, error_size, "invalid server configuration");
        return 0;
    }
    ss_buffer_init(&output);
    if (!ss_buffer_appendf(&output,
                           "# SimpleServe configuration\n"
                           "[server]\nversion=%d\nname=%s\nport=%u\n",
                           SS_PROTOCOL_VERSION, config->server_name,
                           config->port))
        goto memory_error;
    for (size_t index = 0; index < config->network_count; index++) {
        if (!ss_buffer_appendf(&output, "allowed_network=%s\n",
                               config->allowed_networks[index]))
            goto memory_error;
    }
    for (size_t index = 0; index < config->share_count; index++) {
        const SSLocalShare *share = &config->shares[index];

        if (!ss_valid_name(share->name) ||
            !ss_valid_absolute_path(share->configured_path) ||
            !share->filesystem_id[0]) {
            ss_error(error, error_size, "invalid share configuration");
            goto done;
        }
        if (!ss_buffer_appendf(
                &output,
                "\n[share %s]\npath=%s\nfilesystem_id=%s\nsource=%s\n"
                "fstype=%s\naccess=%s\nowner_uid=%llu\nowner_gid=%llu\n",
                share->name, share->configured_path, share->filesystem_id,
                share->source, share->fstype, ss_access_name(share->access),
                (unsigned long long)share->owner_uid,
                (unsigned long long)share->owner_gid))
            goto memory_error;
    }
    ok = ss_atomic_write(path, output.data ? output.data : "", output.length,
                         0600, error, error_size);
    goto done;

memory_error:
    ss_error(error, error_size, "out of memory");
done:
    ss_buffer_free(&output);
    return ok;
}

int ss_load_mount_config(const char *path, SSMountConfig *config,
                         char *error, size_t error_size)
{
    char *contents = NULL;
    char *save = NULL;
    char *line;
    size_t length = 0;
    SSClientMount *mount = NULL;
    unsigned long long version = 0;
    int saw_version = 0;

    if (!path || !config) {
        ss_error(error, error_size, "invalid mount configuration request");
        return 0;
    }
    memset(config, 0, sizeof(*config));
    if (access(path, F_OK) != 0 && errno == ENOENT)
        return 1;
    if (!ss_read_file(path, SS_CONFIG_MAX, &contents, &length, error, error_size))
        return 0;
    (void)length;
    for (line = strtok_r(contents, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        char *value;

        ss_strip_line_end(line);
        while (*line == ' ' || *line == '\t')
            line++;
        if (!*line || *line == '#' || *line == ';')
            continue;
        if (*line == '[') {
            char *end = strrchr(line, ']');

            mount = NULL;
            if (!end || end[1])
                goto malformed;
            *end = '\0';
            if (strcmp(line + 1, "mounts") == 0)
                continue;
            if (strcmp(line + 1, "mount") == 0) {
                if (config->mount_count >= SS_MAX_MOUNTS)
                    goto malformed;
                mount = &config->mounts[config->mount_count++];
                memset(mount, 0, sizeof(*mount));
                mount->remembered = 1;
                continue;
            }
            goto malformed;
        }
        value = ss_line_value(line);
        if (!value)
            goto malformed;
        if (!mount) {
            if (strcmp(line, "version") != 0 ||
                !ss_parse_unsigned(value, UINT_MAX, &version))
                goto malformed;
            saw_version = 1;
            continue;
        }
        {
            unsigned long long parsed;

            if (strcmp(line, "uid") == 0) {
                if (!ss_parse_unsigned(value, UINT_MAX, &parsed))
                    goto malformed;
                mount->uid = (uid_t)parsed;
            } else if (strcmp(line, "gid") == 0) {
                if (!ss_parse_unsigned(value, UINT_MAX, &parsed))
                    goto malformed;
                mount->gid = (gid_t)parsed;
            } else if (strcmp(line, "server") == 0) {
                if (!ss_valid_name(value) ||
                    !ss_copy_string(mount->server, sizeof(mount->server), value))
                    goto malformed;
            } else if (strcmp(line, "share") == 0) {
                if (!ss_valid_name(value) ||
                    !ss_copy_string(mount->share, sizeof(mount->share), value))
                    goto malformed;
            } else if (strcmp(line, "filesystem_id") == 0) {
                if (!*value || strlen(value) > SS_MAX_ID ||
                    !ss_copy_string(mount->filesystem_id,
                                    sizeof(mount->filesystem_id), value))
                    goto malformed;
            } else {
                goto malformed;
            }
        }
    }
    free(contents);
    if (!saw_version || version != SS_PROTOCOL_VERSION) {
        ss_error(error, error_size, "%s has unsupported or missing version", path);
        return 0;
    }
    for (size_t index = 0; index < config->mount_count; index++) {
        SSClientMount *entry = &config->mounts[index];

        if (!entry->server[0] || !entry->share[0] ||
            !entry->filesystem_id[0]) {
            ss_error(error, error_size, "incomplete remembered mount in %s", path);
            return 0;
        }
    }
    return 1;

malformed:
    ss_error(error, error_size, "malformed mount configuration in %s", path);
    free(contents);
    return 0;
}

int ss_save_mount_config(const char *path, const SSMountConfig *config,
                         char *error, size_t error_size)
{
    SSBuffer output;
    int ok = 0;

    if (!path || !config || config->mount_count > SS_MAX_MOUNTS) {
        ss_error(error, error_size, "invalid mount configuration");
        return 0;
    }
    ss_buffer_init(&output);
    if (!ss_buffer_appendf(&output,
                           "# SimpleServe remembered mounts\n"
                           "[mounts]\nversion=%d\n",
                           SS_PROTOCOL_VERSION))
        goto memory_error;
    for (size_t index = 0; index < config->mount_count; index++) {
        const SSClientMount *mount = &config->mounts[index];

        if (!mount->remembered)
            continue;
        if (!ss_valid_name(mount->server) || !ss_valid_name(mount->share) ||
            !mount->filesystem_id[0]) {
            ss_error(error, error_size, "invalid remembered mount");
            goto done;
        }
        if (!ss_buffer_appendf(
                &output,
                "\n[mount]\nuid=%llu\ngid=%llu\nserver=%s\nshare=%s\n"
                "filesystem_id=%s\n",
                (unsigned long long)mount->uid,
                (unsigned long long)mount->gid, mount->server, mount->share,
                mount->filesystem_id))
            goto memory_error;
    }
    ok = ss_atomic_write(path, output.data ? output.data : "", output.length,
                         0600, error, error_size);
    goto done;

memory_error:
    ss_error(error, error_size, "out of memory");
done:
    ss_buffer_free(&output);
    return ok;
}

static long long ss_monotonic_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;
    return (long long)now.tv_sec * 1000LL + now.tv_nsec / 1000000LL;
}

static int ss_trusted_executable(const char *path)
{
    struct stat status;

    if (!path || path[0] != '/' || stat(path, &status) != 0 ||
        !S_ISREG(status.st_mode) || access(path, X_OK) != 0)
        return 0;
    if (getenv("SIMPLESERVE_TEST_MODE"))
        return 1;
    return status.st_uid == 0 && (status.st_mode & (S_IWGRP | S_IWOTH)) == 0;
}

static int ss_capture_command(const char *path, char *const argv[],
                              char *output, size_t output_size,
                              int timeout_ms)
{
    static char safe_path[] =
        "PATH=/sbin:/bin:/usr/sbin:/usr/bin:/usr/local/sbin:/usr/local/bin";
    static char safe_locale[] = "LC_ALL=C";
    static char safe_lang[] = "LANG=C";
    static char safe_home[] = "HOME=/";
    static char *environment[] = {
        safe_path, safe_locale, safe_lang, safe_home, NULL
    };
    int pipes[2];
    int status = 0;
    int flags;
    size_t used = 0;
    int eof = 0;
    int done = 0;
    pid_t pid;
    long long deadline;

    if (!ss_trusted_executable(path) || !argv || !output || output_size < 2 ||
        timeout_ms < 1 || pipe(pipes) != 0)
        return 0;
    output[0] = '\0';
    pid = fork();
    if (pid == 0) {
        int null_fd;

        setpgid(0, 0);
        close(pipes[0]);
        if (dup2(pipes[1], STDOUT_FILENO) < 0)
            _exit(127);
        if (pipes[1] > STDERR_FILENO)
            close(pipes[1]);
        null_fd = open("/dev/null", O_RDWR);
        if (null_fd >= 0) {
            dup2(null_fd, STDIN_FILENO);
            dup2(null_fd, STDERR_FILENO);
            if (null_fd > STDERR_FILENO)
                close(null_fd);
        }
        execve(path, argv, environment);
        _exit(127);
    }
    if (pid < 0) {
        close(pipes[0]);
        close(pipes[1]);
        return 0;
    }
    close(pipes[1]);
    flags = fcntl(pipes[0], F_GETFL, 0);
    if (flags >= 0)
        (void)fcntl(pipes[0], F_SETFL, flags | O_NONBLOCK);
    deadline = ss_monotonic_ms() + timeout_ms;
    while (!eof || !done) {
        struct pollfd descriptor = {pipes[0], POLLIN | POLLHUP, 0};
        long long remaining = deadline - ss_monotonic_ms();
        int wait_ms = remaining > 100 ? 100 : (remaining > 0 ? (int)remaining : 0);

        if (remaining <= 0)
            break;
        (void)poll(&descriptor, 1, wait_ms);
        for (;;) {
            char chunk[512];
            ssize_t received = read(pipes[0], chunk, sizeof(chunk));

            if (received > 0) {
                size_t available = output_size - 1 - used;
                size_t copy = (size_t)received < available ?
                    (size_t)received : available;

                if (copy) {
                    memcpy(output + used, chunk, copy);
                    used += copy;
                }
                if (copy < (size_t)received)
                    break;
                continue;
            }
            if (received == 0)
                eof = 1;
            else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                eof = 1;
            break;
        }
        if (!done) {
            pid_t result = waitpid(pid, &status, WNOHANG);

            if (result == pid)
                done = 1;
            else if (result < 0 && errno != EINTR)
                done = 1;
        }
    }
    if (!done) {
        (void)kill(-pid, SIGKILL);
        (void)kill(pid, SIGKILL);
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
            ;
    }
    close(pipes[0]);
    output[used] = '\0';
    while (used > 0 && isspace((unsigned char)output[used - 1]))
        output[--used] = '\0';
    return done && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static int ss_filesystem_uuid(const char *source, char *identity,
                              size_t identity_size)
{
    static const char *candidates[] = {
        "/usr/local/sbin/blkid", "/sbin/blkid", "/usr/sbin/blkid",
        "/usr/local/bin/blkid", "/bin/blkid", "/usr/bin/blkid"
    };
    char output[SS_MAX_ID + 2];

    if (!source || strncmp(source, "/dev/", 5) != 0)
        return 0;
    for (size_t index = 0; index < sizeof(candidates) / sizeof(candidates[0]);
         index++) {
        char *arguments[] = {
            (char *)"blkid", (char *)"-s", (char *)"UUID", (char *)"-o",
            (char *)"value", (char *)source, NULL
        };

        if (!ss_capture_command(candidates[index], arguments, output,
                                sizeof(output), 3000))
            continue;
        if (output[0] && strlen(output) <= SS_MAX_ID) {
            for (const unsigned char *cursor = (unsigned char *)output;
                 *cursor; cursor++) {
                if (*cursor < 33 || *cursor > 126)
                    return 0;
            }
            return ss_copy_string(identity, identity_size, output);
        }
    }
    return 0;
}

static void ss_mount_size(const char *path, SSMountInfo *info)
{
    struct statvfs values;

    if (statvfs(path, &values) != 0)
        return;
    info->total_bytes = (unsigned long long)values.f_blocks * values.f_frsize;
    info->free_bytes = (unsigned long long)values.f_bavail * values.f_frsize;
    info->read_only = (values.f_flag & ST_RDONLY) != 0;
}

static void ss_mount_identity(SSMountInfo *info, const char *fallback)
{
    if (ss_filesystem_uuid(info->source, info->identity,
                           sizeof(info->identity)))
        return;
    if (strcmp(info->fstype, "zfs") == 0 && info->source[0]) {
        (void)snprintf(info->identity, sizeof(info->identity), "zfs:%.251s",
                       info->source);
        return;
    }
    (void)snprintf(info->identity, sizeof(info->identity),
                   "%.63s:%.125s:%.63s",
                   info->fstype[0] ? info->fstype : "fs",
                   info->source[0] ? info->source : "unknown", fallback);
}

#ifdef __linux__
static int ss_unescape_mount_field(const char *input, char *output,
                                   size_t output_size)
{
    size_t used = 0;

    if (!input || !output || output_size == 0)
        return 0;
    for (size_t index = 0; input[index]; index++) {
        unsigned char value = (unsigned char)input[index];

        if (value == '\\' && isdigit((unsigned char)input[index + 1]) &&
            isdigit((unsigned char)input[index + 2]) &&
            isdigit((unsigned char)input[index + 3])) {
            value = (unsigned char)((input[index + 1] - '0') * 64 +
                                    (input[index + 2] - '0') * 8 +
                                    (input[index + 3] - '0'));
            index += 3;
        }
        if (value == 0 || used + 1 >= output_size)
            return 0;
        output[used++] = (char)value;
    }
    output[used] = '\0';
    return 1;
}
#endif

static int ss_test_mount_lookup(const char *wanted_path,
                                const char *wanted_identity,
                                SSMountInfo *info)
{
    const char *path = getenv("SIMPLESERVE_TEST_MOUNTS");
    char *contents = NULL;
    char *save = NULL;
    char *line;
    size_t length = 0;
    char ignored[256];

    if (!path || !*path)
        return -1;
    if (!ss_read_file(path, SS_CONFIG_MAX, &contents, &length, ignored,
                      sizeof(ignored)))
        return 0;
    (void)length;
    for (line = strtok_r(contents, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        char *fields[7] = {0};
        char *field_save = NULL;
        size_t count = 0;
        char *field;

        for (field = strtok_r(line, "\t", &field_save); field && count < 7;
             field = strtok_r(NULL, "\t", &field_save))
            fields[count++] = field;
        if (count < 4)
            continue;
        if ((wanted_path && strcmp(fields[0], wanted_path) != 0) ||
            (wanted_identity && strcmp(fields[3], wanted_identity) != 0))
            continue;
        memset(info, 0, sizeof(*info));
        ss_copy_string(info->target, sizeof(info->target), fields[0]);
        ss_copy_string(info->source, sizeof(info->source), fields[1]);
        ss_copy_string(info->fstype, sizeof(info->fstype), fields[2]);
        ss_copy_string(info->identity, sizeof(info->identity), fields[3]);
        if (count > 4)
            info->total_bytes = strtoull(fields[4], NULL, 10);
        if (count > 5)
            info->free_bytes = strtoull(fields[5], NULL, 10);
        if (count > 6)
            info->read_only = strcmp(fields[6], "ro") == 0;
        free(contents);
        return 1;
    }
    free(contents);
    return 0;
}

#ifdef __FreeBSD__
static int ss_freebsd_mount_lookup(const char *wanted_path,
                                   const char *wanted_identity,
                                   SSMountInfo *info)
{
    struct statfs *mounts = NULL;
    int count = getmntinfo(&mounts, MNT_NOWAIT);

    if (count <= 0)
        return 0;
    for (int index = 0; index < count; index++) {
        SSMountInfo candidate;
        char fallback[64];

        memset(&candidate, 0, sizeof(candidate));
        if (!ss_copy_string(candidate.target, sizeof(candidate.target),
                            mounts[index].f_mntonname) ||
            !ss_copy_string(candidate.source, sizeof(candidate.source),
                            mounts[index].f_mntfromname) ||
            !ss_copy_string(candidate.fstype, sizeof(candidate.fstype),
                            mounts[index].f_fstypename))
            continue;
        if (wanted_path && strcmp(candidate.target, wanted_path) != 0)
            continue;
        (void)snprintf(fallback, sizeof(fallback), "%08x-%08x",
                       (unsigned int)mounts[index].f_fsid.val[0],
                       (unsigned int)mounts[index].f_fsid.val[1]);
        ss_mount_identity(&candidate, fallback);
        candidate.read_only = (mounts[index].f_flags & MNT_RDONLY) != 0;
        if (wanted_identity && strcmp(candidate.identity, wanted_identity) != 0)
            continue;
        if (strcmp(candidate.fstype, "autofs") != 0 &&
            strncmp(candidate.fstype, "nfs", 3) != 0)
            ss_mount_size(candidate.target, &candidate);
        *info = candidate;
        return 1;
    }
    return 0;
}
#endif

#ifdef __linux__
static int ss_linux_mount_lookup(const char *wanted_path,
                                 const char *wanted_identity,
                                 SSMountInfo *info)
{
    char *contents = NULL;
    char *save = NULL;
    char *line;
    size_t length = 0;
    char ignored[256];

    if (!ss_read_file("/proc/self/mountinfo", 8U * 1024U * 1024U,
                      &contents, &length, ignored, sizeof(ignored)))
        return 0;
    (void)length;
    for (line = strtok_r(contents, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        char *tokens[64];
        char *token_save = NULL;
        char *token;
        size_t count = 0;
        size_t separator = SIZE_MAX;
        SSMountInfo candidate;
        char fallback[64];

        for (token = strtok_r(line, " ", &token_save);
             token && count < sizeof(tokens) / sizeof(tokens[0]);
             token = strtok_r(NULL, " ", &token_save))
            tokens[count++] = token;
        if (count < 10)
            continue;
        for (size_t index = 6; index < count; index++) {
            if (strcmp(tokens[index], "-") == 0) {
                separator = index;
                break;
            }
        }
        if (separator == SIZE_MAX || separator + 2 >= count)
            continue;
        memset(&candidate, 0, sizeof(candidate));
        if (!ss_unescape_mount_field(tokens[4], candidate.target,
                                     sizeof(candidate.target)) ||
            !ss_unescape_mount_field(tokens[separator + 2], candidate.source,
                                     sizeof(candidate.source)) ||
            !ss_copy_string(candidate.fstype, sizeof(candidate.fstype),
                            tokens[separator + 1]))
            continue;
        if (wanted_path && strcmp(candidate.target, wanted_path) != 0)
            continue;
        (void)snprintf(fallback, sizeof(fallback), "%s", tokens[2]);
        ss_mount_identity(&candidate, fallback);
        if (strstr(tokens[5], "ro") == tokens[5] ||
            strstr(tokens[5], ",ro") != NULL)
            candidate.read_only = 1;
        if (wanted_identity && strcmp(candidate.identity, wanted_identity) != 0)
            continue;
        if (strcmp(candidate.fstype, "autofs") != 0 &&
            strncmp(candidate.fstype, "nfs", 3) != 0)
            ss_mount_size(candidate.target, &candidate);
        *info = candidate;
        free(contents);
        return 1;
    }
    free(contents);
    return 0;
}
#endif

static int ss_mount_lookup(const char *wanted_path, const char *wanted_identity,
                           SSMountInfo *info)
{
    int test_result = ss_test_mount_lookup(wanted_path, wanted_identity, info);

    if (test_result >= 0)
        return test_result;
#ifdef __FreeBSD__
    return ss_freebsd_mount_lookup(wanted_path, wanted_identity, info);
#elif defined(__linux__)
    return ss_linux_mount_lookup(wanted_path, wanted_identity, info);
#else
    (void)wanted_path;
    (void)wanted_identity;
    (void)info;
    return 0;
#endif
}

int ss_mount_info_exact(const char *path, SSMountInfo *info,
                        char *error, size_t error_size)
{
    if (!ss_valid_absolute_path(path) || !info) {
        ss_error(error, error_size, "%s is not a valid absolute directory path",
                 path ? path : "path");
        return 0;
    }
    if (!ss_mount_lookup(path, NULL, info)) {
        ss_error(error, error_size,
                 "%s is not the root of a currently mounted filesystem",
                 path);
        return 0;
    }
    return 1;
}

int ss_find_mount_by_identity(const char *identity, SSMountInfo *info,
                              char *error, size_t error_size)
{
    if (!identity || !*identity || !info) {
        ss_error(error, error_size, "invalid filesystem identity");
        return 0;
    }
    if (!ss_mount_lookup(NULL, identity, info)) {
        ss_error(error, error_size, "filesystem %s is not mounted", identity);
        return 0;
    }
    if (strcmp(info->fstype, "autofs") == 0 ||
        strncmp(info->fstype, "nfs", 3) == 0) {
        ss_error(error, error_size, "filesystem %s is not exportable", identity);
        return 0;
    }
    return 1;
}

int ss_user_can_access(uid_t uid, gid_t gid, const char *path,
                       SSAccess access_mode, char *error, size_t error_size)
{
    int required = R_OK | X_OK;
    int status = 0;
    pid_t child;

    if (access_mode == SS_ACCESS_READ_WRITE)
        required |= W_OK;
    if (geteuid() != 0 || uid == geteuid()) {
        if (access(path, required) == 0)
            return 1;
        ss_error(error, error_size, "you do not have %s access to %s",
                 ss_access_name(access_mode), path);
        return 0;
    }
    child = fork();
    if (child == 0) {
        struct passwd *account = getpwuid(uid);

        if (!account || initgroups(account->pw_name, gid) != 0 ||
            setgid(gid) != 0 || setuid(uid) != 0)
            _exit(2);
        _exit(access(path, required) == 0 ? 0 : 1);
    }
    if (child < 0) {
        ss_error(error, error_size, "cannot verify directory access: %s",
                 strerror(errno));
        return 0;
    }
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            ss_error(error, error_size, "cannot verify directory access: %s",
                     strerror(errno));
            return 0;
        }
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        return 1;
    ss_error(error, error_size, "you do not have %s access to %s",
             ss_access_name(access_mode), path);
    return 0;
}

static int ss_network_valid(const char *network)
{
    char copy[64];
    char *slash;
    char *end = NULL;
    struct in_addr address;
    long prefix;

    if (!network || !ss_copy_string(copy, sizeof(copy), network) ||
        !(slash = strchr(copy, '/')))
        return 0;
    *slash++ = '\0';
    errno = 0;
    prefix = strtol(slash, &end, 10);
    return !errno && end && !*end && prefix >= 1 && prefix <= 32 &&
           inet_pton(AF_INET, copy, &address) == 1;
}

static int ss_private_ipv4(uint32_t address)
{
    return (address & 0xff000000U) == 0x0a000000U ||
           (address & 0xfff00000U) == 0xac100000U ||
           (address & 0xffff0000U) == 0xc0a80000U ||
           (address & 0xffff0000U) == 0xa9fe0000U ||
           (address & 0xffc00000U) == 0x64400000U;
}

int ss_private_ipv4_address(const char *text)
{
    struct in_addr address;

    return text && inet_pton(AF_INET, text, &address) == 1 &&
           ss_private_ipv4(ntohl(address.s_addr));
}

int ss_collect_private_networks(char networks[][64], size_t maximum,
                                size_t *count, char *error,
                                size_t error_size)
{
    const char *test = getenv("SIMPLESERVE_TEST_NETWORKS");
    struct ifaddrs *interfaces = NULL;

    if (!networks || maximum == 0 || !count) {
        ss_error(error, error_size, "invalid network collection request");
        return 0;
    }
    *count = 0;
    if (test && *test) {
        char copy[1024];
        char *save = NULL;
        char *entry;

        if (!ss_copy_string(copy, sizeof(copy), test))
            goto no_network;
        for (entry = strtok_r(copy, ",", &save); entry;
             entry = strtok_r(NULL, ",", &save)) {
            if (!ss_network_valid(entry) || *count >= maximum)
                goto no_network;
            ss_copy_string(networks[(*count)++], 64, entry);
        }
        return *count > 0;
    }
    if (getifaddrs(&interfaces) != 0) {
        ss_error(error, error_size, "cannot list network interfaces: %s",
                 strerror(errno));
        return 0;
    }
    for (struct ifaddrs *entry = interfaces; entry && *count < maximum;
         entry = entry->ifa_next) {
        struct sockaddr_in *address;
        struct sockaddr_in *netmask;
        uint32_t host_address;
        uint32_t host_mask;
        uint32_t host_network;
        unsigned int prefix = 0;
        char address_text[INET_ADDRSTRLEN];
        char candidate[64];
        int duplicate = 0;

        if (!entry->ifa_addr || !entry->ifa_netmask ||
            entry->ifa_addr->sa_family != AF_INET ||
            !(entry->ifa_flags & IFF_UP) ||
            !(entry->ifa_flags & IFF_MULTICAST) ||
            (entry->ifa_flags & IFF_LOOPBACK))
            continue;
        address = (struct sockaddr_in *)entry->ifa_addr;
        netmask = (struct sockaddr_in *)entry->ifa_netmask;
        host_address = ntohl(address->sin_addr.s_addr);
        host_mask = ntohl(netmask->sin_addr.s_addr);
        if (!ss_private_ipv4(host_address))
            continue;
        {
            uint32_t mask = host_mask;
            int saw_zero = 0;

            for (int bit = 31; bit >= 0; bit--) {
                if (mask & (1U << bit)) {
                    if (saw_zero) {
                        prefix = 0;
                        break;
                    }
                    prefix++;
                } else {
                    saw_zero = 1;
                }
            }
        }
        if (prefix == 0)
            continue;
        host_network = host_address & host_mask;
        {
            struct in_addr network_address;

            network_address.s_addr = htonl(host_network);
            if (!inet_ntop(AF_INET, &network_address, address_text,
                           sizeof(address_text)))
                continue;
        }
        (void)snprintf(candidate, sizeof(candidate), "%s/%u", address_text,
                       prefix);
        for (size_t index = 0; index < *count; index++) {
            if (strcmp(networks[index], candidate) == 0) {
                duplicate = 1;
                break;
            }
        }
        if (!duplicate)
            ss_copy_string(networks[(*count)++], 64, candidate);
    }
    freeifaddrs(interfaces);
    interfaces = NULL;
    if (*count > 0)
        return 1;

no_network:
    if (interfaces)
        freeifaddrs(interfaces);
    ss_error(error, error_size,
             "no active private IPv4 LAN was found; configure allowed_network explicitly");
    return 0;
}

static int ss_mount_field_escape(const char *input, SSBuffer *output)
{
    for (const unsigned char *cursor = (const unsigned char *)input;
         cursor && *cursor; cursor++) {
        switch (*cursor) {
        case ' ':
            if (!ss_buffer_append(output, "\\040"))
                return 0;
            break;
        case '\t':
            if (!ss_buffer_append(output, "\\011"))
                return 0;
            break;
        case '#':
            if (!ss_buffer_append(output, "\\043"))
                return 0;
            break;
        case '\\':
            if (!ss_buffer_append(output, "\\134"))
                return 0;
            break;
        default:
            if (*cursor < 32 || *cursor == 127 ||
                !ss_buffer_append_n(output, (const char *)cursor, 1))
                return 0;
            break;
        }
    }
    return 1;
}

int ss_render_exports(SSPlatform platform, const SSServerConfig *config,
                      SSBuffer *output, char *error, size_t error_size)
{
    char networks[SS_MAX_NETWORKS][64];
    size_t network_count = 0;

    if (!config || !output || (platform != SS_PLATFORM_FREEBSD &&
                               platform != SS_PLATFORM_LINUX)) {
        ss_error(error, error_size, "unsupported export platform");
        return 0;
    }
    if (config->network_count) {
        network_count = config->network_count;
        for (size_t index = 0; index < network_count; index++) {
            if (!ss_network_valid(config->allowed_networks[index]) ||
                !ss_copy_string(networks[index], sizeof(networks[index]),
                                config->allowed_networks[index])) {
                ss_error(error, error_size, "invalid allowed network %s",
                         config->allowed_networks[index]);
                return 0;
            }
        }
    } else if (!ss_collect_private_networks(networks, SS_MAX_NETWORKS,
                                            &network_count, error,
                                            error_size)) {
        return 0;
    }
    if (!ss_buffer_append(output,
                          "# Generated by SimpleServe. Manual edits are replaced.\n"))
        goto memory_error;
    for (size_t share_index = 0; share_index < config->share_count;
         share_index++) {
        const SSLocalShare *share = &config->shares[share_index];

        if (!share->active)
            continue;
        if (!ss_valid_absolute_path(share->current_path)) {
            ss_error(error, error_size, "share %s has an invalid active path",
                     share->name);
            return 0;
        }
        for (size_t network_index = 0; network_index < network_count;
             network_index++) {
            if (!ss_mount_field_escape(share->current_path, output))
                goto memory_error;
            if (platform == SS_PLATFORM_FREEBSD) {
                if (!ss_buffer_appendf(
                        output, " %s-mapall=%llu:%llu -network=%s\n",
                        share->access == SS_ACCESS_READ_ONLY ? "-ro " : "",
                        (unsigned long long)share->owner_uid,
                        (unsigned long long)share->owner_gid,
                        networks[network_index]))
                    goto memory_error;
            } else {
                if (!ss_buffer_appendf(
                        output,
                        " %s(%s,sync,no_subtree_check,all_squash,anonuid=%llu,"
                        "anongid=%llu)\n",
                        networks[network_index],
                        share->access == SS_ACCESS_READ_ONLY ? "ro" : "rw",
                        (unsigned long long)share->owner_uid,
                        (unsigned long long)share->owner_gid))
                    goto memory_error;
            }
        }
    }
    return 1;

memory_error:
    ss_error(error, error_size, "out of memory while generating NFS exports");
    return 0;
}

static int ss_replace_managed_block(const char *existing, const char *managed,
                                    const char *begin_marker,
                                    const char *end_marker,
                                    const char *description, SSBuffer *output,
                                    char *error, size_t error_size)
{
    const char *cursor = existing ? existing : "";
    const char *begin = strstr(cursor, begin_marker);
    const char *end = NULL;
    const char *extra;

    if (!output || !managed || !begin_marker || !end_marker || !description) {
        ss_error(error, error_size, "invalid managed block request");
        return 0;
    }
    if (begin) {
        end = strstr(begin, end_marker);
        if (!end) {
            ss_error(error, error_size,
                     "existing %s has an unterminated SimpleServe block",
                     description);
            return 0;
        }
        extra = strstr(end + strlen(end_marker), begin_marker);
        if (extra) {
            ss_error(error, error_size,
                     "existing %s has multiple SimpleServe blocks", description);
            return 0;
        }
        if (!ss_buffer_append_n(output, cursor, (size_t)(begin - cursor)))
            goto memory_error;
        end += strlen(end_marker);
        if (*end == '\r')
            end++;
        if (*end == '\n')
            end++;
        cursor = end;
    }
    if (!ss_buffer_append(output, cursor))
        goto memory_error;
    if (managed[0]) {
        if (output->length && output->data[output->length - 1] != '\n' &&
            !ss_buffer_append(output, "\n"))
            goto memory_error;
        if (!ss_buffer_append(output, begin_marker) ||
            !ss_buffer_append(output, "\n") ||
            !ss_buffer_append(output, managed) ||
            (output->length && output->data[output->length - 1] != '\n' &&
             !ss_buffer_append(output, "\n")) ||
            !ss_buffer_append(output, end_marker) ||
            !ss_buffer_append(output, "\n"))
            goto memory_error;
    }
    return 1;

memory_error:
    ss_error(error, error_size, "out of memory while updating %s", description);
    return 0;
}

int ss_replace_managed_exports(const char *existing, const char *managed,
                               SSBuffer *output, char *error,
                               size_t error_size)
{
    return ss_replace_managed_block(existing, managed, SS_EXPORTS_BEGIN,
                                    SS_EXPORTS_END, "/etc/exports", output,
                                    error, error_size);
}

static int ss_uuid_identity_valid(const char *identity)
{
    size_t length;

    if (!identity || (length = strlen(identity)) < 4 || length > SS_MAX_ID)
        return 0;
    for (const unsigned char *cursor = (const unsigned char *)identity;
         *cursor; cursor++) {
        if (!isalnum(*cursor) && *cursor != '-' && *cursor != '_' &&
            *cursor != '.')
            return 0;
    }
    return 1;
}

static int ss_fstype_valid(const char *fstype)
{
    size_t length;

    if (!fstype || (length = strlen(fstype)) == 0 || length >= 64)
        return 0;
    for (const unsigned char *cursor = (const unsigned char *)fstype;
         *cursor; cursor++) {
        if (!isalnum(*cursor) && *cursor != '-' && *cursor != '_' &&
            *cursor != '.' && *cursor != '+')
            return 0;
    }
    return 1;
}

int ss_render_fstab(const SSServerConfig *config, SSBuffer *output,
                    char *error, size_t error_size)
{
    int wrote_header = 0;

    if (!config || !output) {
        ss_error(error, error_size, "invalid fstab generation request");
        return 0;
    }
    for (size_t index = 0; index < config->share_count; index++) {
        const SSLocalShare *share = &config->shares[index];

        /* Composite fallback identities contain ':' and cannot be used as
         * stable UUID= selectors. Such filesystems remain shareable, but are
         * deliberately omitted from boot mount management. */
        if (!ss_uuid_identity_valid(share->filesystem_id))
            continue;
        if (!ss_valid_absolute_path(share->configured_path)) {
            ss_error(error, error_size,
                     "share %s has an invalid configured mount path",
                     share->name);
            return 0;
        }
        if (!ss_fstype_valid(share->fstype)) {
            ss_error(error, error_size,
                     "share %s has an invalid filesystem type", share->name);
            return 0;
        }
        if (!wrote_header) {
            if (!ss_buffer_append(
                    output,
                    "# Generated by SimpleServe. Manual edits are replaced.\n"))
                goto memory_error;
            wrote_header = 1;
        }
        if (!ss_buffer_append(output, "UUID=") ||
            !ss_buffer_append(output, share->filesystem_id) ||
            !ss_buffer_append(output, " ") ||
            !ss_mount_field_escape(share->configured_path, output) ||
            !ss_buffer_appendf(
                output,
                " %s defaults,nofail,nosuid,nodev,x-systemd.device-timeout=10s 0 2\n",
                share->fstype))
            goto memory_error;
    }
    return 1;

memory_error:
    ss_error(error, error_size, "out of memory while generating /etc/fstab");
    return 0;
}

int ss_replace_managed_fstab(const char *existing, const char *managed,
                             SSBuffer *output, char *error,
                             size_t error_size)
{
    return ss_replace_managed_block(existing, managed, SS_FSTAB_BEGIN,
                                    SS_FSTAB_END, "/etc/fstab", output, error,
                                    error_size);
}

int ss_render_manifest(const SSServerConfig *config, SSBuffer *output,
                       char *error, size_t error_size)
{
    char hostname[256] = "";

    if (!config || !output || !ss_valid_name(config->server_name)) {
        ss_error(error, error_size, "invalid manifest configuration");
        return 0;
    }
    if (gethostname(hostname, sizeof(hostname)) != 0)
        ss_copy_string(hostname, sizeof(hostname), config->server_name);
    hostname[sizeof(hostname) - 1] = '\0';
    for (const unsigned char *cursor = (unsigned char *)hostname; *cursor;
         cursor++) {
        if (*cursor < 33 || *cursor > 126 || *cursor == '[' || *cursor == ']' ||
            *cursor == '=') {
            ss_copy_string(hostname, sizeof(hostname), config->server_name);
            break;
        }
    }
    if (!ss_buffer_appendf(output,
                           "[server]\nversion=%d\nname=%s\nhostname=%s\n"
                           "protocol=nfs\n\n",
                           SS_PROTOCOL_VERSION, config->server_name, hostname))
        goto memory_error;
    for (size_t index = 0; index < config->share_count; index++) {
        const SSLocalShare *share = &config->shares[index];

        if (!share->active)
            continue;
        if (!ss_valid_name(share->name) ||
            !ss_valid_absolute_path(share->current_path) ||
            !share->filesystem_id[0]) {
            ss_error(error, error_size, "active share %s is invalid", share->name);
            return 0;
        }
        if (!ss_buffer_appendf(
                output,
                "[share %s]\nprotocol=nfs\nexport=%s\naccess=%s\n"
                "uuid=%s\nsize=%llu\nfree=%llu\n\n",
                share->name, share->current_path,
                ss_access_name(share->access), share->filesystem_id,
                share->total_bytes, share->free_bytes))
            goto memory_error;
    }
    return 1;

memory_error:
    ss_error(error, error_size, "out of memory while generating manifest");
    return 0;
}

int ss_parse_manifest(const char *manifest, const char *address,
                      unsigned int port, SSRemoteServer *server,
                      char *error, size_t error_size)
{
    enum {
        SERVER_VERSION = 1U << 0,
        SERVER_NAME = 1U << 1,
        SERVER_HOSTNAME = 1U << 2,
        SERVER_PROTOCOL = 1U << 3,
        SHARE_PROTOCOL = 1U << 0,
        SHARE_EXPORT = 1U << 1,
        SHARE_ACCESS = 1U << 2,
        SHARE_UUID = 1U << 3,
        SHARE_SIZE = 1U << 4,
        SHARE_FREE = 1U << 5
    };
    const unsigned int required_server_fields =
        SERVER_VERSION | SERVER_NAME | SERVER_HOSTNAME | SERVER_PROTOCOL;
    const unsigned int required_share_fields =
        SHARE_PROTOCOL | SHARE_EXPORT | SHARE_ACCESS | SHARE_UUID |
        SHARE_SIZE | SHARE_FREE;
    char *copy = NULL;
    char *line;
    char *save = NULL;
    SSRemoteShare *share = NULL;
    unsigned int share_fields[SS_MAX_REMOTE_SHARES] = {0};
    unsigned int server_fields = 0;
    unsigned long long version = 0;
    int server_section = 0;

    if (!manifest || !address || !server || port == 0 || port > 65535 ||
        strlen(manifest) > SS_CONFIG_MAX || strlen(address) >= sizeof(server->address)) {
        ss_error(error, error_size, "invalid manifest response");
        return 0;
    }
    memset(server, 0, sizeof(*server));
    ss_copy_string(server->address, sizeof(server->address), address);
    server->port = port;
    copy = strdup(manifest);
    if (!copy) {
        ss_error(error, error_size, "out of memory");
        return 0;
    }
    for (line = strtok_r(copy, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        char *value;

        ss_strip_line_end(line);
        while (*line == ' ' || *line == '\t')
            line++;
        if (!*line || *line == '#' || *line == ';')
            continue;
        if (*line == '[') {
            char *end = strrchr(line, ']');

            share = NULL;
            server_section = 0;
            if (!end || end[1])
                goto malformed;
            *end = '\0';
            if (strcmp(line + 1, "server") == 0) {
                server_section = 1;
                continue;
            }
            if (strncmp(line + 1, "share ", 6) == 0 &&
                ss_valid_name(line + 7)) {
                if (server->share_count >= SS_MAX_REMOTE_SHARES)
                    goto malformed;
                share = &server->shares[server->share_count++];
                memset(share, 0, sizeof(*share));
                ss_copy_string(share->name, sizeof(share->name), line + 7);
                continue;
            }
            goto malformed;
        }
        value = ss_line_value(line);
        if (!value)
            goto malformed;
        if (server_section) {
            unsigned long long parsed;

            if (strcmp(line, "version") == 0) {
                if ((server_fields & SERVER_VERSION) ||
                    !ss_parse_unsigned(value, UINT_MAX, &parsed))
                    goto malformed;
                version = parsed;
                server_fields |= SERVER_VERSION;
            } else if (strcmp(line, "name") == 0) {
                if ((server_fields & SERVER_NAME) || !ss_valid_name(value) ||
                    !ss_copy_string(server->name, sizeof(server->name), value))
                    goto malformed;
                server_fields |= SERVER_NAME;
            } else if (strcmp(line, "hostname") == 0) {
                if ((server_fields & SERVER_HOSTNAME) || !*value ||
                    !ss_copy_string(server->hostname,
                                    sizeof(server->hostname), value))
                    goto malformed;
                server_fields |= SERVER_HOSTNAME;
            } else if (strcmp(line, "protocol") == 0) {
                if ((server_fields & SERVER_PROTOCOL) ||
                    strcmp(value, "nfs") != 0)
                    goto malformed;
                server_fields |= SERVER_PROTOCOL;
            } else {
                goto malformed;
            }
        } else if (share) {
            size_t share_index = (size_t)(share - server->shares);
            unsigned int *fields = &share_fields[share_index];
            unsigned long long parsed;

            if (strcmp(line, "protocol") == 0) {
                if ((*fields & SHARE_PROTOCOL) || strcmp(value, "nfs") != 0)
                    goto malformed;
                *fields |= SHARE_PROTOCOL;
            } else if (strcmp(line, "export") == 0) {
                if ((*fields & SHARE_EXPORT) ||
                    !ss_valid_absolute_path(value) ||
                    !ss_copy_string(share->export_path,
                                    sizeof(share->export_path), value))
                    goto malformed;
                *fields |= SHARE_EXPORT;
            } else if (strcmp(line, "access") == 0) {
                if ((*fields & SHARE_ACCESS) ||
                    !ss_access_parse(value, &share->access))
                    goto malformed;
                *fields |= SHARE_ACCESS;
            } else if (strcmp(line, "uuid") == 0) {
                if ((*fields & SHARE_UUID) || !*value ||
                    strlen(value) > SS_MAX_ID ||
                    !ss_copy_string(share->filesystem_id,
                                    sizeof(share->filesystem_id), value))
                    goto malformed;
                *fields |= SHARE_UUID;
            } else if (strcmp(line, "size") == 0) {
                if ((*fields & SHARE_SIZE) ||
                    !ss_parse_unsigned(value, ULLONG_MAX, &parsed))
                    goto malformed;
                share->total_bytes = parsed;
                *fields |= SHARE_SIZE;
            } else if (strcmp(line, "free") == 0) {
                if ((*fields & SHARE_FREE) ||
                    !ss_parse_unsigned(value, ULLONG_MAX, &parsed))
                    goto malformed;
                share->free_bytes = parsed;
                *fields |= SHARE_FREE;
            } else {
                goto malformed;
            }
        } else {
            goto malformed;
        }
    }
    free(copy);
    if (server_fields != required_server_fields ||
        version != SS_PROTOCOL_VERSION) {
        ss_error(error, error_size, "manifest has unsupported version or no server name");
        return 0;
    }
    for (size_t index = 0; index < server->share_count; index++) {
        SSRemoteShare *entry = &server->shares[index];

        if (share_fields[index] != required_share_fields) {
            ss_error(error, error_size, "share %s is incomplete in manifest",
                     entry->name);
            return 0;
        }
    }
    server->reachable = 1;
    return 1;

malformed:
    ss_error(error, error_size, "malformed SimpleServe manifest");
    free(copy);
    return 0;
}

void ss_command_init(SSCommand *command)
{
    if (!command)
        return;
    memset(command, 0, sizeof(*command));
}

int ss_command_add(SSCommand *command, const char *argument)
{
    if (!command || !argument || command->argc >= SS_COMMAND_MAX_ARGS ||
        !ss_copy_string(command->args[command->argc],
                        sizeof(command->args[command->argc]), argument))
        return 0;
    command->argv[command->argc] = command->args[command->argc];
    command->argc++;
    command->argv[command->argc] = NULL;
    return 1;
}

int ss_build_mount_command(SSPlatform platform, const char *address,
                           const char *export_path, const char *target,
                           SSAccess access, SSCommand *command,
                           char *error, size_t error_size)
{
    char source[PATH_MAX];
    char options[256];
    struct in_addr parsed_address;

    if (!command || !address || inet_pton(AF_INET, address, &parsed_address) != 1 ||
        !ss_valid_absolute_path(export_path) || !ss_valid_absolute_path(target) ||
        snprintf(source, sizeof(source), "%s:%s", address, export_path) >=
            (int)sizeof(source)) {
        ss_error(error, error_size, "invalid NFS mount request");
        return 0;
    }
    ss_command_init(command);
    if (platform == SS_PLATFORM_FREEBSD) {
        /*
         * READDIRPLUS avoids a separate LOOKUP round trip for every entry in
         * large directories.  Four-block read-ahead is FreeBSD's supported
         * maximum and keeps sequential reads moving over a LAN connection.
         */
        (void)snprintf(options, sizeof(options),
                       "nfsv3,tcp,nosuid,rdirplus,readahead=4%s",
                       access == SS_ACCESS_READ_ONLY ? ",ro" : "");
        if (!ss_command_add(command, "/sbin/mount_nfs") ||
            !ss_command_add(command, "-o") ||
            !ss_command_add(command, options) ||
            !ss_command_add(command, source) ||
            !ss_command_add(command, target))
            goto too_long;
        return 1;
    }
    if (platform == SS_PLATFORM_LINUX) {
        (void)snprintf(options, sizeof(options),
                       "vers=3,proto=tcp,nosuid,nodev%s",
                       access == SS_ACCESS_READ_ONLY ? ",ro" : "");
        if (!ss_command_add(command, "/bin/mount") ||
            !ss_command_add(command, "-t") ||
            !ss_command_add(command, "nfs") ||
            !ss_command_add(command, "-o") ||
            !ss_command_add(command, options) ||
            !ss_command_add(command, source) ||
            !ss_command_add(command, target))
            goto too_long;
        return 1;
    }
    ss_error(error, error_size, "unsupported mount platform");
    return 0;

too_long:
    ss_error(error, error_size, "NFS mount command is too long");
    return 0;
}

int ss_build_unmount_command(SSPlatform platform, const char *target,
                             int force, SSCommand *command,
                             char *error, size_t error_size)
{
    const char *program;

    if (!command || !ss_valid_absolute_path(target)) {
        ss_error(error, error_size, "invalid unmount request");
        return 0;
    }
    if (platform == SS_PLATFORM_FREEBSD)
        program = "/sbin/umount";
    else if (platform == SS_PLATFORM_LINUX)
        program = "/bin/umount";
    else {
        ss_error(error, error_size, "unsupported unmount platform");
        return 0;
    }
    ss_command_init(command);
    if (!ss_command_add(command, program) ||
        (force && !ss_command_add(command, "-f")) ||
        !ss_command_add(command, target)) {
        ss_error(error, error_size, "unmount command is too long");
        return 0;
    }
    return 1;
}

static int ss_read_all(int fd, void *data, size_t length)
{
    unsigned char *cursor = data;

    while (length > 0) {
        ssize_t received = read(fd, cursor, length);

        if (received < 0) {
            if (errno == EINTR)
                continue;
            return 0;
        }
        if (received == 0) {
            errno = ECONNRESET;
            return 0;
        }
        cursor += (size_t)received;
        length -= (size_t)received;
    }
    return 1;
}

int ss_send_frame(int fd, const void *data, size_t length,
                  char *error, size_t error_size)
{
    uint32_t encoded;

    if (fd < 0 || (!data && length != 0) || length > SS_MAX_FRAME) {
        ss_error(error, error_size, "invalid protocol frame");
        return 0;
    }
    encoded = htonl((uint32_t)length);
    if (!ss_write_all(fd, &encoded, sizeof(encoded)) ||
        !ss_write_all(fd, data, length)) {
        ss_error(error, error_size, "cannot send daemon request: %s",
                 strerror(errno));
        return 0;
    }
    return 1;
}

int ss_receive_frame(int fd, char **data, size_t *length,
                     char *error, size_t error_size)
{
    uint32_t encoded;
    size_t decoded;
    char *buffer;

    if (fd < 0 || !data || !length) {
        ss_error(error, error_size, "invalid protocol frame request");
        return 0;
    }
    *data = NULL;
    *length = 0;
    if (!ss_read_all(fd, &encoded, sizeof(encoded))) {
        ss_error(error, error_size, "cannot receive daemon response: %s",
                 strerror(errno));
        return 0;
    }
    decoded = ntohl(encoded);
    if (decoded > SS_MAX_FRAME) {
        ss_error(error, error_size, "daemon frame is too large");
        return 0;
    }
    buffer = malloc(decoded + 1);
    if (!buffer) {
        ss_error(error, error_size, "out of memory");
        return 0;
    }
    if (decoded && !ss_read_all(fd, buffer, decoded)) {
        ss_error(error, error_size, "cannot receive daemon response: %s",
                 strerror(errno));
        free(buffer);
        return 0;
    }
    buffer[decoded] = '\0';
    *data = buffer;
    *length = decoded;
    return 1;
}
