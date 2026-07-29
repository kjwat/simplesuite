#define main simplemail_program_main
#include "../simplemail.c"
#undef main

#include <assert.h>
#include <time.h>

static void sleep_10ms(void)
{
    struct timespec delay = {0, 10000000L};

    nanosleep(&delay, NULL);
}

static void assert_contains(const char *text, const char *needle)
{
    assert(text);
    assert(needle);
    if (!strstr(text, needle))
        fprintf(stderr, "missing [%s] from:\n%s\n", needle, text);
    assert(strstr(text, needle));
}

static void assert_omits(const char *text, const char *needle)
{
    assert(text);
    assert(needle);
    if (strstr(text, needle))
        fprintf(stderr, "unexpected [%s] in:\n%s\n", needle, text);
    assert(!strstr(text, needle));
}

int main(void)
{
    char *decoded;
    char *html;
    char *clean;
    char *display;
    MailRenderDocument document;
    char filename[256];
    Message message = {0};
    Message lazy = {0};
    SsrRenderer renderer;

    setlocale(LC_ALL, "");

    assert(parse_mail_csi("[A") == KEY_UP);
    assert(parse_mail_csi("[B") == KEY_DOWN);
    assert(parse_mail_csi("[1;2A") == MAIL_KEY_LINK_PREV);
    assert(parse_mail_csi("[1;2B") == MAIL_KEY_LINK_NEXT);
    assert(parse_mail_csi("[1;4A") == MAIL_KEY_LINK_PREV);
    assert(parse_mail_csi("[1;5A") == 0);

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

    {
        static const char span_text[] = "before linked after";
        SsrSpan span = {
            .start = 7,
            .end = 13,
            .attr = A_UNDERLINE | A_REVERSE
        };

        ssr_init(&renderer);
        assert(ssr_ensure_storage(&renderer, 1, 24));
        ssr_build_desired_cells_spans(&renderer, span_text, 0, 1, 24,
                                      A_NORMAL, &span, 1);
        assert(!(renderer.desired_cells[6].attr & A_UNDERLINE));
        assert(renderer.desired_cells[7].attr & A_UNDERLINE);
        assert(renderer.desired_cells[7].attr & A_REVERSE);
        assert(renderer.desired_cells[12].attr & A_UNDERLINE);
        assert(!(renderer.desired_cells[13].attr & A_UNDERLINE));
        ssr_destroy(&renderer);
    }

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
    document = render_body_document(clean);
    display = document.text;

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
    assert_contains(display, "- Third");
    assert_contains(display, "- Nested");
    assert_contains(display, "- Seventh");
    assert_contains(display, "Adroit\n\nSkillful and nimble.");
    assert_contains(display, "Item\n\nPrice\n\nBook\n\n$12");
    assert_contains(display, "Project guide Safe label");
    assert_omits(display, "https://example.test/guide");
    assert(document.link_count == 1);
    assert(!strcmp(document.links[0].label, "Project guide"));
    assert(!strcmp(document.links[0].url, "https://example.test/guide"));
    assert(!strncmp(display + document.links[0].offset,
                    "Project guide", document.links[0].length));
    assert_contains(display, "[Image: Publishing workflow]");
    assert_contains(display, "A    B\n  indented");

    mail_render_document_free(&document);
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
        char path[] = "/tmp/simplemail-html-check-XXXXXX";
        Message rich = {0};
        int fd = mkstemp(path);
        assert(fd >= 0);
        FILE *mail = fdopen(fd, "w");
        assert(mail);
        assert(fputs(
            "From: Publisher <publisher@example.test>\n"
            "Subject: Mature HTML rendering\n"
            "Date: Tue, 14 Jul 2026 12:00:00 -0400\n"
            "Content-Type: text/html; charset=utf-8\n"
            "Content-Transfer-Encoding: 8bit\n"
            "\n"
            "<html><body>"
            "<div class='preheader'>HIDDEN INBOX PREVIEW</div>"
            "<h1>Today&rsquo;s complete edition</h1>"
            "<p>The &lsquo;rendered&rsquo; document keeps readable prose, "
            "punctuation, and live references.</p>"
            "<p><a href='https://example.test/read?id=7&amp;src=mail'>"
            "Open the complete story</a></p>"
            "<script>NEVER DISPLAY THIS</script>"
            "</body></html>\n", mail) >= 0);
        assert(fclose(mail) == 0);

        snprintf(rich.path, sizeof rich.path, "%s", path);
        parse_message_file(&rich);
        assert(rich.body_loaded);
        assert_contains(rich.body, "Today’s complete edition");
        assert_contains(rich.body, "The ‘rendered’ document");
        assert_contains(rich.body, "Open the complete story");
        assert_omits(rich.body, "HIDDEN INBOX PREVIEW");
        assert_omits(rich.body, "NEVER DISPLAY THIS");
        assert_omits(rich.body, "&lsquo;");
        assert(rich.link_count == 1);
        assert(!strcmp(rich.links[0].label, "Open the complete story"));
        assert(!strcmp(rich.links[0].url,
                       "https://example.test/read?id=7&src=mail"));
        assert(!strncmp(rich.body + rich.links[0].offset,
                        rich.links[0].label, rich.links[0].length));

        free(rich.body);
        free_mail_links(rich.links, rich.link_count);
        assert(unlink(path) == 0);
    }

    {
        static const char signed_order_url[] =
            "https://orders.example.test/my-orders.php?"
            "location=37099&member=2023&key="
            "48c2f94d9b6379c4988289984bf799cda1d4baf17a9f7c123bacab36fac4d4bf";

        html = html_to_text(
            "<div><blockquote><h2>Spin Sudz</h2>"
            "<p>Your order has been accepted. Please have your items ready "
            "for pickup.</p><table><tr><td>Order ID:</td><td>16079</td></tr>"
            "<tr><td>Track Order:</td><td><a href='"
            "https://orders.example.test/my-orders.php?"
            "location=37099&amp;member=2023&amp;key="
            "48c2f94d9b6379c4988289984bf799cda1d4baf17a9f7c123bacab36fac4d4bf"
            "' target='_blank' rel='noreferrer nofollow noopener'>"
            "My Orders</a></td></tr></table></blockquote></div>");
        clean = normalize_html_text(html);
        document = render_body_document(clean);

        assert_contains(document.text, "Track Order:");
        assert_contains(document.text, "My Orders");
        assert(document.link_count == 1);
        assert(!strcmp(document.links[0].label, "My Orders"));
        assert(!strcmp(document.links[0].url, signed_order_url));
        assert(!strncmp(document.text + document.links[0].offset,
                        "My Orders", document.links[0].length));

        mail_render_document_free(&document);
        free(clean);
        free(html);
    }

    {
        Message plain = {0};

        plain.body = extract_mime_display_body(
            &plain,
            "Useful reference: https://example.test/guide?q=one\n"
            "https://tracking.example.test/click/token?utm_source=mail\n",
            "text/plain; charset=utf-8", "7bit");
        assert(plain.body);
        finalize_message_body(&plain);
        assert_contains(plain.body, "https://example.test/guide?q=one");
        assert_omits(plain.body, "tracking.example.test");
        assert(plain.link_count == 1);
        assert(!strcmp(plain.links[0].url,
                       "https://example.test/guide?q=one"));
        assert(!strncmp(plain.body + plain.links[0].offset,
                        plain.links[0].label, plain.links[0].length));
        free(plain.body);
        free_mail_links(plain.links, plain.link_count);
    }

    {
        MailRenderDocument navigation = render_body_document(
            "[First link](https://example.test/first)\n"
            "one\n"
            "two\n"
            "[Second link](https://example.test/second)\n"
            "three\n"
            "four\n"
            "[Third link](https://example.test/third)");
        Message nav_message = {
            .body = navigation.text,
            .links = navigation.links,
            .link_count = navigation.link_count
        };
        int previous_scroll;

        assert(navigation.link_count == 3);
        read_scroll = 0;
        read_selected_link = -1;
        status_msg[0] = '\0';
        simplemail_select_link(&nav_message, 1, 2, 40);
        assert(read_selected_link == 0);
        simplemail_select_link(&nav_message, 1, 2, 40);
        assert(read_selected_link == 1);
        assert(read_scroll > 0);
        previous_scroll = read_scroll;
        simplemail_select_link(&nav_message, 1, 2, 40);
        assert(read_selected_link == 2);
        assert(read_scroll >= previous_scroll);
        simplemail_select_link(&nav_message, 1, 2, 40);
        assert(read_selected_link == 2);
        assert_contains(status_msg, "No next link");

        read_scroll = 100;
        read_selected_link = -1;
        simplemail_select_link(&nav_message, 1, 2, 40);
        assert(read_selected_link == -1);
        assert_contains(status_msg, "No next link");
        simplemail_select_link(&nav_message, -1, 2, 40);
        assert(read_selected_link == 2);
        assert(read_scroll < 100);

        mail_render_document_free(&navigation);
        read_scroll = 0;
        read_selected_link = -1;
        status_msg[0] = '\0';
    }

    {
        char saved_browser[sizeof simplemail_browser_cmd];
        char *command;

        snprintf(saved_browser, sizeof saved_browser, "%s",
                 simplemail_browser_cmd);
        snprintf(simplemail_browser_cmd, sizeof simplemail_browser_cmd,
                 "simplebrowse --fresh %%u");
        command = simplemail_build_browser_command(
            "https://example.test/o'connor?q=two words");
        assert(command);
        assert(!strcmp(
            command,
            "simplebrowse --fresh "
            "'https://example.test/o'\\''connor?q=two words'"));
        free(command);

        snprintf(simplemail_browser_cmd, sizeof simplemail_browser_cmd,
                 "simplebrowse");
        command = simplemail_build_browser_command(
            "https://example.test/plain");
        assert(command);
        assert(!strcmp(command,
                       "simplebrowse 'https://example.test/plain'"));
        free(command);
        snprintf(simplemail_browser_cmd, sizeof simplemail_browser_cmd,
                 "%s", saved_browser);
    }

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
            if (send_running) sleep_10ms();
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
