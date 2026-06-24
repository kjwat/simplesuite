/*
 * simplemail.c - first SimpleSuite-style mail client draft
 *
 * v0.1 scope:
 *   - Local Maildir-style mailbox browser
 *   - Launches directly into Inbox
 *   - m toggles mailbox chooser overlay
 *   - Arrow keys move
 *   - Enter opens full-screen reading view
 *   - Backspace goes back
 *   - q quits
 *   - c compose: prompts To + Subject, then opens SimpleWords for body
 *   - r reply: from reading view, opens SimpleWords for reply body
 *
 * Not yet implemented:
 *   - IMAP sync
 *   - SMTP sending
 *   - ProtonBridge setup
 *   - Attachments extraction/opening
 *   - MIME decoding beyond basic text/plain-ish display
 *
 * Build:
 *   cc -O2 -Wall -Wextra -std=c99 simplemail.c -lncursesw -o simplemail
 */

#define _XOPEN_SOURCE 700

#include <ncurses.h>
#include <locale.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define MAX_MESSAGES 2048
#define MAX_MAILBOXES 32
#define MAX_LINE 4096
#define MAX_BODY 262144

typedef enum {
    VIEW_LIST,
    VIEW_READ
} View;

typedef struct {
    char path[PATH_MAX];
    char from[256];
    char subject[512];
    char date[128];
    int unread;
    char *body;
} Message;

typedef struct {
    char name[128];
    char path[PATH_MAX];
} Mailbox;

static Message messages[MAX_MESSAGES];
static int message_count = 0;
static int selected = 0;
static int list_top = 0;

static Mailbox mailboxes[MAX_MAILBOXES];
static int mailbox_count = 0;
static int current_mailbox = 0;
static int mailbox_overlay = 0;
static int selected_mailbox = 0;

static View view = VIEW_LIST;
static int read_scroll = 0;
static int pending_delete = 0;

static char mail_root[PATH_MAX];

static void free_messages(void) {
    for (int i = 0; i < message_count; i++) {
        free(messages[i].body);
        messages[i].body = NULL;
    }
    message_count = 0;
    selected = 0;
    list_top = 0;
}

static void trim(char *s) {
    if (!s) return;
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || isspace((unsigned char)s[n - 1]))) {
        s[--n] = '\0';
    }
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
}

static int starts_case(const char *s, const char *prefix) {
    while (*prefix && *s) {
        if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix)) return 0;
        s++;
        prefix++;
    }
    return *prefix == '\0';
}

static void copy_field(char *dst, size_t dstsz, const char *src) {
    if (!src || !dstsz) return;
    snprintf(dst, dstsz, "%s", src);
    trim(dst);
}

static void ensure_dir(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) return;
    mkdir(path, 0700);
}

static void make_maildir(const char *base) {
    ensure_dir(base);

    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/cur", base); ensure_dir(p);
    snprintf(p, sizeof p, "%s/new", base); ensure_dir(p);
    snprintf(p, sizeof p, "%s/tmp", base); ensure_dir(p);
}

static void write_sample_mail(void) {
    char inbox[PATH_MAX], sample[PATH_MAX];
    snprintf(inbox, sizeof inbox, "%s/Inbox", mail_root);
    make_maildir(inbox);
    snprintf(sample, sizeof sample, "%s/new/sample-simplemail.eml", inbox);

    struct stat st;
    if (stat(sample, &st) == 0) return;

    FILE *f = fopen(sample, "w");
    if (!f) return;

    fprintf(f,
        "From: SimpleMail Draft <simplemail@localhost>\n"
        "To: Keelan\n"
        "Subject: SimpleMail first boot\n"
        "Date: Tue, 23 Jun 2026 19:00:00 -0400\n"
        "Content-Type: text/plain; charset=UTF-8\n"
        "\n"
        "SimpleMail is alive.\n"
        "\n"
        "This is only the local-reader draft: mailbox list, reading screen,\n"
        "mailbox overlay, compose handoff, and reply handoff.\n"
        "\n"
        "Next comes real account setup: IMAP, SMTP, and ProtonBridge done sanely.\n"
    );

    fclose(f);
}

