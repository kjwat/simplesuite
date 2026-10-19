#define _POSIX_C_SOURCE 200809L
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <glib/gstdio.h>
#include <libswscale/swscale.h>
#include <math.h>
#include <string.h>
#include "simpleplay-engine.h"
#include "simpleplay-state.h"
#include "simpleplay-formats.h"
#include "simpleplay-headless.h"

#define VERSION "0.1.0"
#define QUEUE_LIMIT 4000

typedef struct {
    GtkApplication *application;
    GtkWidget *window, *header, *drawing, *empty, *audio_labels, *audio_title, *audio_artist;
    GtkWidget *sidebar, *list, *queue_label, *queue_button, *controls, *title, *detail;
    GtkWidget *play, *seek, *elapsed, *duration, *volume, *volume_button, *repeat, *shuffle;
    GtkWidget *toast, *toast_label, *tracks_button, *speed_button;
    GtkWidget *error_bar, *error_label;
    SpEngine *engine;
    SpSettings settings;
    SpMediaInfo info;
    SpStatus status;
    SpFrame *frame;
    SpFrame *scaled_frame;
    cairo_surface_t *scaled_picture;
    struct SwsContext *picture_scale;
    GPtrArray *queue;
    GCancellable *import_cancel;
    guint import_generation;
    int current, repeat_mode, aspect;
    gboolean shuffled, fullscreen, dragging, updating, closing, pending_subtitle;
    char *filename, *sidecar, *last_error;
    char *format_filename, *input_format;
    int input_rate, input_channels;
    double last_volume, subtitle_delay, pending_seek;
    gint64 last_activity, last_save, last_frame_tick;
    guint tick, frame_tick, toast_timer, seek_timer, click_timer, inhibit, track_id;
    guint mpris_owner, mpris_root, mpris_player;
    GDBusConnection *bus;
    GDBusNodeInfo *mpris_xml;
} Player;

static Player player;
static void open_files(Player *p, gboolean append, gboolean folder);
static void play_index(Player *p, int index);
static void rebuild_queue(Player *p);
static void next_track(Player *p, gboolean automatic);
static void toggle_fullscreen(Player *p);
static void show_options(Player *p, GtkWidget *relative, const GdkRectangle *point);
static void show_main_menu(Player *p, GtkWidget *relative);
static void add_path(Player *p, const char *path, int depth);
static void mpris_changed(Player *p);
static void request_quit(Player *p);

static const char css[] =
    "window.simpleplay { background: #10161c; color: #e6edf0; }"
    ".simpleplay.background, dialog.simpleplay { background: #17212a; color: #dce8ed; }"
    "popover.simpleplay { background: #1c2730; color: #dce8ed; }"
    ".simpleplay headerbar { background: #171e25; color: #e6edf0; border-bottom: 1px solid #27323a; box-shadow: none; padding: 7px 10px; }"
    ".simpleplay headerbar .title { font-size: 14px; font-weight: 700; }"
    ".simpleplay headerbar .subtitle { font-size: 11px; color: #95a8b2; }"
    ".simpleplay button { background: transparent; color: #bdcbd2; border: 1px solid transparent; box-shadow: none; text-shadow: none; border-radius: 9px; min-height: 28px; min-width: 26px; padding: 5px 7px; }"
    ".simpleplay button:hover { background: #29363f; color: #f4f8fa; }"
    ".simpleplay button:checked { background: #23443f; color: #8ce1cb; }"
    ".simpleplay button:focus { border-color: #659d94; }"
    ".simpleplay button:disabled { color: #4d5d67; }"
    ".simpleplay button.open-button { background: #26383c; color: #9ce8d5; padding-left: 13px; padding-right: 13px; }"
    ".simpleplay button.primary { background: #85ddc8; color: #112c2a; font-weight: 700; padding: 10px 22px; }"
    ".simpleplay button.primary:hover { background: #a3ecd9; }"
    ".simpleplay button.play-button { background: #85ddc8; color: #122d2a; border-radius: 28px; min-width: 42px; min-height: 42px; padding: 6px; }"
    ".simpleplay button.play-button:hover { background: #b0efdf; }"
    ".simpleplay .transport { background: #171f27; border-top: 1px solid #2a353e; padding: 17px 24px 14px; }"
    ".simpleplay .now-title { font-size: 14px; font-weight: 700; color: #e7eef2; }"
    ".simpleplay .muted { color: #8fa4af; font-size: 12px; }"
    ".simpleplay .time { color: #9aafb9; font-size: 11px; font-feature-settings: 'tnum'; }"
    ".simpleplay scale { padding: 8px 0; }"
    ".simpleplay scale trough { min-height: 4px; background: #33414c; border: none; border-radius: 3px; }"
    ".simpleplay scale highlight { background: #85ddc8; border: none; border-radius: 3px; }"
    ".simpleplay scale slider { min-width: 11px; min-height: 11px; margin: -4px; background: #a5ead8; border: none; box-shadow: none; }"
    ".simpleplay .sidebar { background: #171e25; border-left: 1px solid #2a353e; }"
    ".simpleplay .sidebar-head { padding: 15px 12px 10px; }"
    ".simpleplay .section-title { font-size: 13px; font-weight: 700; color: #cddce3; }"
    ".simpleplay list { background: transparent; }"
    ".simpleplay row { padding: 9px 8px; border-radius: 8px; margin: 2px 8px; color: #b6c7d0; }"
    ".simpleplay row:hover { background: #25313a; }"
    ".simpleplay row:selected { background: #243d3d; color: #a4e7d7; }"
    ".simpleplay row .queue-subtitle { color: #8299a6; font-size: 11px; }"
    ".simpleplay .queue-number { color: #83b9ae; font-size: 11px; min-width: 20px; }"
    ".simpleplay .empty-title { font-size: 29px; font-weight: 700; letter-spacing: -0.5px; color: #e4eef1; }"
    ".simpleplay .empty-detail { font-size: 14px; color: #91a8b4; }"
    ".simpleplay .format-hint { font-size: 10px; color: #77909d; letter-spacing: 2px; }"
    ".simpleplay .audio-title { font-size: 25px; font-weight: 700; color: #e7f0f3; }"
    ".simpleplay .audio-artist { font-size: 15px; color: #93b4bd; }"
    ".simpleplay .toast { background: alpha(#263c42, 0.95); color: #d0f1e6; border: 1px solid #42645f; border-radius: 12px; padding: 11px 20px; font-size: 13px; }"
    ".simpleplay infobar { background: #4a2b2b; color: #f4d6cd; border: none; }"
    ".simpleplay popover, .simpleplay dialog { background: #1c2730; color: #dce8ed; }"
    ".simpleplay popover label, .simpleplay dialog label { color: #dce8ed; }"
    ".simpleplay .menu-item { padding: 8px 12px; border-radius: 6px; }"
    ".simpleplay combobox button, .simpleplay spinbutton, .simpleplay entry { background: #263640; color: #e0eaf0; border-color: #3c535e; }"
    ".simpleplay separator { background: #30414b; min-height: 1px; }"
    ".simpleplay treeview.view { background: #17242d; color: #d8e6ec; }"
    ".simpleplay treeview.view:selected { background: #294b48; color: #a8efdc; }";

static void style(GtkWidget *widget, const char *name)
{
    gtk_style_context_add_class(gtk_widget_get_style_context(widget), name);
}

static GtkWidget *label_new(const char *text, const char *class_name)
{
    GtkWidget *label = gtk_label_new(text);
    if (class_name) style(label, class_name);
    return label;
}

static GtkWidget *icon_button(const char *icon, const char *tooltip)
{
    GtkWidget *button = gtk_button_new_from_icon_name(icon, GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(button, tooltip);
    return button;
}

static void set_icon(GtkWidget *button, const char *icon)
{
    gtk_button_set_image(GTK_BUTTON(button), gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_BUTTON));
}

static char *display_basename(const char *filename)
{
    if (!filename) return g_strdup("Nothing playing");
    g_autofree char *base = g_path_get_basename(filename);
    return g_filename_display_name(base);
}

static char *time_text(double seconds)
{
    int value = MAX(0, (int)seconds);
    if (value >= 3600) return g_strdup_printf("%d:%02d:%02d", value / 3600, value / 60 % 60, value % 60);
    return g_strdup_printf("%d:%02d", value / 60, value % 60);
}

static gboolean toast_hide(gpointer opaque)
{
    Player *p = opaque;
    p->toast_timer = 0;
    gtk_revealer_set_reveal_child(GTK_REVEALER(p->toast), FALSE);
    return G_SOURCE_REMOVE;
}

static void toast(Player *p, const char *text)
{
    gtk_label_set_text(GTK_LABEL(p->toast_label), text);
    gtk_revealer_set_reveal_child(GTK_REVEALER(p->toast), TRUE);
    if (p->toast_timer) g_source_remove(p->toast_timer);
    p->toast_timer = g_timeout_add(2400, toast_hide, p);
}

static void save_position(Player *p)
{
    if (!p->engine || !p->filename) return;
    SpStatus status;
    sp_engine_status(p->engine, &status);
    sp_settings_position(&p->settings, p->filename, status.position, status.duration);
}

static void queue_visibility(Player *p, gboolean visible)
{
    p->settings.queue_visible = visible;
    gtk_widget_set_visible(p->sidebar, visible && !p->fullscreen);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(p->queue_button), visible);
}

static void play_index(Player *p, int index)
{
    if (index < 0 || (guint)index >= p->queue->len) return;
    save_position(p);
    p->current = index;
    p->track_id++;
    g_free(p->filename);
    p->filename = g_strdup(g_ptr_array_index(p->queue, index));
    g_clear_pointer(&p->sidecar, g_free);
    g_clear_pointer(&p->last_error, g_free);
    p->subtitle_delay = 0;
    p->pending_subtitle = TRUE;
    double resume = sp_settings_resume(&p->settings, p->filename);
    sp_engine_input_format(p->engine, p->format_filename && !strcmp(p->format_filename, p->filename) ? p->input_format : NULL,
                           p->input_rate ? p->input_rate : 48000, p->input_channels ? p->input_channels : 2);
    sp_engine_open(p->engine, p->filename, resume, FALSE);
    sp_settings_add_recent(&p->settings, p->filename);
    g_autofree char *title = display_basename(p->filename);
    gtk_label_set_text(GTK_LABEL(p->title), title);
    gtk_header_bar_set_subtitle(GTK_HEADER_BAR(p->header), title);
    g_autofree char *window_title = g_strdup_printf("%s — SimplePlay", title);
    gtk_window_set_title(GTK_WINDOW(p->window), window_title);
    gtk_widget_hide(p->error_bar);
    rebuild_queue(p);
    if (resume > 0) {
        g_autofree char *time = time_text(resume);
        g_autofree char *message = g_strdup_printf("Resuming at %s · Home starts over", time);
        toast(p, message);
    }
    mpris_changed(p);
}

static void next_track(Player *p, gboolean automatic)
{
    if (!p->queue->len) return;
    if (automatic && p->repeat_mode == 1) { sp_engine_seek(p->engine, 0); sp_engine_pause(p->engine, FALSE); return; }
    if (automatic && !p->settings.auto_next) return;
    int index = p->current + 1;
    if (p->shuffled && p->queue->len > 1) {
        index = g_random_int_range(0, p->queue->len - 1);
        if (index >= p->current) index++;
    }
    if ((guint)index >= p->queue->len) {
        if (p->repeat_mode == 2 || !automatic) index = 0;
        else return;
    }
    play_index(p, index);
}

static void previous_track(Player *p)
{
    if (p->status.position > 3) sp_engine_seek(p->engine, 0);
    else if (p->queue->len) play_index(p, p->current > 0 ? p->current - 1 : (int)p->queue->len - 1);
}

static void play_pause(Player *p)
{
    if (p->status.state == SP_IDLE || p->status.state == SP_ERROR) {
        if (p->queue->len) play_index(p, MAX(0, p->current));
        else open_files(p, FALSE, FALSE);
    } else if (p->status.state == SP_ENDED) {
        sp_engine_seek(p->engine, 0);
        sp_engine_pause(p->engine, FALSE);
    } else sp_engine_pause(p->engine, p->status.state != SP_PAUSED);
}

static void relative_seek(Player *p, double seconds)
{
    sp_engine_seek(p->engine, p->status.position + seconds);
    g_autofree char *message = g_strdup_printf("%s %.0f seconds", seconds < 0 ? "Back" : "Forward", fabs(seconds));
    toast(p, message);
}

static void volume_change(Player *p, double volume, gboolean announce)
{
    if (!isfinite(volume)) return;
    volume = CLAMP(volume, 0, 1);
    p->settings.volume = volume;
    sp_engine_volume(p->engine, volume);
    p->updating = TRUE;
    gtk_range_set_value(GTK_RANGE(p->volume), volume);
    p->updating = FALSE;
    set_icon(p->volume_button, volume < 0.001 ? "audio-volume-muted-symbolic" :
                              volume < 0.5 ? "audio-volume-low-symbolic" : "audio-volume-high-symbolic");
    if (announce) {
        g_autofree char *message = g_strdup_printf("Volume %.0f%%", volume * 100);
        toast(p, message);
    }
}

static void mute(Player *p)
{
    if (p->settings.volume > 0) { p->last_volume = p->settings.volume; volume_change(p, 0, TRUE); }
    else volume_change(p, p->last_volume > 0 ? p->last_volume : 0.8, TRUE);
}

static void repeat_update(Player *p)
{
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(p->repeat), p->repeat_mode != 0);
    set_icon(p->repeat, p->repeat_mode == 1 ? "media-playlist-repeat-song-symbolic" : "media-playlist-repeat-symbolic");
    const char *text[] = {"Repeat off", "Repeat this file", "Repeat queue"};
    gtk_widget_set_tooltip_text(p->repeat, text[p->repeat_mode]);
    toast(p, text[p->repeat_mode]);
    mpris_changed(p);
}

static gboolean supported_media(const char *path, GCancellable *cancel)
{
    return sp_formats_candidate(path, cancel);
}

static gint natural_compare(gconstpointer a, gconstpointer b)
{
    g_autofree char *da = g_filename_display_name(*(const char *const *)a);
    g_autofree char *db = g_filename_display_name(*(const char *const *)b);
    g_autofree char *ka = g_utf8_collate_key_for_filename(da, -1);
    g_autofree char *kb = g_utf8_collate_key_for_filename(db, -1);
    return strcmp(ka, kb);
}

