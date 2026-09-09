/* Private SimpleFiles read cache. Included after build_directory_entries().
 * Only the Linux UI uses this cache; child workers keep ordinary filesystem
 * semantics. In-flight reads are never killed when a mount is replaced. */
#define NETWORK_READ_SLOTS 8
#define NETWORK_READ_WORKERS 16
#define NETWORK_TEXT_BYTES 65536

typedef struct {
    int error, stat_error, vfs_error, text_error, binary, count;
    struct stat link_stat, file_stat;
    struct statvfs vfs;
    size_t text_size;
    char text[NETWORK_TEXT_BYTES];
    Entry entries[MAX_ENTRIES];
} NetworkReadResult;

typedef struct {
    char path[PATH_MAX];
    pid_t pid;
    int fd;
    long long started, used;
    NetworkReadResult *ready;
} NetworkRead;

static NetworkRead network_reads[NETWORK_READ_SLOTS];
static pid_t network_retired[NETWORK_READ_WORKERS];
static uint64_t network_mounts;
static long long network_mount_check;
static int network_show_hidden;

static long long network_now_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int network_ui_path(const char *path) {
#ifdef __linux__
    return network_ui_pid == getpid() && path_is_simpleserve_file(path);
#else
    (void)path;
    return 0;
#endif
}

static int network_pending(void) {
    int count = 0;
    for (int i = 0; i < NETWORK_READ_SLOTS; i++)
        count += network_reads[i].pid > 0;
    for (int i = 0; i < NETWORK_READ_WORKERS; i++)
        count += network_retired[i] > 0;
    return count;
}

static void network_forget_reads(void) {
    for (int i = 0; i < NETWORK_READ_SLOTS; i++) {
        NetworkRead *read = &network_reads[i];
        if (read->pid > 0) {
            for (int j = 0; j < NETWORK_READ_WORKERS; j++) {
                if (network_retired[j] == 0) {
                    network_retired[j] = read->pid;
                    break;
                }
            }
            close(read->fd);
        }
        free(read->ready);
        memset(read, 0, sizeof(*read));
    }
}

static uint64_t network_mount_fingerprint(void) {
    uint64_t hash = UINT64_C(1469598103934665603);
#ifdef __linux__
    FILE *file = fopen("/proc/self/mountinfo", "r");
    char *line = NULL;
    size_t size = 0;
    if (!file)
        return network_mounts;
    while (getline(&line, &size, file) >= 0) {
        /* Includes the mount ID, so a replacement with the same NFS source
         * also invalidates results obtained through the detached mount. */
        if (!strstr(line, "/SimpleServe/") || !strstr(line, " - nfs"))
            continue;
        for (const unsigned char *p = (const unsigned char *)line; *p; p++) {
            hash ^= *p;
            hash *= UINT64_C(1099511628211);
        }
    }
    free(line);
    fclose(file);
#endif
    return hash;
}

static int network_poll(void) {
    int changed = 0;
    long long now = network_now_ms();
    if (now - network_mount_check >= 200) {
        uint64_t mounts = network_mount_fingerprint();
        network_mount_check = now;
        if (mounts != network_mounts) {
            network_mounts = mounts;
            network_forget_reads();
            changed = 1;
        }
    }
    for (int i = 0; i < NETWORK_READ_WORKERS; i++) {
        pid_t pid = network_retired[i];
        if (pid > 0) {
            pid_t done = waitpid(pid, NULL, WNOHANG);
            if (done == pid || (done < 0 && errno == ECHILD))
                network_retired[i] = 0;
        }
    }
    for (int i = 0; i < NETWORK_READ_SLOTS; i++) {
        NetworkRead *read = &network_reads[i];
        int status = 0;
        pid_t done;
        if (read->pid <= 0)
            continue;
        done = waitpid(read->pid, &status, WNOHANG);
        if (done == 0 || (done < 0 && errno == EINTR))
            continue;
        NetworkReadResult *result = calloc(1, sizeof(*result));
        if (result) {
            size_t used = 0;
            while (used < sizeof(*result)) {
                ssize_t bytes = pread(read->fd, (char *)result + used,
                                      sizeof(*result) - used, (off_t)used);
                if (bytes > 0)
                    used += (size_t)bytes;
                else if (bytes < 0 && errno == EINTR)
                    continue;
                else
                    break;
            }
            if (done != read->pid || !WIFEXITED(status) ||
                WEXITSTATUS(status) != 0 || used != sizeof(*result)) {
                memset(result, 0, sizeof(*result));
                result->error = EIO;
            }
            if (!read->ready || memcmp(read->ready, result, sizeof(*result)) != 0)
                changed = 1;
            free(read->ready);
            read->ready = result;
        }
        close(read->fd);
        read->pid = 0;
    }
    return changed;
}