static void init_paths(void) {
    const char *home = getenv("HOME");
    if (!home) home = ".";

    snprintf(mail_root, sizeof mail_root, "%s/.local/share/simplemail/mail", home);

    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/.local", home); ensure_dir(p);
    snprintf(p, sizeof p, "%s/.local/share", home); ensure_dir(p);
    snprintf(p, sizeof p, "%s/.local/share/simplemail", home); ensure_dir(p);
    ensure_dir(mail_root);

    const char *default_boxes[] = {"Inbox", "Sent", "Drafts", "Archive", "Trash"};
    for (size_t i = 0; i < sizeof(default_boxes) / sizeof(default_boxes[0]); i++) {
        snprintf(p, sizeof p, "%s/%s", mail_root, default_boxes[i]);
        make_maildir(p);
    }

    write_sample_mail();
}

static void init_mailboxes(void) {
    mailbox_count = 0;
    const char *default_boxes[] = {"Inbox", "Sent", "Drafts", "Archive", "Trash"};

    for (size_t i = 0; i < sizeof(default_boxes) / sizeof(default_boxes[0]) && mailbox_count < MAX_MAILBOXES; i++) {
        snprintf(mailboxes[mailbox_count].name, sizeof mailboxes[mailbox_count].name, "%s", default_boxes[i]);
        snprintf(mailboxes[mailbox_count].path, sizeof mailboxes[mailbox_count].path, "%s/%s", mail_root, default_boxes[i]);
        mailbox_count++;
    }
}

static int path_is_regular(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static void append_body(char **body, size_t *used, size_t *cap, const char *line) {
    size_t len = strlen(line);
    if (*used + len + 2 >= *cap) {
        size_t newcap = *cap ? *cap * 2 : 8192;
        while (*used + len + 2 >= newcap) newcap *= 2;
        if (newcap > MAX_BODY) newcap = MAX_BODY;
        if (newcap <= *cap) return;
        char *nb = realloc(*body, newcap);
        if (!nb) return;
        *body = nb;
        *cap = newcap;
    }
    memcpy(*body + *used, line, len);
    *used += len;
    (*body)[*used] = '\0';
}

static void parse_message_file(Message *m) {
    FILE *f = fopen(m->path, "r");
    if (!f) {
        m->body = strdup("(Could not open message.)\n");
        return;
    }

    char line[MAX_LINE];
    int in_headers = 1;
    size_t used = 0, cap = 0;
    char *body = NULL;

    m->from[0] = m->subject[0] = m->date[0] = '\0';

    while (fgets(line, sizeof line, f)) {
        if (in_headers) {
            if (line[0] == '\n' || line[0] == '\r') {
                in_headers = 0;
                continue;
            }

            if (starts_case(line, "From:")) {
                copy_field(m->from, sizeof m->from, line + 5);
            } else if (starts_case(line, "Subject:")) {
                copy_field(m->subject, sizeof m->subject, line + 8);
            } else if (starts_case(line, "Date:")) {
                copy_field(m->date, sizeof m->date, line + 5);
            }
        } else {
            append_body(&body, &used, &cap, line);
        }
    }

    fclose(f);

    if (!m->from[0]) snprintf(m->from, sizeof m->from, "(unknown)");
    if (!m->subject[0]) snprintf(m->subject, sizeof m->subject, "(no subject)");
    if (!m->date[0]) snprintf(m->date, sizeof m->date, "");

    if (!body) body = strdup("(No displayable message body.)\n");
    m->body = body;
}

static void load_dir_messages(const char *dir, int unread) {
    DIR *d = opendir(dir);
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d)) && message_count < MAX_MESSAGES) {
        if (ent->d_name[0] == '.') continue;

        char path[PATH_MAX];
        snprintf(path, sizeof path, "%s/%s", dir, ent->d_name);
        if (!path_is_regular(path)) continue;

        Message *m = &messages[message_count++];
        memset(m, 0, sizeof *m);
        snprintf(m->path, sizeof m->path, "%s", path);
        m->unread = unread;
        parse_message_file(m);
    }

    closedir(d);
}