static gboolean is_playlist(const char *path)
{
    const char *ext = strrchr(path, '.');
    return ext && (!g_ascii_strcasecmp(ext, ".m3u") || !g_ascii_strcasecmp(ext, ".m3u8") || !g_ascii_strcasecmp(ext, ".pls"));
}

static void add_playlist(Player *p, const char *path, int depth)
{
    gchar *contents = NULL;
    gsize size = 0;
    if (!g_file_get_contents(path, &contents, &size, NULL) || size > 4 * 1024 * 1024) { g_free(contents); return; }
    g_auto(GStrv) lines = g_strsplit(contents, "\n", -1);
    g_autofree char *directory = g_path_get_dirname(path);
    const char *extension = strrchr(path, '.');
    gboolean pls = extension && !g_ascii_strcasecmp(extension, ".pls");
    for (int n = 0; lines[n] && p->queue->len < QUEUE_LIMIT; n++) {
        char *line = g_strstrip(lines[n]);
        if (n == 0 && g_str_has_prefix(line, "\357\273\277")) line += 3;
        if (!*line || *line == '#') continue;
        if (pls) { if (!g_str_has_prefix(line, "File") || !strchr(line, '=')) continue; line = strchr(line, '=') + 1; }
        if (g_path_is_absolute(line) || sp_formats_is_uri(line)) add_path(p, line, depth + 1);
        else { g_autofree char *full = g_build_filename(directory, line, NULL); add_path(p, full, depth + 1); }
    }
    g_free(contents);
}

static void add_path(Player *p, const char *path, int depth)
{
    if (!path || !*path || depth > 5 || p->queue->len >= QUEUE_LIMIT ||
        (p->import_cancel && g_cancellable_is_cancelled(p->import_cancel))) return;
    g_autofree char *local = g_str_has_prefix(path, "file://") ? g_filename_from_uri(path, NULL, NULL) : NULL;
    if (local) path = local;
    if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
        GDir *dir = g_dir_open(path, 0, NULL);
        if (!dir) return;
        GPtrArray *children = g_ptr_array_new_with_free_func(g_free);
        const char *name;
        while ((name = g_dir_read_name(dir))) {
            if (*name == '.') continue;
            g_autofree char *full = g_build_filename(path, name, NULL);
            /* Do not follow directory symlinks into cycles or another volume. */
            if (g_file_test(full, G_FILE_TEST_IS_SYMLINK) && g_file_test(full, G_FILE_TEST_IS_DIR)) continue;
            if (g_file_test(full, G_FILE_TEST_IS_DIR) || is_playlist(full) || supported_media(full, p->import_cancel))
                g_ptr_array_add(children, g_strdup(full));
        }
        g_dir_close(dir);
        g_ptr_array_sort(children, natural_compare);
        for (guint n = 0; n < children->len; n++) add_path(p, g_ptr_array_index(children, n), depth + 1);
        g_ptr_array_free(children, TRUE);
    } else if (is_playlist(path) && !sp_formats_is_uri(path)) add_playlist(p, path, depth);
    else {
        g_autofree char *normalized = sp_formats_is_uri(path) ? g_strdup(path) : g_canonicalize_filename(path, NULL);
        g_ptr_array_add(p->queue, g_strdup(normalized));
    }
}

typedef struct {
    char **paths;
    gboolean append;
    guint generation;
} MediaImport;

static void import_free(MediaImport *job)
{
    g_strfreev(job->paths);
    g_free(job);
}

static void import_worker(GTask *task, gpointer source, gpointer task_data, GCancellable *cancel)
{
    (void)source;
    MediaImport *job = task_data;
    Player scan = {0};
    scan.queue = g_ptr_array_new_with_free_func(g_free);
    scan.import_cancel = cancel;
    for (int n = 0; job->paths[n]; n++) add_path(&scan, job->paths[n], 0);
    if (g_task_return_error_if_cancelled(task)) { g_ptr_array_unref(scan.queue); return; }
    g_task_return_pointer(task, scan.queue, (GDestroyNotify)g_ptr_array_unref);
}

static void import_complete(GObject *source, GAsyncResult *result, gpointer opaque)
{
    (void)source;
    Player *p = opaque;
    GTask *task = G_TASK(result);
    MediaImport *job = g_task_get_task_data(task);
    GPtrArray *files = g_task_propagate_pointer(task, NULL);
    if (!files) return;
    if (p->closing || job->generation != p->import_generation) { g_ptr_array_unref(files); return; }
    if (!files->len) { toast(p, "No media files found in this folder"); g_ptr_array_unref(files); return; }
    gboolean start = !job->append || p->current < 0 || p->status.state == SP_IDLE;
    if (!job->append) { save_position(p); p->current = -1; g_ptr_array_set_size(p->queue, 0); }
    guint first = p->queue->len;
    for (guint n = 0; n < files->len && p->queue->len < QUEUE_LIMIT; n++)
        g_ptr_array_add(p->queue, g_strdup(g_ptr_array_index(files, n)));
    g_ptr_array_unref(files);
    rebuild_queue(p);
    if (p->queue->len > first && start) play_index(p, first);
    else if (job->append) toast(p, "Added to queue");
}

static void open_paths(Player *p, char **paths, gboolean append)
{
    /* Even stat() can block on a sleeping NAS. Keep all discovery off GTK. */
    if (!append || !p->import_cancel) {
        if (p->import_cancel) g_cancellable_cancel(p->import_cancel);
        g_clear_object(&p->import_cancel);
        p->import_cancel = g_cancellable_new();
        p->import_generation++;
    }
    MediaImport *job = g_new0(MediaImport, 1);
    job->paths = g_strdupv(paths);
    job->append = append;
    job->generation = p->import_generation;
    GTask *task = g_task_new(NULL, p->import_cancel, import_complete, p);
    g_task_set_task_data(task, job, (GDestroyNotify)import_free);
    g_task_run_in_thread(task, import_worker);
    g_object_unref(task);
}

static void open_files(Player *p, gboolean append, gboolean folder)
{
    GtkFileChooserNative *dialog = gtk_file_chooser_native_new(
        folder ? "Open a media folder" : append ? "Add to queue" : "Open audio or video",
        GTK_WINDOW(p->window), folder ? GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER : GTK_FILE_CHOOSER_ACTION_OPEN,
        folder ? "Open folder" : "Open", "Cancel");
    gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(dialog), TRUE);
    if (p->settings.last_directory) gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog), p->settings.last_directory);
    if (!folder) {
        GtkFileFilter *filter = gtk_file_filter_new();
        gtk_file_filter_set_name(filter, "All files (detect format from contents)");
        gtk_file_filter_add_pattern(filter, "*");
        gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);
        filter = gtk_file_filter_new();
        gtk_file_filter_set_name(filter, "Audio, video, and playlists");
        gtk_file_filter_add_mime_type(filter, "audio/*");
        gtk_file_filter_add_mime_type(filter, "video/*");
        const char *patterns[] = {"*.ape", "*.APE", "*.mkv", "*.MKV", "*.wv", "*.tta", "*.tak", "*.dsf", "*.dff", "*.m3u", "*.m3u8", "*.pls", "*.mka", NULL};
        for (int n = 0; patterns[n]; n++) gtk_file_filter_add_pattern(filter, patterns[n]);
        gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);
        filter = gtk_file_filter_new();
        gtk_file_filter_set_name(filter, "All files");
        gtk_file_filter_add_pattern(filter, "*");
        gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);
    }
    if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        GSList *files = gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(dialog));
        GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);
        for (GSList *node = files; node; node = node->next) g_ptr_array_add(paths, node->data);
        g_slist_free(files);
        g_ptr_array_add(paths, NULL);
        g_free(p->settings.last_directory);
        p->settings.last_directory = gtk_file_chooser_get_current_folder(GTK_FILE_CHOOSER(dialog));
        open_paths(p, (char **)paths->pdata, append);
        g_ptr_array_free(paths, TRUE);
    }
    g_object_unref(dialog);
}

static void subtitle_choose(Player *p)
{
    GtkFileChooserNative *dialog = gtk_file_chooser_native_new("Open subtitles", GTK_WINDOW(p->window),
        GTK_FILE_CHOOSER_ACTION_OPEN, "Open", "Cancel");
    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Subtitles");
    const char *patterns[] = {"*.srt", "*.ass", "*.ssa", "*.vtt", "*.sub", "*.idx", NULL};
    for (int n = 0; patterns[n]; n++) gtk_file_filter_add_pattern(filter, patterns[n]);
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);
    if (p->filename && !sp_formats_is_uri(p->filename)) {
        g_autofree char *dir = g_path_get_dirname(p->filename);
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog), dir);
    }
    if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        g_free(p->sidecar);
        p->sidecar = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        sp_engine_subtitle_file(p->engine, p->sidecar);
        toast(p, "Subtitles loaded");
    }
    g_object_unref(dialog);
}

static void auto_subtitles(Player *p)
{
    if (!p->filename || sp_formats_is_uri(p->filename) || p->status.subtitle_track >= 0 || !p->info.has_video) return;
    g_autofree char *stem = g_strdup(p->filename);
    char *dot = strrchr(stem, '.');
    if (dot) *dot = 0;
    const char *suffixes[] = {".srt", ".ass", ".ssa", ".vtt", NULL};
    for (int n = 0; suffixes[n]; n++) {
        g_autofree char *path = g_strconcat(stem, suffixes[n], NULL);
        if (g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
            g_free(p->sidecar); p->sidecar = g_strdup(path);
            sp_engine_subtitle_file(p->engine, path);
            break;
        }
    }
}

static void queue_remove(GtkButton *button, Player *p)
{
    int index = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "index"));
    gboolean active = index == p->current;
    if (index < p->current) p->current--;
    g_ptr_array_remove_index(p->queue, index);
    if (active) {
        save_position(p);
        sp_engine_stop(p->engine);
        p->current = -1;
        if (p->queue->len) play_index(p, MIN(index, (int)p->queue->len - 1));
        else { g_clear_pointer(&p->filename, g_free); gtk_header_bar_set_subtitle(GTK_HEADER_BAR(p->header), NULL); }
    }
    rebuild_queue(p);
}

static void row_activated(GtkListBox *list, GtkListBoxRow *row, Player *p)
{
    (void)list;
    play_index(p, gtk_list_box_row_get_index(row));
}

static void rebuild_queue(Player *p)
{
    GList *children = gtk_container_get_children(GTK_CONTAINER(p->list));
    for (GList *node = children; node; node = node->next) gtk_widget_destroy(node->data);
    g_list_free(children);
    for (guint n = 0; n < p->queue->len; n++) {
        const char *path = g_ptr_array_index(p->queue, n);
        GtkWidget *row = gtk_list_box_row_new();
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 9);
        gtk_container_add(GTK_CONTAINER(row), box);
        g_autofree char *number = (int)n == p->current ? g_strdup("▶") : g_strdup_printf("%02u", n + 1);
        gtk_box_pack_start(GTK_BOX(box), label_new(number, "queue-number"), FALSE, FALSE, 0);
        GtkWidget *text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        g_autofree char *base = display_basename(path);
        GtkWidget *name = label_new(base, NULL);
        gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_MIDDLE);
        gtk_label_set_xalign(GTK_LABEL(name), 0);
        gtk_label_set_max_width_chars(GTK_LABEL(name), 24);
        gtk_box_pack_start(GTK_BOX(text), name, FALSE, FALSE, 0);
        g_autofree char *dir = g_path_get_dirname(path);
        g_autofree char *parent = display_basename(dir);
        GtkWidget *detail = label_new(parent, "queue-subtitle");
        gtk_label_set_xalign(GTK_LABEL(detail), 0);
        gtk_label_set_ellipsize(GTK_LABEL(detail), PANGO_ELLIPSIZE_MIDDLE);
        gtk_box_pack_start(GTK_BOX(text), detail, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(box), text, TRUE, TRUE, 0);
        GtkWidget *remove = icon_button("window-close-symbolic", "Remove from queue");
        g_object_set_data(G_OBJECT(remove), "index", GINT_TO_POINTER(n));
        g_signal_connect(remove, "clicked", G_CALLBACK(queue_remove), p);
        gtk_box_pack_end(GTK_BOX(box), remove, FALSE, FALSE, 0);
        gtk_widget_set_tooltip_text(row, path);
        gtk_list_box_insert(GTK_LIST_BOX(p->list), row, -1);
    }
    gtk_widget_show_all(p->list);
    if (p->current >= 0)
        gtk_list_box_select_row(GTK_LIST_BOX(p->list), gtk_list_box_get_row_at_index(GTK_LIST_BOX(p->list), p->current));
    g_autofree char *title = g_strdup_printf("Up next · %u", p->queue->len);
    gtk_label_set_text(GTK_LABEL(p->queue_label), title);
    if (p->queue->len >= QUEUE_LIMIT) toast(p, "Queue limit reached (4,000 files)");
}

static void record_draw(cairo_t *cr, double cx, double cy, double radius, gboolean playing)
{
    cairo_pattern_t *halo = cairo_pattern_create_radial(cx, cy, radius * 0.5, cx, cy, radius * 1.9);
    cairo_pattern_add_color_stop_rgba(halo, 0, 0.23, 0.65, 0.59, 0.13);
    cairo_pattern_add_color_stop_rgba(halo, 1, 0.07, 0.13, 0.17, 0);
    cairo_set_source(cr, halo); cairo_paint(cr); cairo_pattern_destroy(halo);
    cairo_set_source_rgb(cr, 0.10, 0.16, 0.19);
    cairo_arc(cr, cx, cy, radius, 0, 2 * G_PI); cairo_fill(cr);
    for (int n = 0; n < 9; n++) {
        cairo_set_source_rgba(cr, 0.45, 0.7, 0.67, n % 2 ? 0.08 : 0.14);
        cairo_set_line_width(cr, 1);
        cairo_arc(cr, cx, cy, radius * (0.43 + n * 0.062), 0, 2 * G_PI); cairo_stroke(cr);
    }
    cairo_pattern_t *center = cairo_pattern_create_linear(cx - radius, cy - radius, cx + radius, cy + radius);
    cairo_pattern_add_color_stop_rgb(center, 0, 0.57, 0.88, 0.78);
    cairo_pattern_add_color_stop_rgb(center, 1, 0.17, 0.49, 0.51);
    cairo_set_source(cr, center);
    cairo_arc(cr, cx, cy, radius * 0.34, 0, 2 * G_PI); cairo_fill(cr); cairo_pattern_destroy(center);
    cairo_set_source_rgb(cr, 0.08, 0.20, 0.23);
    cairo_arc(cr, cx, cy, radius * 0.055, 0, 2 * G_PI); cairo_fill(cr);
    /* A small, stable waveform makes audio mode recognizable without a visualizer. */
    for (int n = 0; n < 17; n++) {
        double h = playing ? 5 + (sin(n * 1.9) + 1) * 9 : 4;
        double x = cx - 48 + n * 6;
        cairo_set_line_width(cr, 2.5);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_set_source_rgba(cr, 0.52, 0.86, 0.77, 0.45 + (n % 3) * 0.1);
        cairo_move_to(cr, x, cy + radius + 34 - h / 2);
        cairo_line_to(cr, x, cy + radius + 34 + h / 2); cairo_stroke(cr);
    }
}

