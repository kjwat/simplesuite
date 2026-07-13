#define main simplebrowse_program_main
#include "../simplebrowse.c"
#undef main

#include <assert.h>

int main(void)
{
    const char *html =
        "<main><form action=\"/search-redirect.php\">"
        "<input type=\"hidden\" name=\"family\" value=\"wiktionary\">"
        "<input type=\"search\" name=\"search\" value=\"putat\">"
        "<button type=\"submit\">Search</button>"
        "</form></main>";
    Page page = parse_html(html, strlen(html), "https://www.wiktionary.org/");
    char *query;
    const char *filtered_html =
        "<!doctype html><html><head><title>Wiktionary</title></head>"
        "<body><main><h1>Wiktionary</h1>"
        "<p>This deliberately substantial introduction represents the text "
        "around a portal search form. Reader filtering keeps this useful "
        "content while discarding page chrome, but it must not discard the "
        "hidden controls belonging to a retained form. Additional prose keeps "
        "the selected reader region above the small-page fallback threshold, "
        "so this exercises the normal filtered-page path used by large portal "
        "pages instead of replacing it with the complete unfiltered document. "
        "Search routing values are part of the form even though they have no "
        "visible marker in the terminal rendering.</p>"
        "<form action='/search-redirect.php'>"
        "<input type='hidden' name='family' value='wiktionary'>"
        "<input type='search' name='search' value='putare'>"
        "<select name='language'><option value='en' selected>English</option></select>"
        "<button type='submit'>Search</button>"
        "</form></main></body></html>";
    Page filtered;
    int search_index = -1;
    size_t i;

    assert(page.control_count == 3);
    assert(page.controls[0].type == CONTROL_HIDDEN);
    assert(strcmp(page.controls[0].name, "family") == 0);
    assert(strcmp(page.controls[0].value, "wiktionary") == 0);
    assert(form_uses_standard_get(&page.controls[1]));

    query = build_urlencoded_form(&page, page.controls[1].form_index, 2);
    assert(strcmp(query, "family=wiktionary&search=putat") == 0);

    free(query);
    page_free(&page);

    filtered = parse_html(filtered_html, strlen(filtered_html),
                          "https://www.wiktionary.org/");
    for (i = 0; i < filtered.control_count; i++) {
        if (filtered.controls[i].type == CONTROL_SEARCH &&
            !strcmp(filtered.controls[i].name, "search")) {
            search_index = (int)i;
            break;
        }
    }
    assert(search_index >= 0);
    query = build_urlencoded_form(&filtered,
                                  filtered.controls[search_index].form_index,
                                  search_index);
    assert(strcmp(query, "family=wiktionary&search=putare&language=en") == 0);

    free(query);
    page_free(&filtered);
    return 0;
}