static void load_current_mailbox(void) {
    free_messages();

    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/new", mailboxes[current_mailbox].path);
    load_dir_messages(p, 1);

    snprintf(p, sizeof p, "%s/cur", mailboxes[current_mailbox].path);
    load_dir_messages(p, 0);

    if (selected >= message_count) selected = message_count - 1;
    if (selected < 0) selected = 0;
}


static int count_regular_files_in_dir(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return 0;

    int count = 0;
    struct dirent *ent;

    while ((ent = readdir(d))) {
        if (ent->d_name[0] == '.') continue;

        char path[PATH_MAX];
        snprintf(path, sizeof path, "%s/%s", dir, ent->d_name);

        if (path_is_regular(path)) count++;
    }

    closedir(d);
    return count;
}

static int mailbox_attention_count(int idx) {
    if (idx < 0 || idx >= mailbox_count) return 0;

    char path[PATH_MAX];

    if (strcmp(mailboxes[idx].name, "Inbox") == 0) {
        snprintf(path, sizeof path, "%s/new", mailboxes[idx].path);
        return count_regular_files_in_dir(path);
    }

    if (strcmp(mailboxes[idx].name, "Drafts") == 0) {
        snprintf(path, sizeof path, "%s/new", mailboxes[idx].path);
        return count_regular_files_in_dir(path);
    }

    return 0;
}

static int unread_count(void) {
    int n = 0;
    for (int i = 0; i < message_count; i++) if (messages[i].unread) n++;
    return n;
}

static void draw_footer(const char *text) {
    int h, w;
    getmaxyx(stdscr, h, w);
    mvhline(h - 2, 0, ACS_HLINE, w);

    move(h - 1, 0);
    clrtoeol();

    if (pending_delete)
        mvaddnstr(h - 1, 1, "dD Delete", w - 2);
    else
        mvaddnstr(h - 1, 1, text, w - 2);

    move(0, 0);
}

static void draw_ready_to_send_footer(void) {
    int h, w;
    getmaxyx(stdscr, h, w);

    noecho();
    curs_set(0);

    mvhline(h - 3, 0, ACS_HLINE, w);

    move(h - 2, 0);
    clrtoeol();
    mvaddnstr(h - 2, 1, "Ready to send.", w - 2);

    move(h - 1, 0);
    clrtoeol();
    mvaddnstr(h - 1, 1, "s Send    v Save Draft    e Edit    d Discard", w - 2);

    move(0, 0);
}

static void draw_list(void) {
    erase();
    int h, w;
    getmaxyx(stdscr, h, w);

    int unread = unread_count();
    char title[512];
    snprintf(title, sizeof title, " SimpleMail - %s%s%d unread%s ",
             mailboxes[current_mailbox].name,
             unread ? " (" : "",
             unread,
             unread ? ")" : "");
    if (!unread) snprintf(title, sizeof title, " SimpleMail - %s ", mailboxes[current_mailbox].name);

    mvhline(0, 0, ACS_HLINE, w);
    mvaddnstr(0, 2, title, w - 4);

    int rows = h - 4;
    if (selected < list_top) list_top = selected;
    if (selected >= list_top + rows) list_top = selected - rows + 1;
    if (list_top < 0) list_top = 0;

    if (message_count == 0) {
        mvaddnstr(3, 2, "(No messages here.)", w - 4);
    } else {
        for (int y = 0; y < rows && list_top + y < message_count; y++) {
            int idx = list_top + y;
            Message *m = &messages[idx];

            char line[1024];
            char from[26];
            snprintf(from, sizeof from, "%.24s", m->from);

            snprintf(line, sizeof line, "%c %-24s  %s",
                     m->unread ? 'N' : ' ',
                     from,
                     m->subject);

            if (idx == selected) attron(A_REVERSE);
            mvaddnstr(y + 2, 1, idx == selected ? ">" : " ", 1);
            mvaddnstr(y + 2, 3, line, w - 4);
            if (idx == selected) attroff(A_REVERSE);
        }
    }

    draw_footer("↑↓ Move  Enter Open  m Mailboxes  c Compose  / Search  q Quit");
    refresh();
}

static int body_line_count(const char *body) {
    int count = 1;
    if (!body) return 0;
    for (const char *p = body; *p; p++) if (*p == '\n') count++;
    return count;
}

