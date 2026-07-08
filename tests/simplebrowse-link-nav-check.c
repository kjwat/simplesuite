#define main simplebrowse_main
#include "../simplebrowse.c"
#undef main

#include <stdio.h>

static void set_stop(Page *p, int i, int link, int line)
{
    p->links[i].label = "x";
    p->links[i].url = "https://example.com/";
    p->links[i].marker_offset = (size_t)(line * 10);
    p->stops[i].first_link = link;
    p->stops[i].last_link = link;
    p->stops[i].kind = STOP_LINK;
    p->stops[i].control_index = -1;
    p->stops[i].start = p->links[i].marker_offset;
    p->stops[i].end = p->stops[i].start + 1;
    p->stops[i].start_line = line;
    p->stops[i].end_line = line;
}

int main(void)
{
    App a = {0};
    Link links[16] = {0};
    LinkStop stops[16] = {0};

    a.page.links = links;
    a.page.link_count = 3;
    a.page.stops = stops;
    a.page.stop_count = 3;
    a.page.display_count = 40;
    a.page.layout_width = 80;
    a.top = 10;
    a.selected_link = -1;
    a.selected_control = -1;

    set_stop(&a.page, 0, 0, 5);
    set_stop(&a.page, 1, 1, 12);
    set_stop(&a.page, 2, 2, 18);

    next_link_stop(&a, -1, 10, 80);
    if (a.selected_link != 2 || a.top != 10) {
        fprintf(stderr,
                "reverse visible selection got link=%d top=%d, want link=2 top=10\n",
                a.selected_link, a.top);
        return 1;
    }

    next_link_stop(&a, -1, 10, 80);
    if (a.selected_link != 1 || a.top != 10) {
        fprintf(stderr,
                "reverse locked movement got link=%d top=%d, want link=1 top=10\n",
                a.selected_link, a.top);
        return 1;
    }

    next_link_stop(&a, -1, 10, 80);
    if (a.selected_link != 0 || a.top != 5) {
        fprintf(stderr,
                "reverse offscreen selection got link=%d top=%d, want link=0 top=5\n",
                a.selected_link, a.top);
        return 1;
    }

    memset(&a, 0, sizeof(a));
    memset(links, 0, sizeof(links));
    memset(stops, 0, sizeof(stops));
    a.page.links = links;
    a.page.link_count = 16;
    a.page.stops = stops;
    a.page.stop_count = 16;
    a.page.display_count = 80;
    a.page.layout_width = 80;
    a.top = 19;
    a.selected_link = 15;
    a.selected_control = -1;

    set_stop(&a.page, 0, 0, 16);
    set_stop(&a.page, 1, 1, 19);
    set_stop(&a.page, 2, 2, 20);
    set_stop(&a.page, 3, 3, 21);
    set_stop(&a.page, 4, 4, 21);
    set_stop(&a.page, 5, 5, 22);
    set_stop(&a.page, 6, 6, 23);
    set_stop(&a.page, 7, 7, 26);
    set_stop(&a.page, 8, 8, 26);
    set_stop(&a.page, 9, 9, 28);
    set_stop(&a.page, 10, 10, 28);
    set_stop(&a.page, 11, 11, 28);
    set_stop(&a.page, 12, 12, 29);
    set_stop(&a.page, 13, 13, 29);
    set_stop(&a.page, 14, 14, 29);
    set_stop(&a.page, 15, 15, 29);

    while (a.selected_link > 1)
        next_link_stop(&a, -1, 49, 80);
    if (a.selected_link != 1 || a.top != 19) {
        fprintf(stderr,
                "reverse article movement got link=%d top=%d, want link=1 top=19\n",
                a.selected_link, a.top);
        return 1;
    }

    next_link_stop(&a, -1, 49, 80);
    if (a.selected_link != 0 || a.top != 16) {
        fprintf(stderr,
                "reverse article pull got link=%d top=%d, want link=0 top=16\n",
                a.selected_link, a.top);
        return 1;
    }

    return 0;
}
