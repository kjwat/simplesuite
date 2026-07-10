#define main simplebrowse_main
#include "../simplebrowse.c"
#undef main

#include <stdio.h>

static int contains(const char *haystack, const char *needle)
{
    return strstr(haystack, needle) != NULL;
}

int main(void)
{
    static const char html[] =
        "<!doctype html><html><head><title>Milton - Wikipedia</title></head>"
        "<body><main><div class=\"mw-parser-output\">"
        "<h1>Milton</h1>"
        "<p>From Wikipedia, the free encyclopedia</p>"
        "<p>Milton may refer to:</p>"
        "<h2>Names</h2>"
        "<ul>"
        "<li><a href=\"/wiki/Milton_(surname)\">Milton (surname)</a>, a surname"
        "<ul><li><a href=\"/wiki/John_Milton\">John Milton</a> (1608-1674), English poet</li></ul>"
        "</li>"
        "<li><a href=\"/wiki/Milton_(given_name)\">Milton (given name)</a></li>"
        "</ul>"
        "<h2>Places</h2>"
        "<ul><li><a href=\"/wiki/Milton,_Florida\">Milton, Florida</a></li></ul>"
        "</div></main></body></html>";
    Page page = parse_html(html, strlen(html), "https://en.wikipedia.org/wiki/Milton");
    int ok = 1;
    size_t i;
    int found_john = 0;

    if (!contains(page.text, "Milton may refer to:") ||
        !contains(page.text, "John Milton") ||
        !contains(page.text, "Milton (given name)")) {
        fprintf(stderr, "disambiguation bullets missing from rendered text:\n%s\n",
                page.text);
        ok = 0;
    }

    for (i = 0; i < page.link_count; i++) {
        if (page.links[i].label && !strcmp(page.links[i].label, "John Milton"))
            found_john = 1;
    }
    if (!found_john) {
        fprintf(stderr, "John Milton link missing from %zu extracted links\n",
                page.link_count);
        ok = 0;
    }

    page_free(&page);
    return ok ? 0 : 1;
}