static void draw_wrapped_body_line(int *y, int max_y, int w, const char *line) {
    int len = (int)strlen(line);
    if (len == 0) {
        if (*y < max_y) mvaddch((*y)++, 1, ' ');
        return;
    }

    int pos = 0;
    int width = w - 4;
    while (pos < len && *y < max_y) {
        mvaddnstr((*y)++, 2, line + pos, width);
        pos += width;
    }
}

static void draw_read(void) {
    erase();
    int h, w;
    getmaxyx(stdscr, h, w);

    if (message_count == 0 || selected < 0 || selected >= message_count) {
        mvaddstr(2, 2, "(No message selected.)");
        draw_footer("Backspace Back  q Quit");
        refresh();
        return;
    }

    Message *m = &messages[selected];

    mvhline(0, 0, ACS_HLINE, w);
    mvaddnstr(0, 2, " Message ", w - 4);

    mvprintw(2, 2, "From: %.200s", m->from);
    mvprintw(3, 2, "Subject: %.200s", m->subject);
    if (m->date[0]) mvprintw(4, 2, "Date: %.120s", m->date);

    mvhline(6, 0, ACS_HLINE, w);

    int y = 8;
    int max_y = h - 3;

    char *copy = strdup(m->body ? m->body : "");
    if (!copy) copy = strdup("");

    int skip = read_scroll;
    char *saveptr = NULL;
    char *line = strtok_r(copy, "\n", &saveptr);

    while (line && skip > 0) {
        skip--;
        line = strtok_r(NULL, "\n", &saveptr);
    }

    while (line && y < max_y) {
        draw_wrapped_body_line(&y, max_y, w, line);
        line = strtok_r(NULL, "\n", &saveptr);
    }

    free(copy);

    draw_footer("↑↓ Scroll  Backspace Inbox  r Reply  a Archive  dD Delete  q Quit");
    refresh();
}

static void draw_mailbox_overlay(void) {
    erase();
    int h, w;
    getmaxyx(stdscr, h, w);

    mvhline(0, 0, ACS_HLINE, w);
    mvaddnstr(0, 2, " Mailboxes ", w - 4);

    int start_y = 3;
    for (int i = 0; i < mailbox_count; i++) {
        if (i == selected_mailbox) attron(A_REVERSE);
        mvaddnstr(start_y + i, 2, i == selected_mailbox ? ">" : " ", 1);
        char label[256];
        int attention = mailbox_attention_count(i);
        if (attention > 0)
            snprintf(label, sizeof label, "%s (%d)", mailboxes[i].name, attention);
        else
            snprintf(label, sizeof label, "%s", mailboxes[i].name);

        mvaddnstr(start_y + i, 4, label, w - 6);
        if (i == selected_mailbox) attroff(A_REVERSE);
    }

    draw_footer("↑↓ Move  Enter Select  m Return  q Quit");
    refresh();
}

static void prompt_line(const char *label, char *out, size_t outsz) {
    echo();
    curs_set(1);

    int h, w;
    getmaxyx(stdscr, h, w);
    mvhline(h - 3, 0, ' ', w);
    mvprintw(h - 3, 1, "%s", label);
    clrtoeol();

    move(h - 3, (int)strlen(label) + 2);
    char buf[1024];
    memset(buf, 0, sizeof buf);
    getnstr(buf, (int)sizeof(buf) - 1);

    snprintf(out, outsz, "%s", buf);
    trim(out);

    noecho();
    curs_set(0);
}

static int run_editor_on_file(const char *path) {
    const char *editor = getenv("SIMPLEMAIL_EDITOR");
    if (!editor || !*editor) editor = "simplewords";

    def_prog_mode();
    endwin();

    pid_t pid = fork();
    if (pid < 0) {
        reset_prog_mode();
        refresh();
        return -1;
    }

    if (pid == 0) {
        execlp(editor, editor, path, (char *)NULL);
        execlp("nano", "nano", path, (char *)NULL);
        _exit(127);
    }

    int status = 0;
    waitpid(pid, &status, 0);

    reset_prog_mode();
    raw();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    clear();
    touchwin(stdscr);
    refresh();

    return status;
}