static cairo_surface_t *picture_scaled(Player *p, SpFrame *f, int width, int height)
{
    if (width == f->width && height == f->height)
        return cairo_image_surface_create_for_data(f->pixels, CAIRO_FORMAT_RGB24, f->width, f->height, f->stride);
    if (p->scaled_frame == f && p->scaled_picture &&
        cairo_image_surface_get_width(p->scaled_picture) == width &&
        cairo_image_surface_get_height(p->scaled_picture) == height)
        return cairo_surface_reference(p->scaled_picture);
    if (p->scaled_picture) { cairo_surface_destroy(p->scaled_picture); p->scaled_picture = NULL; }
    sp_frame_unref(p->scaled_frame); p->scaled_frame = NULL;
    p->picture_scale = sws_getCachedContext(p->picture_scale, f->width, f->height, AV_PIX_FMT_RGB32,
        width, height, AV_PIX_FMT_RGB32, SWS_LANCZOS | SWS_ACCURATE_RND, NULL, NULL, NULL);
    if (!p->picture_scale) return NULL;
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, width, height);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) { cairo_surface_destroy(surface); return NULL; }
    const guint8 *source[] = {f->pixels, NULL, NULL, NULL};
    int source_stride[] = {f->stride, 0, 0, 0};
    guint8 *destination[] = {cairo_image_surface_get_data(surface), NULL, NULL, NULL};
    int destination_stride[] = {cairo_image_surface_get_stride(surface), 0, 0, 0};
    sws_scale(p->picture_scale, source, source_stride, 0, f->height, destination, destination_stride);
    cairo_surface_mark_dirty(surface);
    p->scaled_picture = surface;
    p->scaled_frame = sp_frame_ref(f);
    return cairo_surface_reference(surface);
}

static void frame_draw(Player *p, cairo_t *cr, SpFrame *f, double width, double height)
{
    gboolean rotated = f->rotation == 90 || f->rotation == 270;
    double aspect = p->aspect == 1 ? 16.0 / 9 : p->aspect == 2 ? 4.0 / 3 : f->aspect;
    if (rotated) aspect = 1 / aspect;
    double dw = width, dh = width / aspect;
    if ((p->aspect == 3 && dh < height) || (p->aspect != 3 && dh > height)) { dh = height; dw = dh * aspect; }
    double x = (width - dw) / 2, y = (height - dh) / 2;
    cairo_save(cr);
    cairo_rectangle(cr, 0, 0, width, height); cairo_clip(cr);
    cairo_translate(cr, x + dw / 2, y + dh / 2);
    cairo_rotate(cr, f->rotation * G_PI / 180);
    double rw = rotated ? dh : dw, rh = rotated ? dw : dh;
    double sx, sy;
    cairo_surface_get_device_scale(cairo_get_target(cr), &sx, &sy);
    int pw = MAX(1, (int)llround(rw * sx)), ph = MAX(1, (int)llround(rh * sy));
    cairo_surface_t *surface = picture_scaled(p, f, pw, ph);
    if (surface) {
        cairo_scale(cr, rw / pw, rh / ph);
        cairo_set_source_surface(cr, surface, -pw / 2.0, -ph / 2.0);
        /* Lanczos already produced the display pixels, including HiDPI scale. */
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
        cairo_paint(cr);
        cairo_surface_destroy(surface);
    }
    cairo_restore(cr);
    sp_engine_draw_subtitles(p->engine, cr, x, y, dw, dh);
}

static gboolean draw(GtkWidget *widget, cairo_t *cr, Player *p)
{
    double width = gtk_widget_get_allocated_width(widget), height = gtk_widget_get_allocated_height(widget);
    cairo_set_source_rgb(cr, 0.039, 0.059, 0.075); cairo_paint(cr);
    SpFrame *f = sp_frame_ref(p->frame);
    if (f && p->info.has_video) frame_draw(p, cr, f, width, height);
    else {
        cairo_pattern_t *bg = cairo_pattern_create_linear(0, 0, width, height);
        cairo_pattern_add_color_stop_rgb(bg, 0, 0.068, 0.11, 0.14);
        cairo_pattern_add_color_stop_rgb(bg, 1, 0.039, 0.065, 0.09);
        cairo_set_source(cr, bg); cairo_paint(cr); cairo_pattern_destroy(bg);
        if (p->info.has_audio && p->status.state != SP_IDLE) {
            record_draw(cr, width / 2, height * 0.40, MIN(104, height * 0.23), p->status.state == SP_PLAYING);
        } else {
            double cx = width / 2, cy = MAX(56, height / 2 - 115);
            cairo_pattern_t *halo = cairo_pattern_create_radial(cx, cy, 10, cx, cy, 190);
            cairo_pattern_add_color_stop_rgba(halo, 0, 0.25, 0.65, 0.57, 0.13);
            cairo_pattern_add_color_stop_rgba(halo, 1, 0.1, 0.15, 0.2, 0);
            cairo_set_source(cr, halo); cairo_paint(cr); cairo_pattern_destroy(halo);
            cairo_set_source_rgba(cr, 0.5, 0.8, 0.73, 0.15);
            cairo_set_line_width(cr, 1);
            cairo_arc(cr, cx, cy, 45, 0, 2 * G_PI); cairo_stroke(cr);
            cairo_arc(cr, cx, cy, 58, 0, 2 * G_PI); cairo_stroke(cr);
            cairo_set_source_rgb(cr, 0.5, 0.85, 0.75);
            cairo_move_to(cr, cx - 10, cy - 19); cairo_line_to(cr, cx + 22, cy);
            cairo_line_to(cr, cx - 10, cy + 19); cairo_close_path(cr); cairo_fill(cr);
        }
    }
    sp_frame_unref(f);
    return FALSE;
}

static void activity(Player *p)
{
    p->last_activity = g_get_monotonic_time();
    GdkWindow *window = gtk_widget_get_window(p->drawing);
    if (window) gdk_window_set_cursor(window, NULL);
}

static gboolean pointer_motion(GtkWidget *widget, GdkEventMotion *event, Player *p)
{
    (void)widget; (void)event;
    activity(p);
    return FALSE;
}

static gboolean picture_clicked(gpointer opaque)
{
    Player *p = opaque;
    p->click_timer = 0;
    play_pause(p);
    return G_SOURCE_REMOVE;
}

static gboolean pointer_press(GtkWidget *widget, GdkEventButton *event, Player *p)
{
    (void)widget;
    activity(p);
    if (event->button == 1 && event->type == GDK_2BUTTON_PRESS) {
        if (p->click_timer) { g_source_remove(p->click_timer); p->click_timer = 0; }
        toggle_fullscreen(p); return TRUE;
    }
    if (event->button == 3) {
        GdkRectangle point = {(int)event->x, (int)event->y, 1, 1};
        show_options(p, p->drawing, &point);
        return TRUE;
    }
    if (event->button == 1 && event->type == GDK_BUTTON_PRESS && p->status.state != SP_IDLE) {
        if (p->click_timer) g_source_remove(p->click_timer);
        int interval = 250;
        g_object_get(gtk_settings_get_default(), "gtk-double-click-time", &interval, NULL);
        p->click_timer = g_timeout_add(interval, picture_clicked, p);
        return TRUE;
    }
    return FALSE;
}

static gboolean scroll(GtkWidget *widget, GdkEventScroll *event, Player *p)
{
    (void)widget;
    double delta = event->direction == GDK_SCROLL_UP ? 1 : event->direction == GDK_SCROLL_DOWN ? -1 : -event->delta_y;
    volume_change(p, p->settings.volume + delta * 0.05, TRUE);
    activity(p);
    return TRUE;
}

static void toggle_fullscreen(Player *p)
{
    if (p->fullscreen) gtk_window_unfullscreen(GTK_WINDOW(p->window));
    else gtk_window_fullscreen(GTK_WINDOW(p->window));
}

static gboolean window_state(GtkWidget *widget, GdkEventWindowState *event, Player *p)
{
    (void)widget;
    p->fullscreen = (event->new_window_state & GDK_WINDOW_STATE_FULLSCREEN) != 0;
    gtk_widget_set_visible(p->header, !p->fullscreen);
    gtk_widget_set_visible(p->sidebar, !p->fullscreen && p->settings.queue_visible);
    gtk_widget_set_visible(p->controls, !p->fullscreen);
    activity(p);
    return FALSE;
}

static void screenshot(Player *p)
{
    SpFrame *f = sp_engine_frame(p->engine);
    if (!f || !p->info.has_video) { toast(p, "Play a video to save a frame"); sp_frame_unref(f); return; }
    const char *pictures = g_get_user_special_dir(G_USER_DIRECTORY_PICTURES);
    g_autofree char *directory = g_build_filename(pictures ? pictures : g_get_home_dir(), "SimplePlay", NULL);
    if (g_mkdir_with_parents(directory, 0700) < 0) { toast(p, "Could not create the screenshot folder"); sp_frame_unref(f); return; }
    GDateTime *date = g_date_time_new_now_local();
    g_autofree char *stamp = g_date_time_format(date, "%Y-%m-%d_%H-%M-%S-%f");
    g_date_time_unref(date);
    g_autofree char *name = g_strdup_printf("SimplePlay-%s.png", stamp);
    g_autofree char *path = g_build_filename(directory, name, NULL);
    int w = f->rotation == 90 || f->rotation == 270 ? f->height : f->width;
    int h = f->rotation == 90 || f->rotation == 270 ? f->width : f->height;
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
    cairo_t *cr = cairo_create(surface);
    frame_draw(p, cr, f, w, h);
    cairo_status_t result = cairo_surface_write_to_png(surface, path);
    cairo_destroy(cr); cairo_surface_destroy(surface); sp_frame_unref(f);
    if (result == CAIRO_STATUS_SUCCESS) {
        g_autofree char *message = g_strdup_printf("Frame saved to %s", directory); toast(p, message);
    } else toast(p, cairo_status_to_string(result));
}

static GtkWidget *dialog_box(Player *p, const char *title)
{
    GtkWidget *dialog = gtk_dialog_new_with_buttons(title, GTK_WINDOW(p->window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, "Close", GTK_RESPONSE_CLOSE, NULL);
    style(dialog, "simpleplay");
    gtk_window_set_default_size(GTK_WINDOW(dialog), 470, -1);
    GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(area), 22);
    gtk_box_set_spacing(GTK_BOX(area), 14);
    return dialog;
}

static void information(Player *p)
{
    GtkWidget *dialog = dialog_box(p, "Media information");
    GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    g_autofree char *name = display_basename(p->filename);
    GtkWidget *title = label_new(*p->info.title ? p->info.title : name, "now-title");
    gtk_label_set_line_wrap(GTK_LABEL(title), TRUE);
    gtk_box_pack_start(GTK_BOX(area), title, FALSE, FALSE, 0);
    g_autofree char *duration = time_text(p->info.duration);
    g_autofree char *text = g_strdup_printf("%s\n\n%s\nDuration: %s\n%s%s%s%s%s%s\n%s",
        p->info.format, p->info.description, duration,
        *p->info.artist ? "Artist: " : "", p->info.artist, *p->info.artist ? "\n" : "",
        *p->info.album ? "Album: " : "", p->info.album, *p->info.album ? "\n" : "",
        p->filename ? p->filename : "Open a file to see its information.");
    GtkWidget *label = label_new(text, NULL);
    gtk_label_set_selectable(GTK_LABEL(label), TRUE);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 64);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_box_pack_start(GTK_BOX(area), label, FALSE, FALSE, 0);
    gtk_widget_show_all(dialog);
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

static void preferences(Player *p)
{
    GtkWidget *dialog = dialog_box(p, "Preferences");
    GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *remember = gtk_check_button_new_with_label("Remember positions in films and long recordings");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(remember), p->settings.remember);
    GtkWidget *next = gtk_check_button_new_with_label("Continue to the next file in the queue");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(next), p->settings.auto_next);
    gtk_box_pack_start(GTK_BOX(area), remember, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(area), next, FALSE, FALSE, 0);
    GtkWidget *note = label_new("Positions stay on this computer. Short songs start from the beginning.", "muted");
    gtk_label_set_line_wrap(GTK_LABEL(note), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(note), 55);
    gtk_label_set_xalign(GTK_LABEL(note), 0);
    gtk_box_pack_start(GTK_BOX(area), note, FALSE, FALSE, 0);
    gtk_widget_show_all(dialog);
    gtk_dialog_run(GTK_DIALOG(dialog));
    p->settings.remember = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(remember));
    p->settings.auto_next = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(next));
    if (!p->settings.remember) g_key_file_remove_group(p->settings.file, "positions", NULL);
    sp_settings_save(&p->settings);
    gtk_widget_destroy(dialog);
}

