#define main simplepdf_program_main
#include "../simplepdf.c"
#undef main

#include <assert.h>

static void write_fixture(const char *path)
{
    FILE *fp = fopen(path, "w");

    assert(fp);
    assert(fputs(
        "\f"
        "                  READING TEST\n"
        "\n"
        "This is a deliberately long paragraph that should be reflowed to the "
        "reader width instead of being clipped at both sides of the terminal.\n"
        "It continues on a second source line so the reader must join it cleanly.\n"
        "\n"
        "42\n"
        "\f"
        "Contents\n"
        "FIRST LINKED ENTRY\n"
        "SECOND LINKED ENTRY\n"
        "Version 1\n"
        "\f"
        "The next physical page remains visible and searchable.\n",
        fp) >= 0);
    assert(fclose(fp) == 0);
}

int main(void)
{
    char path[] = "/tmp/simplepdf-render-check.XXXXXX";
    int fd;
    int breaks = 0;
    int last_break = -1;
    int heading = -1;
    int next_page_text = -1;
    int left, view_w, max_hscroll;

    setlocale(LC_ALL, "");

    ensure_line_capacity(2);

    fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    write_fixture(path);

    epub_mode = 0;
    reading_mode = 1;
    load_reading_text(path, 80);

    assert(wrap_width == 70);
    assert(page_count == 4);
    assert(line_count > 5);
    assert(line_kinds[0] == LINE_PAGE_BREAK);
    assert(page_for_line(0) == 1);
    page_viewport(80, &left, &view_w, &max_hscroll);
    assert(left == 5);
    assert(view_w == 70);
    assert(max_hscroll == 0);

    for (int i = 0; i < line_count; i++) {
        if (line_kinds[i] == LINE_PAGE_BREAK) {
            breaks++;
            last_break = i;
        }
        if (strstr(lines[i], "READING TEST"))
            heading = i;
        if (strstr(lines[i], "next physical page"))
            next_page_text = i;
        if (line_kinds[i] == LINE_BODY)
            assert(visual_len(lines[i]) <= wrap_width);
        assert(strcmp(lines[i], "42") != 0);
    }

    assert(breaks == 3);
    assert(last_break > 0 && last_break + 1 < line_count);
    assert(line_kinds[last_break - 1] == LINE_BLANK);
    assert(line_kinds[last_break + 1] == LINE_BLANK);
    assert(heading >= 0 && line_kinds[heading] == LINE_HEADING);
    assert(next_page_text >= 0 && page_for_line(next_page_text) == 3);
    int first_toc = -1;
    int second_toc = -1;
    for (int i = 0; i < line_count; i++) {
        if (!strcmp(lines[i], "FIRST LINKED ENTRY"))
            first_toc = i;
        if (!strcmp(lines[i], "SECOND LINKED ENTRY"))
            second_toc = i;
    }
    assert(first_toc >= 0 && second_toc == first_toc + 1);
    assert(line_kinds[first_toc] == LINE_TOC);
    assert(line_kinds[second_toc] == LINE_TOC);
    assert(find_next_match("searchable", 0) == next_page_text);
    assert(find_heading_target_after("READING TEST", 0, -1) == heading);

    parse_pdftohtml_link_line(
        "<text top=\"100\"><a href=\"fixture.html#2\">"
        "The next physical page</a></text>\n", 3);
    assert(document_link_count == 1);
    remap_document_links();
    assert(!strcmp(document_links[0].label, "The next physical page"));
    assert(document_links[0].source_page == 3);
    assert(document_links[0].target_page == 1);
    assert(document_links[0].line_idx == next_page_text);
    assert(document_links[0].start_byte == 0);

    top = next_page_text;
    selected_link = 0;
    assert(follow_selected_link());
    assert(page_for_line(top) == 1);
    assert(nav_history_count == 1);
    assert(pop_nav_history());
    assert(top == next_page_text);
    assert(nav_history_count == 0);

    append_extracted_link("1 2 3", 1, 1);
    assert(document_link_count == 4);
    assert(!strcmp(document_links[1].label, "1"));
    assert(!strcmp(document_links[2].label, "2"));
    assert(!strcmp(document_links[3].label, "3"));
    assert(document_links[2].source_offset == 2);
    free_document_links();

    size_t old_capacity = line_capacity;
    ensure_line_capacity(old_capacity + 1);
    assert(line_capacity > old_capacity);
    assert(next_page_text < line_count);
    assert(strstr(lines[next_page_text], "next physical page"));

    reading_mode = 0;
    load_layout_text(path);
    assert(page_count == 4);
    assert(line_kinds[0] == LINE_PAGE_BREAK);
    assert(page_for_line(0) == 1);
    assert(page_width() > 0);
    page_viewport(80, &left, &view_w, &max_hscroll);
    assert(left >= 2);
    assert(view_w <= 76);

    unlink(path);
    free_lines();
    free(lines);
    free(line_kinds);
    free(line_pages);
    lines = NULL;
    line_kinds = NULL;
    line_pages = NULL;
    line_capacity = 0;
    return 0;
}
