/* Read-only probe of the real parser/loaders, used by the compatibility tour.
 * Keeping this in tests avoids adding diagnostic data to the terminal UI. */
#define main simplebrowse_program_main
#include "../simplebrowse.c"
#undef main

static void member(Buffer *b, const char *key, const char *value)
{
    json_string_append(b, key);
    buf_addc(b, ':');
    json_string_append(b, value);
}

int main(int argc, char **argv)
{
    Page page = {0};
    Buffer input = {0}, out = {0};
    FetchResult result = {0};
    char *html = NULL;
    char chunk[4096];
    size_t n;
    int rc = 0;

    if (argc != 3) return 2;
    setlocale(LC_ALL, "");
    curl_global_init(CURL_GLOBAL_DEFAULT);
    if (!strcmp(argv[1], "parse")) {
        while ((n = fread(chunk, 1, sizeof(chunk), stdin)) != 0)
            buf_addn(&input, chunk, n);
        page = parse_html(input.data ? input.data : "", input.len, argv[2]);
    } else if (!strcmp(argv[1], "reader")) {
        html = fetch_url(argv[2], &result);
        if (!html) rc = 1;
        else page = parse_html(html, strlen(html),
                               result.effective[0] ? result.effective : argv[2]);
    } else if (!strcmp(argv[1], "auto") || !strcmp(argv[1], "js")) {
        rc = fetch_page_for_dump(argv[2], &page, !strcmp(argv[1], "js"));
    } else rc = 2;

    buf_addc(&out, '{');
    member(&out, "url", page.url ? page.url : argv[2]);
    buf_addc(&out, ','); member(&out, "title", page.title);
    buf_addc(&out, ','); member(&out, "text", page.text);
    buf_addc(&out, ','); member(&out, "error", result.error);
    snprintf(chunk, sizeof(chunk), ",\"http_status\":%ld,\"links\":[", result.code);
    buf_addn(&out, chunk, strlen(chunk));
    for (size_t i = 0; i < page.link_count; i++) {
        if (i) buf_addc(&out, ',');
        buf_addc(&out, '{');
        member(&out, "label", page.links[i].label);
        buf_addc(&out, ','); member(&out, "url", page.links[i].url);
        buf_addc(&out, '}');
    }
    buf_addn(&out, "],\"controls\":[", strlen("],\"controls\":["));
    for (size_t i = 0; i < page.control_count; i++) {
        FormControl *c = &page.controls[i];
        char *payload = build_form_json(&page, c->form_index, (int)i);
        if (i) buf_addc(&out, ',');
        buf_addc(&out, '{');
        member(&out, "label", c->label);
        buf_addc(&out, ','); member(&out, "name", c->name);
        buf_addc(&out, ','); member(&out, "type", control_type_name(c));
        buf_addc(&out, ','); member(&out, "value", c->value);
        buf_addc(&out, ','); member(&out, "action", c->form_action);
        buf_addn(&out, ",\"payload\":", strlen(",\"payload\":"));
        buf_addn(&out, payload, strlen(payload));
        buf_addc(&out, '}');
        free(payload);
    }
    buf_addn(&out, "]}", 2);
    puts(out.data);
    page_free(&page);
    free(html);
    buf_clear(&input);
    buf_clear(&out);
    webkitd_stop();
    browser_curl_cleanup();
    curl_global_cleanup();
    return rc;
}