static void shortcuts(Player *p)
{
    GtkWidget *dialog = dialog_box(p, "Keyboard shortcuts");
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 35);
    const char *items[][2] = {
        {"Space / K", "Play or pause"}, {"← / →", "Seek 10 seconds"}, {"Shift + ← / →", "Seek 1 minute"},
        {"Home", "Start over"}, {"↑ / ↓", "Volume"}, {"M", "Mute"}, {"F / F11", "Fullscreen"},
        {"Escape", "Leave fullscreen"}, {"N / P", "Next / previous file"}, {"S / R", "Shuffle / repeat"},
        {"B / V", "Cycle audio / subtitle tracks"}, {"[ / ]", "Subtitle delay − / + 0.5 s"},
        {"Ctrl + O", "Open files"}, {"Ctrl + Shift + O", "Open folder"}, {"Ctrl + L", "Open a stream URL"},
        {"Ctrl + E", "Show queue"}, {"Ctrl + I", "Media information"}, {"Ctrl + S", "Save video frame"}, {"Ctrl + Q", "Quit"}
    };
    for (guint n = 0; n < G_N_ELEMENTS(items); n++) {
        GtkWidget *keys = label_new(items[n][0], "muted");
        GtkWidget *text = label_new(items[n][1], NULL);
        gtk_label_set_xalign(GTK_LABEL(keys), 0); gtk_label_set_xalign(GTK_LABEL(text), 0);
        gtk_grid_attach(GTK_GRID(grid), keys, 0, n, 1, 1); gtk_grid_attach(GTK_GRID(grid), text, 1, n, 1, 1);
    }
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), grid, TRUE, TRUE, 0);
    gtk_widget_show_all(dialog); gtk_dialog_run(GTK_DIALOG(dialog)); gtk_widget_destroy(dialog);
}

static void about(Player *p)
{
    gtk_show_about_dialog(GTK_WINDOW(p->window), "program-name", "SimplePlay", "version", VERSION,
        "comments", "A quiet place for films and music.\nNative GTK · Direct FFmpeg decoding",
        "website", "https://github.com/kjwat/simplesuite", "license-type", GTK_LICENSE_GPL_3_0,
        "logo-icon-name", "multimedia-player", NULL);
}

static gboolean format_visible(GtkTreeModel *model, GtkTreeIter *iter, gpointer opaque)
{
    GtkEntry *search = opaque;
    const char *text = gtk_entry_get_text(search);
    if (!*text) return TRUE;
    char *kind, *name, *description, *extensions;
    gtk_tree_model_get(model, iter, 0, &kind, 1, &name, 2, &description, 3, &extensions, -1);
    g_autofree char *joined = g_strjoin(" ", kind, name, description, extensions, NULL);
    g_autofree char *folded = g_utf8_casefold(joined, -1);
    g_autofree char *query = g_utf8_casefold(text, -1);
    gboolean visible = strstr(folded, query) != NULL;
    g_free(kind); g_free(name); g_free(description); g_free(extensions);
    return visible;
}

static void format_search(GtkSearchEntry *entry, GtkTreeModelFilter *filter)
{
    (void)entry;
    gtk_tree_model_filter_refilter(filter);
}

static void format_support(Player *p)
{
    GtkWidget *dialog = dialog_box(p, "Format support");
    gtk_window_set_default_size(GTK_WINDOW(dialog), 850, 600);
    GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gboolean complete;
    g_autofree char *report = sp_formats_report(FALSE, &complete);
    GtkWidget *summary = label_new(report, "muted");
    gtk_label_set_xalign(GTK_LABEL(summary), 0);
    gtk_label_set_line_wrap(GTK_LABEL(summary), TRUE);
    gtk_box_pack_start(GTK_BOX(area), summary, FALSE, FALSE, 0);
    GtkWidget *search = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(search), "Search codecs, containers, and extensions…");
    gtk_box_pack_start(GTK_BOX(area), search, FALSE, FALSE, 0);
    GtkListStore *store = gtk_list_store_new(4, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    GPtrArray *formats = sp_formats_list();
    for (guint n = 0; n < formats->len; n++) {
        SpFormat *f = g_ptr_array_index(formats, n);
        GtkTreeIter iter;
        gtk_list_store_append(store, &iter);
        gtk_list_store_set(store, &iter, 0, f->kind, 1, f->name, 2, f->description, 3, f->extensions, -1);
    }
    g_ptr_array_unref(formats);
    GtkTreeModelFilter *filter = GTK_TREE_MODEL_FILTER(gtk_tree_model_filter_new(GTK_TREE_MODEL(store), NULL));
    gtk_tree_model_filter_set_visible_func(filter, format_visible, search, NULL);
    g_signal_connect(search, "search-changed", G_CALLBACK(format_search), filter);
    GtkWidget *view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(filter));
    const char *columns[] = {"Type", "Decoder / demuxer", "Description", "Extensions"};
    for (int n = 0; n < 4; n++) {
        GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
        GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes(columns[n], renderer, "text", n, NULL);
        gtk_tree_view_column_set_resizable(column, TRUE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(view), column);
    }
    GtkWidget *scroller = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(scroller), view);
    gtk_box_pack_start(GTK_BOX(area), scroller, TRUE, TRUE, 0);
    g_object_unref(store); g_object_unref(filter);
    gtk_widget_show_all(dialog); gtk_dialog_run(GTK_DIALOG(dialog)); gtk_widget_destroy(dialog);
}

static void format_browse(GtkButton *button, GtkEntry *entry)
{
    GtkWindow *parent = GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(button)));
    GtkFileChooserNative *chooser = gtk_file_chooser_native_new("Choose media", parent, GTK_FILE_CHOOSER_ACTION_OPEN, "Choose", "Cancel");
    if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT) {
        g_autofree char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
        gtk_entry_set_text(entry, filename);
    }
    g_object_unref(chooser);
}

static void open_with_format(Player *p)
{
    GtkWidget *dialog = gtk_dialog_new_with_buttons("Open with an input format", GTK_WINDOW(p->window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, "Cancel", GTK_RESPONSE_CANCEL, "Open", GTK_RESPONSE_ACCEPT, NULL);
    style(dialog, "simpleplay");
    GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(area), 22); gtk_box_set_spacing(GTK_BOX(area), 12);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *filename = gtk_entry_new(); gtk_entry_set_width_chars(GTK_ENTRY(filename), 48);
    gtk_entry_set_placeholder_text(GTK_ENTRY(filename), "File path or stream URL");
    GtkWidget *browse = gtk_button_new_with_label("Browse…");
    g_signal_connect(browse, "clicked", G_CALLBACK(format_browse), filename);
    gtk_box_pack_start(GTK_BOX(row), filename, TRUE, TRUE, 0); gtk_box_pack_end(GTK_BOX(row), browse, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(area), row, FALSE, FALSE, 0);
    GtkWidget *format = gtk_combo_box_text_new_with_entry();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(format), "", "Auto detect");
    GPtrArray *formats = sp_formats_list();
    for (guint n = 0; n < formats->len; n++) {
        SpFormat *f = g_ptr_array_index(formats, n);
        if (strcmp(f->kind, "Container")) continue;
        g_auto(GStrv) aliases = g_strsplit(f->name, ",", 2);
        g_autofree char *label = g_strdup_printf("%s · %s", aliases[0], f->description);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(format), aliases[0], label);
    }
    g_ptr_array_unref(formats); gtk_combo_box_set_active(GTK_COMBO_BOX(format), 0);
    gtk_box_pack_start(GTK_BOX(area), label_new("Input format (choose or type a demuxer name)", "muted"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(area), format, FALSE, FALSE, 0);
    GtkWidget *grid = gtk_grid_new(); gtk_grid_set_column_spacing(GTK_GRID(grid), 14); gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    GtkWidget *rate = gtk_spin_button_new_with_range(1000, 768000, 1000);
    GtkWidget *channels = gtk_spin_button_new_with_range(1, 32, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(rate), 48000); gtk_spin_button_set_value(GTK_SPIN_BUTTON(channels), 2);
    gtk_grid_attach(GTK_GRID(grid), label_new("Sample rate (raw audio)", "muted"), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), rate, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), label_new("Channels (raw audio)", "muted"), 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), channels, 1, 1, 1, 1);
    gtk_box_pack_start(GTK_BOX(area), grid, FALSE, FALSE, 0);
    GtkWidget *hint = label_new("Useful for headerless PCM, raw streams, or containers that need an explicit format. Most files open automatically.", "muted");
    gtk_label_set_line_wrap(GTK_LABEL(hint), TRUE); gtk_label_set_max_width_chars(GTK_LABEL(hint), 65);
    gtk_box_pack_start(GTK_BOX(area), hint, FALSE, FALSE, 0);
    gtk_widget_show_all(dialog);
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT && *gtk_entry_get_text(GTK_ENTRY(filename))) {
        const char *path = gtk_entry_get_text(GTK_ENTRY(filename));
        g_free(p->format_filename); p->format_filename = sp_formats_is_uri(path) ? g_strdup(path) : g_canonicalize_filename(path, NULL);
        const char *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(format));
        if (!id) id = gtk_entry_get_text(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(format))));
        g_free(p->input_format); p->input_format = g_strdup(id);
        p->input_rate = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(rate));
        p->input_channels = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(channels));
        char *paths[] = {p->format_filename, NULL}; open_paths(p, paths, FALSE);
    }
    gtk_widget_destroy(dialog);
}

static void location(Player *p)
{
    GtkWidget *dialog = gtk_dialog_new_with_buttons("Open a stream", GTK_WINDOW(p->window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, "Cancel", GTK_RESPONSE_CANCEL, "Open", GTK_RESPONSE_ACCEPT, NULL);
    style(dialog, "simpleplay");
    GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(area), 20);
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "https://…");
    gtk_entry_set_width_chars(GTK_ENTRY(entry), 55);
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    gtk_box_pack_start(GTK_BOX(area), entry, TRUE, TRUE, 0);
    gtk_widget_show_all(dialog);
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        const char *url = gtk_entry_get_text(GTK_ENTRY(entry));
        if (*url) { char *paths[] = {(char *)url, NULL}; open_paths(p, paths, FALSE); }
    }
    gtk_widget_destroy(dialog);
}

static void export_queue(Player *p)
{
    GtkFileChooserNative *dialog = gtk_file_chooser_native_new("Save queue", GTK_WINDOW(p->window),
        GTK_FILE_CHOOSER_ACTION_SAVE, "Save", "Cancel");
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), "SimplePlay.m3u8");
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dialog), TRUE);
    if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        g_autofree char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        GString *text = g_string_new("#EXTM3U\n");
        for (guint n = 0; n < p->queue->len; n++) {
            const char *file = g_ptr_array_index(p->queue, n);
            g_autofree char *uri = sp_formats_is_uri(file) ? g_strdup(file) : g_filename_to_uri(file, NULL, NULL);
            if (uri) g_string_append_printf(text, "%s\n", uri);
        }
        GError *error = NULL;
        if (!g_file_set_contents(path, text->str, text->len, &error)) { toast(p, error->message); g_error_free(error); }
        else toast(p, "Queue saved");
        g_string_free(text, TRUE);
    }
    g_object_unref(dialog);
}

static void cancel_imports(Player *p)
{
    if (p->import_cancel) g_cancellable_cancel(p->import_cancel);
    g_clear_object(&p->import_cancel);
    p->import_generation++;
}

static void action_run(Player *p, const char *action)
{
    if (!strcmp(action, "open")) open_files(p, FALSE, FALSE);
    else if (!strcmp(action, "folder")) open_files(p, FALSE, TRUE);
    else if (!strcmp(action, "add")) open_files(p, TRUE, FALSE);
    else if (!strcmp(action, "location")) location(p);
    else if (!strcmp(action, "info")) information(p);
    else if (!strcmp(action, "screenshot")) screenshot(p);
    else if (!strcmp(action, "preferences")) preferences(p);
    else if (!strcmp(action, "shortcuts")) shortcuts(p);
    else if (!strcmp(action, "about")) about(p);
    else if (!strcmp(action, "formats")) format_support(p);
    else if (!strcmp(action, "open-format")) open_with_format(p);
    else if (!strcmp(action, "export")) export_queue(p);
    else if (!strcmp(action, "clear")) {
        cancel_imports(p);
        save_position(p); sp_engine_stop(p->engine); g_ptr_array_set_size(p->queue, 0); p->current = -1;
        g_clear_pointer(&p->filename, g_free); rebuild_queue(p);
        gtk_header_bar_set_subtitle(GTK_HEADER_BAR(p->header), NULL);
        gtk_window_set_title(GTK_WINDOW(p->window), "SimplePlay");
    } else if (!strcmp(action, "quit")) request_quit(p);
    else if (g_str_has_prefix(action, "recent:")) { char *paths[] = {(char *)action + 7, NULL}; open_paths(p, paths, FALSE); }
}

static void menu_clicked(GtkButton *button, Player *p)
{
    char *action = g_strdup(g_object_get_data(G_OBJECT(button), "action"));
    GtkWidget *popover = gtk_widget_get_ancestor(GTK_WIDGET(button), GTK_TYPE_POPOVER);
    if (popover) gtk_popover_popdown(GTK_POPOVER(popover));
    action_run(p, action);
    g_free(action);
}

static void menu_item(GtkWidget *box, Player *p, const char *title, const char *action)
{
    GtkWidget *button = gtk_button_new_with_label(title);
    gtk_widget_set_halign(gtk_bin_get_child(GTK_BIN(button)), GTK_ALIGN_START);
    style(button, "menu-item");
    g_object_set_data_full(G_OBJECT(button), "action", g_strdup(action), g_free);
    g_signal_connect(button, "clicked", G_CALLBACK(menu_clicked), p);
    gtk_box_pack_start(GTK_BOX(box), button, FALSE, FALSE, 0);
}

static gboolean popover_press(GtkWidget *widget, GdkEventButton *event, Player *p)
{
    if (event->button != 3 || event->type != GDK_BUTTON_PRESS) return FALSE;
    GtkWidget *target = gtk_get_event_widget((GdkEvent *)event);
    if (!target || target == widget || gtk_widget_is_ancestor(target, widget)) return FALSE;
    GtkWidget *relative = gtk_bin_get_child(GTK_BIN(p->window));
    GdkRectangle point = {0, 0, 1, 1};
    if (!gtk_widget_translate_coordinates(target, relative, (int)event->x, (int)event->y,
                                         &point.x, &point.y)) return FALSE;
    activity(p);
    /* The modal popover receives outside presses before the picture does. */
    gtk_popover_popdown(GTK_POPOVER(widget));
    show_options(p, relative, &point);
    return TRUE;
}

static GtkWidget *popover_box(Player *p, GtkWidget *relative)
{
    GtkWidget *popover = gtk_popover_new(relative);
    style(popover, "simpleplay");
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll),
        MAX(160, gtk_widget_get_allocated_height(p->window) - 64));
    gtk_container_add(GTK_CONTAINER(popover), scroll);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    gtk_container_add(GTK_CONTAINER(scroll), box);
    g_signal_connect(popover, "button-press-event", G_CALLBACK(popover_press), p);
    g_signal_connect_swapped(popover, "closed", G_CALLBACK(gtk_widget_destroy), popover);
    return box;
}

