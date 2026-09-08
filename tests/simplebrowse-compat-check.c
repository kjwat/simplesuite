#define main simplebrowse_program_main
#include "../simplebrowse.c"
#undef main

#include <assert.h>

int main(void)
{
    static const char snapshot[] =
        "<html><head><title>Browser interface</title></head><body>"
        "<main data-simplebrowse-snapshot=\"visible\" data-simplebrowse-snapshot-version=\"2\">"
        "<nav><a href='/search'>Search</a><a href='/login'>Sign in</a></nav>"
        "<article><h1>Article</h1><p>Readable article content.</p>"
        "<pre>  const example = document.querySelector('main'); "
        "window.addEventListener('click', function() { return JSON.parse('{\"hello\":\"world\"}'); }); "
        "// This is a visible code example, not an executable script.</pre></article>"
        "<aside><a href='/next'>Next chapter</a></aside>"
        "<button type='button' data-simplebrowse-control-id='epoch:1'>Close</button>"
        "</main></body></html>";
    Page p = parse_html(snapshot, sizeof(snapshot) - 1, "https://example.test/");
    Page copy = {0};
    char *payload;
    char *query;

    setlocale(LC_ALL, "");
    assert(p.link_count == 3);
    assert(strstr(p.text, "Sign in"));
    assert(strstr(p.text, "visible code example"));
    assert(p.control_count == 1 && !strcmp(p.controls[0].dom_id, "epoch:1"));
    assert(page_clone(&copy, &p));
    page_free(&p);
    payload = build_form_json(&copy, -1, 0);
    assert(strstr(payload, "\"target_id\":\"epoch:1\""));
    assert(strstr(payload, "\"action\":\"click\""));
    free(payload);
    page_free(&copy);

    /* Documentation chapter lists are content, including toctree wrappers. */
    assert(!attr_value_is_clutter("toctree-wrapper compound"));
    {
        const char *html = "<main><h1>Tutorial</h1><div class='toctree-wrapper'>"
                           "<ul><li><a href='/chapter/one'>First chapter</a></li>"
                           "<li><a href='/chapter/two'>Second chapter</a></li></ul></div></main>";
        p = parse_html(html, strlen(html), "https://example.test/tutorial");
        assert(p.link_count == 2);
        page_free(&p);
    }

    /* Empty option values, checkboxes without values, and textarea newlines
       are observable by the receiving server, not merely cosmetic details. */
    {
        const char *html = "<form action='/submit'>"
            "<input type='checkbox' name='enabled' checked>"
            "<select name='choice'><option value='' selected>Choose</option></select>"
            "<textarea name='text'>\n  first\nsecond &amp; &lt;literal&gt;  </textarea>"
            "<button name='submit'>Send form</button></form>";
        p = parse_html_fragment(html, strlen(html), "https://example.test/");
        assert(p.control_count == 4);
        assert(!strcmp(p.controls[0].value, "on"));
        assert(!strcmp(control_submit_value(&p.controls[1]), ""));
        assert(!strcmp(p.controls[2].value, "  first\nsecond & <literal>  "));
        assert(!strcmp(p.controls[3].label, "Send form"));
        assert(!strcmp(p.controls[3].value, ""));
        query = build_urlencoded_form(&p, 0, 3);
        assert(strstr(query, "enabled=on&choice=&text="));
        free(query);
        page_free(&p);
    }
    query = form_url_with_query("https://example.test/search?old=value#results", "q=new");
    assert(!strcmp(query, "https://example.test/search?q=new#results"));
    free(query);
    query = form_url_with_query("https://example.test/search?old=value#results", "");
    assert(!strcmp(query, "https://example.test/search#results"));
    free(query);

    assert(urls_share_origin("https://EXAMPLE.test/a", "https://example.test:443/b"));
    assert(!urls_share_origin("https://example.test/a", "http://example.test/a"));
    assert(!urls_share_origin("https://example.test/a", "https://example.test:444/a"));
    assert(!urls_share_origin("https://example.test/a", "https://another.test/a"));

    /* The screen and link selection must use terminal columns, not UTF-8
       byte counts. Otherwise wide text wraps early and tabs hide characters. */
    p.text = xstrdup_local("    indented\n1234567890123456\tABCD\n日本語日本語日本語日本語");
    p.layout_width = -1;
    page_layout(&p, 20);
    assert(p.display[0].start == 0);
    for (size_t i = 0; i < p.display_count; i++)
        assert(browse_visual_width_n(p.text + p.display[i].start, p.display[i].len) <= 20);
    page_free(&p);

    puts("PASS C compatibility checks");
    return 0;
}
