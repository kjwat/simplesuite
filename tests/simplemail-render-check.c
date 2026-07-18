#define main simplemail_program_main
#include "../simplemail.c"
#undef main

#include <assert.h>

static void assert_contains(const char *text, const char *needle)
{
    assert(text);
    assert(needle);
    assert(strstr(text, needle));
}

static void assert_omits(const char *text, const char *needle)
{
    assert(text);
    assert(needle);
    assert(!strstr(text, needle));
}

int main(void)
{
    char *decoded;
    char *html;
    char *clean;
    char *display;
    char filename[256];
    Message message = {0};
    Message lazy = {0};
    SsrRenderer renderer;

    setlocale(LC_ALL, "");

    assert(ssr_visual_col_range("café", (int)strlen("café"),
                                0, (int)strlen("café")) == 4);
    assert(ssr_visual_col_range("👩🏽‍💻", (int)strlen("👩🏽‍💻"),
                                0, (int)strlen("👩🏽‍💻")) == 2);
    ssr_init(&renderer);
    assert(ssr_ensure_storage(&renderer, 1, 4));
    ssr_build_desired_cells(&renderer, "👩🏽‍💻", 0, 1, 4, A_NORMAL);
    assert(renderer.desired_cells[0].kind == SSR_CELL_GLYPH);
    assert(renderer.desired_cells[0].text[0] == 0x1F469);
    if (CCHARW_MAX >= 5) {
        assert(renderer.desired_cells[0].text[1] == 0x1F3FD);
        assert(renderer.desired_cells[0].text[2] == 0x200D);
        assert(renderer.desired_cells[0].text[3] == 0x1F4BB);
    }
    assert(renderer.desired_cells[1].kind == SSR_CELL_CONTINUATION);
    assert(renderer.desired_cells[2].kind == SSR_CELL_BLANK);
    ssr_destroy(&renderer);

    decoded = decode_text_part(
        "Cr=E8me br=FBl=E9e costs =A312.50.",
        "quoted-printable",
        "text/plain; charset=iso-8859-1");
    assert_contains(decoded, "Crème brûlée costs £12.50.");
    free(decoded);

    decoded = decode_rfc2047_header(
        "=?ISO-8859-1?Q?Andr=E9?= <andre@example.test>");
    assert_contains(decoded, "André <andre@example.test>");
    free(decoded);

    assert(headers_attachment_filename(
        "Content-Type: text/plain; charset=utf-8\r\n"
        "Content-Disposition: attachment;\r\n"
        " filename*=utf-8''r%C3%A9sum%C3%A9%20notes.txt\r\n",
        filename, sizeof filename));
    assert(!strcmp(filename, "résumé notes.txt"));
    assert(headers_attachment_filename(
        "Content-Type: application/octet-stream\r\n"
        "Content-Disposition: attachment; filename*0*=utf-8''chapter%20;"
        " filename*1*=one%20notes.txt\r\n",
        filename, sizeof filename));
    assert(!strcmp(filename, "chapter one notes.txt"));

    decoded = decode_text_part(
        "A sentence ends here. \r\nNext begins.\r\n\r\n"
        "> A quoted thought ends here. \r\n> It continues cleanly.",
        "7bit", "text/plain; charset=utf-8; format=flowed; delsp=yes");
    assert_contains(decoded, "A sentence ends here. Next begins.");
    assert_contains(decoded,
                    "> A quoted thought ends here. It continues cleanly.");
    free(decoded);

    decoded = decode_text_part("-- \r\nKeelan\r\nSecond signature line",
                               "7bit",
                               "text/plain; charset=utf-8; format=flowed");
    clean = normalize_mail_text(decoded);
    assert_contains(clean, "--\nKeelan\nSecond signature line");
    free(clean);
    free(decoded);

    html = html_to_text(
        "<html><body>"
        "<div class='preheader'>HIDDEN PREHEADER</div>"
        "<span style='display: none'>HIDDEN INLINE</span>"
        "<div hidden><div>HIDDEN NESTED</div>HIDDEN TAIL</div>"
        "<!--[if mso]><p>OUTLOOK ONLY</p><![endif]-->"
        "<!--[if !mso]><!--><p>Visible everywhere else.</p><!--<![endif]-->"
        "<h1>Rendering specimen</h1>"
        "<p>First line<br>Second line &mdash; intact.</p>"
        "<ol start='3'><li>Third<ul><li>Nested</li></ul></li>"
        "<li value='7'>Seventh</li></ol>"
        "<dl><dt>Adroit</dt><dd>Skillful and nimble.</dd></dl>"
        "<table><tr><th>Item</th><th>Price</th></tr>"
        "<tr><td>Book</td><td>$12</td></tr></table>"
        "<p><a title='1 > 0' href='https://example.test/guide'>Project guide</a> "
        "<a href='javascript:alert(1)'>Safe label</a></p>"
        "<img src='diagram.png' width='640' height='320' "
        "alt='Publishing workflow'>"
        "<img src='pixel.gif' width='1' height='1' alt='Tracking pixel'>"
        "<pre>A    B\n  indented</pre>"
        "<footer>You received this because you requested updates.</footer>"
        "<script>ACTIVE CONTENT</script>"
        "</body></html>");
    clean = normalize_html_text(html);
    display = render_body_text(clean);

    assert_omits(display, "HIDDEN PREHEADER");
    assert_omits(display, "HIDDEN INLINE");
    assert_omits(display, "HIDDEN NESTED");
    assert_omits(display, "HIDDEN TAIL");
    assert_omits(display, "OUTLOOK ONLY");
    assert_omits(display, "ACTIVE CONTENT");
    assert_omits(display, "javascript:");
    assert_omits(display, "Tracking pixel");
    assert_omits(display, "requested updates");
    assert_contains(display, "Visible everywhere else.");
    assert_contains(display, "First line\nSecond line — intact.");
    assert_contains(display, "3. Third\n  * Nested");
    assert_contains(display, "7. Seventh");
    assert_contains(display, "Adroit\n  Skillful and nimble.");
    assert_contains(display, "Item | Price\nBook | $12");
    assert_contains(display,
                    "Project guide (https://example.test/guide) Safe label");
    assert_contains(display, "[Image: Publishing workflow]");
    assert_contains(display, "    A    B\n      indented");

    free(display);
    free(clean);
    free(html);

    clean = normalize_mail_text("Emoji sequence: 👩🏽‍💻\nRTL: مرحبا بالعالم");
    assert_contains(clean, "👩🏽‍💻");
    assert_contains(clean, "مرحبا بالعالم");
    free(clean);

    decoded = extract_mime_display_body(
        &message,
        "--alt\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n\r\n"
        "View online: https://tracking.example.test/click/token?utm_source=mail\r\n"
        "--alt\r\n"
        "Content-Type: text/html; charset=utf-8\r\n\r\n"
        "<h1>Complete edition</h1><p>This HTML alternative contains the "
        "complete readable story, not merely a tracking link.</p>\r\n"
        "--alt--\r\n",
        "multipart/alternative; boundary=alt", "7bit");
    assert_contains(decoded, "Complete edition");
    assert_contains(decoded, "complete readable story");
    assert_omits(decoded, "View online");
    free(decoded);

    {
        char path[] = "/tmp/simplemail-lazy-check-XXXXXX";
        int fd = mkstemp(path);
        assert(fd >= 0);
        FILE *mail = fdopen(fd, "w");
        assert(mail);
        assert(fputs("From: Reader <reader@example.test>\n"
                     "Subject: Lazy body check\n"
                     "Date: Tue, 14 Jul 2026 12:00:00 -0400\n"
                     "Content-Type: text/plain; charset=utf-8\n"
                     "\n"
                     "Body decoding happens only when opened.\n", mail) >= 0);
        assert(fclose(mail) == 0);

        snprintf(lazy.path, sizeof lazy.path, "%s", path);
        parse_message_headers(&lazy);
        assert_contains(lazy.subject, "Lazy body check");
        assert(!lazy.body_loaded);
        assert(!lazy.body);

        parse_message_file(&lazy);
        assert(lazy.body_loaded);
        assert_contains(lazy.body, "only when opened");
        free(lazy.body);
        unlink(path);
    }

    {
        char root[] = "/tmp/simplemail-send-check-XXXXXX";
        char sent[PATH_MAX];
        char sent_cur[PATH_MAX];
        char body[PATH_MAX];

        assert(mkdtemp(root));
        snprintf(mail_root, sizeof mail_root, "%s", root);
        snprintf(sent, sizeof sent, "%s/Sent", root);
        snprintf(sent_cur, sizeof sent_cur, "%s/cur", sent);
        assert(mkdir(sent, 0700) == 0);
        assert(mkdir(sent_cur, 0700) == 0);
        snprintf(body, sizeof body, "%s/body", root);
        FILE *body_file = fopen(body, "w");
        assert(body_file);
        assert(fputs("Background send body.\n", body_file) >= 0);
        assert(fclose(body_file) == 0);
        snprintf(simplemail_send_cmd, sizeof simplemail_send_cmd,
                 "sleep 0.2; cat >/dev/null");

        assert(start_background_send("reader@example.test", "Async send",
                                     body, "", NULL, NULL));
        assert(send_running && send_pid > 0);
        assert(access(body, F_OK) == 0);
        for (int i = 0; i < 300 && send_running; i++) {
            finish_send_if_done();
            if (send_running) usleep(10000);
        }
        assert(!send_running && send_pid == 0);
        assert(access(body, F_OK) != 0);
        assert_contains(status_msg, "Mail sent");
        DIR *sent_dir = opendir(sent_cur);
        assert(sent_dir);
        struct dirent *entry;
        while ((entry = readdir(sent_dir)) != NULL) {
            if (entry->d_name[0] == '.') continue;
            char sent_file[PATH_MAX];
            snprintf(sent_file, sizeof sent_file, "%s/%s",
                     sent_cur, entry->d_name);
            assert(unlink(sent_file) == 0);
        }
        closedir(sent_dir);
        assert(rmdir(sent_cur) == 0);
        assert(rmdir(sent) == 0);
        assert(rmdir(root) == 0);
    }

    return 0;
}
