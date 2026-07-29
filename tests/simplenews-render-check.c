#define main simplenews_program_main
#include "../simplenews.c"
#undef main

#include <assert.h>

int main(void)
{
    static const char encoded[] =
        "The &lsquo;burbs&rsquo; weren&rsquo;t &ldquo;quiet&rdquo;"
        "&mdash;really&hellip; Named: &auml; &larr; &copy;. "
        "Numeric: &#8217; &#x2019;. Unknown: &bogus;";
    Article article = {0};
    char *text;

    text = decode_text(encoded, sizeof(encoded) - 1);
    assert(text);
    assert(!strcmp(text,
                   "The 'burbs' weren't \"quiet\"-really... "
                   "Named: \xc3\xa4 \xe2\x86\x90 \xc2\xa9. "
                   "Numeric: ' '. Unknown: &bogus;"));
    free(text);

    text = html_plain(
        "<p>A&nbsp;story &mdash; "
        "<a href='https://example.test/?a=1&amp;b=2'>linked</a>.</p>",
        &article);
    assert(text);
    assert(!strcmp(text, "A story - linked."));
    assert(article.link_count == 1);
    assert(!strcmp(article.links[0].url, "https://example.test/?a=1&b=2"));
    free(text);
    article_free(&article);
    return 0;
}