static void network_collect(const char *path, NetworkReadResult *result) {
    if (lstat(path, &result->link_stat) != 0) {
        result->error = errno;
        return;
    }
    if (stat(path, &result->file_stat) != 0) {
        result->stat_error = errno;
        return;
    }
    if (statvfs(path, &result->vfs) != 0)
        result->vfs_error = errno;
    if (S_ISDIR(result->file_stat.st_mode)) {
        result->count = build_directory_entries(path, result->entries,
                                                MAX_ENTRIES, "network-reader");
        if (result->count < 0)
            result->error = errno;
        for (int i = 0; i < result->count; i++) {
            if (result->entries[i].is_dir < 0) {
                char full[PATH_MAX];
                struct stat st;
                join_path(full, path, result->entries[i].name);
                if (stat(full, &st) == 0)
                    result->entries[i].is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
            }
        }
    } else if (S_ISREG(result->file_stat.st_mode)) {
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            result->text_error = errno;
            return;
        }
        ssize_t bytes;
        do {
            bytes = read(fd, result->text, sizeof(result->text));
        } while (bytes < 0 && errno == EINTR);
        if (bytes < 0)
            result->text_error = errno;
        else {
            result->text_size = (size_t)bytes;
            result->binary = memchr(result->text, '\0', (size_t)bytes) != NULL;
        }
        close(fd);
    }
}

static const NetworkReadResult *network_read(const char *path) {
    NetworkRead *slot = NULL;
    long long now = network_now_ms();
    if (network_show_hidden != show_hidden) {
        network_show_hidden = show_hidden;
        network_forget_reads();
    }
    for (int i = 0; i < NETWORK_READ_SLOTS; i++) {
        if (strcmp(network_reads[i].path, path) == 0) {
            slot = &network_reads[i];
            break;
        }
    }
    if (!slot) {
        for (int i = 0; i < NETWORK_READ_SLOTS; i++) {
            NetworkRead *candidate = &network_reads[i];
            if (candidate->pid <= 0 && (!slot || candidate->used < slot->used))
                slot = candidate;
        }
        if (!slot)
            return NULL;
        free(slot->ready);
        memset(slot, 0, sizeof(*slot));
        safe_copy(slot->path, sizeof(slot->path), path);
    }
    slot->used = now;
    if (slot->pid > 0 || (slot->ready && now - slot->started < 2000) ||
        network_pending() >= NETWORK_READ_WORKERS)
        return slot->ready;
    int fd = image_temporary_fd();
    if (fd < 0)
        return slot->ready;
    pid_t pid = fork();
    if (pid == 0) {
        NetworkReadResult *result = calloc(1, sizeof(*result));
        if (!result)
            _exit(1);
        if (instance_lock_fd >= 0)
            close(instance_lock_fd);
        for (int i = 0; i < NETWORK_READ_SLOTS; i++)
            if (network_reads[i].pid > 0)
                close(network_reads[i].fd);
        signal(SIGINT, SIG_IGN);
        signal(SIGTERM, SIG_DFL);
        signal(SIGHUP, SIG_IGN);
        (void)setsid();
        redirect_background_stdio();
        if (chdir("/") != 0)
            _exit(1);
        network_collect(path, result);
#ifdef SIMPLEFILES_NETWORK_TEST_AFTER_READ
        SIMPLEFILES_NETWORK_TEST_AFTER_READ();
#endif
        size_t written = 0;
        while (written < sizeof(*result)) {
            ssize_t bytes = write(fd, (char *)result + written, sizeof(*result) - written);
            if (bytes > 0)
                written += (size_t)bytes;
            else if (bytes < 0 && errno == EINTR)
                continue;
            else
                _exit(1);
        }
        _exit(0);
    }
    if (pid < 0) {
        close(fd);
        return slot->ready;
    }
    slot->fd = fd;
    slot->pid = pid;
    slot->started = now;
    return slot->ready;
}

static int network_directory_entries(const char *path, Entry *target, int capacity) {
    const NetworkReadResult *result = network_read(path);
    if (!result || result->error || result->stat_error || !S_ISDIR(result->file_stat.st_mode)) {
        errno = !result ? EAGAIN : result->error ? result->error :
                result->stat_error ? result->stat_error : ENOTDIR;
        return -1;
    }
    int count = result->count < capacity ? result->count : capacity;
    memcpy(target, result->entries, (size_t)count * sizeof(*target));
    return count;
}

static int browsing_stat(const char *path, struct stat *st) {
    if (!network_ui_path(path))
        return stat(path, st);
    const NetworkReadResult *result = network_read(path);
    if (!result || result->error || result->stat_error) {
        errno = !result ? EAGAIN : result->error ? result->error : result->stat_error;
        return -1;
    }
    *st = result->file_stat;
    return 0;
}

static int browsing_lstat(const char *path, struct stat *st) {
    if (!network_ui_path(path))
        return lstat(path, st);
    const NetworkReadResult *result = network_read(path);
    if (!result || result->error) {
        errno = result ? result->error : EAGAIN;
        return -1;
    }
    *st = result->link_stat;
    return 0;
}

static int browsing_statvfs(const char *path, struct statvfs *st) {
    if (!network_ui_path(path))
        return statvfs(path, st);
    const NetworkReadResult *result = network_read(path);
    if (!result || result->error || result->vfs_error) {
        errno = !result ? EAGAIN : result->error ? result->error : result->vfs_error;
        return -1;
    }
    *st = result->vfs;
    return 0;
}

static int network_change_directory(const char *path) {
    char *absolute = g_canonicalize_filename(path, cwd_path[0] ? cwd_path : NULL);
    if (!absolute)
        return 0;
    if (!network_ui_path(absolute)) {
        int ok = startup_chdir_resolved(absolute);
        g_free(absolute);
        return ok;
    }
    /* Keep the UI's real cwd on a local filesystem. Only workers hold NFS
     * references; every new request resolves through the current mount. */
    int ok = chdir("/") == 0 && safe_copy(cwd_path, sizeof(cwd_path), absolute);
    g_free(absolute);
    return ok;
}