static void popover_show(GtkWidget *box)
{
    GtkWidget *popover = gtk_widget_get_ancestor(box, GTK_TYPE_POPOVER);
    gtk_widget_show_all(popover);
    gtk_popover_popup(GTK_POPOVER(popover));
}

static void show_main_menu(Player *p, GtkWidget *relative)
{
    GtkWidget *box = popover_box(p, relative);
    menu_item(box, p, "Open files…                       Ctrl+O", "open");
    menu_item(box, p, "Open folder…               Ctrl+Shift+O", "folder");
    menu_item(box, p, "Open stream…                   Ctrl+L", "location");
    menu_item(box, p, "Open with format…", "open-format");
    g_auto(GStrv) recent = sp_settings_recent(&p->settings);
    if (recent[0]) {
        gtk_box_pack_start(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 6);
        GtkWidget *caption = label_new("RECENT", "muted");
        gtk_label_set_xalign(GTK_LABEL(caption), 0); gtk_widget_set_margin_start(caption, 12);
        gtk_box_pack_start(GTK_BOX(box), caption, FALSE, FALSE, 3);
        for (int n = 0; recent[n] && n < 4; n++) {
            g_autofree char *name = display_basename(recent[n]);
            g_autofree char *short_name = g_utf8_strlen(name, -1) > 38 ? g_utf8_substring(name, 0, 38) : g_strdup(name);
            g_autofree char *action = g_strconcat("recent:", recent[n], NULL);
            menu_item(box, p, short_name, action);
        }
    }
    gtk_box_pack_start(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 6);
    menu_item(box, p, "Media information", "info");
    menu_item(box, p, "Format support", "formats");
    menu_item(box, p, "Save video frame", "screenshot");
    menu_item(box, p, "Preferences", "preferences");
    menu_item(box, p, "Keyboard shortcuts", "shortcuts");
    menu_item(box, p, "About SimplePlay", "about");
    popover_show(box);
}

static void track_changed(GtkComboBox *combo, Player *p)
{
    const char *id = gtk_combo_box_get_active_id(combo);
    if (!id) return;
    gboolean audio = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(combo), "audio"));
    int index = (int)g_ascii_strtoll(id, NULL, 10);
    if (audio) sp_engine_audio(p->engine, index);
    else if (index == -3 && p->sidecar) sp_engine_subtitle_file(p->engine, p->sidecar);
    else sp_engine_subtitle(p->engine, index);
}

static void rate_changed(GtkComboBox *combo, Player *p)
{
    const char *id = gtk_combo_box_get_active_id(combo);
    if (id) sp_engine_rate(p->engine, g_ascii_strtod(id, NULL));
}

static void chapter_changed(GtkComboBox *combo, Player *p)
{
    int index = gtk_combo_box_get_active(combo);
    if (index >= 0 && index < p->info.n_chapters) sp_engine_seek(p->engine, p->info.chapters[index].position);
}

static void delay_changed(GtkSpinButton *spin, Player *p)
{
    p->subtitle_delay = gtk_spin_button_get_value(spin);
    sp_engine_subtitle_delay(p->engine, p->subtitle_delay);
    gtk_widget_queue_draw(p->drawing);
}

static void aspect_changed(GtkComboBox *combo, Player *p)
{
    p->aspect = gtk_combo_box_get_active(combo);
    gtk_widget_queue_draw(p->drawing);
}

static void deinterlace_changed(GtkComboBox *combo, Player *p)
{
    p->settings.deinterlace = gtk_combo_box_get_active(combo);
    sp_engine_deinterlace(p->engine, p->settings.deinterlace, p->settings.deinterlace_double_rate);
    sp_settings_save(&p->settings);
}

static void deinterlace_mode_changed(GtkComboBox *combo, Player *p)
{
    p->settings.deinterlace_double_rate = gtk_combo_box_get_active(combo) == 1;
    sp_engine_deinterlace(p->engine, p->settings.deinterlace, p->settings.deinterlace_double_rate);
    sp_settings_save(&p->settings);
}

static void subtitle_clicked(GtkButton *button, Player *p)
{
    GtkWidget *popover = gtk_widget_get_ancestor(GTK_WIDGET(button), GTK_TYPE_POPOVER);
    if (popover) gtk_popover_popdown(GTK_POPOVER(popover));
    subtitle_choose(p);
}

static void option_label(GtkWidget *box, const char *title)
{
    GtkWidget *label = label_new(title, "section-title");
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_widget_set_margin_top(label, 9); gtk_widget_set_margin_bottom(label, 3);
    gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);
}

static void show_options(Player *p, GtkWidget *relative, const GdkRectangle *point)
{
    GtkWidget *box = popover_box(p, relative);
    if (point) gtk_popover_set_pointing_to(GTK_POPOVER(gtk_widget_get_ancestor(box, GTK_TYPE_POPOVER)), point);
    gtk_widget_set_size_request(box, 330, -1);
    for (int type = SP_AUDIO; type <= SP_SUBTITLE; type++) {
        option_label(box, type == SP_AUDIO ? "Audio track" : "Subtitles");
        GtkWidget *combo = gtk_combo_box_text_new();
        if (type == SP_SUBTITLE) gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), "-1", "Off");
        for (int n = 0; n < p->info.n_tracks; n++) {
            SpTrack *track = &p->info.tracks[n];
            if ((int)track->type != type) continue;
            g_autofree char *id = g_strdup_printf("%d", track->index);
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), id, track->label);
        }
        if (type == SP_SUBTITLE && p->sidecar) {
            g_autofree char *name = display_basename(p->sidecar);
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), "-3", name);
        }
        g_autofree char *active = g_strdup_printf("%d", type == SP_AUDIO ? p->status.audio_track : p->status.subtitle_track);
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo), active);
        g_object_set_data(G_OBJECT(combo), "audio", GINT_TO_POINTER(type == SP_AUDIO));
        g_signal_connect(combo, "changed", G_CALLBACK(track_changed), p);
        gtk_widget_set_sensitive(combo, p->info.seekable);
        gtk_box_pack_start(GTK_BOX(box), combo, FALSE, FALSE, 0);
    }
    GtkWidget *load = gtk_button_new_with_label("Load subtitle file…");
    g_signal_connect(load, "clicked", G_CALLBACK(subtitle_clicked), p);
    gtk_widget_set_sensitive(load, p->filename != NULL);
    gtk_box_pack_start(GTK_BOX(box), load, FALSE, FALSE, 0);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_pack_start(GTK_BOX(row), label_new("Subtitle delay (seconds)", "muted"), TRUE, TRUE, 0);
    GtkWidget *spin = gtk_spin_button_new_with_range(-60, 60, 0.5);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(spin), 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), p->subtitle_delay);
    g_signal_connect(spin, "value-changed", G_CALLBACK(delay_changed), p);
    gtk_box_pack_end(GTK_BOX(row), spin, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 5);
    option_label(box, "Playback speed");
    GtkWidget *rate = gtk_combo_box_text_new();
    const char *rates[] = {"0.5", "0.75", "1", "1.25", "1.5", "2"};
    for (guint n = 0; n < G_N_ELEMENTS(rates); n++) {
        g_autofree char *name = g_strconcat(rates[n], "×", NULL);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(rate), rates[n], name);
        if (fabs(g_ascii_strtod(rates[n], NULL) - p->status.rate) < 0.01) gtk_combo_box_set_active(GTK_COMBO_BOX(rate), n);
    }
    g_signal_connect(rate, "changed", G_CALLBACK(rate_changed), p);
    gtk_widget_set_sensitive(rate, p->info.seekable);
    gtk_box_pack_start(GTK_BOX(box), rate, FALSE, FALSE, 0);
    if (p->info.has_video) {
        option_label(box, "Deinterlace");
        GtkWidget *deinterlace = gtk_combo_box_text_new();
        const char *states[] = {"Off", "Automatic", "On"};
        for (guint n = 0; n < G_N_ELEMENTS(states); n++) gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(deinterlace), states[n]);
        gtk_combo_box_set_active(GTK_COMBO_BOX(deinterlace), p->settings.deinterlace);
        gtk_widget_set_tooltip_text(deinterlace, "Automatic deinterlaces frames marked as interlaced; On forces deinterlacing");
        g_signal_connect(deinterlace, "changed", G_CALLBACK(deinterlace_changed), p);
        gtk_box_pack_start(GTK_BOX(box), deinterlace, FALSE, FALSE, 0);
        option_label(box, "Deinterlace mode");
        GtkWidget *mode = gtk_combo_box_text_new();
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(mode), "Yadif");
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(mode), "Yadif (2×)");
        gtk_combo_box_set_active(GTK_COMBO_BOX(mode), p->settings.deinterlace_double_rate ? 1 : 0);
        gtk_widget_set_tooltip_text(mode, "Yadif (2×) displays each field for smoother motion in interlaced video");
        g_signal_connect(mode, "changed", G_CALLBACK(deinterlace_mode_changed), p);
        gtk_box_pack_start(GTK_BOX(box), mode, FALSE, FALSE, 0);
        option_label(box, "Picture");
        GtkWidget *aspect = gtk_combo_box_text_new();
        const char *aspects[] = {"Original proportions", "16:9", "4:3", "Fill window"};
        for (guint n = 0; n < G_N_ELEMENTS(aspects); n++) gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(aspect), aspects[n]);
        gtk_combo_box_set_active(GTK_COMBO_BOX(aspect), p->aspect);
        g_signal_connect(aspect, "changed", G_CALLBACK(aspect_changed), p);
        gtk_box_pack_start(GTK_BOX(box), aspect, FALSE, FALSE, 0);
    }
    if (p->info.n_chapters) {
        option_label(box, "Chapters");
        GtkWidget *chapters = gtk_combo_box_text_new();
        int active = 0;
        for (int n = 0; n < p->info.n_chapters; n++) {
            g_autofree char *time = time_text(p->info.chapters[n].position);
            g_autofree char *title = g_strdup_printf("%s  %s", time, p->info.chapters[n].title);
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(chapters), title);
            if (p->info.chapters[n].position <= p->status.position) active = n;
        }
        gtk_combo_box_set_active(GTK_COMBO_BOX(chapters), active);
        g_signal_connect(chapters, "changed", G_CALLBACK(chapter_changed), p);
        gtk_box_pack_start(GTK_BOX(box), chapters, FALSE, FALSE, 0);
    }
    popover_show(box);
}

static void cycle_track(Player *p, SpTrackType type)
{
    int current = type == SP_AUDIO ? p->status.audio_track : p->status.subtitle_track;
    int first = -1, next = -1;
    gboolean found = FALSE;
    for (int n = 0; n < p->info.n_tracks; n++) {
        SpTrack *t = &p->info.tracks[n];
        if (t->type != type) continue;
        if (first < 0) first = t->index;
        if (found) { next = t->index; break; }
        if (t->index == current) found = TRUE;
    }
    if (next < 0) next = type == SP_SUBTITLE && current >= 0 ? -1 : first;
    if (type == SP_AUDIO && next >= 0) sp_engine_audio(p->engine, next);
    else if (type == SP_SUBTITLE) sp_engine_subtitle(p->engine, next);
    toast(p, type == SP_AUDIO ? "Audio track changed" : next < 0 ? "Subtitles off" : "Subtitle track changed");
}

static gboolean key_press(GtkWidget *widget, GdkEventKey *event, Player *p)
{
    (void)widget;
    guint key = gdk_keyval_to_lower(event->keyval);
    gboolean ctrl = (event->state & GDK_CONTROL_MASK) != 0;
    gboolean shift = (event->state & GDK_SHIFT_MASK) != 0;
    activity(p);
    if (ctrl) {
        switch (key) {
        case GDK_KEY_o: open_files(p, FALSE, shift); return TRUE;
        case GDK_KEY_l: location(p); return TRUE;
        case GDK_KEY_e: queue_visibility(p, !p->settings.queue_visible); return TRUE;
        case GDK_KEY_i: if (shift) format_support(p); else information(p); return TRUE;
        case GDK_KEY_s: screenshot(p); return TRUE;
        case GDK_KEY_q: request_quit(p); return TRUE;
        default: return FALSE;
        }
    }
    switch (key) {
    case GDK_KEY_space: case GDK_KEY_k: case GDK_KEY_AudioPlay: case GDK_KEY_AudioPause: play_pause(p); return TRUE;
    case GDK_KEY_Left: relative_seek(p, shift ? -60 : -10); return TRUE;
    case GDK_KEY_Right: relative_seek(p, shift ? 60 : 10); return TRUE;
    case GDK_KEY_Up: volume_change(p, p->settings.volume + 0.05, TRUE); return TRUE;
    case GDK_KEY_Down: volume_change(p, p->settings.volume - 0.05, TRUE); return TRUE;
    case GDK_KEY_Home: sp_engine_seek(p->engine, 0); return TRUE;
    case GDK_KEY_m: mute(p); return TRUE;
    case GDK_KEY_f: case GDK_KEY_F11: toggle_fullscreen(p); return TRUE;
    case GDK_KEY_Escape: if (p->fullscreen) toggle_fullscreen(p); return TRUE;
    case GDK_KEY_n: case GDK_KEY_AudioNext: next_track(p, FALSE); return TRUE;
    case GDK_KEY_p: case GDK_KEY_AudioPrev: previous_track(p); return TRUE;
    case GDK_KEY_s: p->shuffled = !p->shuffled; gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(p->shuffle), p->shuffled); mpris_changed(p); return TRUE;
    case GDK_KEY_r: p->repeat_mode = (p->repeat_mode + 1) % 3; repeat_update(p); return TRUE;
    case GDK_KEY_b: cycle_track(p, SP_AUDIO); return TRUE;
    case GDK_KEY_v: cycle_track(p, SP_SUBTITLE); return TRUE;
    case GDK_KEY_bracketleft: case GDK_KEY_bracketright:
        p->subtitle_delay += key == GDK_KEY_bracketleft ? -0.5 : 0.5;
        p->subtitle_delay = CLAMP(p->subtitle_delay, -60, 60);
        sp_engine_subtitle_delay(p->engine, p->subtitle_delay);
        gtk_widget_queue_draw(p->drawing);
        { g_autofree char *message = g_strdup_printf("Subtitle delay %+.1f s", p->subtitle_delay); toast(p, message); }
        return TRUE;
    default: return FALSE;
    }
}

