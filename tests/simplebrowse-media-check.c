#define main simplebrowse_program_main
#include "../simplebrowse.c"
#undef main

#include <assert.h>

int main(void)
{
    char ext[16];
    char tmp[] = "/tmp/simplebrowse-media-test.XXXXXX";
    char page_path[PATH_MAX];
    char media_path[PATH_MAX];
    char filename[512];
    char dir[PATH_MAX];
    FILE *fp;
    App app = {0};

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
    media_filename("https://upload.example/%27Rule%2C_Britannia%21%27.oga?x=1",
                   filename, sizeof(filename));
    assert(strcmp(filename, "'Rule,_Britannia!'.oga") == 0);
    assert(parse_browser_csi("[13;2u") == BROWSE_KEY_SHIFT_ENTER);
    assert(parse_browser_csi("[27;2;13~") == BROWSE_KEY_SHIFT_ENTER);

    assert(mkdtemp(tmp));
    assert(setenv("XDG_CACHE_HOME", tmp, 1) == 0);
    assert(cache_file_path(page_path, sizeof(page_path), "auto", "https://example.test"));
    assert(media_cache_path("https://example.test/song.oga", ".oga",
                            media_path, sizeof(media_path)));
    fp = fopen(page_path, "wb"); assert(fp); fclose(fp);
    fp = fopen(media_path, "wb"); assert(fp); fclose(fp);
    app.pending_cache_clear = 1;
    handle_page_key(&app, 'n');
    assert(access(page_path, F_OK) == 0);
    assert(access(media_path, F_OK) == 0);
    assert(strstr(app.status, "cancelled"));
    app.pending_cache_clear = 1;
    handle_page_key(&app, 'y');
    assert(strstr(app.status, "2 files"));
    assert(access(page_path, F_OK) != 0);
    assert(access(media_path, F_OK) != 0);
    assert(cache_kind_path(dir, sizeof(dir), "pages")); rmdir(dir);
    assert(cache_kind_path(dir, sizeof(dir), "media")); rmdir(dir);
    snprintf(dir, sizeof(dir), "%s/simplebrowse", tmp); rmdir(dir);
    rmdir(tmp);
    return 0;
}
