/* Render the real player while HTTP range reads deliberately stall seeks. */
#define main simpleplay_program_main
#include "../simpleplay.c"
#undef main

static void refresh(Player *p)
{
    sp_engine_status(p->engine, &p->status);
    sp_engine_info(p->engine, &p->info);
    present_frame(p, 0.04);
}

static cairo_surface_t *snapshot(Player *p)
{
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 320, 180);
    cairo_t *cr = cairo_create(surface);
    draw(p->drawing, cr, p);
    cairo_destroy(cr);
    cairo_surface_flush(surface);
    g_assert_cmpint(cairo_surface_status(surface), ==, CAIRO_STATUS_SUCCESS);
    return surface;
}

static void check_seek(Player *p, double target, gboolean paused)
{
    SpFrame *previous = sp_frame_ref(p->frame);
    cairo_surface_t *before = snapshot(p);
    guint revision = p->status.seek_revision, waiting = 0;
    sp_engine_pause(p->engine, paused);
    sp_engine_seek(p->engine, target);
    gint64 deadline = g_get_monotonic_time() + 6 * G_USEC_PER_SEC;
    while (g_get_monotonic_time() < deadline) {
        refresh(p);
        g_assert_cmpstr(p->status.error, ==, "");
        g_assert_nonnull(p->frame);
        if (p->frame->serial != previous->serial) break;
        if (p->status.seek_revision != revision) waiting++;
        g_assert_true(p->frame == previous);
        cairo_surface_t *during = snapshot(p);
        g_assert_cmpmem(cairo_image_surface_get_data(before), 320 * 180 * 4,
                        cairo_image_surface_get_data(during), 320 * 180 * 4);
        cairo_surface_destroy(during);
        g_usleep(5000);
    }
    g_assert_cmpuint(waiting, >, 25);
    g_assert_cmpuint(p->frame->serial, >, previous->serial);
    g_assert_cmpfloat(p->frame->pts, >=, target - 0.001);
    if (paused) {
        /* A target between frame timestamps must still replace the old frame. */
        g_assert_cmpfloat(p->frame->pts, <, target + 0.05);
        g_assert_cmpfloat(fabs(p->status.position - target), <, 0.001);
    }
    cairo_surface_t *after = snapshot(p);
    g_assert_cmpint(memcmp(cairo_image_surface_get_data(before), cairo_image_surface_get_data(after),
                           320 * 180 * 4), !=, 0);
    cairo_surface_destroy(after);
    cairo_surface_destroy(before);
    sp_frame_unref(previous);
    g_print("OK %s seek to %.2f holds the displayed pixels through delayed network reads, then replaces them\n",
             paused ? "paused" : "playing", target);
}

int main(int argc, char **argv)
{
    gtk_init(&argc, &argv);
    g_assert_cmpint(argc, ==, 4);
    Player *p = &player;
    p->engine = sp_engine_new();
    p->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    p->drawing = gtk_drawing_area_new();
    gtk_widget_set_size_request(p->drawing, 320, 180);
    gtk_container_add(GTK_CONTAINER(p->window), p->drawing);
    gtk_widget_show_all(p->window);
    while (g_main_context_iteration(NULL, FALSE)) {}

    p->info.has_video = p->info.has_audio = TRUE;
    p->status.state = SP_LOADING;
    cairo_surface_t *empty = snapshot(p);
    const guint32 *pixels = (const guint32 *)cairo_image_surface_get_data(empty);
    for (int n = 1; n < 320 * 180; n++) g_assert_cmphex(pixels[n], ==, pixels[0]);
    cairo_surface_destroy(empty);
    g_print("OK video waiting for its first frame has a plain background without audio artwork\n");

    sp_engine_open(p->engine, argv[1], 0, TRUE);
    gint64 deadline = g_get_monotonic_time() + 8 * G_USEC_PER_SEC;
    do {
        refresh(p);
        g_assert_cmpstr(p->status.error, ==, "");
        if (p->frame && p->status.state == SP_PAUSED) break;
        g_usleep(5000);
    } while (g_get_monotonic_time() < deadline);
    g_assert_nonnull(p->frame);
    g_assert_true(p->info.has_video && p->info.has_audio && p->info.seekable);
    g_assert_true(g_file_set_contents(argv[3], "delay range bodies\n", -1, NULL));
    check_seek(p, 18.03, TRUE);
    check_seek(p, 2.03, TRUE);
    check_seek(p, 12.03, FALSE);

    sp_engine_stop(p->engine);
    refresh(p);
    g_assert_null(p->frame);
    g_assert_cmpint(p->status.state, ==, SP_IDLE);
    sp_engine_open(p->engine, argv[2], 0, TRUE);
    deadline = g_get_monotonic_time() + 3 * G_USEC_PER_SEC;
    do {
        refresh(p);
        g_assert_null(p->frame);
        if (p->status.state == SP_PAUSED) break;
        g_usleep(5000);
    } while (g_get_monotonic_time() < deadline);
    g_assert_cmpint(p->status.state, ==, SP_PAUSED);
    g_assert_true(p->info.has_audio);
    g_assert_false(p->info.has_video);
    g_print("OK stop clears the held video frame and audio files retain their audio view\n");

    gtk_widget_destroy(p->window);
    sp_engine_free(p->engine);
    sp_frame_unref(p->frame);
    sp_frame_unref(p->scaled_frame);
    if (p->scaled_picture) cairo_surface_destroy(p->scaled_picture);
    sws_freeContext(p->picture_scale);
    return 0;
}