static void clicked(GtkButton *button, Player *p)
{
    const char *action = g_object_get_data(G_OBJECT(button), "control");
    if (!strcmp(action, "play")) play_pause(p);
    else if (!strcmp(action, "next")) next_track(p, FALSE);
    else if (!strcmp(action, "previous")) previous_track(p);
    else if (!strcmp(action, "back")) relative_seek(p, -10);
    else if (!strcmp(action, "forward")) relative_seek(p, 10);
    else if (!strcmp(action, "mute")) mute(p);
    else if (!strcmp(action, "fullscreen")) toggle_fullscreen(p);
    else if (!strcmp(action, "queue")) queue_visibility(p, gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(button)));
    else if (!strcmp(action, "shuffle")) { p->shuffled = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(button)); mpris_changed(p); }
    else if (!strcmp(action, "repeat")) { p->repeat_mode = (p->repeat_mode + 1) % 3; repeat_update(p); }
    else if (!strcmp(action, "options")) show_options(p, GTK_WIDGET(button), NULL);
    else if (!strcmp(action, "menu")) show_main_menu(p, GTK_WIDGET(button));
    else if (!strcmp(action, "stop")) { cancel_imports(p); save_position(p); sp_engine_stop(p->engine); }
    else action_run(p, action);
    activity(p);
}

static void control_connect(GtkWidget *button, Player *p, const char *action)
{
    g_object_set_data(G_OBJECT(button), "control", (gpointer)action);
    g_signal_connect(button, "clicked", G_CALLBACK(clicked), p);
}

static void volume_value(GtkRange *range, Player *p)
{
    if (!p->updating) volume_change(p, gtk_range_get_value(range), FALSE);
}

static gboolean seek_commit(gpointer opaque)
{
    Player *p = opaque;
    p->seek_timer = 0;
    sp_engine_seek(p->engine, p->pending_seek);
    return G_SOURCE_REMOVE;
}

static void seek_value(GtkRange *range, Player *p)
{
    if (p->updating) return;
    p->pending_seek = gtk_range_get_value(range);
    if (p->seek_timer) g_source_remove(p->seek_timer);
    p->seek_timer = g_timeout_add(100, seek_commit, p);
}

static gboolean seek_press(GtkWidget *widget, GdkEventButton *event, Player *p)
{
    (void)widget;
    if (event->button == 1) p->dragging = TRUE;
    return FALSE;
}

static gboolean seek_release(GtkWidget *widget, GdkEventButton *event, Player *p)
{
    (void)widget;
    if (event->button == 1) { p->dragging = FALSE; activity(p); }
    return FALSE;
}

static void drop_received(GtkWidget *widget, GdkDragContext *context, int x, int y,
                          GtkSelectionData *data, guint info, guint time, Player *p)
{
    (void)x; (void)y; (void)info;
    g_auto(GStrv) uris = gtk_selection_data_get_uris(data);
    if (uris) open_paths(p, uris, widget == p->sidebar);
    gtk_drag_finish(context, uris != NULL, FALSE, time);
}

static void error_dismiss(GtkInfoBar *bar, int response, Player *p)
{
    (void)response; (void)p;
    gtk_widget_hide(GTK_WIDGET(bar));
}

static void metadata_update(Player *p)
{
    sp_engine_info(p->engine, &p->info);
    if (p->filename) {
        g_autofree char *base = display_basename(p->filename);
        const char *title = *p->info.title ? p->info.title : base;
        gtk_label_set_text(GTK_LABEL(p->title), title);
        gtk_label_set_text(GTK_LABEL(p->audio_title), title);
        g_autofree char *artist = g_strdup_printf("%s%s%s", p->info.artist,
            *p->info.artist && *p->info.album ? "  ·  " : "", p->info.album);
        gtk_label_set_text(GTK_LABEL(p->audio_artist), *artist ? artist : p->info.description);
        gtk_header_bar_set_subtitle(GTK_HEADER_BAR(p->header), title);
    }
    mpris_changed(p);
}

static void present_frame(Player *p, double presentation_delay)
{
    SpFrame *frame = sp_engine_frame_at(p->engine, presentation_delay);
    gboolean changed = frame != p->frame;
    sp_frame_unref(p->frame);
    p->frame = frame;
    if (changed) gtk_widget_queue_draw(p->drawing);
}

static gboolean frame_tick(GtkWidget *widget, GdkFrameClock *clock, gpointer opaque)
{
    (void)widget;
    Player *p = opaque;
    gint64 now = gdk_frame_clock_get_frame_time(clock), interval = 0, presentation = 0;
    gdk_frame_clock_get_refresh_info(clock, now, &interval, &presentation);
    p->last_frame_tick = g_get_monotonic_time();
    present_frame(p, presentation > now ? (presentation - now) / 1000000.0 : 0);
    return G_SOURCE_CONTINUE;
}

static gboolean tick(gpointer opaque)
{
    Player *p = opaque;
    SpStatus old = p->status;
    sp_engine_status(p->engine, &p->status);
    if (old.revision != p->status.revision) metadata_update(p);
    gboolean active = p->status.state != SP_IDLE && p->status.state != SP_ERROR;
    gtk_widget_set_visible(p->empty, !active);
    gtk_widget_set_visible(p->audio_labels, active && p->info.has_audio && !p->info.has_video);
    gtk_label_set_text(GTK_LABEL(p->detail), p->status.state == SP_LOADING ? "Opening…" :
        p->status.state == SP_IDLE ? "Open a file or drop it into the window" :
        p->status.state == SP_ERROR ? "Could not play this file" : p->info.description);
    if (p->status.state == SP_IDLE) gtk_label_set_text(GTK_LABEL(p->title), "Nothing playing");
    if (old.state != p->status.state) {
        set_icon(p->play, p->status.state == SP_PLAYING ? "media-playback-pause-symbolic" : "media-playback-start-symbolic");
        gtk_widget_set_tooltip_text(p->play, p->status.state == SP_PLAYING ? "Pause (Space)" : "Play (Space)");
        if (p->status.state == SP_PLAYING && p->info.has_video && !p->inhibit)
            p->inhibit = gtk_application_inhibit(p->application, GTK_WINDOW(p->window), GTK_APPLICATION_INHIBIT_IDLE, "Playing a video");
        else if (p->status.state != SP_PLAYING && p->inhibit) {
            gtk_application_uninhibit(p->application, p->inhibit); p->inhibit = 0;
        }
        mpris_changed(p);
    }
    if (old.seek_revision != p->status.seek_revision && p->bus)
        g_dbus_connection_emit_signal(p->bus, NULL, "/org/mpris/MediaPlayer2", "org.mpris.MediaPlayer2.Player", "Seeked",
            g_variant_new("(x)", (gint64)(p->status.position * 1000000)), NULL);
    if (old.rate != p->status.rate || old.volume != p->status.volume) mpris_changed(p);
    if (*p->status.error && g_strcmp0(p->last_error, p->status.error)) {
        g_free(p->last_error); p->last_error = g_strdup(p->status.error);
        gtk_label_set_text(GTK_LABEL(p->error_label), p->status.error); gtk_widget_show(p->error_bar);
    }
    if (p->pending_subtitle && (p->status.state == SP_PLAYING || p->status.state == SP_PAUSED)) {
        p->pending_subtitle = FALSE;
        auto_subtitles(p);
    }
    g_autofree char *elapsed = time_text(p->dragging ? gtk_range_get_value(GTK_RANGE(p->seek)) : p->status.position);
    g_autofree char *duration = p->status.duration > 0 ? time_text(p->status.duration) : g_strdup("–:––");
    gtk_label_set_text(GTK_LABEL(p->elapsed), elapsed); gtk_label_set_text(GTK_LABEL(p->duration), duration);
    p->updating = TRUE;
    gtk_range_set_range(GTK_RANGE(p->seek), 0, MAX(1, p->status.duration));
    if (!p->dragging && !p->seek_timer) gtk_range_set_value(GTK_RANGE(p->seek), p->status.position);
    p->updating = FALSE;
    gtk_widget_set_sensitive(p->seek, p->info.seekable && active);
    gtk_widget_set_sensitive(p->tracks_button, active);
    g_autofree char *speed = g_strdup_printf("%.2g×", p->status.rate);
    gtk_button_set_label(GTK_BUTTON(p->speed_button), speed);
    /* Keep the bounded decoder queues moving when a minimized window's frame
     * clock stops, so background audio playback continues uninterrupted. */
    if (g_get_monotonic_time() - p->last_frame_tick > 200000) present_frame(p, 0);
    if (old.state != p->status.state || old.revision != p->status.revision ||
        old.subtitle_track != p->status.subtitle_track)
        gtk_widget_queue_draw(p->drawing);
    if (p->fullscreen && p->status.state == SP_PLAYING && p->info.has_video &&
        g_get_monotonic_time() - p->last_activity > 3 * G_USEC_PER_SEC) {
        GdkWindow *window = gtk_widget_get_window(p->drawing);
        if (window) { GdkCursor *cursor = gdk_cursor_new_for_display(gdk_window_get_display(window), GDK_BLANK_CURSOR); gdk_window_set_cursor(window, cursor); g_object_unref(cursor); }
    }
    if (g_get_monotonic_time() - p->last_save > 15 * G_USEC_PER_SEC) {
        save_position(p); sp_settings_save(&p->settings); p->last_save = g_get_monotonic_time();
    }
    if (p->status.state == SP_ENDED && old.state != SP_ENDED) next_track(p, TRUE);
    return G_SOURCE_CONTINUE;
}

static gboolean closing(GtkWidget *widget, GdkEvent *event, Player *p)
{
    (void)widget; (void)event;
    p->closing = TRUE;
    if (p->import_cancel) g_cancellable_cancel(p->import_cancel);
    save_position(p);
    if (!p->fullscreen) gtk_window_get_size(GTK_WINDOW(p->window), &p->settings.width, &p->settings.height);
    sp_settings_save(&p->settings);
    if (p->tick) { g_source_remove(p->tick); p->tick = 0; }
    if (p->frame_tick) { gtk_widget_remove_tick_callback(p->drawing, p->frame_tick); p->frame_tick = 0; }
    if (p->toast_timer) { g_source_remove(p->toast_timer); p->toast_timer = 0; }
    if (p->seek_timer) { g_source_remove(p->seek_timer); p->seek_timer = 0; }
    if (p->click_timer) { g_source_remove(p->click_timer); p->click_timer = 0; }
    return FALSE;
}

static gboolean quit_when_ready(gpointer opaque)
{
    Player *p = opaque;
    if (!p->closing) gtk_window_close(GTK_WINDOW(p->window));
    return G_SOURCE_REMOVE;
}

static void request_quit(Player *p)
{
    /* D-Bus Quit also works inside a nested gtk_dialog_run(). Give each dialog
     * its normal response so its caller can release it before closing GTK. */
    GList *windows = gtk_window_list_toplevels();
    for (GList *n = windows; n; n = n->next) {
        if (!GTK_IS_DIALOG(n->data)) continue;
        GtkWindow *parent = gtk_window_get_transient_for(GTK_WINDOW(n->data));
        while (parent && parent != GTK_WINDOW(p->window)) parent = gtk_window_get_transient_for(parent);
        if (parent) gtk_dialog_response(GTK_DIALOG(n->data), GTK_RESPONSE_CANCEL);
    }
    g_list_free(windows);
    g_idle_add(quit_when_ready, p);
}

static GtkWidget *transport_button(Player *p, const char *icon, const char *tooltip,
                                   const char *action, gboolean toggle)
{
    GtkWidget *button = toggle ? gtk_toggle_button_new() : gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(button), gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_BUTTON));
    gtk_widget_set_tooltip_text(button, tooltip);
    control_connect(button, p, action);
    return button;
}

