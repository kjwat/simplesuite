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

    assert(page.control_count == 3);
    assert(page.controls[0].type == CONTROL_HIDDEN);
    assert(strcmp(page.controls[0].name, "family") == 0);
    assert(strcmp(page.controls[0].value, "wiktionary") == 0);
    assert(form_uses_standard_get(&page.controls[1]));

    query = build_urlencoded_form(&page, page.controls[1].form_index, 2);
    assert(strcmp(query, "family=wiktionary&search=putat") == 0);

    free(query);
    page_free(&page);
    return 0;
}
