#define main simplemail_program_main
#include "../simplemail.c"
#undef main

#include <assert.h>

static void join_checked(char *out, const char *base, const char *name)
{
    assert(simplemail_join_path(base, name, out, PATH_MAX));
}

static void write_fixture(const char *path, const char *text)
{
    FILE *file = fopen(path, "w");
    assert(file);
    assert(fputs(text, file) >= 0);
    assert(fclose(file) == 0);
}

static void wait_for_pull(void)
{
    struct timespec pause = {0, 10000000L};
    for (int i = 0; i < 500 && pull_running; i++) {
        finish_pull_if_done();
        if (pull_running) nanosleep(&pause, NULL);
    }
    assert(!pull_running);
}

static void remove_fixture_tree(const char *path)
{
    DIR *dir = opendir(path);
    assert(dir);
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        char child[PATH_MAX];
        struct stat st;
        join_checked(child, path, entry->d_name);
        assert(lstat(child, &st) == 0);
        if (S_ISDIR(st.st_mode)) remove_fixture_tree(child);
        else assert(unlink(child) == 0);
    }
    closedir(dir);
    assert(rmdir(path) == 0);
}

int main(void)
{
    char root[] = "/tmp/simplemail-delivery-check-XXXXXX";
    char config_dir[PATH_MAX], config_path[PATH_MAX], state_dir[PATH_MAX];
    char inbox[PATH_MAX], original[PATH_MAX], recent[PATH_MAX], spam[PATH_MAX];
    char sent[PATH_MAX], sent_cur[PATH_MAX], sent_tmp[PATH_MAX], body[PATH_MAX];
    char attachment[PATH_MAX], capture[PATH_MAX], command[PATH_MAX + 32];
    assert(mkdtemp(root));
    assert(setenv("HOME", root, 1) == 0);
    join_checked(config_dir, root, "config");
    join_checked(state_dir, root, "state");
    assert(setenv("XDG_CONFIG_HOME", config_dir, 1) == 0);
    assert(setenv("XDG_STATE_HOME", state_dir, 1) == 0);
    unsetenv("SIMPLEMAIL_MAILDIR");
    unsetenv("SIMPLEMAIL_SYNC_CMD");
    unsetenv("SIMPLEMAIL_SEND_CMD");
    assert(mkdir_p_checked(config_dir));
    join_checked(config_path, config_dir, "simplemail");
    assert(mkdir_p_checked(config_path));
    char config_parent[PATH_MAX];
    config_copy(config_parent, sizeof config_parent, config_path);
    join_checked(config_path, config_parent, "config");
    write_fixture(config_path, "maildir=~/mail\nsync_cmd=:\nfetch_on_start=true\ncheck_interval=60\n");
    load_simplemail_config();
    assert(simplemail_fetch_on_start == 1 && simplemail_check_interval == 60);
    assert(init_paths());
    init_mailboxes();
    join_checked(inbox, mail_root, "Inbox/new");
    join_checked(original, inbox, "original.eml");
    join_checked(recent, inbox, "recent.eml");
    write_fixture(original, "From: test@example.test\nSubject: Old message\nMessage-ID: <old@example.test>\nDate: 01 Oct 2026 10:00:00 +0000\n\nOriginal body.\n");
    load_current_mailbox();
    assert(message_count == 1 && !strcmp(messages[selected].path, original));

    /* Empty live checks preserve the parsed message instead of rereading disk. */
    parse_message_file(&messages[selected]);
    char *saved_body = messages[selected].body;
    unsigned long generation = mail_watch_generation;
    handle_mail_watch_event("SIMPLEMAIL CHECKED");
    assert(messages[selected].body == saved_body && !mailbox_reload_pending);
    assert(mail_watch_generation == generation);
    mail_watch_check_pending = 1;
    handle_mail_watch_event("SIMPLEMAIL CHECKED");
    assert(!strcmp(status_msg, "Mail checked.") && !mail_watch_check_pending);
    assert(messages[selected].body == saved_body && !mailbox_reload_pending);

    /* A completed download preserves the reader and waits to reload the list. */
    view = VIEW_READ;
    parse_message_file(&messages[selected]);
    read_scroll = 3;
    write_fixture(recent, "From: test@example.test\nSubject: New message\nMessage-ID: <new@example.test>\nDate: 02 Oct 2026 10:00:00 +0000\n\nNew body.\n");
    handle_mail_watch_event("SIMPLEMAIL MAIL");
    handle_mail_watch_event("SIMPLEMAIL MAIL");
    handle_mail_watch_event("SIMPLEMAIL CHECKED");
    assert(mailbox_reload_pending && message_count == 1);
    assert(read_scroll == 3 && !strcmp(messages[selected].path, original));
    assert(messages[selected].body && strstr(messages[selected].body, "Original body"));
    view = VIEW_LIST;
    reload_mailbox_after_delivery();
    assert(!mailbox_reload_pending && message_count == 2);
    assert(!strcmp(messages[selected].path, original));

    /* A burst in list view leaves one refresh for the event loop. */
    parse_message_file(&messages[selected]);
    saved_body = messages[selected].body;
    handle_mail_watch_event("SIMPLEMAIL MAIL");
    handle_mail_watch_event("SIMPLEMAIL MAIL");
    handle_mail_watch_event("SIMPLEMAIL CHECKED");
    assert(mailbox_reload_pending && messages[selected].body == saved_body);
    reload_mailbox_after_delivery();
    assert(!mailbox_reload_pending && message_count == 2);
    assert(!strcmp(messages[selected].path, original));

    selected_flags[selected] = 1;
    reload_mailbox_after_delivery();
    assert(mailbox_reload_pending && selection_count() == 1);
    clear_selection();
    reload_mailbox_after_delivery();
    assert(!mailbox_reload_pending);

    join_checked(spam, mail_root, "Spam");
    make_maildir(spam);
    init_mailboxes();
    assert(mailbox_count == 6 && !strcmp(mailboxes[5].name, "Spam"));

    next_mail_check = time(NULL) + 60;
    check_mail_when_due(next_mail_check - 1);
    assert(!pull_running);
    check_mail_when_due(next_mail_check);
    assert(pull_running && pull_pid > 0);
    wait_for_pull();
    assert(strstr(status_msg, "complete") || strstr(status_msg, "checked"));
    assert(!strcmp(messages[selected].path, original));
    snprintf(simplemail_sync_cmd, sizeof simplemail_sync_cmd, "false");
    pull_requested = 1;
    check_mail_when_due(time(NULL));
    wait_for_pull();
    assert(strstr(status_msg, "failed"));
    assert(message_count == 2);

    /* Preserve the actual failure across successful automatic checks. */
    config_copy(simplemail_sync_cmd, sizeof simplemail_sync_cmd,
                "(printf 'simplemail-fetch: Mail server disconnected during the check.\\n' >&2; exit 1)");
    pull_mail();
    wait_for_pull();
    assert(strstr(status_msg, "Mail server disconnected"));
    assert(strstr(status_msg, "Retrying automatically"));
    char reader_footer[256];
    simplemail_make_read_footer(&messages[selected], reader_footer, sizeof reader_footer);
    assert(strstr(reader_footer, "Mail server disconnected"));
    char error_log[PATH_MAX], latest_log[PATH_MAX], failure_detail[181];
    struct stat log_stat;
    assert(simplemail_pull_error_log_path(error_log, sizeof error_log));
    assert(!strncmp(error_log, state_dir, strlen(state_dir)));
    assert(stat(error_log, &log_stat) == 0 && (log_stat.st_mode & 0777) == 0600);
    assert(simplemail_pull_log_path(latest_log, sizeof latest_log));
    assert(stat(latest_log, &log_stat) == 0 && (log_stat.st_mode & 0777) == 0600);
    config_copy(simplemail_sync_cmd, sizeof simplemail_sync_cmd, ":");
    pull_mail();
    wait_for_pull();
    assert(!strstr(status_msg, "disconnected"));
    simplemail_make_read_footer(&messages[selected], reader_footer, sizeof reader_footer);
    assert(strstr(reader_footer, "complete") || strstr(reader_footer, "checked"));
    simplemail_pull_failure_detail(failure_detail, sizeof failure_detail);
    assert(strstr(failure_detail, "Mail server disconnected"));
    simplemail_check_interval = 0;
    check_mail_when_due(time(NULL) + 100000);
    assert(!pull_running);

    join_checked(sent, mail_root, "Sent");
    join_checked(sent_cur, sent, "cur");
    join_checked(sent_tmp, sent, "tmp");
    join_checked(body, root, "body");
    join_checked(attachment, root, "attachment");
    join_checked(capture, root, "captured-mime");
    write_fixture(body, "The outgoing body.\n");
    write_fixture(attachment, "attachment-bytes");
    snprintf(command, sizeof command, "cat > '%s'", capture);
    config_copy(simplemail_send_cmd, sizeof simplemail_send_cmd, command);
    assert(send_mail_msmtp_attach_ex("receiver@example.test", "Local sent copy", body,
                                    attachment, NULL, NULL) == 0);
    assert(count_regular_files_in_dir(sent_cur) == 1);
    assert(count_regular_files_in_dir(sent_tmp) == 0);
    assert(path_is_regular(capture));
    char mime[16384];
    FILE *mime_file = fopen(capture, "rb");
    assert(mime_file);
    size_t mime_length = fread(mime, 1, sizeof mime - 1, mime_file);
    assert(!ferror(mime_file) && feof(mime_file));
    assert(fclose(mime_file) == 0);
    mime[mime_length] = '\0';
    assert(strstr(mime, "The outgoing body"));
    assert(strstr(mime, "YXR0YWNobWVudC1ieXRlcw=="));
    assert(strstr(mime, "Date: "));
    assert(mime[mime_length - 1] == '\n');

    /* SMTP is not attempted when the complete local MIME cannot be staged. */
    assert(unlink(capture) == 0);
    assert(rmdir(sent_tmp) == 0);
    write_fixture(sent_tmp, "not a directory");
    assert(send_mail_msmtp_attach_ex("receiver@example.test", "Must stay unsent", body,
                                    NULL, NULL, NULL) < 0);
    assert(access(capture, F_OK) != 0);
    assert(unlink(sent_tmp) == 0);
    assert(mkdir(sent_tmp, 0700) == 0);

    /* A failed SMTP command does not publish a successful Sent copy. */
    config_copy(simplemail_send_cmd, sizeof simplemail_send_cmd, "false");
    assert(send_mail_msmtp_attach_ex("receiver@example.test", "Failed send", body,
                                    NULL, NULL, NULL) < 0);
    assert(count_regular_files_in_dir(sent_cur) == 1);
    assert(count_regular_files_in_dir(sent_tmp) == 0);

    /* An unreadable attachment or body must not result in a partial send. */
    config_copy(simplemail_send_cmd, sizeof simplemail_send_cmd, command);
    char missing[PATH_MAX];
    join_checked(missing, root, "missing-content");
    assert(send_mail_msmtp_attach_ex("receiver@example.test", "Missing attachment", body,
                                    missing, NULL, NULL) < 0);
    assert(send_mail_msmtp_attach_ex("receiver@example.test", "Missing body", missing,
                                    NULL, NULL, NULL) < 0);
    assert(access(capture, F_OK) != 0);

    /* Sent cleanup is requested even if the next periodic check is disabled. */
    send_pid = fork();
    assert(send_pid >= 0);
    if (send_pid == 0) _exit(0);
    send_running = 1;
    struct timespec pause = {0, 10000000L};
    for (int i = 0; i < 500 && send_running; i++) {
        finish_send_if_done();
        if (send_running) nanosleep(&pause, NULL);
    }
    assert(!send_running && pull_requested && mailbox_reload_pending);

    free_messages();
    remove_fixture_tree(root);
    puts("SimpleMail delivery checks passed.");
    return 0;
}
