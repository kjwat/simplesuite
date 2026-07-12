#define main simplebrowse_program_main
#include "../simplebrowse.c"
#undef main

#include <assert.h>

static int has_text_control(Page *page, const char *name)
{
    size_t i;
    for (i = 0; i < page->control_count; i++)
        if (control_is_textual(&page->controls[i]) &&
            !strcmp(page->controls[i].name, name)) return 1;
    return 0;
}

int main(void)
{
    Buffer listing = {0};
    Page page;
    int i;

    /* Reader-region selection must not discard a useful form elsewhere. */
    page = parse_html(
        "<body><form action='/search'><input type='text' name='q' "
        "aria-label='Search catalog'><button>Search</button></form>"
        "<main><article><h1>Welcome</h1><p>This deliberately substantial "
        "article prose makes the semantic region attractive to reader mode. "
        "The search field must nevertheless remain part of the document.</p>"
        "<p>More readable prose follows here to keep the selected article "
        "comfortably above the normal meaningful-content threshold.</p>"
        "</article></main></body>",
        strlen("<body><form action='/search'><input type='text' name='q' "
               "aria-label='Search catalog'><button>Search</button></form>"
               "<main><article><h1>Welcome</h1><p>This deliberately substantial "
               "article prose makes the semantic region attractive to reader mode. "
               "The search field must nevertheless remain part of the document.</p>"
               "<p>More readable prose follows here to keep the selected article "
               "comfortably above the normal meaningful-content threshold.</p>"
               "</article></main></body>"),
        "https://example.test/");
    assert(has_text_control(&page, "q"));
    page_free(&page);

    /* JavaScript strings containing markup are raw text, never DOM links. */
    page = parse_html("<main><script>const fake=\"<a href='/fake'>Fake</a>\";"
                      "</script><p><a href='/real'>Real result</a></p></main>",
                      strlen("<main><script>const fake=\"<a href='/fake'>Fake</a>\";"
                             "</script><p><a href='/real'>Real result</a></p></main>"),
                      "https://example.test/");
    assert(page.link_count == 1);
    assert(strstr(page.links[0].url, "/real"));
    page_free(&page);

    /* Div-based repeated records are listings even without article/li tags. */
    buf_addn(&listing, "<main class='main'><h1>Results</h1>",
             strlen("<main class='main'><h1>Results</h1>"));
    for (i = 0; i < 12; i++) {
        char record[512];
        int n = snprintf(record, sizeof(record),
            "<div class='record'><a href='/record/%d'>Result title %d</a>"
            "<p>This is meaningful catalog description text for result %d, "
            "including author, edition, language, format, and publication "
            "details that a reader needs when choosing a record.</p></div>",
            i, i, i);
        buf_addn(&listing, record, (size_t)n);
    }
    buf_addn(&listing, "</main>", 7);
    page = parse_html(listing.data, listing.len, "https://example.test/search");
    assert(page.link_count == 12);
    assert(strstr(page.text, "Result title 0"));
    assert(strstr(page.text, "Result title 11"));
    assert(strstr(page.text, "meaningful catalog description text for result 0"));
    assert(strstr(page.text, "meaningful catalog description text for result 11"));
    page_free(&page);
    buf_clear(&listing);
    return 0;
}