static void build_window(Player *p)
{
    p->window = gtk_application_window_new(p->application);
    style(p->window, "simpleplay");
    gtk_window_set_title(GTK_WINDOW(p->window), "SimplePlay");
    gtk_window_set_default_size(GTK_WINDOW(p->window), p->settings.width, p->settings.height);
    gtk_widget_set_size_request(p->window, 850, 540);
    gtk_window_set_icon_name(GTK_WINDOW(p->window), "multimedia-player");
    const char *icons[] = {"/usr/local/share/simplesuite/simpleplay/simpleplay.svg", "assets/simpleplay.svg", NULL};
    for (int n = 0; icons[n]; n++) if (g_file_test(icons[n], G_FILE_TEST_IS_REGULAR)) { gtk_window_set_icon_from_file(GTK_WINDOW(p->window), icons[n], NULL); break; }
    p->header = gtk_header_bar_new();
    gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(p->header), TRUE);
    gtk_header_bar_set_title(GTK_HEADER_BAR(p->header), "SimplePlay");
    gtk_window_set_titlebar(GTK_WINDOW(p->window), p->header);
    GtkWidget *open = gtk_button_new_with_label("Open");
    gtk_button_set_image(GTK_BUTTON(open), gtk_image_new_from_icon_name("document-open-symbolic", GTK_ICON_SIZE_BUTTON));
    gtk_button_set_always_show_image(GTK_BUTTON(open), TRUE); style(open, "open-button");
    control_connect(open, p, "open"); gtk_widget_set_tooltip_text(open, "Open files (Ctrl+O)");
    gtk_header_bar_pack_start(GTK_HEADER_BAR(p->header), open);
    GtkWidget *menu = transport_button(p, "open-menu-symbolic", "Menu", "menu", FALSE);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(p->header), menu);
    p->tracks_button = transport_button(p, "preferences-system-symbolic", "Audio, subtitles, speed, and chapters", "options", FALSE);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(p->header), p->tracks_button);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(p->window), root);
    p->error_bar = gtk_info_bar_new();
    gtk_info_bar_set_show_close_button(GTK_INFO_BAR(p->error_bar), TRUE);
    p->error_label = label_new("", NULL);
    gtk_label_set_line_wrap(GTK_LABEL(p->error_label), TRUE);
    gtk_label_set_xalign(GTK_LABEL(p->error_label), 0);
    gtk_container_add(GTK_CONTAINER(gtk_info_bar_get_content_area(GTK_INFO_BAR(p->error_bar))), p->error_label);
    g_signal_connect(p->error_bar, "response", G_CALLBACK(error_dismiss), p);
    gtk_box_pack_start(GTK_BOX(root), p->error_bar, FALSE, FALSE, 0);
    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(root), body, TRUE, TRUE, 0);
    GtkWidget *overlay = gtk_overlay_new();
    gtk_box_pack_start(GTK_BOX(body), overlay, TRUE, TRUE, 0);
    p->drawing = gtk_drawing_area_new();
    gtk_widget_set_hexpand(p->drawing, TRUE); gtk_widget_set_vexpand(p->drawing, TRUE);
    /* GTK popovers dismiss on release, including releases over the picture. */
    gtk_widget_add_events(p->drawing, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
                         GDK_POINTER_MOTION_MASK | GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK);
    gtk_container_add(GTK_CONTAINER(overlay), p->drawing);
    g_signal_connect(p->drawing, "draw", G_CALLBACK(draw), p);
    g_signal_connect(p->drawing, "button-press-event", G_CALLBACK(pointer_press), p);
    g_signal_connect(p->drawing, "motion-notify-event", G_CALLBACK(pointer_motion), p);
    g_signal_connect(p->drawing, "scroll-event", G_CALLBACK(scroll), p);
    p->empty = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_widget_set_halign(p->empty, GTK_ALIGN_CENTER); gtk_widget_set_valign(p->empty, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(p->empty, 88);
    gtk_box_pack_start(GTK_BOX(p->empty), label_new("Open something good.", "empty-title"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(p->empty), label_new("Drop audio or video here", "empty-detail"), FALSE, FALSE, 0);
    GtkWidget *start = gtk_button_new_with_label("Choose a file"); style(start, "primary");
    gtk_widget_set_halign(start, GTK_ALIGN_CENTER); gtk_widget_set_margin_top(start, 6);
    control_connect(start, p, "open"); gtk_box_pack_start(GTK_BOX(p->empty), start, FALSE, FALSE, 0);
    GtkWidget *hint = label_new("MKV   MP4   FLAC   APE   + MORE", "format-hint");
    gtk_widget_set_margin_top(hint, 13); gtk_box_pack_start(GTK_BOX(p->empty), hint, FALSE, FALSE, 0);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), p->empty);
    p->audio_labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 9);
    gtk_widget_set_halign(p->audio_labels, GTK_ALIGN_CENTER); gtk_widget_set_valign(p->audio_labels, GTK_ALIGN_END);
    gtk_widget_set_margin_bottom(p->audio_labels, 43);
    gtk_widget_set_margin_start(p->audio_labels, 30); gtk_widget_set_margin_end(p->audio_labels, 30);
    p->audio_title = label_new("", "audio-title"); p->audio_artist = label_new("", "audio-artist");
    gtk_label_set_ellipsize(GTK_LABEL(p->audio_title), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(p->audio_title), 45);
    gtk_label_set_ellipsize(GTK_LABEL(p->audio_artist), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(p->audio_artist), 60);
    gtk_box_pack_start(GTK_BOX(p->audio_labels), p->audio_title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(p->audio_labels), p->audio_artist, FALSE, FALSE, 0);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), p->audio_labels);
    gtk_overlay_set_overlay_pass_through(GTK_OVERLAY(overlay), p->audio_labels, TRUE);
    p->toast = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(p->toast), GTK_REVEALER_TRANSITION_TYPE_CROSSFADE);
    gtk_widget_set_halign(p->toast, GTK_ALIGN_CENTER); gtk_widget_set_valign(p->toast, GTK_ALIGN_START);
    gtk_widget_set_margin_top(p->toast, 28);
    p->toast_label = label_new("", "toast");
    gtk_label_set_line_wrap(GTK_LABEL(p->toast_label), TRUE); gtk_label_set_max_width_chars(GTK_LABEL(p->toast_label), 55);
    gtk_container_add(GTK_CONTAINER(p->toast), p->toast_label);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), p->toast);
    gtk_overlay_set_overlay_pass_through(GTK_OVERLAY(overlay), p->toast, TRUE);

    p->sidebar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0); style(p->sidebar, "sidebar");
    gtk_widget_set_size_request(p->sidebar, 300, -1);
    gtk_box_pack_end(GTK_BOX(body), p->sidebar, FALSE, FALSE, 0);
    GtkWidget *queue_head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5); style(queue_head, "sidebar-head");
    p->queue_label = label_new("Up next · 0", "section-title");
    gtk_box_pack_start(GTK_BOX(queue_head), p->queue_label, TRUE, TRUE, 0);
    GtkWidget *add = transport_button(p, "list-add-symbolic", "Add files", "add", FALSE);
    gtk_box_pack_end(GTK_BOX(queue_head), add, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(p->sidebar), queue_head, FALSE, FALSE, 0);
    GtkWidget *scroller = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    p->list = gtk_list_box_new();
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(p->list), FALSE);
    gtk_container_add(GTK_CONTAINER(scroller), p->list);
    gtk_box_pack_start(GTK_BOX(p->sidebar), scroller, TRUE, TRUE, 0);
    g_signal_connect(p->list, "row-activated", G_CALLBACK(row_activated), p);
    GtkWidget *queue_bottom = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(queue_bottom), 10);
    GtkWidget *save = gtk_button_new_with_label("Save queue…"); control_connect(save, p, "export");
    GtkWidget *clear = gtk_button_new_with_label("Clear"); control_connect(clear, p, "clear");
    gtk_box_pack_start(GTK_BOX(queue_bottom), save, TRUE, TRUE, 0); gtk_box_pack_end(GTK_BOX(queue_bottom), clear, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(p->sidebar), queue_bottom, FALSE, FALSE, 0);

    p->controls = gtk_box_new(GTK_ORIENTATION_VERTICAL, 7); style(p->controls, "transport");
    gtk_box_pack_end(GTK_BOX(root), p->controls, FALSE, FALSE, 0);
    p->title = label_new("Nothing playing", "now-title"); p->detail = label_new("Open a file or drop it into the window", "muted");
    gtk_label_set_xalign(GTK_LABEL(p->title), 0); gtk_label_set_xalign(GTK_LABEL(p->detail), 0);
    gtk_label_set_ellipsize(GTK_LABEL(p->title), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_ellipsize(GTK_LABEL(p->detail), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(p->controls), p->title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(p->controls), p->detail, FALSE, FALSE, 0);
    GtkWidget *timeline = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    p->elapsed = label_new("0:00", "time"); p->duration = label_new("–:––", "time");
    gtk_widget_set_size_request(p->elapsed, 52, -1); gtk_widget_set_size_request(p->duration, 52, -1);
    p->seek = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 1, 0.1);
    gtk_scale_set_draw_value(GTK_SCALE(p->seek), FALSE);
    gtk_widget_set_tooltip_text(p->seek, "Seek (Left / Right)");
    gtk_box_pack_start(GTK_BOX(timeline), p->elapsed, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(timeline), p->seek, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(timeline), p->duration, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(p->controls), timeline, FALSE, FALSE, 0);
    g_signal_connect(p->seek, "value-changed", G_CALLBACK(seek_value), p);
    g_signal_connect(p->seek, "button-press-event", G_CALLBACK(seek_press), p);
    g_signal_connect(p->seek, "button-release-event", G_CALLBACK(seek_release), p);
    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(p->controls), buttons, FALSE, FALSE, 0);
    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    p->queue_button = transport_button(p, "view-list-symbolic", "Show queue (Ctrl+E)", "queue", TRUE);
    gtk_box_pack_start(GTK_BOX(left), p->queue_button, FALSE, FALSE, 0);
    GtkWidget *stop = transport_button(p, "media-playback-stop-symbolic", "Stop", "stop", FALSE);
    gtk_box_pack_start(GTK_BOX(left), stop, FALSE, FALSE, 0);
    gtk_widget_set_hexpand(left, TRUE); gtk_widget_set_halign(left, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(buttons), left, TRUE, TRUE, 0);
    GtkWidget *center = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 7);
    p->shuffle = transport_button(p, "media-playlist-shuffle-symbolic", "Shuffle (S)", "shuffle", TRUE);
    gtk_box_pack_start(GTK_BOX(center), p->shuffle, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(center), transport_button(p, "media-skip-backward-symbolic", "Previous (P)", "previous", FALSE), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(center), transport_button(p, "media-seek-backward-symbolic", "Back 10 seconds (Left)", "back", FALSE), FALSE, FALSE, 0);
    p->play = transport_button(p, "media-playback-start-symbolic", "Play (Space)", "play", FALSE); style(p->play, "play-button");
    gtk_box_pack_start(GTK_BOX(center), p->play, FALSE, FALSE, 3);
    gtk_box_pack_start(GTK_BOX(center), transport_button(p, "media-seek-forward-symbolic", "Forward 10 seconds (Right)", "forward", FALSE), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(center), transport_button(p, "media-skip-forward-symbolic", "Next (N)", "next", FALSE), FALSE, FALSE, 0);
    p->repeat = transport_button(p, "media-playlist-repeat-symbolic", "Repeat off (R)", "repeat", TRUE);
    gtk_box_pack_start(GTK_BOX(center), p->repeat, FALSE, FALSE, 0);
    gtk_box_set_center_widget(GTK_BOX(buttons), center);
    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    p->speed_button = gtk_button_new_with_label("1×"); control_connect(p->speed_button, p, "options");
    gtk_widget_set_tooltip_text(p->speed_button, "Playback speed");
    gtk_box_pack_start(GTK_BOX(right), p->speed_button, FALSE, FALSE, 0);
    p->volume_button = transport_button(p, "audio-volume-high-symbolic", "Mute (M)", "mute", FALSE);
    gtk_box_pack_start(GTK_BOX(right), p->volume_button, FALSE, FALSE, 0);
    p->volume = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 1, 0.01);
    gtk_scale_set_draw_value(GTK_SCALE(p->volume), FALSE); gtk_widget_set_size_request(p->volume, 84, -1);
    gtk_widget_set_tooltip_text(p->volume, "Volume (Up / Down)");
    gtk_box_pack_start(GTK_BOX(right), p->volume, FALSE, FALSE, 0);
    g_signal_connect(p->volume, "value-changed", G_CALLBACK(volume_value), p);
    gtk_box_pack_start(GTK_BOX(right), transport_button(p, "view-fullscreen-symbolic", "Fullscreen (F)", "fullscreen", FALSE), FALSE, FALSE, 0);
    gtk_widget_set_hexpand(right, TRUE); gtk_widget_set_halign(right, GTK_ALIGN_END);
    gtk_box_pack_end(GTK_BOX(buttons), right, TRUE, TRUE, 0);
    g_signal_connect(p->window, "delete-event", G_CALLBACK(closing), p);
    g_signal_connect(p->window, "key-press-event", G_CALLBACK(key_press), p);
    g_signal_connect(p->window, "window-state-event", G_CALLBACK(window_state), p);
    const GtkTargetEntry targets[] = {{"text/uri-list", 0, 0}};
    gtk_drag_dest_set(p->drawing, GTK_DEST_DEFAULT_ALL, targets, 1, GDK_ACTION_COPY);
    gtk_drag_dest_set(p->sidebar, GTK_DEST_DEFAULT_ALL, targets, 1, GDK_ACTION_COPY);
    g_signal_connect(p->drawing, "drag-data-received", G_CALLBACK(drop_received), p);
    g_signal_connect(p->sidebar, "drag-data-received", G_CALLBACK(drop_received), p);
    gtk_widget_show_all(p->window);
    gtk_widget_hide(p->error_bar); gtk_widget_hide(p->audio_labels);
    queue_visibility(p, p->settings.queue_visible);
    volume_change(p, p->settings.volume, FALSE);
    p->last_activity = p->last_save = g_get_monotonic_time();
    p->tick = g_timeout_add(100, tick, p);
    p->frame_tick = gtk_widget_add_tick_callback(p->drawing, frame_tick, p, NULL);
}

static GVariant *mpris_metadata(Player *p)
{
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
    g_autofree char *track = g_strdup_printf("/org/mpris/MediaPlayer2/track/%u", p->track_id);
    g_variant_builder_add(&b, "{sv}", "mpris:trackid", g_variant_new_object_path(track));
    g_variant_builder_add(&b, "{sv}", "mpris:length", g_variant_new_int64(p->status.duration * 1000000));
    g_autofree char *base = display_basename(p->filename);
    g_variant_builder_add(&b, "{sv}", "xesam:title", g_variant_new_string(*p->info.title ? p->info.title : base));
    const char *artists[] = {p->info.artist, NULL};
    g_variant_builder_add(&b, "{sv}", "xesam:artist", g_variant_new_strv(artists, -1));
    g_variant_builder_add(&b, "{sv}", "xesam:album", g_variant_new_string(p->info.album));
    if (p->filename) {
        g_autofree char *uri = sp_formats_is_uri(p->filename) ? g_strdup(p->filename) : g_filename_to_uri(p->filename, NULL, NULL);
        if (uri) g_variant_builder_add(&b, "{sv}", "xesam:url", g_variant_new_string(uri));
    }
    return g_variant_builder_end(&b);
}

static const char *playback_status(Player *p)
{
    return p->status.state == SP_PLAYING ? "Playing" : p->status.state == SP_PAUSED ? "Paused" : "Stopped";
}