static void save_draft_record(const char *to, const char *subject, const char *body_path) {
    char drafts[PATH_MAX];
    snprintf(drafts, sizeof drafts, "%s/Drafts/new", mail_root);

    time_t now = time(NULL);
    char out[PATH_MAX];
    snprintf(out, sizeof out, "%s/draft-%ld.eml", drafts, (long)now);

    FILE *in = fopen(body_path, "r");
    FILE *f = fopen(out, "w");
    if (!f) {
        if (in) fclose(in);
        return;
    }

    fprintf(f, "From: SimpleMail User\n");
    fprintf(f, "To: %s\n", to && *to ? to : "(unset)");
    fprintf(f, "Subject: %s\n", subject && *subject ? subject : "(no subject)");
    fprintf(f, "Date: draft\n");
    fprintf(f, "\n");

    if (in) {
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, n, f);
        fclose(in);
    }

    fclose(f);
}

static void compose_new(void) {
    char to[512] = {0};
    char subject[512] = {0};

    prompt_line("To:", to, sizeof to);
    if (!to[0]) return;

    prompt_line("Subject:", subject, sizeof subject);

    char tmpl[PATH_MAX];
    snprintf(tmpl, sizeof tmpl, "/tmp/simplemail-compose-XXXXXX");
    int fd = mkstemp(tmpl);
    if (fd < 0) return;

    FILE *f = fdopen(fd, "w");
    if (f) {
        fprintf(f, "\n");
        fclose(f);
    } else {
        close(fd);
    }

    run_editor_on_file(tmpl);

    erase();
    draw_ready_to_send_footer();
    refresh();

    int ch;
    while ((ch = getch())) {
        if (ch == 's' || ch == 'S') {
            /* SMTP sending is not implemented yet; keep the draft safe. */
            save_draft_record(to, subject, tmpl);
            break;
        } else if (ch == 'v' || ch == 'V') {
            save_draft_record(to, subject, tmpl);
            break;
        } else if (ch == 'e' || ch == 'E') {
            run_editor_on_file(tmpl);
            erase();
            draw_ready_to_send_footer();
            refresh();
        } else if (ch == 'd' || ch == 'D' || ch == 'q') {
            break;
        }
    }

    curs_set(0);
    noecho();
    unlink(tmpl);
    load_current_mailbox();
}

static void reply_current(void) {
    if (message_count == 0 || selected < 0 || selected >= message_count) return;

    Message *m = &messages[selected];

    char to[512];
    snprintf(to, sizeof to, "%s", m->from);

    char subject[512];
    if (starts_case(m->subject, "Re:")) snprintf(subject, sizeof subject, "%s", m->subject);
    else snprintf(subject, sizeof subject, "Re: %s", m->subject);

    char tmpl[PATH_MAX];
    snprintf(tmpl, sizeof tmpl, "/tmp/simplemail-reply-XXXXXX");
    int fd = mkstemp(tmpl);
    if (fd < 0) return;

    FILE *f = fdopen(fd, "w");
    if (f) {
        fprintf(f, "\n");
        fclose(f);
    } else {
        close(fd);
    }

    run_editor_on_file(tmpl);

    erase();
    mvprintw(2, 2, "To: %.200s", to);
    mvprintw(3, 2, "Subject: %.200s", subject);
    draw_ready_to_send_footer();
    refresh();

    int ch;
    while ((ch = getch())) {
        if (ch == 's' || ch == 'S') {
            /* SMTP sending is not implemented yet; keep the draft safe. */
            save_draft_record(to, subject, tmpl);
            break;
        } else if (ch == 'v' || ch == 'V') {
            save_draft_record(to, subject, tmpl);
            break;
        } else if (ch == 'e' || ch == 'E') {
            run_editor_on_file(tmpl);
            erase();
            mvprintw(2, 2, "To: %.200s", to);
            mvprintw(3, 2, "Subject: %.200s", subject);
            draw_ready_to_send_footer();
            refresh();
        } else if (ch == 'd' || ch == 'D' || ch == 'q') {
            break;
        }
    }

    curs_set(0);
    noecho();
    unlink(tmpl);
}


