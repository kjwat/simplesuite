#define main simplebrowse_program_main
#include "../simplebrowse.c"
#undef main

#include <assert.h>

int main(void)
{
    char ext[16];

    assert(direct_file_extension("https://upload.example/Rule.oga", ext, sizeof(ext)));
    assert(strcmp(ext, ".oga") == 0);
    assert(direct_file_extension("https://example.test/movie.MP4?download=1", ext,
                                 sizeof(ext)));
    assert(strcmp(ext, ".mp4") == 0);
    assert(direct_file_extension("https://example.test/book.pdf#page=2", NULL, 0));
    assert(!direct_file_extension(
        "https://en.wikipedia.org/wiki/File:Rule,_Britannia!.oga", NULL, 0));
    assert(!direct_file_extension(
        "https://commons.wikimedia.org/wiki/File%3ARule.oga", NULL, 0));
    assert(!direct_file_extension("https://example.test/article.html", NULL, 0));
    assert(!direct_file_extension("https://example.test/search?q=song.ogg", NULL, 0));
    return 0;
}