static GVariant *mpris_get(GDBusConnection *bus, const char *sender, const char *path,
                           const char *interface, const char *property, GError **error, gpointer opaque)
{
    (void)bus; (void)sender; (void)path; (void)interface; (void)error;
    Player *p = opaque;
    if (!strcmp(property, "Identity")) return g_variant_new_string("SimplePlay");
    if (!strcmp(property, "DesktopEntry")) return g_variant_new_string("org.simplesuite.Simpleplay");
    if (!strcmp(property, "CanQuit") || !strcmp(property, "CanRaise") || !strcmp(property, "CanControl") ||
        !strcmp(property, "CanPlay") || !strcmp(property, "CanPause") || !strcmp(property, "CanSetFullscreen")) return g_variant_new_boolean(TRUE);
    if (!strcmp(property, "Fullscreen")) return g_variant_new_boolean(p->fullscreen);
    if (!strcmp(property, "HasTrackList")) return g_variant_new_boolean(FALSE);
    if (!strcmp(property, "CanGoNext") || !strcmp(property, "CanGoPrevious")) return g_variant_new_boolean(p->queue->len > 1);
    if (!strcmp(property, "CanSeek")) return g_variant_new_boolean(p->info.seekable);
    if (!strcmp(property, "PlaybackStatus")) return g_variant_new_string(playback_status(p));
    if (!strcmp(property, "LoopStatus")) return g_variant_new_string(p->repeat_mode == 1 ? "Track" : p->repeat_mode == 2 ? "Playlist" : "None");
    if (!strcmp(property, "Shuffle")) return g_variant_new_boolean(p->shuffled);
    if (!strcmp(property, "Rate")) return g_variant_new_double(p->status.rate);
    if (!strcmp(property, "MinimumRate")) return g_variant_new_double(0.5);
    if (!strcmp(property, "MaximumRate")) return g_variant_new_double(2);
    if (!strcmp(property, "Volume")) return g_variant_new_double(p->settings.volume);
    if (!strcmp(property, "Position")) { SpStatus status; sp_engine_status(p->engine, &status); return g_variant_new_int64(status.position * 1000000); }
    if (!strcmp(property, "Metadata")) return mpris_metadata(p);
    if (!strcmp(property, "SupportedUriSchemes")) { const char *schemes[] = {"file", "http", "https", "rtsp", "rtmp", NULL}; return g_variant_new_strv(schemes, -1); }
    if (!strcmp(property, "SupportedMimeTypes")) { const char *types[] = {"video/x-matroska", "video/mp4", "video/webm", "audio/x-ape", "audio/flac", "audio/mpeg", "audio/ogg", NULL}; return g_variant_new_strv(types, -1); }
    return NULL;
}

static gboolean mpris_set(GDBusConnection *bus, const char *sender, const char *path,
                           const char *interface, const char *property, GVariant *value,
                           GError **error, gpointer opaque)
{
    (void)bus; (void)sender; (void)path; (void)interface; (void)error;
    Player *p = opaque;
    if (!strcmp(property, "Volume")) volume_change(p, g_variant_get_double(value), FALSE);
    else if (!strcmp(property, "Rate")) sp_engine_rate(p->engine, g_variant_get_double(value));
    else if (!strcmp(property, "Shuffle")) { p->shuffled = g_variant_get_boolean(value); gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(p->shuffle), p->shuffled); }
    else if (!strcmp(property, "LoopStatus")) {
        const char *text = g_variant_get_string(value, NULL);
        p->repeat_mode = !strcmp(text, "Track") ? 1 : !strcmp(text, "Playlist") ? 2 : 0;
        repeat_update(p);
    } else if (!strcmp(property, "Fullscreen")) {
        if (g_variant_get_boolean(value) != p->fullscreen) toggle_fullscreen(p);
    } else return FALSE;
    mpris_changed(p);
    return TRUE;
}

static void mpris_method(GDBusConnection *bus, const char *sender, const char *path,
                          const char *interface, const char *method, GVariant *args,
                          GDBusMethodInvocation *invocation, gpointer opaque)
{
    (void)bus; (void)sender; (void)path; (void)interface;
    Player *p = opaque;
    if (!strcmp(method, "Raise")) gtk_window_present(GTK_WINDOW(p->window));
    else if (!strcmp(method, "Quit")) request_quit(p);
    else if (!strcmp(method, "PlayPause")) play_pause(p);
    else if (!strcmp(method, "Pause")) sp_engine_pause(p->engine, TRUE);
    else if (!strcmp(method, "Play")) {
        if (p->status.state != SP_PLAYING) play_pause(p);
    } else if (!strcmp(method, "Stop")) { cancel_imports(p); save_position(p); sp_engine_stop(p->engine); }
    else if (!strcmp(method, "Next")) next_track(p, FALSE);
    else if (!strcmp(method, "Previous")) previous_track(p);
    else if (!strcmp(method, "Seek")) {
        gint64 offset; g_variant_get(args, "(x)", &offset); relative_seek(p, offset / 1000000.0);
    } else if (!strcmp(method, "SetPosition")) {
        const char *track; gint64 position; g_variant_get(args, "(&ox)", &track, &position);
        g_autofree char *current = g_strdup_printf("/org/mpris/MediaPlayer2/track/%u", p->track_id);
        if (!strcmp(track, current)) sp_engine_seek(p->engine, position / 1000000.0);
    } else if (!strcmp(method, "OpenUri")) {
        const char *uri; g_variant_get(args, "(&s)", &uri); char *paths[] = {(char *)uri, NULL}; open_paths(p, paths, FALSE);
    }
    g_dbus_method_invocation_return_value(invocation, NULL);
}

static void mpris_changed(Player *p)
{
    if (!p->bus || !p->engine) return;
    GVariantBuilder props;
    g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&props, "{sv}", "PlaybackStatus", g_variant_new_string(playback_status(p)));
    g_variant_builder_add(&props, "{sv}", "Metadata", mpris_metadata(p));
    g_variant_builder_add(&props, "{sv}", "Volume", g_variant_new_double(p->settings.volume));
    g_variant_builder_add(&props, "{sv}", "Rate", g_variant_new_double(p->status.rate > 0 ? p->status.rate : 1));
    g_variant_builder_add(&props, "{sv}", "Shuffle", g_variant_new_boolean(p->shuffled));
    g_variant_builder_add(&props, "{sv}", "LoopStatus", g_variant_new_string(p->repeat_mode == 1 ? "Track" : p->repeat_mode == 2 ? "Playlist" : "None"));
    g_variant_builder_add(&props, "{sv}", "CanSeek", g_variant_new_boolean(p->info.seekable));
    g_dbus_connection_emit_signal(p->bus, NULL, "/org/mpris/MediaPlayer2", "org.freedesktop.DBus.Properties", "PropertiesChanged",
        g_variant_new("(sa{sv}as)", "org.mpris.MediaPlayer2.Player", &props, NULL), NULL);
}

static void mpris_ready(GDBusConnection *bus, const char *name, gpointer opaque)
{
    (void)name;
    Player *p = opaque;
    p->bus = g_object_ref(bus);
    const char *xml =
        "<node><interface name='org.mpris.MediaPlayer2'>"
        "<method name='Raise'/><method name='Quit'/>"
        "<property name='CanQuit' type='b' access='read'/><property name='CanRaise' type='b' access='read'/>"
        "<property name='HasTrackList' type='b' access='read'/><property name='Identity' type='s' access='read'/>"
        "<property name='DesktopEntry' type='s' access='read'/><property name='SupportedUriSchemes' type='as' access='read'/>"
        "<property name='SupportedMimeTypes' type='as' access='read'/><property name='Fullscreen' type='b' access='readwrite'/>"
        "<property name='CanSetFullscreen' type='b' access='read'/></interface>"
        "<interface name='org.mpris.MediaPlayer2.Player'>"
        "<method name='Next'/><method name='Previous'/><method name='Pause'/><method name='PlayPause'/><method name='Stop'/><method name='Play'/>"
        "<method name='Seek'><arg name='Offset' type='x' direction='in'/></method>"
        "<method name='SetPosition'><arg name='TrackId' type='o' direction='in'/><arg name='Position' type='x' direction='in'/></method>"
        "<method name='OpenUri'><arg name='Uri' type='s' direction='in'/></method>"
        "<signal name='Seeked'><arg name='Position' type='x'/></signal>"
        "<property name='PlaybackStatus' type='s' access='read'/><property name='LoopStatus' type='s' access='readwrite'/>"
        "<property name='Rate' type='d' access='readwrite'/><property name='Shuffle' type='b' access='readwrite'/>"
        "<property name='Metadata' type='a{sv}' access='read'/><property name='Volume' type='d' access='readwrite'/>"
        "<property name='Position' type='x' access='read'/><property name='MinimumRate' type='d' access='read'/>"
        "<property name='MaximumRate' type='d' access='read'/><property name='CanGoNext' type='b' access='read'/>"
        "<property name='CanGoPrevious' type='b' access='read'/><property name='CanPlay' type='b' access='read'/>"
        "<property name='CanPause' type='b' access='read'/><property name='CanSeek' type='b' access='read'/>"
        "<property name='CanControl' type='b' access='read'/></interface></node>";
    p->mpris_xml = g_dbus_node_info_new_for_xml(xml, NULL);
    static const GDBusInterfaceVTable vtable = {.method_call = mpris_method, .get_property = mpris_get, .set_property = mpris_set};
    p->mpris_root = g_dbus_connection_register_object(bus, "/org/mpris/MediaPlayer2", p->mpris_xml->interfaces[0], &vtable, p, NULL, NULL);
    p->mpris_player = g_dbus_connection_register_object(bus, "/org/mpris/MediaPlayer2", p->mpris_xml->interfaces[1], &vtable, p, NULL, NULL);
}

static void activate(GApplication *application, Player *p)
{
    (void)application;
    if (!p->window) {
        p->engine = sp_engine_new();
        sp_engine_deinterlace(p->engine, p->settings.deinterlace, p->settings.deinterlace_double_rate);
        GtkCssProvider *provider = gtk_css_provider_new();
        GError *error = NULL;
        if (!gtk_css_provider_load_from_data(provider, css, -1, &error)) { g_warning("SimplePlay style: %s", error->message); g_error_free(error); }
        gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_object_unref(provider);
        build_window(p);
        p->mpris_owner = g_bus_own_name(G_BUS_TYPE_SESSION, "org.mpris.MediaPlayer2.simpleplay", G_BUS_NAME_OWNER_FLAGS_NONE, mpris_ready, NULL, NULL, p, NULL);
    }
    gtk_window_present(GTK_WINDOW(p->window));
}

static int command_line(GApplication *application, GApplicationCommandLine *command, Player *p)
{
    activate(application, p);
    GVariantDict *options = g_application_command_line_get_options_dict(command);
    gboolean append = FALSE, fullscreen = FALSE;
    g_variant_dict_lookup(options, "enqueue", "b", &append);
    g_variant_dict_lookup(options, "fullscreen", "b", &fullscreen);
    char **arguments = NULL;
    if (g_variant_dict_lookup(options, G_OPTION_REMAINING, "^aay", &arguments)) {
        GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);
        for (int n = 0; arguments[n]; n++) {
            GFile *file = g_application_command_line_create_file_for_arg(command, arguments[n]);
            char *path = g_file_get_path(file);
            g_ptr_array_add(paths, path ? path : g_file_get_uri(file));
            g_object_unref(file);
        }
        g_ptr_array_add(paths, NULL);
        open_paths(p, (char **)paths->pdata, append);
        g_ptr_array_free(paths, TRUE);
        g_strfreev(arguments);
    }
    if (fullscreen && !p->fullscreen) gtk_window_fullscreen(GTK_WINDOW(p->window));
    return 0;
}

static int local_options(GApplication *application, GVariantDict *options, Player *p)
{
    (void)application; (void)p;
    if (g_variant_dict_contains(options, "version")) { g_print("SimplePlay %s · GTK / FFmpeg\n", VERSION); return 0; }
    if (g_variant_dict_contains(options, "formats") || g_variant_dict_contains(options, "check-formats")) {
        gboolean complete;
        g_autofree char *report = sp_formats_report(g_variant_dict_contains(options, "formats"), &complete);
        g_print("%s", report);
        return g_variant_dict_contains(options, "check-formats") && !complete ? 1 : 0;
    }
    return -1;
}

int main(int argc, char **argv)
{
    for (int n = 1; n < argc; n++) {
        if (!strcmp(argv[n], "--")) break;
        if (!strcmp(argv[n], "--headless") || !strcmp(argv[n], "--no-video") || !strcmp(argv[n], "--vid=no") ||
            !strcmp(argv[n], "--vo=null") ||
            !strcmp(argv[n], "--input-ipc-server") || g_str_has_prefix(argv[n], "--input-ipc-server="))
            return sp_headless_main(argc, argv);
    }
    Player *p = &player;
    p->current = -1;
    p->queue = g_ptr_array_new_with_free_func(g_free);
    p->status.rate = 1;
    sp_settings_load(&p->settings);
    g_set_application_name("SimplePlay");
    g_set_prgname("simpleplay");
    p->application = gtk_application_new("org.simplesuite.Simpleplay", G_APPLICATION_HANDLES_COMMAND_LINE);
    const GOptionEntry entries[] = {
        {"version", 0, 0, G_OPTION_ARG_NONE, NULL, "Show version", NULL},
        {"headless", 0, 0, G_OPTION_ARG_NONE, NULL, "Play audio without a window (use --headless --help for IPC options)", NULL},
        {"formats", 0, 0, G_OPTION_ARG_NONE, NULL, "List all available decoders and demuxers", NULL},
        {"check-formats", 0, 0, G_OPTION_ARG_NONE, NULL, "Check the VLC audio/video compatibility baseline", NULL},
        {"enqueue", 'e', 0, G_OPTION_ARG_NONE, NULL, "Add files to the existing queue", NULL},
        {"fullscreen", 'f', 0, G_OPTION_ARG_NONE, NULL, "Start fullscreen", NULL},
        {G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_FILENAME_ARRAY, NULL, "Files, folders, playlists, or stream URLs", "FILE…"},
        {NULL, 0, 0, 0, NULL, NULL, NULL}
    };
    g_application_add_main_option_entries(G_APPLICATION(p->application), entries);
    g_signal_connect(p->application, "activate", G_CALLBACK(activate), p);
    g_signal_connect(p->application, "command-line", G_CALLBACK(command_line), p);
    g_signal_connect(p->application, "handle-local-options", G_CALLBACK(local_options), p);
    int result = g_application_run(G_APPLICATION(p->application), argc, argv);
    if (p->mpris_owner) g_bus_unown_name(p->mpris_owner);
    if (p->bus) {
        if (p->mpris_root) g_dbus_connection_unregister_object(p->bus, p->mpris_root);
        if (p->mpris_player) g_dbus_connection_unregister_object(p->bus, p->mpris_player);
        g_object_unref(p->bus);
    }
    if (p->mpris_xml) g_dbus_node_info_unref(p->mpris_xml);
    sp_engine_free(p->engine);
    sp_frame_unref(p->frame);
    sp_frame_unref(p->scaled_frame);
    if (p->scaled_picture) cairo_surface_destroy(p->scaled_picture);
    sws_freeContext(p->picture_scale);
    g_clear_object(&p->import_cancel);
    g_object_unref(p->application);
    sp_settings_clear(&p->settings);
    g_ptr_array_free(p->queue, TRUE);
    g_free(p->filename); g_free(p->sidecar); g_free(p->last_error);
    g_free(p->format_filename); g_free(p->input_format);
    return result;
}