static int move_file_to_mailbox(const char *src, const char *boxname) {
    char destdir[PATH_MAX];
    char dest[PATH_MAX];

    snprintf(destdir, sizeof destdir, "%s/%s/cur", mail_root, boxname);

    const char *base = strrchr(src, '/');
    base = base ? base + 1 : src;

    snprintf(dest, sizeof dest, "%s/%ld-%s", destdir, (long)time(NULL), base);

    if (rename(src, dest) == 0) return 0;
    return -1;
}

static void move_current_message_to(const char *boxname) {
    if (message_count == 0 || selected < 0 || selected >= message_count) return;

    int old_selected = selected;

    move_file_to_mailbox(messages[selected].path, boxname);
    load_current_mailbox();

    if (message_count <= 0) {
        selected = 0;
    } else if (old_selected >= message_count) {
        selected = message_count - 1;
    } else {
        selected = old_selected;
    }

    if (view == VIEW_READ) view = VIEW_LIST;
}

static void delete_current_message(void) {
    move_current_message_to("Trash");
}

static void archive_current_message(void) {
    move_current_message_to("Archive");
}


static int handle_delete_sequence(int ch) {
    if (!pending_delete) {
        if (ch == 'd') {
            pending_delete = 1;
            return 1;
        }
        return 0;
    }

    if (ch == 'D') {
        pending_delete = 0;

        int h, w;
        getmaxyx(stdscr, h, w);
        mvhline(h - 2, 0, ACS_HLINE, w);
        move(h - 1, 0);
        clrtoeol();
        mvaddnstr(h - 1, 1, "Move message to Trash? y/N", w - 2);
        refresh();

        int ans = getch();
        if (ans == 'y' || ans == 'Y')
            delete_current_message();

        return 1;
    }

    pending_delete = 0;
    return 0;
}

static void handle_list_key(int ch) {
    if (!mailbox_overlay && handle_delete_sequence(ch)) return;

    if (mailbox_overlay) {
        if (ch == KEY_UP && selected_mailbox > 0) selected_mailbox--;
        else if (ch == KEY_DOWN && selected_mailbox < mailbox_count - 1) selected_mailbox++;
        else if (ch == '\n' || ch == KEY_ENTER) {
            current_mailbox = selected_mailbox;
            mailbox_overlay = 0;
            load_current_mailbox();
        } else if (ch == 'm' || ch == 'M') {
            mailbox_overlay = 0;
        }
        return;
    }

    if (ch == KEY_UP && selected > 0) selected--;
    else if (ch == KEY_DOWN && selected < message_count - 1) selected++;
    else if ((ch == '\n' || ch == KEY_ENTER) && message_count > 0) {
        view = VIEW_READ;
        read_scroll = 0;
    } else if (ch == 'm' || ch == 'M') {
        selected_mailbox = current_mailbox;
        mailbox_overlay = 1;
    } else if (ch == 'a' || ch == 'A') {
        archive_current_message();
    } else if (ch == 'c' || ch == 'C') {
        compose_new();
    }
}

static void handle_read_key(int ch) {
    if (handle_delete_sequence(ch)) return;

    int lines = 0;
    if (message_count > 0 && selected >= 0 && selected < message_count) {
        lines = body_line_count(messages[selected].body);
    }

    if (ch == KEY_UP && read_scroll > 0) read_scroll--;
    else if (ch == KEY_DOWN && read_scroll < lines - 1) read_scroll++;
    else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
        view = VIEW_LIST;
    } else if (ch == 'r' || ch == 'R') {
        reply_current();
    } else if (ch == 'a' || ch == 'A') {
        archive_current_message();
    }
}

int main(void) {
    setlocale(LC_ALL, "");

    init_paths();
    init_mailboxes();
    load_current_mailbox();

    initscr();
    raw();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);

    int running = 1;
    while (running) {
        if (mailbox_overlay) draw_mailbox_overlay();
        else if (view == VIEW_READ) draw_read();
        else draw_list();

        int ch = getch();

        if (ch == 'q' || ch == 'Q') {
            running = 0;
            break;
        }

        if (view == VIEW_READ && !mailbox_overlay) {
            handle_read_key(ch);
        } else {
            handle_list_key(ch);
        }
    }

    endwin();
    free_messages();
    return 0;
}
