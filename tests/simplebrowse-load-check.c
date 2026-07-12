#define main simplebrowse_program_main
#include "../simplebrowse.c"
#undef main

#include <assert.h>

int main(void)
{
    App app = {0};
    Page substantial = {0};
    Page thin = {0};
    char useful_text[801];

    assert(load_mode_force_js(LOAD_AUTO) == 0);
    assert(load_mode_force_js(LOAD_READER) == -1);
    assert(load_mode_force_js(LOAD_WEBKIT) == 1);

    app.load_mode = LOAD_AUTO;
    app.page_uses_webkit = 0;
    assert(!strcmp(load_mode_label(&app), "AUTO/FAST"));
    app.page_uses_webkit = 1;
    assert(!strcmp(load_mode_label(&app), "AUTO/WEBKIT"));
    app.load_mode = LOAD_READER;
    assert(!strcmp(load_mode_label(&app), "READER"));
    app.load_mode = LOAD_WEBKIT;
    assert(!strcmp(load_mode_label(&app), "WEBKIT"));

    assert(!webkit_first_url("https://en.wikipedia.org/wiki/Test"));
    assert(webkit_first_url("https://www.google.com/search?q=test"));
    assert(webkit_first_url("https://consent.google.com/example"));

    /* Compatibility notices on an otherwise complete page must not trigger
     * a second fetch through WebKit. */
    memset(useful_text, 'a', sizeof(useful_text) - 1);
    useful_text[sizeof(useful_text) - 1] = 0;
    substantial.text = useful_text;
    assert(!static_page_should_retry_js(
        "https://example.test/article",
        "<main><article>Useful article</article></main>"
        "<footer>Please enable JavaScript for enhanced features.</footer>",
        strlen("<main><article>Useful article</article></main>"
               "<footer>Please enable JavaScript for enhanced features.</footer>"),
        &substantial));

    /* A genuinely empty application shell should still get the JS fallback. */
    thin.text = "Loading";
    assert(static_page_should_retry_js(
        "https://example.test/app",
        "<html><body><div id='root'>Loading</div>"
        "<script></script><script></script><script></script><script></script>"
        "<script></script><script></script><script></script><script></script>"
        "</body></html>",
        strlen("<html><body><div id='root'>Loading</div>"
               "<script></script><script></script><script></script><script></script>"
               "<script></script><script></script><script></script><script></script>"
               "</body></html>"),
        &thin));

    return 0;
}
