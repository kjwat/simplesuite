#define _XOPEN_SOURCE 700

/*
 * simplewords - a small terminal word processor.
 *
 * Build: ./build.sh
 * Run:   ./simplewords [file ...]
 */

#include <assert.h>
#include <stdatomic.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <locale.h>
#include <curses.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>
#include <wchar.h>

#include "simpleproc.h"

#include "third_party/miniaudio/miniaudio_config.h"

#define MAX_LINES 10000
#define MAX_LINE  4096
#define TEXT_WIDTH 80
#define TAB_WIDTH 4
#define TOP_PAD 3
#define UNDO_DEPTH 256
#define MAX_BUFFERS 32
#define MAX_EDITOR_WINDOWS 8
#define MAX_LAYOUT_NODES (MAX_EDITOR_WINDOWS * 2 - 1)
#define TYPEWRITER_AUDIO_CHANNELS 2
#define TYPEWRITER_AUDIO_VOICES 32
#define TYPEWRITER_AUDIO_QUEUE_SIZE 32
#define TYPEWRITER_SOUND_DEFAULT_FILE \
    "~/.local/share/simplesuite/simplewords-typewriter.wav"
#define TYPEWRITER_SOUND_ALT_DEFAULT_FILE \
    "~/.local/share/simplesuite/simplewords-typewriter-alt.wav"
#define TYPEWRITER_SOUND_SPACE_DEFAULT_FILE \
    "~/.local/share/simplesuite/simplewords-typewriter-space.wav"
#define TYPEWRITER_SOUND_ENTER_DEFAULT_FILE \
    "~/.local/share/simplesuite/simplewords-typewriter-enter.wav"
#define TYPEWRITER_SOUND_DELETE_DEFAULT_FILE \
    "~/.local/share/simplesuite/simplewords-typewriter-delete.wav"

/* Private key codes for modified navigation. KEY_SR/KEY_SF mean terminal
 * scroll commands, not Shift+Up/Down, despite their misleading names. */
#define KEY_EXTEND_UP       (KEY_MAX + 1)
#define KEY_EXTEND_DOWN     (KEY_MAX + 2)
#define KEY_EXTEND_PAGE_UP  (KEY_MAX + 3)
#define KEY_EXTEND_PAGE_DOWN (KEY_MAX + 4)
#define KEY_BRACKETED_PASTE (KEY_MAX + 5)

#define BRACKETED_PASTE_ENABLE "\033[?2004h"
#define BRACKETED_PASTE_DISABLE "\033[?2004l"
#define BRACKETED_PASTE_BEGIN "[200~"
#define BRACKETED_PASTE_END "\033[201~"
#define BRACKETED_PASTE_IDLE_TICKS 20

typedef struct {
    int y;
    int x;
} EditPos;

typedef struct {
    EditPos start;
    EditPos old_end;
    EditPos new_end;
    char *old_text;
    char *new_text;
} UndoOp;

typedef struct {
    UndoOp *ops;
    int op_count;
    int op_capacity;
    int before_cy;
    int before_cx;
    int before_top;
    int after_cy;
    int after_cx;
    int after_top;
} UndoGroup;

typedef struct {
    int autosave_interval;
    int text_width;
    int top_pad;
    int typewriter_sound;
    char typewriter_sound_file[PATH_MAX];
    char typewriter_sound_alt_file[PATH_MAX];
    char typewriter_sound_space_file[PATH_MAX];
    char typewriter_sound_enter_file[PATH_MAX];
    char typewriter_sound_delete_file[PATH_MAX];
    int typewriter_sound_volume;
} Config;

typedef enum {
    TYPEWRITER_SOUND_KEY,
    TYPEWRITER_SOUND_KEY_ALT,
    TYPEWRITER_SOUND_SPACE,
    TYPEWRITER_SOUND_ENTER,
    TYPEWRITER_SOUND_DELETE,
    TYPEWRITER_SOUND_COUNT
} TypewriterSound;

typedef struct {
    float *pcm_frames;
    ma_uint64 frame_count;
} TypewriterSample;

typedef struct {
    const TypewriterSample *sample;
    ma_uint64 cursor;
} TypewriterVoice;

typedef struct {
    ma_device device;
    TypewriterSample samples[TYPEWRITER_SOUND_COUNT];
    TypewriterVoice voices[TYPEWRITER_AUDIO_VOICES];
    unsigned char event_queue[TYPEWRITER_AUDIO_QUEUE_SIZE];
    atomic_uint event_head;
    atomic_uint event_tail;
    unsigned int next_voice;
    ma_uint32 sample_rate;
    float volume;
    int device_initialized;
} TypewriterAudio;

typedef struct {
    int left;
    int body_width;
    int top_pad;
    int bottom;
    int visible_rows;
    int screen_left;
    int screen_right;
    int screen_top;
    int screen_bottom;
} BodyGeometry;

typedef struct {
    int line;
    int render_start;
    int render_end;
    int next_start;
    int cursor_start;
    int cursor_end;
    int visual_width;
    int doc_row;
} WrapRow;

typedef struct {
    WrapRow *rows;
    int count;
    int capacity;
} LineWrapCache;

static Config config = {
    .autosave_interval = 1,
    .text_width = TEXT_WIDTH,
    .top_pad = TOP_PAD,
    .typewriter_sound = 0,
    .typewriter_sound_file = TYPEWRITER_SOUND_DEFAULT_FILE,
    .typewriter_sound_alt_file = TYPEWRITER_SOUND_ALT_DEFAULT_FILE,
    .typewriter_sound_space_file = TYPEWRITER_SOUND_SPACE_DEFAULT_FILE,
    .typewriter_sound_enter_file = TYPEWRITER_SOUND_ENTER_DEFAULT_FILE,
    .typewriter_sound_delete_file = TYPEWRITER_SOUND_DELETE_DEFAULT_FILE,
    .typewriter_sound_volume = 70
};

static TypewriterAudio typewriter_audio;

#ifdef SIMPLEWORDS_TYPEWRITER_TEST
static int typewriter_audio_test_mode = 0;
static unsigned int typewriter_audio_test_requests[TYPEWRITER_SOUND_COUNT];
#endif

static char last_open_file[512] = "";
static char last_open_directory[512] = "";
static char last_save_directory[512] = "";
static int distraction_free = 0;

static char *clip = NULL;
static char *pending_bracketed_paste = NULL;
static int bracketed_paste_mode_enabled = 0;

enum {
    CLIP_BACKEND_UNKNOWN = -1,
    CLIP_BACKEND_NONE = 0,
    CLIP_BACKEND_WL,
    CLIP_BACKEND_XCLIP,
    CLIP_BACKEND_XSEL
#ifdef __APPLE__
    ,
    CLIP_BACKEND_MACOS
#endif
};

#define SYSTEM_CLIPBOARD_LIMIT (16u * 1024u * 1024u)
#define SYSTEM_CLIPBOARD_TIMEOUT_MS 500

static int clip_backend = CLIP_BACKEND_UNKNOWN;
static int clip_warned = 0;

static char status_msg[512] = "";
static char last_find[256] = "";
static time_t status_time = 0;
static volatile sig_atomic_t terminate_requested = 0;

#define LEGACY_SESSION_FILE ".simplewords-session"
#define SESSION_UNTITLED_MARKER "@simplewords-untitled-v1"

typedef enum {
    LOAD_RESULT_FAILED = 0,
    LOAD_RESULT_DISK = 1,
    LOAD_RESULT_AUTOSAVE = 2,
    LOAD_RESULT_NEW = 3
} LoadResult;

static void persistence_log_event(const char *func, const char *fmt, ...);
static void persistence_log_state(const char *func, const char *phase, const char *path);
static void persistence_log_loaded_file(const char *func, const char *path);
static void set_dirty_logged(int value, const char *func, int line, const char *reason);
static void set_autosave_dirty_logged(int value, const char *func, int line, const char *reason);
static void set_last_edit_time_logged(time_t value, const char *func, int line, const char *reason);

#define SET_DIRTY(value, reason) set_dirty_logged((value), __func__, __LINE__, (reason))
#define SET_AUTOSAVE_DIRTY(value, reason) set_autosave_dirty_logged((value), __func__, __LINE__, (reason))
#define SET_LAST_EDIT_TIME(value, reason) set_last_edit_time_logged((value), __func__, __LINE__, (reason))

/*
 * Buffers own text and editing state. Windows below are only views: changing
 * the buffer shown in one window never destroys the document that was there.
 * The field names deliberately differ from the historical globals; the small
 * aliases keep the editing engine focused on the active buffer while making
 * the ownership boundary explicit in one place.
 */
typedef struct {
    int used;
    char *text_lines[MAX_LINES];
    int text_line_count;
    int cursor_y;
    int cursor_x;
    int affinity_line;
    int affinity_x;
    int affinity_doc_row;
    int affinity_col;
    int view_top;
    int preferred_col;
    char path[512];
    char draft_name[128];
    char recovery_doc[PATH_MAX];
    char recovery_autosave[PATH_MAX];
    char opened_recovery_doc_path[PATH_MAX];
    char opened_recovery_autosave_path[PATH_MAX];
    int modified;
    int selection_active;
    int selection_y;
    int selection_x;
    int search_mode;
    int search_active;
    int search_match_y;
    int search_match_x;
    int search_match_len;
    time_t edit_time;
    int needs_autosave;
    UndoGroup undo_items[UNDO_DEPTH];
    int undo_item_count;
    UndoGroup redo_items[UNDO_DEPTH];
    int redo_item_count;
    UndoGroup pending_undo;
    int pending_undo_active;
    int pending_undo_depth;
    time_t typing_time;
    int typing_chars;
    unsigned long long last_used;
    int document_lock_fd;
    int lock_blocked;
    int disk_revision_known;
    time_t disk_mtime;
    off_t disk_size;
    dev_t disk_device;
    ino_t disk_inode;
} EditorBuffer;

static EditorBuffer editor_buffers[MAX_BUFFERS] = {
    [0] = {
        .used = 1,
        .text_line_count = 1,
        .affinity_line = -1,
        .affinity_x = -1,
        .affinity_doc_row = -1,
        .affinity_col = -1,
        .search_match_y = -1,
        .search_match_x = -1,
        .document_lock_fd = -1
    }
};
static int active_buffer_index = 0;
static int buffer_system_ready = 0;
static unsigned long long buffer_use_clock = 1;
static int autosaving_all_buffers = 0;
static int autosave_wrote_any = 0;
static int workspace_lock_fd = -1;
static int workspace_session_owner = 0;
static int workspace_server_fd = -1;
static char workspace_socket_path[PATH_MAX] = "";
static int allow_stale_document_write = 0;

enum {
    EDITOR_WINDOW_DOCUMENT,
    EDITOR_WINDOW_BUFFER_SHELF
};

typedef struct {
    int buffer_index;
    int cursor_y;
    int cursor_x;
    int view_top;
} WindowBufferState;

typedef struct {
    int used;
    int kind;
    int buffer_index;
    int cursor_y;
    int cursor_x;
    int view_top;
    WindowBufferState previous_buffers[MAX_BUFFERS];
    int previous_buffer_count;
    WindowBufferState next_buffers[MAX_BUFFERS];
    int next_buffer_count;
} EditorWindow;

enum {
    LAYOUT_UNUSED,
    LAYOUT_LEAF,
    LAYOUT_ABOVE_BELOW,
    LAYOUT_SIDE_BY_SIDE
};

typedef struct {
    int used;
    int kind;
    int parent;
    int first;
    int second;
    int window_index;
    int ratio;
} LayoutNode;

typedef struct {
    int y;
    int x;
    int height;
    int width;
} EditorRect;

static EditorWindow editor_windows[MAX_EDITOR_WINDOWS] = {
    [0] = {.used = 1, .buffer_index = 0}
};
static LayoutNode layout_nodes[MAX_LAYOUT_NODES] = {
    [0] = {
        .used = 1,
        .kind = LAYOUT_LEAF,
        .parent = -1,
        .first = -1,
        .second = -1,
        .window_index = 0,
        .ratio = 50
    }
};
static EditorRect editor_window_rects[MAX_EDITOR_WINDOWS];
static int layout_root = 0;
static int active_window_index = 0;
static int buffer_drawer_selected = 0;
static int buffer_drawer_scroll = 0;
static int buffer_drawer_order[MAX_BUFFERS];
static int buffer_drawer_count = 0;

typedef enum {
    CONTROL_X_BUFFER_COMMAND_NONE,
    CONTROL_X_BUFFER_COMMAND_NEW,
    CONTROL_X_BUFFER_COMMAND_LIST
} ControlXBufferCommand;

static int pane_rendering = 0;
static EditorRect pane_rect;

#define lines (editor_buffers[active_buffer_index].text_lines)
#define line_count (editor_buffers[active_buffer_index].text_line_count)
#define cy (editor_buffers[active_buffer_index].cursor_y)
#define cx (editor_buffers[active_buffer_index].cursor_x)
#define cursor_affinity_line (editor_buffers[active_buffer_index].affinity_line)
#define cursor_affinity_x (editor_buffers[active_buffer_index].affinity_x)
#define cursor_affinity_doc_row (editor_buffers[active_buffer_index].affinity_doc_row)
#define cursor_affinity_col (editor_buffers[active_buffer_index].affinity_col)
#define top (editor_buffers[active_buffer_index].view_top)
#define goal_col (editor_buffers[active_buffer_index].preferred_col)
#define filename (editor_buffers[active_buffer_index].path)
#define untitled_name (editor_buffers[active_buffer_index].draft_name)
#define pending_recovery_doc (editor_buffers[active_buffer_index].recovery_doc)
#define pending_recovery_autosave (editor_buffers[active_buffer_index].recovery_autosave)
#define opened_recovery_doc (editor_buffers[active_buffer_index].opened_recovery_doc_path)
#define opened_recovery_autosave (editor_buffers[active_buffer_index].opened_recovery_autosave_path)
#define dirty (editor_buffers[active_buffer_index].modified)
#define selecting (editor_buffers[active_buffer_index].selection_active)
#define sel_cy (editor_buffers[active_buffer_index].selection_y)
#define sel_cx (editor_buffers[active_buffer_index].selection_x)
#define find_mode (editor_buffers[active_buffer_index].search_mode)
#define find_active (editor_buffers[active_buffer_index].search_active)
#define find_match_y (editor_buffers[active_buffer_index].search_match_y)
#define find_match_x (editor_buffers[active_buffer_index].search_match_x)
#define find_match_len (editor_buffers[active_buffer_index].search_match_len)
#define last_edit_time (editor_buffers[active_buffer_index].edit_time)
#define autosave_dirty (editor_buffers[active_buffer_index].needs_autosave)
#define undo_stack (editor_buffers[active_buffer_index].undo_items)
#define undo_count (editor_buffers[active_buffer_index].undo_item_count)
#define redo_stack (editor_buffers[active_buffer_index].redo_items)
#define redo_count (editor_buffers[active_buffer_index].redo_item_count)
#define pending_undo_group (editor_buffers[active_buffer_index].pending_undo)
#define undo_group_active (editor_buffers[active_buffer_index].pending_undo_active)
#define undo_group_depth (editor_buffers[active_buffer_index].pending_undo_depth)
#define last_type_time (editor_buffers[active_buffer_index].typing_time)
#define burst_chars (editor_buffers[active_buffer_index].typing_chars)

static int cursor_visibility = -1;

enum {
    SCREEN_ROW_UNUSED,
    SCREEN_ROW_TEXT,
    SCREEN_ROW_EMPTY
};

typedef struct {
    int kind;
    WrapRow wrap;
} ScreenRow;

enum {
    SCREEN_CELL_BLANK,
    SCREEN_CELL_GLYPH,
    SCREEN_CELL_CONTINUATION
};

typedef struct {
    wchar_t wc;
    attr_t attr;
    unsigned char kind;
} ScreenCell;

static ScreenRow *desired_rows = NULL;
static int screen_row_capacity = 0;
static ScreenCell *screen_cells = NULL;
static ScreenCell *desired_cells = NULL;
static size_t screen_cell_capacity = 0;
static int screen_cache_valid = 0;
static int screen_cache_lines = 0;
static int screen_cache_cols = 0;
static int screen_cache_left = 0;
static int screen_cache_text_width = 0;
static int screen_cache_top_pad = 0;
static int screen_cache_distraction_free = 0;
static char screen_cache_title[700] = "";
static char screen_cache_wc[64] = "";
static char screen_cache_status[512] = "";
static int screen_cache_top = 0;
static int center_lock_enabled = 0;
static int windowed_redraw_enabled = 0;
static int idle_cursor_enabled = 0;
static WINDOW *body_window = NULL;
static int body_window_height = 0;
static int body_window_width = 0;
static int body_window_top = 0;
static int body_window_left = 0;
static long long last_keypress_ms = 0;
static int idle_cursor_hidden = 0;
static LineWrapCache *wrap_cache = NULL;
static int wrap_cache_line_capacity = 0;
static int wrap_cache_line_count = 0;
static int wrap_cache_width = 0;
static int wrap_cache_valid = 0;
static int wrap_cache_debug = 0;
static const char help_text[] =
    "C-x b new  C-x C-b buffers  C-x C-f open  C-x n new  "
    "C-x 2/3 split  C-x o next pane  "
    "C-x C-s save  C-s find  C-x C-c quit";
static const char recovery_footer_text[] =
    "Recovered draft preserved. Viewing saved file. r open recovery   d discard";

static void clamp_cursor(void);
static void clamp_top(void);
static void clear_cursor_affinity(void);
static void invalidate_wrap_cache(void);
static void reset_wrap_cache(void);
static void ensure_wrap_cache(void);
static const char *current_footer_text(void);
static int visual_to_pos_in_row(const WrapRow *row, int target_col);
static void typewriter_audio_callback(ma_device *device, void *output,
                                      const void *input, ma_uint32 frame_count);
static void put_wch_no_wrap(WINDOW *window, int row, int col,
                            const cchar_t *cell);
static void put_blank_run_no_wrap(WINDOW *window, int row, int left,
                                  int start, int end, attr_t attr);
static void draw_workspace_screen(void);
static void show_buffer_shelf_window(void);
static void close_buffer_shelf_window(int shelf_window);
static int buffer_shelf_window_index(void);
static int buffer_shelf_companion_window_index(int shelf_window);
static int active_window_is_buffer_shelf(void);
static void visit_file_in_buffer(const char *path);
static void create_blank_buffer(void);
static void kill_current_buffer(void);
static int split_editor_window(int kind);
static int remove_editor_window_from_layout(int window_index);
static void delete_editor_window(void);
static void delete_other_editor_windows(void);
static void select_other_editor_window(void);
static void cycle_editor_buffer(int direction);
static void save_session(void);
static void autosave_file_now(void);
static void activate_buffer_raw(int index);
static void save_active_window_view(void);
static void load_editor_window(int index);
static int canonical_visit_path(const char *path, char *out, size_t outsz);
static int terminal_input_disconnected(int fd);
static int forward_files_to_workspace(int argc, char **argv);
static int poll_workspace_requests(void);
static int start_workspace_server(void);
static void stop_workspace_server(void);

/*
 * Terminal editors such as nvim and emacs -nw are easiest on the eyes when the
 * prose itself is just the terminal's normal face. The terminal owns the actual
 * font choice; simplewords only chooses attributes and spacing.
 */
static attr_t body_attr(void)
{
    return A_NORMAL;
}

static attr_t selection_attr(void)
{
    return A_REVERSE;
}

static attr_t chrome_attr(void)
{
    return A_REVERSE;
}

static int env_enabled(const char *name)
{
    const char *value = getenv(name);

    return value && strcmp(value, "1") == 0;
}

static ControlXBufferCommand control_x_buffer_command_for_key(int ch)
{
    if (ch == 'b' || ch == 'B')
        return CONTROL_X_BUFFER_COMMAND_NEW;
    if (ch == 2)
        return CONTROL_X_BUFFER_COMMAND_LIST;
    return CONTROL_X_BUFFER_COMMAND_NONE;
}

static long long monotonic_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;
    return (long long)now.tv_sec * 1000LL + now.tv_nsec / 1000000LL;
}

static int write_terminal_control(const char *sequence)
{
    size_t length;
    size_t offset = 0;

    if (!sequence || !isatty(STDOUT_FILENO))
        return 0;

    (void)fflush(stdout);
    length = strlen(sequence);
    while (offset < length) {
        ssize_t written = write(STDOUT_FILENO, sequence + offset,
                                length - offset);

        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            return 0;
        offset += (size_t)written;
    }
    return 1;
}

static void enable_bracketed_paste(void)
{
    if (!bracketed_paste_mode_enabled &&
        write_terminal_control(BRACKETED_PASTE_ENABLE))
        bracketed_paste_mode_enabled = 1;
}

static void disable_bracketed_paste(void)
{
    if (!bracketed_paste_mode_enabled)
        return;

    (void)write_terminal_control(BRACKETED_PASTE_DISABLE);
    bracketed_paste_mode_enabled = 0;
}

static void configure_settle_options(void)
{
    int settle = env_enabled("SW_SETTLE");

    center_lock_enabled = settle || env_enabled("SW_CENTER_LOCK");
    windowed_redraw_enabled = settle || env_enabled("SW_WINDOWED_REDRAW");
    idle_cursor_enabled = settle || env_enabled("SW_IDLE_CURSOR");
    if (settle)
        distraction_free = 1;
}

static void trim_config_setting(char *value)
{
    char *start = value;
    size_t len;

    while (*start == ' ' || *start == '\t')
        start++;
    if (start != value)
        memmove(value, start, strlen(start) + 1);

    len = strlen(value);
    while (len > 0 && (value[len - 1] == ' ' ||
                       value[len - 1] == '\t' ||
                       value[len - 1] == '\r' ||
                       value[len - 1] == '\n'))
        value[--len] = '\0';

    if (len >= 2 && ((value[0] == '"' && value[len - 1] == '"') ||
                     (value[0] == '\'' && value[len - 1] == '\''))) {
        memmove(value, value + 1, len - 2);
        value[len - 2] = '\0';
    }
}

static int config_bool_value(const char *value)
{
    return strcasecmp(value, "true") == 0 ||
           strcasecmp(value, "yes") == 0 ||
           strcasecmp(value, "on") == 0 ||
           strcmp(value, "1") == 0;
}

static int clamp_typewriter_volume(const char *value, int fallback)
{
    char *end = NULL;
    long parsed;

    errno = 0;
    parsed = strtol(value, &end, 10);
    if (end == value)
        return fallback;
    while (*end == ' ' || *end == '\t')
        end++;
    if (*end != '\0')
        return fallback;
    if (errno == ERANGE && parsed > 0)
        return 100;
    if (errno == ERANGE && parsed < 0)
        return 0;
    if (parsed < 0)
        return 0;
    if (parsed > 100)
        return 100;
    return (int)parsed;
}

static int expand_typewriter_sound_path(const char *input,
                                        char *out, size_t outsz)
{
    const char *home = getenv("HOME");
    int written;

    if (!input || !*input || !out || outsz == 0)
        return 0;

    if (input[0] == '~' && (input[1] == '\0' || input[1] == '/')) {
        if (!home || !*home)
            return 0;
        written = snprintf(out, outsz, "%s%s", home, input + 1);
    } else if (strncmp(input, "$HOME", 5) == 0 &&
               (input[5] == '\0' || input[5] == '/')) {
        if (!home || !*home)
            return 0;
        written = snprintf(out, outsz, "%s%s", home, input + 5);
    } else {
        written = snprintf(out, outsz, "%s", input);
    }

    if (written < 0 || (size_t)written >= outsz) {
        out[0] = '\0';
        return 0;
    }
    return 1;
}

static void expand_configured_sound_path(char *path, size_t path_size)
{
    char expanded[PATH_MAX];

    if (expand_typewriter_sound_path(path, expanded, sizeof(expanded)))
        snprintf(path, path_size, "%s", expanded);
    else if (path_size > 0)
        path[0] = '\0';
}

static void load_simplewords_config(void)
{
    const char *home = getenv("HOME");
    char config_path[PATH_MAX];
    FILE *fp = NULL;

    config.typewriter_sound = 0;
    snprintf(config.typewriter_sound_file,
             sizeof(config.typewriter_sound_file), "%s",
             TYPEWRITER_SOUND_DEFAULT_FILE);
    snprintf(config.typewriter_sound_alt_file,
             sizeof(config.typewriter_sound_alt_file), "%s",
             TYPEWRITER_SOUND_ALT_DEFAULT_FILE);
    snprintf(config.typewriter_sound_space_file,
             sizeof(config.typewriter_sound_space_file), "%s",
             TYPEWRITER_SOUND_SPACE_DEFAULT_FILE);
    snprintf(config.typewriter_sound_enter_file,
             sizeof(config.typewriter_sound_enter_file), "%s",
             TYPEWRITER_SOUND_ENTER_DEFAULT_FILE);
    snprintf(config.typewriter_sound_delete_file,
             sizeof(config.typewriter_sound_delete_file), "%s",
             TYPEWRITER_SOUND_DELETE_DEFAULT_FILE);
    config.typewriter_sound_volume = 70;

    if (home && *home &&
        snprintf(config_path, sizeof(config_path),
                 "%s/.config/simplewords/config", home) > 0 &&
        strlen(config_path) < sizeof(config_path) - 1)
        fp = fopen(config_path, "r");

    if (fp) {
        char line[PATH_MAX + 128];

        while (fgets(line, sizeof(line), fp)) {
            char *comment = strchr(line, '#');
            char *equals;
            char *key;
            char *value;

            if (comment)
                *comment = '\0';
            equals = strchr(line, '=');
            if (!equals)
                continue;
            *equals = '\0';
            key = line;
            value = equals + 1;
            trim_config_setting(key);
            trim_config_setting(value);

            if (strcmp(key, "typewriter_sound") == 0) {
                config.typewriter_sound = config_bool_value(value);
            } else if (strcmp(key, "typewriter_sound_file") == 0) {
                snprintf(config.typewriter_sound_file,
                         sizeof(config.typewriter_sound_file), "%s", value);
            } else if (strcmp(key, "typewriter_sound_alt_file") == 0) {
                snprintf(config.typewriter_sound_alt_file,
                         sizeof(config.typewriter_sound_alt_file), "%s",
                         value);
            } else if (strcmp(key, "typewriter_sound_space_file") == 0) {
                snprintf(config.typewriter_sound_space_file,
                         sizeof(config.typewriter_sound_space_file), "%s",
                         value);
            } else if (strcmp(key, "typewriter_sound_enter_file") == 0) {
                snprintf(config.typewriter_sound_enter_file,
                         sizeof(config.typewriter_sound_enter_file), "%s",
                         value);
            } else if (strcmp(key, "typewriter_sound_delete_file") == 0) {
                snprintf(config.typewriter_sound_delete_file,
                         sizeof(config.typewriter_sound_delete_file), "%s",
                         value);
            } else if (strcmp(key, "typewriter_sound_volume") == 0) {
                config.typewriter_sound_volume =
                    clamp_typewriter_volume(value,
                                            config.typewriter_sound_volume);
            }
        }
        fclose(fp);
    }

    expand_configured_sound_path(config.typewriter_sound_file,
                                 sizeof(config.typewriter_sound_file));
    expand_configured_sound_path(config.typewriter_sound_alt_file,
                                 sizeof(config.typewriter_sound_alt_file));
    expand_configured_sound_path(config.typewriter_sound_space_file,
                                 sizeof(config.typewriter_sound_space_file));
    expand_configured_sound_path(config.typewriter_sound_enter_file,
                                 sizeof(config.typewriter_sound_enter_file));
    expand_configured_sound_path(config.typewriter_sound_delete_file,
                                 sizeof(config.typewriter_sound_delete_file));
}

static int save_typewriter_sound_setting(void)
{
    const char *home = getenv("HOME");
    char config_path[PATH_MAX];
    char temp_path[PATH_MAX];
    char line[PATH_MAX + 128];
    FILE *input = NULL;
    FILE *output = NULL;
    int fd = -1;
    int found = 0;
    int ok = 0;

    if (!home || !*home ||
        snprintf(config_path, sizeof(config_path),
                 "%s/.config/simplewords/config", home) <= 0 ||
        strlen(config_path) >= sizeof(config_path) - 1 ||
        snprintf(temp_path, sizeof(temp_path), "%s.tmp.XXXXXX", config_path) <= 0 ||
        strlen(temp_path) >= sizeof(temp_path) - 1)
        return 0;

    input = fopen(config_path, "r");
    if (!input)
        return 0;

    fd = mkstemp(temp_path);
    if (fd < 0)
        goto done;
    output = fdopen(fd, "w");
    if (!output)
        goto done;
    fd = -1;

    while (fgets(line, sizeof(line), input)) {
        char setting[PATH_MAX + 128];
        char *key;
        char *equals;
        char *comment;

        snprintf(setting, sizeof(setting), "%s", line);
        comment = strchr(setting, '#');
        if (comment)
            *comment = '\0';
        equals = strchr(setting, '=');
        if (equals) {
            *equals = '\0';
            key = setting;
            trim_config_setting(key);
            if (strcmp(key, "typewriter_sound") == 0) {
                if (fprintf(output, "typewriter_sound=%s\n",
                            config.typewriter_sound ? "true" : "false") < 0)
                    goto done;
                found = 1;
                continue;
            }
        }
        if (fputs(line, output) == EOF)
            goto done;
    }
    if (ferror(input))
        goto done;
    if (!found && fprintf(output, "typewriter_sound=%s\n",
                          config.typewriter_sound ? "true" : "false") < 0)
        goto done;
    if (fflush(output) != 0 || fsync(fileno(output)) != 0)
        goto done;
    if (fclose(output) != 0) {
        output = NULL;
        goto done;
    }
    output = NULL;
    if (rename(temp_path, config_path) != 0)
        goto done;
    ok = 1;

done:
    if (input)
        fclose(input);
    if (output)
        fclose(output);
    else if (fd >= 0)
        close(fd);
    if (!ok)
        unlink(temp_path);
    return ok;
}

static const char *typewriter_sound_path(TypewriterSound sound)
{
    switch (sound) {
    case TYPEWRITER_SOUND_KEY:
        return config.typewriter_sound_file;
    case TYPEWRITER_SOUND_KEY_ALT:
        return config.typewriter_sound_alt_file;
    case TYPEWRITER_SOUND_SPACE:
        return config.typewriter_sound_space_file;
    case TYPEWRITER_SOUND_ENTER:
        return config.typewriter_sound_enter_file;
    case TYPEWRITER_SOUND_DELETE:
        return config.typewriter_sound_delete_file;
    default:
        return "";
    }
}

static const TypewriterSample *typewriter_sample_for_sound(
    const TypewriterAudio *audio, TypewriterSound sound)
{
    const TypewriterSample *sample;

    if (!audio || sound < 0 || sound >= TYPEWRITER_SOUND_COUNT)
        return NULL;
    sample = &audio->samples[sound];
    if (sample->pcm_frames && sample->frame_count > 0)
        return sample;

    /* WriteMonkey's old schemes predate key2/delete. Preserve its fallback to
     * the ordinary key sample for a partial custom scheme. */
    if (sound == TYPEWRITER_SOUND_KEY_ALT ||
        sound == TYPEWRITER_SOUND_DELETE) {
        sample = &audio->samples[TYPEWRITER_SOUND_KEY];
        if (sample->pcm_frames && sample->frame_count > 0)
            return sample;
    }
    return NULL;
}

static void clear_typewriter_audio_samples(void)
{
    for (unsigned int i = 0; i < TYPEWRITER_SOUND_COUNT; i++) {
        if (typewriter_audio.samples[i].pcm_frames)
            ma_free(typewriter_audio.samples[i].pcm_frames, NULL);
        typewriter_audio.samples[i].pcm_frames = NULL;
        typewriter_audio.samples[i].frame_count = 0;
    }
    for (unsigned int i = 0; i < TYPEWRITER_AUDIO_VOICES; i++) {
        typewriter_audio.voices[i].sample = NULL;
        typewriter_audio.voices[i].cursor = 0;
    }
    typewriter_audio.next_voice = 0;
    typewriter_audio.sample_rate = 0;
    typewriter_audio.volume = 0.0f;
    typewriter_audio.device_initialized = 0;
    atomic_store_explicit(&typewriter_audio.event_head, 0,
                          memory_order_relaxed);
    atomic_store_explicit(&typewriter_audio.event_tail, 0,
                          memory_order_relaxed);
}

static int decode_typewriter_sample(const char *path, ma_uint32 sample_rate,
                                    TypewriterSample *sample,
                                    ma_uint32 *decoded_rate)
{
    ma_decoder_config decoder_config;
    void *decoded_frames = NULL;
    ma_uint64 frame_count = 0;
    struct stat sound_stat;

    if (!path || !*path || !sample ||
        stat(path, &sound_stat) != 0 || !S_ISREG(sound_stat.st_mode) ||
        access(path, R_OK) != 0)
        return 0;

    decoder_config = ma_decoder_config_init(ma_format_f32,
                                             TYPEWRITER_AUDIO_CHANNELS,
                                             sample_rate);
    if (ma_decode_file(path, &decoder_config, &frame_count,
                       &decoded_frames) != MA_SUCCESS ||
        !decoded_frames || frame_count == 0 ||
        decoder_config.sampleRate == 0) {
        if (decoded_frames)
            ma_free(decoded_frames, NULL);
        return 0;
    }

    sample->pcm_frames = decoded_frames;
    sample->frame_count = frame_count;
    if (decoded_rate)
        *decoded_rate = decoder_config.sampleRate;
    return 1;
}

static int start_typewriter_audio(void)
{
    ma_device_config device_config;
    int saved_stderr = -1;
    int null_stderr = -1;
    int volume;
    int initialized = 0;

    if (!config.typewriter_sound)
        return 0;

    clear_typewriter_audio_samples();
    if (!decode_typewriter_sample(config.typewriter_sound_file, 0,
                                  &typewriter_audio.samples[
                                      TYPEWRITER_SOUND_KEY],
                                  &typewriter_audio.sample_rate))
        return 0;

    for (TypewriterSound sound = TYPEWRITER_SOUND_KEY_ALT;
         sound < TYPEWRITER_SOUND_COUNT; sound++) {
        (void)decode_typewriter_sample(typewriter_sound_path(sound),
                                       typewriter_audio.sample_rate,
                                       &typewriter_audio.samples[sound], NULL);
    }

    volume = config.typewriter_sound_volume;
    if (volume < 0)
        volume = 0;
    else if (volume > 100)
        volume = 100;
    typewriter_audio.volume = (float)volume / 100.0f;

    device_config = ma_device_config_init(ma_device_type_playback);
    device_config.playback.format = ma_format_f32;
    device_config.playback.channels = TYPEWRITER_AUDIO_CHANNELS;
    device_config.sampleRate = typewriter_audio.sample_rate;
    device_config.periodSizeInMilliseconds = 5;
    device_config.periods = 2;
    device_config.performanceProfile = ma_performance_profile_low_latency;
    device_config.dataCallback = typewriter_audio_callback;
    device_config.pUserData = &typewriter_audio;

    /* Some platform backends write diagnostics directly to stderr while they
     * probe the default device. Keep an unavailable device invisible to the
     * terminal UI, just like an unavailable sound file. */
    fflush(stderr);
    saved_stderr = dup(STDERR_FILENO);
    if (saved_stderr < 0)
        goto fail;
    null_stderr = open("/dev/null", O_WRONLY);
    if (null_stderr < 0 || dup2(null_stderr, STDERR_FILENO) < 0)
        goto fail;
    close(null_stderr);
    null_stderr = -1;

    if (ma_device_init(NULL, &device_config, &typewriter_audio.device) !=
        MA_SUCCESS)
        goto fail;
    initialized = 1;
    if (ma_device_start(&typewriter_audio.device) != MA_SUCCESS)
        goto fail;

    fflush(stderr);
    (void)dup2(saved_stderr, STDERR_FILENO);
    close(saved_stderr);
    typewriter_audio.device_initialized = 1;
    return 1;

fail:
    if (initialized)
        ma_device_uninit(&typewriter_audio.device);
    if (null_stderr >= 0)
        close(null_stderr);
    if (saved_stderr >= 0) {
        fflush(stderr);
        (void)dup2(saved_stderr, STDERR_FILENO);
        close(saved_stderr);
    }
    clear_typewriter_audio_samples();
    return 0;
}

static int request_typewriter_sound(TypewriterSound sound)
{
    unsigned int head;
    unsigned int next;

    if (!config.typewriter_sound ||
        sound < 0 || sound >= TYPEWRITER_SOUND_COUNT)
        return 0;

#ifdef SIMPLEWORDS_TYPEWRITER_TEST
    if (typewriter_audio_test_mode) {
        typewriter_audio_test_requests[sound]++;
        return 1;
    }
#endif

    if (!typewriter_audio.device_initialized ||
        !typewriter_sample_for_sound(&typewriter_audio, sound))
        return 0;

    head = atomic_load_explicit(&typewriter_audio.event_head,
                                memory_order_relaxed);
    next = (head + 1) % TYPEWRITER_AUDIO_QUEUE_SIZE;
    if (next == atomic_load_explicit(&typewriter_audio.event_tail,
                                     memory_order_acquire))
        return 0;

    typewriter_audio.event_queue[head] = (unsigned char)sound;
    atomic_store_explicit(&typewriter_audio.event_head, next,
                          memory_order_release);
    return 1;
}

static TypewriterSound typewriter_sound_for_printable(int ch)
{
    int lower;

    if (ch == ' ')
        return TYPEWRITER_SOUND_SPACE;

    lower = tolower((unsigned char)ch);
    if (lower == 'a' || lower == 'e' || lower == 'i' || lower == 'n' ||
        lower == 'o' || lower == 's' || lower == 't' || lower == 'u')
        return TYPEWRITER_SOUND_KEY_ALT;
    return TYPEWRITER_SOUND_KEY;
}

static void stop_typewriter_audio(void)
{
    if (typewriter_audio.device_initialized) {
        typewriter_audio.device_initialized = 0;
        ma_device_uninit(&typewriter_audio.device);
    }
    clear_typewriter_audio_samples();
}

static void typewriter_audio_callback(ma_device *device, void *output,
                                      const void *input, ma_uint32 frame_count)
{
    TypewriterAudio *audio = device ? device->pUserData : NULL;
    float *output_frames = output;
    unsigned int head;
    unsigned int tail;

    (void)input;
    if (!output_frames)
        return;
    memset(output_frames, 0,
           (size_t)frame_count * TYPEWRITER_AUDIO_CHANNELS * sizeof(float));
    if (!audio || !audio->samples[TYPEWRITER_SOUND_KEY].pcm_frames)
        return;

    tail = atomic_load_explicit(&audio->event_tail, memory_order_relaxed);
    head = atomic_load_explicit(&audio->event_head, memory_order_acquire);
    while (tail != head) {
        TypewriterSound sound = (TypewriterSound)audio->event_queue[tail];
        const TypewriterSample *sample =
            typewriter_sample_for_sound(audio, sound);

        tail = (tail + 1) % TYPEWRITER_AUDIO_QUEUE_SIZE;
        if (sample) {
            TypewriterVoice *voice = &audio->voices[audio->next_voice];

            voice->sample = sample;
            voice->cursor = 0;
            audio->next_voice =
                (audio->next_voice + 1) % TYPEWRITER_AUDIO_VOICES;
        }
    }
    atomic_store_explicit(&audio->event_tail, tail, memory_order_release);

    for (unsigned int voice_index = 0;
         voice_index < TYPEWRITER_AUDIO_VOICES; voice_index++) {
        TypewriterVoice *voice = &audio->voices[voice_index];
        const TypewriterSample *sample = voice->sample;
        ma_uint64 available;
        ma_uint32 to_mix;

        if (!sample || voice->cursor >= sample->frame_count) {
            voice->sample = NULL;
            continue;
        }
        available = sample->frame_count - voice->cursor;
        to_mix = available < frame_count ? (ma_uint32)available : frame_count;
        for (ma_uint32 frame = 0; frame < to_mix; frame++) {
            size_t source = (size_t)(voice->cursor + frame) *
                            TYPEWRITER_AUDIO_CHANNELS;
            size_t destination = (size_t)frame *
                                 TYPEWRITER_AUDIO_CHANNELS;

            for (unsigned int channel = 0;
                 channel < TYPEWRITER_AUDIO_CHANNELS; channel++) {
                output_frames[destination + channel] +=
                    sample->pcm_frames[source + channel] * audio->volume;
            }
        }
        voice->cursor += to_mix;
        if (voice->cursor >= sample->frame_count)
            voice->sample = NULL;
    }
}

static void set_cursor_visibility(int visible)
{
    visible = visible ? 1 : 0;
    if (visible == cursor_visibility)
        return;

    curs_set(visible);
    cursor_visibility = visible;
}

static char *new_line(const char *s)
{
    char *p = calloc(MAX_LINE, 1);
    if (!p) {
        endwin();
        perror("calloc");
        exit(1);
    }

    if (s) {
        strncpy(p, s, MAX_LINE - 1);
        p[MAX_LINE - 1] = '\0';
    }

    return p;
}

static void set_status(const char *msg)
{
    strncpy(status_msg, msg, sizeof(status_msg) - 1);
    status_msg[sizeof(status_msg) - 1] = '\0';
    status_time = time(NULL);
}

static void clear_status(void)
{
    status_msg[0] = '\0';
}

static void make_untitled_name(void)
{
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);

    snprintf(untitled_name, sizeof(untitled_name),
             "untitled-%04d%02d%02d-%02d%02d%02d",
             tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
             tm->tm_hour, tm->tm_min, tm->tm_sec);
}

static void display_name(char *out, size_t outsz)
{
    if (filename[0]) {
        const char *slash = strrchr(filename, '/');
        snprintf(out, outsz, "%s", slash ? slash + 1 : filename);
    } else {
        snprintf(out, outsz, "%s", untitled_name);
    }
}

static int pos_before(int ay, int ax, int by, int bx)
{
    return ay < by || (ay == by && ax < bx);
}

static void *xmalloc(size_t size)
{
    void *p = malloc(size ? size : 1);

    if (!p) {
        endwin();
        perror("malloc");
        exit(1);
    }

    return p;
}

static void *xrealloc(void *ptr, size_t size)
{
    void *p = realloc(ptr, size ? size : 1);

    if (!p) {
        endwin();
        perror("realloc");
        exit(1);
    }

    return p;
}

static char *xstrndup_local(const char *s, size_t len)
{
    char *copy = xmalloc(len + 1);

    if (len)
        memcpy(copy, s, len);
    copy[len] = '\0';
    return copy;
}

static char *xstrdup_local(const char *s)
{
    if (!s)
        s = "";
    return xstrndup_local(s, strlen(s));
}

static void free_undo_op(UndoOp *op)
{
    free(op->old_text);
    free(op->new_text);
    memset(op, 0, sizeof(*op));
}

static void free_undo_group(UndoGroup *group)
{
    for (int i = 0; i < group->op_count; i++)
        free_undo_op(&group->ops[i]);
    free(group->ops);
    memset(group, 0, sizeof(*group));
}

static void clear_stack(UndoGroup *stack, int *count)
{
    for (int i = 0; i < *count; i++)
        free_undo_group(&stack[i]);
    *count = 0;
}

static void discard_pending_undo_group(void)
{
    free_undo_group(&pending_undo_group);
    undo_group_active = 0;
    undo_group_depth = 0;
    burst_chars = 0;
    last_type_time = 0;
}

static void clear_undo_history(void)
{
    discard_pending_undo_group();
    clear_stack(undo_stack, &undo_count);
    clear_stack(redo_stack, &redo_count);
}

static void init_pending_undo_group(void)
{
    memset(&pending_undo_group, 0, sizeof(pending_undo_group));
    pending_undo_group.before_cy = cy;
    pending_undo_group.before_cx = cx;
    pending_undo_group.before_top = top;
    pending_undo_group.after_cy = cy;
    pending_undo_group.after_cx = cx;
    pending_undo_group.after_top = top;
    undo_group_active = 1;
}

static void ensure_pending_undo_group(void)
{
    if (!undo_group_active)
        init_pending_undo_group();
}

static void push_group(UndoGroup *stack, int *count, UndoGroup *group)
{
    if (!group->op_count) {
        free_undo_group(group);
        return;
    }

    if (*count == UNDO_DEPTH) {
        free_undo_group(&stack[0]);
        memmove(&stack[0], &stack[1],
                sizeof(stack[0]) * (UNDO_DEPTH - 1));
        *count = UNDO_DEPTH - 1;
    }

    stack[(*count)++] = *group;
    memset(group, 0, sizeof(*group));
}

static UndoGroup pop_group(UndoGroup *stack, int *count)
{
    UndoGroup group;

    (*count)--;
    group = stack[*count];
    memset(&stack[*count], 0, sizeof(stack[*count]));
    return group;
}

static void finalize_pending_undo_group(void)
{
    if (!undo_group_active)
        return;

    pending_undo_group.after_cy = cy;
    pending_undo_group.after_cx = cx;
    pending_undo_group.after_top = top;
    push_group(undo_stack, &undo_count, &pending_undo_group);
    undo_group_active = 0;
    undo_group_depth = 0;
}

static void begin_undo_group(void)
{
    ensure_pending_undo_group();
    undo_group_depth++;
}

static void end_undo_group(void)
{
    if (undo_group_depth <= 0)
        return;

    undo_group_depth--;
    if (undo_group_depth == 0)
        finalize_pending_undo_group();
}

static void append_undo_op(UndoOp op)
{
    ensure_pending_undo_group();

    if (pending_undo_group.op_count == 0)
        clear_stack(redo_stack, &redo_count);

    if (pending_undo_group.op_count == pending_undo_group.op_capacity) {
        int new_capacity = pending_undo_group.op_capacity ?
                           pending_undo_group.op_capacity * 2 : 8;

        pending_undo_group.ops = xrealloc(pending_undo_group.ops,
                                          sizeof(pending_undo_group.ops[0]) *
                                          (size_t)new_capacity);
        pending_undo_group.op_capacity = new_capacity;
    }

    pending_undo_group.ops[pending_undo_group.op_count++] = op;
    pending_undo_group.after_cy = cy;
    pending_undo_group.after_cx = cx;
    pending_undo_group.after_top = top;
}

/*
 * Undo is operation based. Each edit records the exact replaced range plus the
 * text that was removed and inserted; no whole-document copy is taken per edit.
 * A group owns one or more replacement operations, so typing bursts, paste, and
 * selection deletion can still undo as a single user-visible action.
 */
static int edit_range_valid(int sy, int sx, int ey, int ex)
{
    if (sy < 0 || ey < sy || ey >= line_count)
        return 0;
    if (sx < 0 || ex < 0)
        return 0;
    if (sx > (int)strlen(lines[sy]) || ex > (int)strlen(lines[ey]))
        return 0;
    if (sy == ey && sx > ex)
        return 0;
    return 1;
}

static char *range_text(int sy, int sx, int ey, int ex)
{
    size_t len = 0;
    char *text;
    char *out;

    if (sy == ey) {
        len = (size_t)(ex - sx);
    } else {
        len += strlen(lines[sy] + sx) + 1;
        for (int y = sy + 1; y < ey; y++)
            len += strlen(lines[y]) + 1;
        len += (size_t)ex;
    }

    text = xmalloc(len + 1);
    out = text;

    if (sy == ey) {
        if (ex > sx) {
            memcpy(out, lines[sy] + sx, (size_t)(ex - sx));
            out += ex - sx;
        }
    } else {
        size_t part_len = strlen(lines[sy] + sx);

        if (part_len) {
            memcpy(out, lines[sy] + sx, part_len);
            out += part_len;
        }
        *out++ = '\n';

        for (int y = sy + 1; y < ey; y++) {
            part_len = strlen(lines[y]);
            if (part_len) {
                memcpy(out, lines[y], part_len);
                out += part_len;
            }
            *out++ = '\n';
        }

        if (ex > 0) {
            memcpy(out, lines[ey], (size_t)ex);
            out += ex;
        }
    }

    *out = '\0';
    return text;
}

static EditPos text_end_pos(int sy, int sx, const char *text)
{
    EditPos end = { sy, sx };

    if (!text)
        return end;

    for (const char *p = text; *p; p++) {
        if (*p == '\n') {
            end.y++;
            end.x = 0;
        } else {
            end.x++;
        }
    }

    return end;
}

typedef struct {
    char **items;
    int count;
} TextPieces;

static TextPieces split_text_lines(const char *text)
{
    TextPieces pieces;
    const char *start;
    int index = 0;

    if (!text)
        text = "";

    pieces.count = 1;
    for (const char *p = text; *p; p++) {
        if (*p == '\n')
            pieces.count++;
    }

    pieces.items = xmalloc(sizeof(pieces.items[0]) * (size_t)pieces.count);
    start = text;
    for (const char *p = text; ; p++) {
        if (*p == '\n' || *p == '\0') {
            pieces.items[index++] = xstrndup_local(start, (size_t)(p - start));
            if (*p == '\0')
                break;
            start = p + 1;
        }
    }

    return pieces;
}

static void free_text_pieces(TextPieces *pieces)
{
    for (int i = 0; i < pieces->count; i++)
        free(pieces->items[i]);
    free(pieces->items);
    memset(pieces, 0, sizeof(*pieces));
}

static int replacement_line_too_long(size_t len)
{
    return len > MAX_LINE - 1;
}

static int replace_range_raw(int sy, int sx, int ey, int ex,
                             const char *text, EditPos *new_end)
{
    TextPieces pieces;
    char **replacement;
    int old_line_count;
    int new_doc_line_count;
    int tail_count;
    size_t suffix_len;

    if (!edit_range_valid(sy, sx, ey, ex))
        return 0;

    pieces = split_text_lines(text);
    old_line_count = ey - sy + 1;
    new_doc_line_count = line_count - old_line_count + pieces.count;
    if (new_doc_line_count < 1 || new_doc_line_count > MAX_LINES) {
        free_text_pieces(&pieces);
        set_status("Document is full");
        return 0;
    }

    suffix_len = strlen(lines[ey] + ex);
    if (pieces.count == 1) {
        if (replacement_line_too_long((size_t)sx +
                                      strlen(pieces.items[0]) +
                                      suffix_len)) {
            free_text_pieces(&pieces);
            set_status("Line is full");
            return 0;
        }
    } else {
        if (replacement_line_too_long((size_t)sx +
                                      strlen(pieces.items[0]))) {
            free_text_pieces(&pieces);
            set_status("Line is full");
            return 0;
        }
        for (int i = 1; i < pieces.count - 1; i++) {
            if (replacement_line_too_long(strlen(pieces.items[i]))) {
                free_text_pieces(&pieces);
                set_status("Line is full");
                return 0;
            }
        }
        if (replacement_line_too_long(strlen(pieces.items[pieces.count - 1]) +
                                      suffix_len)) {
            free_text_pieces(&pieces);
            set_status("Line is full");
            return 0;
        }
    }

    replacement = xmalloc(sizeof(replacement[0]) * (size_t)pieces.count);
    memset(replacement, 0, sizeof(replacement[0]) * (size_t)pieces.count);
    for (int i = 0; i < pieces.count; i++)
        replacement[i] = new_line(NULL);

    memcpy(replacement[0], lines[sy], (size_t)sx);
    replacement[0][sx] = '\0';
    strcat(replacement[0], pieces.items[0]);
    if (pieces.count == 1) {
        strcat(replacement[0], lines[ey] + ex);
    } else {
        for (int i = 1; i < pieces.count - 1; i++)
            strcpy(replacement[i], pieces.items[i]);
        strcpy(replacement[pieces.count - 1],
               pieces.items[pieces.count - 1]);
        strcat(replacement[pieces.count - 1], lines[ey] + ex);
    }

    if (new_end)
        *new_end = text_end_pos(sy, sx, text);

    for (int y = sy; y <= ey; y++)
        free(lines[y]);

    tail_count = line_count - ey - 1;
    if (tail_count > 0) {
        memmove(&lines[sy + pieces.count], &lines[ey + 1],
                sizeof(char *) * (size_t)tail_count);
    }

    for (int i = 0; i < pieces.count; i++)
        lines[sy + i] = replacement[i];

    line_count = new_doc_line_count;
    if (new_end) {
        cy = new_end->y;
        cx = new_end->x;
    }
    clamp_cursor();

    free(replacement);
    free_text_pieces(&pieces);
    return 1;
}

static int replace_range_recorded(int sy, int sx, int ey, int ex,
                                  const char *text)
{
    UndoOp op;
    EditPos end;

    if (!edit_range_valid(sy, sx, ey, ex))
        return 0;

    ensure_pending_undo_group();
    memset(&op, 0, sizeof(op));
    op.start.y = sy;
    op.start.x = sx;
    op.old_end.y = ey;
    op.old_end.x = ex;
    op.new_end = text_end_pos(sy, sx, text);
    op.old_text = range_text(sy, sx, ey, ex);
    op.new_text = xstrdup_local(text);

    if (!replace_range_raw(sy, sx, ey, ex, text, &end)) {
        free_undo_op(&op);
        return 0;
    }

    append_undo_op(op);
    return 1;
}

static void break_undo_burst(void)
{
    burst_chars = 0;
    last_type_time = 0;
    if (undo_group_depth == 0)
        finalize_pending_undo_group();
}

static void maybe_save_typing_undo(void)
{
    time_t now = time(NULL);

    if (undo_group_depth > 0) {
        ensure_pending_undo_group();
        return;
    }

    if (!burst_chars || now - last_type_time > 1 || burst_chars > 30) {
        finalize_pending_undo_group();
        ensure_pending_undo_group();
        burst_chars = 0;
    }

    burst_chars++;
    last_type_time = now;
}

static void mark_edit(void)
{
    SET_DIRTY(1, "mark_edit");
    SET_AUTOSAVE_DIRTY(1, "mark_edit");
    SET_LAST_EDIT_TIME(time(NULL), "mark_edit");
    invalidate_wrap_cache();
    goal_col = -1;
    clear_cursor_affinity();
    clamp_top();
    screen_cache_valid = 0;
}

static int utf8_char_width(const char *s, int *bytes_used)
{
    mbstate_t st;
    wchar_t wc;
    size_t n;
    int w;

    memset(&st, 0, sizeof(st));
    n = mbrtowc(&wc, s, MB_CUR_MAX, &st);

    if (n == (size_t)-1 || n == (size_t)-2 || n == 0) {
        if (bytes_used)
            *bytes_used = 1;
        return 1;
    }

    if (bytes_used)
        *bytes_used = (int)n;

    w = wcwidth(wc);
    return w < 1 ? 1 : w;
}

static int utf8_decode(const char *s, wchar_t *wc, int *bytes_used)
{
    mbstate_t st;
    size_t n;
    int w;

    memset(&st, 0, sizeof(st));
    n = mbrtowc(wc, s, MB_CUR_MAX, &st);
    if (n == (size_t)-1 || n == (size_t)-2 || n == 0) {
        *wc = L'\xfffd';
        *bytes_used = 1;
        return 1;
    }

    *bytes_used = (int)n;
    w = wcwidth(*wc);
    return w < 1 ? 1 : w;
}

static int char_visual_width(int col, char c)
{
    if (c == '\t')
        return TAB_WIDTH - (col % TAB_WIDTH);
    return 1;
}

static BodyGeometry body_geometry(void)
{
    BodyGeometry geo;
    int cols = pane_rendering ? pane_rect.width :
               (COLS > 0 ? COLS : config.text_width);
    int rows = pane_rendering ? pane_rect.height :
               (LINES > 0 ? LINES : config.top_pad + 1);
    int origin_x = pane_rendering ? pane_rect.x : 0;
    int origin_y = pane_rendering ? pane_rect.y : 0;
    int inner_left = origin_x + (pane_rendering ? 1 : 0);
    int inner_width = cols - (pane_rendering ? 2 : 0);

    if (inner_width < 1)
        inner_width = 1;

    geo.screen_left = origin_x;
    geo.screen_right = origin_x + cols;
    geo.screen_top = origin_y;
    geo.screen_bottom = origin_y + rows;

    geo.left = inner_left + (inner_width - config.text_width) / 2;
    if (geo.left < inner_left)
        geo.left = inner_left;

    geo.body_width = config.text_width;
    if (geo.body_width > origin_x + cols - (pane_rendering ? 1 : 0) - geo.left)
        geo.body_width = origin_x + cols - (pane_rendering ? 1 : 0) - geo.left;
    if (geo.body_width < 1)
        geo.body_width = 1;

    if (pane_rendering) {
        geo.top_pad = origin_y + 1;
    } else {
        geo.top_pad = config.top_pad;
        if (geo.top_pad < 0)
            geo.top_pad = 0;
    }

    geo.bottom = pane_rendering ? origin_y + rows - 1 :
                 (distraction_free ? rows : rows - 1);
    if (geo.bottom < origin_y)
        geo.bottom = origin_y;

    geo.visible_rows = geo.bottom - geo.top_pad;
    if (geo.visible_rows < 1)
        geo.visible_rows = 1;

    return geo;
}

static int layout_width(void)
{
    return body_geometry().body_width;
}

static void clear_cursor_affinity(void)
{
    cursor_affinity_line = -1;
    cursor_affinity_x = -1;
    cursor_affinity_doc_row = -1;
    cursor_affinity_col = -1;
}

static void set_cursor_affinity(int line, int index, int doc_row, int col)
{
    cursor_affinity_line = line;
    cursor_affinity_x = index;
    cursor_affinity_doc_row = doc_row;
    cursor_affinity_col = col;
}

static int active_cursor_affinity(int *doc_row, int *col)
{
    if (cursor_affinity_line != cy || cursor_affinity_x != cx ||
        cursor_affinity_doc_row < 0 || cursor_affinity_col < 0)
        return 0;

    *doc_row = cursor_affinity_doc_row;
    *col = cursor_affinity_col;
    return 1;
}

static int wrap_space(char c)
{
    return c == ' ' || c == '\t';
}

static void finish_wrap_row(WrapRow *row, int end, int visual_width)
{
    row->render_end = end;
    row->next_start = end;
    row->cursor_end = end;
    row->visual_width = visual_width;
}

static int char_width_at(const char *line, int index, int col, int *used)
{
    if (line[index] == '\t') {
        if (used)
            *used = 1;
        return char_visual_width(col, line[index]);
    }

    return utf8_char_width(line + index, used);
}

static int measure_visual_width(const char *line, int start, int upto)
{
    int col = 0;

    if (upto < start)
        upto = start;

    for (int i = start; line[i] && i < upto; ) {
        int used = 1;
        int w = char_width_at(line, i, col, &used);

        if (i + used > upto)
            break;
        col += w;
        i += used;
    }

    return col;
}

static void build_wrap_row_width(const char *line, int line_no, int start,
                                 int doc_row, int width, WrapRow *row)
{
    int len = (int)strlen(line);
    int visual_text_capacity;
    int col = 0;
    int last_break = -1;
    int last_break_col = 0;
    int last_space_run_start = -1;

    row->line = line_no;
    row->render_start = start;
    row->render_end = start;
    row->next_start = start;
    row->cursor_start = start;
    row->cursor_end = start;
    row->visual_width = 0;
    row->doc_row = doc_row;

    if (width < 1)
        width = 1;
    visual_text_capacity = width;

    if (len == 0 || start >= len) {
        row->render_start = 0;
        row->render_end = 0;
        row->next_start = 0;
        row->cursor_start = 0;
        row->cursor_end = 0;
        return;
    }

    for (int i = start; i < len; ) {
        int used = 1;
        int w = char_width_at(line, i, col, &used);

        if (col + w > visual_text_capacity) {
            if (last_break > start && last_break < i) {
                /*
                 * Normal word wrap may leave one separator space at the end
                 * of the previous visual row. But a run of typed spaces before
                 * the overflowing word must not become invisible cargo.
                 *
                 * Example: "large          X"
                 * should wrap as:
                 *   large
                 *            X
                 * not:
                 *   large[hidden spaces]
                 *   X
                 */
                if (!wrap_space(line[i]) &&
                    last_space_run_start > start &&
                    i - last_space_run_start > 1) {
                    finish_wrap_row(row, last_space_run_start,
                                    measure_visual_width(line, start,
                                                         last_space_run_start));
                } else {
                    finish_wrap_row(row, last_break, last_break_col);
                }
            } else {
                finish_wrap_row(row, i, col);
                if (row->render_end == start)
                    finish_wrap_row(row, i + used,
                                    w > visual_text_capacity ?
                                    visual_text_capacity : w);
            }
            return;
        }

        col += w;
        if (wrap_space(line[i])) {
            if (i == start || !wrap_space(line[i - 1]))
                last_space_run_start = i;
            last_break = i + used;
            last_break_col = col;
        } else {
            last_space_run_start = -1;
        }
        i += used;
    }

    finish_wrap_row(row, len, col);
}

static void invalidate_wrap_cache(void)
{
    wrap_cache_valid = 0;
}

static void reset_wrap_cache(void)
{
    if (!wrap_cache)
        return;

    for (int i = 0; i < wrap_cache_line_capacity; i++) {
        free(wrap_cache[i].rows);
        wrap_cache[i].rows = NULL;
        wrap_cache[i].count = 0;
        wrap_cache[i].capacity = 0;
    }
    wrap_cache_line_count = 0;
    wrap_cache_width = 0;
    wrap_cache_valid = 0;
}

static void append_cached_wrap_row(LineWrapCache *cache, const WrapRow *row)
{
    WrapRow *grown;

    if (cache->count >= cache->capacity) {
        int new_capacity = cache->capacity ? cache->capacity * 2 : 4;

        grown = realloc(cache->rows,
                        sizeof(*cache->rows) * (size_t)new_capacity);
        if (!grown) {
            endwin();
            perror("realloc");
            exit(1);
        }
        cache->rows = grown;
        cache->capacity = new_capacity;
    }

    cache->rows[cache->count++] = *row;
}

static void build_greedy_wrap_rows_for_line(int line_no, int width,
                                            LineWrapCache *new_cache)
{
    const char *line = lines[line_no];
    int len = (int)strlen(line);
    int start = 0;
    int doc_row = 0;

    new_cache->count = 0;

    if (len == 0) {
        WrapRow row;

        build_wrap_row_width(line, line_no, 0, 0, width, &row);
        append_cached_wrap_row(new_cache, &row);
        return;
    }

    while (start < len) {
        WrapRow row;

        build_wrap_row_width(line, line_no, start, doc_row, width, &row);

        /*
         * Defensive guarantee: wrapping a nonempty line must always consume
         * at least one complete character.
         */
        if (row.next_start <= start) {
            int used = 1;

            (void)char_width_at(line, start, 0, &used);
            finish_wrap_row(&row, start + used,
                            measure_visual_width(line, start, start + used));
        }

        append_cached_wrap_row(new_cache, &row);

        if (wrap_cache_debug)
            fprintf(stderr, "wrap line=%d row=%d start=%d end=%d width=%d reason=greedy\n",
                    line_no, doc_row, row.render_start, row.render_end,
                    row.visual_width);

        if (row.next_start <= start)
            break;
        start = row.next_start;
        doc_row++;
    }
}

static int wrap_cache_invariants_hold(void)
{
    if (!wrap_cache || wrap_cache_line_count != line_count)
        return 0;

    for (int li = 0; li < line_count; li++) {
        int len = (int)strlen(lines[li]);
        int expected_start = 0;

        if (wrap_cache[li].count < 1)
            return 0;

        for (int i = 0; i < wrap_cache[li].count; i++) {
            const WrapRow *row = &wrap_cache[li].rows[i];

            if (row->line != li ||
                row->render_start != expected_start ||
                row->cursor_start != row->render_start ||
                row->cursor_end != row->render_end ||
                row->next_start != row->render_end ||
                row->render_end < row->render_start ||
                row->render_end > len)
                return 0;

            if (len > 0 && row->render_end == row->render_start)
                return 0;

            expected_start = row->render_end;
        }

        if (expected_start != len)
            return 0;
    }

    return 1;
}

static void rebuild_wrap_cache(int width)
{
    LineWrapCache *old_cache = wrap_cache;
    int old_capacity = wrap_cache_line_capacity;
    LineWrapCache *new_cache = NULL;

    if (width < 1)
        width = 1;

    new_cache = calloc((size_t)line_count, sizeof(*new_cache));
    if (!new_cache) {
        endwin();
        perror("calloc");
        exit(1);
    }

    wrap_cache = new_cache;
    wrap_cache_line_capacity = line_count;
    wrap_cache_line_count = line_count;
    wrap_cache_width = width;
    wrap_cache_valid = 0;

    for (int li = 0; li < line_count; li++)
        build_greedy_wrap_rows_for_line(li, width, &wrap_cache[li]);

    if (old_cache) {
        for (int i = 0; i < old_capacity; i++)
            free(old_cache[i].rows);
        free(old_cache);
    }

    wrap_cache_valid = 1;
    assert(wrap_cache_invariants_hold());
}

static void ensure_wrap_cache(void)
{
    int width = layout_width();

    if (wrap_cache_valid &&
        wrap_cache_width == width &&
        wrap_cache_line_count == line_count)
        return;

    wrap_cache_debug = env_enabled("SW_TRACE_WRAP");
    rebuild_wrap_cache(width);
}

static int layout_rows_for_line_width(const char *line, int width)
{
    int len = (int)strlen(line);
    int rows = 0;
    int start = 0;

    if (!len)
        return 1;

    while (start < len) {
        WrapRow row;

        build_wrap_row_width(line, -1, start, rows, width, &row);
        rows++;
        if (row.next_start <= start)
            break;
        start = row.next_start;
    }

    return rows;
}

static int layout_document_visual_rows(void)
{
    int rows = 0;

    ensure_wrap_cache();
    for (int i = 0; i < wrap_cache_line_count; i++)
        rows += wrap_cache[i].count;
    return rows;
}

static int row_col_for_pos(const WrapRow *row, const char *line, int target)
{
    if (target <= row->render_start)
        return 0;
    if (target >= row->render_end)
        return row->visual_width;
    return measure_visual_width(line, row->render_start, target);
}

static int layout_row_for_line_position_width(const char *line, int line_no,
                                              int target, int doc_row_base,
                                              int width, WrapRow *out)
{
    int len = (int)strlen(line);
    int start = 0;
    int row_no = 0;

    if (target < 0)
        target = 0;
    if (target > len)
        target = len;

    if (!len) {
        build_wrap_row_width(line, line_no, 0, doc_row_base, width, out);
        return 1;
    }

    while (start < len) {
        WrapRow row;

        build_wrap_row_width(line, line_no, start, doc_row_base + row_no,
                             width, &row);
        if (target < row.render_end ||
            (target == row.render_end &&
             !(row.visual_width >= width && row.next_start == row.render_end &&
               row.next_start < len))) {
            *out = row;
            return 1;
        }
        if (row.next_start <= start)
            break;
        start = row.next_start;
        row_no++;
    }

    build_wrap_row_width(line, line_no, start, doc_row_base + row_no,
                         width, out);
    return 1;
}

static int layout_row_for_position(int line_no, int target, WrapRow *out)
{
    int doc_row = 0;

    ensure_wrap_cache();

    if (line_no < 0)
        line_no = 0;
    if (line_no >= line_count)
        line_no = line_count - 1;

    for (int i = 0; i < line_no; i++)
        doc_row += wrap_cache[i].count;

    if (target < 0)
        target = 0;
    if (target > (int)strlen(lines[line_no]))
        target = (int)strlen(lines[line_no]);

    for (int i = 0; i < wrap_cache[line_no].count; i++) {
        WrapRow row = wrap_cache[line_no].rows[i];

        row.doc_row = doc_row + i;
        if (target < row.render_end ||
            (target == row.render_end &&
             i + 1 >= wrap_cache[line_no].count)) {
            *out = row;
            return 1;
        }
    }

    if (wrap_cache[line_no].count > 0) {
        *out = wrap_cache[line_no].rows[wrap_cache[line_no].count - 1];
        out->doc_row = doc_row + wrap_cache[line_no].count - 1;
        return 1;
    }

    return layout_row_for_line_position_width(lines[line_no], line_no, target,
                                              doc_row, layout_width(), out);
}

static int layout_row_for_doc_row_width(int target_doc_row, int width,
                                        WrapRow *out)
{
    int doc_row = 0;

    (void)width;
    ensure_wrap_cache();

    if (target_doc_row < 0)
        target_doc_row = 0;

    for (int li = 0; li < line_count; li++) {
        for (int row_no = 0; row_no < wrap_cache[li].count; row_no++) {
            if (doc_row + row_no == target_doc_row) {
                *out = wrap_cache[li].rows[row_no];
                out->doc_row = doc_row + row_no;
                return 1;
            }
        }
        doc_row += wrap_cache[li].count;
    }

    return 0;
}

static int layout_row_for_doc_row(int target_doc_row, WrapRow *out)
{
    return layout_row_for_doc_row_width(target_doc_row, layout_width(), out);
}

static void pos_to_visual_with_affinity(int line_no, int index,
                                        int preferred_doc_row,
                                        int preferred_col,
                                        int *out_doc_row, int *out_col)
{
    WrapRow row;

    if (preferred_doc_row >= 0 && preferred_col >= 0 &&
        layout_row_for_doc_row(preferred_doc_row, &row) &&
        row.line == line_no &&
        preferred_col <= row.visual_width &&
        visual_to_pos_in_row(&row, preferred_col) == index) {
        *out_doc_row = row.doc_row;
        *out_col = preferred_col;
        return;
    }

    if (!layout_row_for_position(line_no, index, &row)) {
        *out_doc_row = 0;
        *out_col = 0;
        return;
    }

    *out_doc_row = row.doc_row;
    *out_col = row_col_for_pos(&row, lines[row.line], index);
}

static void pos_to_visual(int line_no, int index, int *out_doc_row, int *out_col)
{
    pos_to_visual_with_affinity(line_no, index, -1, -1,
                                out_doc_row, out_col);
}

static int visual_to_pos_in_row(const WrapRow *row, int target_col)
{
    const char *line = lines[row->line];
    int col = 0;

    if (target_col <= 0)
        return row->cursor_start;
    if (target_col >= row->visual_width)
        return row->cursor_end;

    for (int i = row->render_start; i < row->render_end; ) {
        int used = 1;
        int w = char_width_at(line, i, col, &used);

        if (col + w > target_col)
            return i;
        col += w;
        i += used;
    }

    return row->cursor_end;
}

static int visual_to_pos_with_affinity(int doc_row, int target_col,
                                       int *out_line, int *out_index,
                                       int *out_affinity_col)
{
    WrapRow row;
    int actual_col;

    if (!layout_row_for_doc_row(doc_row, &row))
        return 0;

    actual_col = target_col;
    if (actual_col < 0)
        actual_col = 0;
    if (actual_col > row.visual_width)
        actual_col = row.visual_width;

    *out_line = row.line;
    *out_index = visual_to_pos_in_row(&row, actual_col);
    *out_affinity_col = actual_col;
    return 1;
}

static void cursor_visual_pos(int *out_doc_row, int *out_col)
{
    int affinity_doc_row;
    int affinity_col;

    if (active_cursor_affinity(&affinity_doc_row, &affinity_col)) {
        pos_to_visual_with_affinity(cy, cx, affinity_doc_row, affinity_col,
                                    out_doc_row, out_col);
        return;
    }

    pos_to_visual(cy, cx, out_doc_row, out_col);
}

static int layout_row_for_cursor(WrapRow *out)
{
    int affinity_doc_row;
    int affinity_col;

    if (active_cursor_affinity(&affinity_doc_row, &affinity_col) &&
        layout_row_for_doc_row(affinity_doc_row, out) &&
        out->line == cy &&
        affinity_col <= out->visual_width &&
        visual_to_pos_in_row(out, affinity_col) == cx)
        return 1;

    return layout_row_for_position(cy, cx, out);
}

static void clamp_top(void)
{
    BodyGeometry geo = body_geometry();
    int max_top = layout_document_visual_rows() - geo.visible_rows;

    if (max_top < 0)
        max_top = 0;
    if (top < 0)
        top = 0;
    if (top > max_top)
        top = max_top;
}

static int visual_rows_for_line(const char *line)
{
    for (int i = 0; i < line_count; i++) {
        if (line == lines[i]) {
            ensure_wrap_cache();
            return wrap_cache[i].count;
        }
    }

    return layout_rows_for_line_width(line, layout_width());
}

static int logical_cursor_row(void)
{
    BodyGeometry geo = body_geometry();
    int row;
    int col;

    cursor_visual_pos(&row, &col);
    if (col >= geo.body_width && geo.left + col >= geo.screen_right)
        row++;
    return row;
}

static int document_visual_rows(void)
{
    return layout_document_visual_rows();
}

static void keep_cursor_visible(void)
{
    BodyGeometry geo = body_geometry();
    int crow;

    crow = logical_cursor_row();
    if (center_lock_enabled) {
        int anchor = (geo.visible_rows * 45) / 100;
        int max_top = document_visual_rows() - geo.visible_rows;

        if (max_top < 0)
            max_top = 0;
        top = crow - anchor;
        if (top < 0)
            top = 0;
        if (top > max_top)
            top = max_top;
        return;
    }

    if (crow < top)
        top = crow;
    else if (crow >= top + geo.visible_rows)
        top = crow - geo.visible_rows + 1;

    if (top < 0)
        top = 0;
    clamp_top();
}

static void cursor_screen_pos(int *out_row, int *out_col)
{
    BodyGeometry geo = body_geometry();
    int doc_row;
    int wc;
    int min_row = geo.top_pad;
    int max_row = geo.bottom - 1;
    int max_col = geo.screen_right - geo.left - 1;

    if (max_row < 0)
        max_row = 0;
    if (min_row > max_row)
        min_row = max_row;
    if (max_col < 0)
        max_col = 0;

    cursor_visual_pos(&doc_row, &wc);
    *out_row = geo.top_pad + (doc_row - top);
    *out_col = wc;

    if (*out_col >= geo.body_width &&
        geo.left + *out_col >= geo.screen_right) {
        (*out_row)++;
        *out_col = 0;
    }

    if (*out_row < min_row)
        *out_row = min_row;
    if (*out_row > max_row)
        *out_row = max_row;
    if (*out_col < 0)
        *out_col = 0;
    if (*out_col > max_col)
        *out_col = max_col;
}

static void begin_selection_if_needed(void)
{
    if (!selecting) {
        selecting = 1;
        sel_cy = cy;
        sel_cx = cx;
    }
}

static void clear_selection(void)
{
    selecting = 0;
}

static int selection_nonempty(void)
{
    return selecting && (sel_cy != cy || sel_cx != cx);
}

static int editor_cursor_visibility(void)
{
    if (idle_cursor_hidden)
        return 0;
    if (find_active && find_match_y >= 0 && find_match_len > 0)
        return 0;
    return selection_nonempty() ? 0 : 1;
}

static void ordered_selection(int *sy, int *sx, int *ey, int *ex)
{
    *sy = sel_cy;
    *sx = sel_cx;
    *ey = cy;
    *ex = cx;

    if (pos_before(*ey, *ex, *sy, *sx)) {
        *sy = cy;
        *sx = cx;
        *ey = sel_cy;
        *ex = sel_cx;
    }
}

static int char_selected(int y, int x)
{
    int sy;
    int sx;
    int ey;
    int ex;

    if (!selecting)
        return 0;

    ordered_selection(&sy, &sx, &ey, &ex);
    return !pos_before(y, x, sy, sx) && pos_before(y, x, ey, ex);
}

static int line_break_selected(int y)
{
    int sy;
    int sx;
    int ey;
    int ex;
    int line_end;

    if (!selecting || y < 0 || y >= line_count - 1)
        return 0;

    ordered_selection(&sy, &sx, &ey, &ex);
    line_end = (int)strlen(lines[y]);

    /*
     * A selection is half-open. Represent the newline after this logical
     * line only once the active range has actually crossed it. Painting the
     * empty endpoint row early made one Shift-Down appear to do nothing, then
     * the following press seemed to jump two rows.
     */
    return !pos_before(y, line_end, sy, sx) &&
           pos_before(y, line_end, ey, ex);
}

static int empty_row_selected(int y)
{
    return y >= 0 && y < line_count && lines[y][0] == '\0' &&
           line_break_selected(y);
}

static int char_find_highlight(int y, int x)
{
    return find_active &&
           y == find_match_y &&
           x >= find_match_x &&
           x < find_match_x + find_match_len;
}

static int word_count(void)
{
    int words = 0;
    int in_word = 0;

    for (int y = 0; y < line_count; y++) {
        for (unsigned char *p = (unsigned char *)lines[y]; *p; p++) {
            if (isspace(*p)) {
                in_word = 0;
            } else if (!in_word) {
                words++;
                in_word = 1;
            }
        }
        in_word = 0;
    }

    return words;
}

static void draw_text_clipped(int row, int col, const char *s, attr_t attr, int maxw)
{
    int used_width = 0;

    if (row < 0 || row >= LINES || col >= COLS || maxw <= 0)
        return;

    if (col < 0)
        return;
    if (maxw > COLS - col)
        maxw = COLS - col;

    while (*s && used_width < maxw) {
        wchar_t wc;
        wchar_t text[2];
        cchar_t cell;
        int bytes;
        int width = utf8_decode(s, &wc, &bytes);

        if (used_width + width > maxw)
            break;
        text[0] = wc;
        text[1] = L'\0';
        setcchar(&cell, text, attr, 0, NULL);
        mvadd_wch(row, col + used_width, &cell);
        used_width += width;
        s += bytes;
    }
}

static void fill_body_row(int row, int left, int used_width)
{
    int width = body_geometry().body_width;

    if (left >= COLS || row < 0 || row >= LINES)
        return;
    if (used_width < 0)
        used_width = 0;
    if (used_width < width)
        put_blank_run_no_wrap(stdscr, row, left, used_width, width,
                              body_attr());
}

static void draw_line_wrapped_from(int *rowp, int left, int li, const char *line,
                                   int skip_rows, int bottom)
{
    int row = *rowp;
    int len = (int)strlen(line);

    if (!len) {
        if (skip_rows <= 0 && row < bottom) {
            fill_body_row(row, left, 0);
            if (empty_row_selected(li))
                mvaddch(row, left, ' ' | selection_attr());
            row++;
        }
        *rowp = row;
        return;
    }

    ensure_wrap_cache();
    for (int wrapped_row = 0;
         wrapped_row < wrap_cache[li].count && row < bottom;
         wrapped_row++) {
        WrapRow wrap = wrap_cache[li].rows[wrapped_row];
        int col = 0;
        int painted_width = 0;

        if (wrapped_row >= skip_rows) {
            for (int i = wrap.render_start; i < wrap.render_end && row < bottom; ) {
                attr_t attr = char_selected(li, i) || char_find_highlight(li, i) ? selection_attr() : body_attr();

                if (line[i] == '\t') {
                    int spaces = TAB_WIDTH - (col % TAB_WIDTH);
                    int visible_spaces = spaces;

                    if (visible_spaces > wrap.visual_width - col)
                        visible_spaces = wrap.visual_width - col;
                    if (visible_spaces > 0) {
                        put_blank_run_no_wrap(stdscr, row, left, col,
                                              col + visible_spaces, attr);
                        painted_width = col + visible_spaces;
                    }
                    col += spaces;
                    i++;
                } else {
                    wchar_t wc;
                    wchar_t text[2];
                    cchar_t cell;
                    int used;
                    int w = utf8_decode(line + i, &wc, &used);

                    if (col + w <= wrap.visual_width) {
                        text[0] = wc;
                        text[1] = L'\0';
                        setcchar(&cell, text, attr, 0, NULL);
                        put_wch_no_wrap(stdscr, row, left + col, &cell);
                        painted_width = col + w;
                    }

                    col += w;
                    i += used;
                }
            }
            fill_body_row(row, left, painted_width);
            if (wrap.render_end == len &&
                line_break_selected(li) &&
                wrap.visual_width < body_geometry().body_width)
                mvaddch(row, left + wrap.visual_width,
                        ' ' | selection_attr());
            row++;
        }
    }

    *rowp = row;
}

static void ensure_screen_storage(int width)
{
    ScreenRow *grown_rows;
    ScreenCell *grown_cells;
    size_t cell_count = (size_t)LINES * (size_t)width;

    if (screen_row_capacity < LINES) {
        grown_rows = realloc(desired_rows,
                             sizeof(*desired_rows) * (size_t)LINES);
        if (!grown_rows) {
            endwin();
            perror("realloc");
            exit(1);
        }
        desired_rows = grown_rows;
        screen_row_capacity = LINES;
        screen_cache_valid = 0;
    }

    if (screen_cell_capacity >= cell_count)
        return;

    grown_cells = realloc(screen_cells, sizeof(*screen_cells) * cell_count);
    if (!grown_cells) {
        endwin();
        perror("realloc");
        exit(1);
    }
    screen_cells = grown_cells;

    grown_cells = realloc(desired_cells, sizeof(*desired_cells) * cell_count);
    if (!grown_cells) {
        endwin();
        perror("realloc");
        exit(1);
    }
    desired_cells = grown_cells;
    screen_cell_capacity = cell_count;
    screen_cache_valid = 0;
}

static void describe_screen_row(ScreenRow *desc, int kind, const WrapRow *wrap)
{
    desc->kind = kind;
    desc->wrap = *wrap;
}

static void build_visible_screen_rows(ScreenRow *rows)
{
    BodyGeometry geo = body_geometry();
    int physical_row = geo.top_pad;
    int doc_row = 0;

    memset(rows, 0, sizeof(*rows) * (size_t)LINES);
    ensure_wrap_cache();

    for (int li = 0; li < line_count && physical_row < geo.bottom; li++) {
        for (int i = 0; i < wrap_cache[li].count; i++) {
            WrapRow wrap = wrap_cache[li].rows[i];
            int kind = lines[li][0] ? SCREEN_ROW_TEXT : SCREEN_ROW_EMPTY;

            wrap.doc_row = doc_row;
            if (doc_row >= top && physical_row < geo.bottom) {
                describe_screen_row(&rows[physical_row], kind, &wrap);
                physical_row++;
            }
            doc_row++;
        }
    }
}

static void set_desired_blank(ScreenCell *cell, attr_t attr)
{
    cell->wc = L' ';
    cell->attr = attr;
    cell->kind = SCREEN_CELL_BLANK;
}

static void build_desired_body_cells(int width)
{
    BodyGeometry geo = body_geometry();
    size_t cell_count = (size_t)LINES * (size_t)width;

    for (size_t i = 0; i < cell_count; i++)
        set_desired_blank(&desired_cells[i], body_attr());

    build_visible_screen_rows(desired_rows);
    for (int row = geo.top_pad; row < geo.bottom; row++) {
        const ScreenRow *desc = &desired_rows[row];
        ScreenCell *cells = desired_cells + (size_t)row * (size_t)width;
        int col = 0;

        if (desc->kind == SCREEN_ROW_UNUSED)
            continue;

        if (desc->kind == SCREEN_ROW_EMPTY) {
            if (width > 0 && empty_row_selected(desc->wrap.line))
                set_desired_blank(&cells[0], selection_attr());
            continue;
        }

        for (int i = desc->wrap.render_start; i < desc->wrap.render_end; ) {
            attr_t attr = char_selected(desc->wrap.line, i) ||
                          char_find_highlight(desc->wrap.line, i) ?
                          selection_attr() : body_attr();

            if (lines[desc->wrap.line][i] == '\t') {
                int spaces = TAB_WIDTH - (col % TAB_WIDTH);

                for (int k = 0;
                     k < spaces && col + k < desc->wrap.visual_width;
                     k++)
                    set_desired_blank(&cells[col + k], attr);
                col += spaces;
                i++;
            } else {
                wchar_t wc;
                int used;
                int glyph_width = utf8_decode(
                    lines[desc->wrap.line] + i, &wc, &used);

                if (col + glyph_width <= desc->wrap.visual_width) {
                    cells[col].wc = wc;
                    cells[col].attr = attr;
                    cells[col].kind = SCREEN_CELL_GLYPH;
                    for (int k = 1; k < glyph_width; k++) {
                        cells[col + k].wc = L'\0';
                        cells[col + k].attr = attr;
                        cells[col + k].kind = SCREEN_CELL_CONTINUATION;
                    }
                }
                col += glyph_width;
                i += used;
            }
        }
        if (desc->wrap.render_end ==
                (int)strlen(lines[desc->wrap.line]) &&
            line_break_selected(desc->wrap.line) &&
            desc->wrap.visual_width < width)
            set_desired_blank(&cells[desc->wrap.visual_width],
                              selection_attr());
    }
}

static int screen_cells_equal(const ScreenCell *a, const ScreenCell *b)
{
    return a->wc == b->wc && a->attr == b->attr && a->kind == b->kind;
}

static int body_cells_valid(const ScreenCell *cells, int width)
{
    for (int row = 0; row < LINES; row++) {
        const ScreenCell *line = cells + (size_t)row * (size_t)width;

        for (int col = 0; col < width; col++) {
            if (line[col].kind > SCREEN_CELL_CONTINUATION)
                return 0;
            if (line[col].kind == SCREEN_CELL_CONTINUATION &&
                (col == 0 || line[col - 1].kind == SCREEN_CELL_BLANK))
                return 0;
        }
    }
    return 1;
}

static int window_width(WINDOW *window)
{
    int h;
    int w;

    getmaxyx(window, h, w);
    (void)h;
    return w;
}

static void put_wch_no_wrap(WINDOW *window, int row, int col,
                            const cchar_t *cell)
{
    int width = window_width(window);

    if (width > 0 && col == width - 1)
        mvwins_wch(window, row, col, cell);
    else
        mvwadd_wch(window, row, col, cell);
}

static void put_blank_run_no_wrap(WINDOW *window, int row, int left,
                                  int start, int end, attr_t attr)
{
    cchar_t blank;
    wchar_t text[2] = {L' ', L'\0'};
    int width = window_width(window);
    int run_end = end;

    if (end <= start)
        return;
    wattrset(window, attr);
    if (width > 0 && left + end == width)
        run_end = end - 1;
    if (run_end > start)
        mvwhline(window, row, left + start, ' ', run_end - start);
    if (run_end < end) {
        setcchar(&blank, text, attr, 0, NULL);
        mvwins_wch(window, row, left + run_end, &blank);
    }
}

static void destroy_body_window(void)
{
    if (body_window)
        delwin(body_window);
    body_window = NULL;
    body_window_height = 0;
    body_window_width = 0;
    body_window_top = 0;
    body_window_left = 0;
}

static int ensure_body_window(int left, int width, int bottom)
{
    BodyGeometry geo = body_geometry();
    int height = bottom - geo.top_pad;
    int window_width = width;

    if (left + window_width < COLS)
        window_width++;

    if (!windowed_redraw_enabled || height < 1 || width < 1)
        return 0;
    if (body_window &&
        body_window_height == height && body_window_width == window_width &&
        body_window_top == geo.top_pad && body_window_left == left)
        return 1;

    destroy_body_window();
    body_window = newwin(height, window_width, geo.top_pad, left);
    if (!body_window) {
        windowed_redraw_enabled = 0;
        return 0;
    }

    body_window_height = height;
    body_window_width = window_width;
    body_window_top = geo.top_pad;
    body_window_left = left;
    wbkgdset(body_window, (chtype)' ' | body_attr());
    scrollok(body_window, FALSE);
    idlok(body_window, FALSE);
    leaveok(body_window, FALSE);
    screen_cache_valid = 0;
    return 1;
}

static void sync_body_window_from_stdscr(void)
{
    if (!body_window)
        return;
    copywin(stdscr, body_window,
            body_window_top, body_window_left, 0, 0,
            body_window_height - 1, body_window_width - 1, FALSE);
}

static int move_body_window_cursor(int screen_row, int col)
{
    int row = screen_row - body_window_top;

    if (!body_window || row < 0 || row >= body_window_height ||
        col < 0 || col >= body_window_width)
        return 0;
    wmove(body_window, row, col);
    return 1;
}

static void refresh_windowed_screen(int cursor_row, int cursor_col, int left)
{
    if (!body_window ||
        !move_body_window_cursor(cursor_row, cursor_col)) {
        move(cursor_row, left + cursor_col);
        refresh();
        return;
    }

    wnoutrefresh(stdscr);
    wnoutrefresh(body_window);
    doupdate();
}

static void mark_wide_group(const ScreenCell *cells, unsigned char *dirty_cells,
                            int col, int width)
{
    int start = col;
    int end;

    while (start > 0 &&
           cells[start].kind == SCREEN_CELL_CONTINUATION)
        start--;
    end = start + 1;
    while (end < width &&
           cells[end].kind == SCREEN_CELL_CONTINUATION)
        end++;
    memset(dirty_cells + start, 1, (size_t)(end - start));
}

static void emit_desired_run(WINDOW *window, int row, int left,
                             int start, int end,
                             const ScreenCell *cells)
{
    for (int col = start; col < end; ) {
        const ScreenCell *cell = &cells[col];

        if (cell->kind == SCREEN_CELL_BLANK) {
            int blank_end = col + 1;

            while (blank_end < end &&
                   cells[blank_end].kind == SCREEN_CELL_BLANK &&
                   cells[blank_end].attr == cell->attr)
                blank_end++;
            put_blank_run_no_wrap(window, row, left, col, blank_end,
                                  cell->attr);
            col = blank_end;
        } else if (cell->kind == SCREEN_CELL_GLYPH) {
            cchar_t output;
            wchar_t text[2] = {cell->wc, L'\0'};
            int glyph_end = col + 1;

            while (glyph_end < end &&
                   cells[glyph_end].kind == SCREEN_CELL_CONTINUATION)
                glyph_end++;
            setcchar(&output, text, cell->attr, 0, NULL);
            put_wch_no_wrap(window, row, left + col, &output);
            col = glyph_end;
        } else {
            col++;
        }
    }
}

static void repaint_changed_body_row(int row, int left, int width)
{
    WINDOW *window = body_window ? body_window : stdscr;
    int window_row = body_window ? row - body_window_top : row;
    int window_left = body_window ? 0 : left;
    ScreenCell *old;
    ScreenCell *desired;

    if (width <= 0)
        return;

    unsigned char dirty_cells[width];

    old = screen_cells + (size_t)row * (size_t)width;
    desired = desired_cells + (size_t)row * (size_t)width;

    memset(dirty_cells, 0, sizeof(dirty_cells));
    for (int col = 0; col < width; col++) {
        if (!screen_cells_equal(&old[col], &desired[col])) {
            mark_wide_group(old, dirty_cells, col, width);
            mark_wide_group(desired, dirty_cells, col, width);
        }
    }

    for (int col = 0; col < width; ) {
        int end;

        if (!dirty_cells[col]) {
            col++;
            continue;
        }
        end = col + 1;
        while (end < width && dirty_cells[end])
            end++;
        emit_desired_run(window, window_row, window_left,
                         col, end, desired);
        col = end;
    }

    memcpy(old, desired, sizeof(*old) * (size_t)width);
}

static void format_screen_chrome(char *title, size_t titlesz,
                                 char *wc, size_t wcsz,
                                 char *status, size_t statussz)
{
    char shown[512];

    snprintf(wc, wcsz, "%d words", word_count());
    display_name(shown, sizeof(shown));
    snprintf(title, titlesz, "%s%s%s", shown, dirty ? " *" : "",
             editor_buffers[active_buffer_index].lock_blocked ?
             " [open elsewhere]" : "");
    snprintf(status, statussz, "%s", current_footer_text());
}

static void capture_screen_cache(int left)
{
    int width = body_geometry().body_width;
    size_t cell_count;

    ensure_screen_storage(width);
    build_desired_body_cells(width);
    cell_count = (size_t)LINES * (size_t)width;
    memcpy(screen_cells, desired_cells, sizeof(*screen_cells) * cell_count);
    format_screen_chrome(screen_cache_title, sizeof(screen_cache_title),
                         screen_cache_wc, sizeof(screen_cache_wc),
                         screen_cache_status, sizeof(screen_cache_status));
    screen_cache_lines = LINES;
    screen_cache_cols = COLS;
    screen_cache_left = left;
    screen_cache_text_width = config.text_width;
    screen_cache_top_pad = config.top_pad;
    screen_cache_distraction_free = distraction_free;
    screen_cache_top = top;
    screen_cache_valid = 1;
}

static int screen_cache_geometry_matches(int left)
{
    return screen_cache_valid &&
           screen_cache_lines == LINES &&
           screen_cache_cols == COLS &&
           screen_cache_left == left &&
           screen_cache_text_width == config.text_width &&
           screen_cache_top_pad == config.top_pad &&
           screen_cache_distraction_free == distraction_free;
}

static void draw_screen_impl(int update)
{
    BodyGeometry geo;

    if (update)
        set_cursor_visibility(0);

    char wc[64];
    char shown[512];
    char title[700];
    int row;
    int logical_row = 0;
    int cr;
    int cc;

    screen_cache_valid = 0;
    keep_cursor_visible();
    geo = body_geometry();
    erase();

    snprintf(wc, sizeof(wc), "%d words", word_count());
    display_name(shown, sizeof(shown));
    snprintf(title, sizeof(title), "%s%s%s", shown, dirty ? " *" : "",
             editor_buffers[active_buffer_index].lock_blocked ?
             " [open elsewhere]" : "");

    if (!distraction_free) {
        int wc_width = (int)strlen(wc);
        int title_width = COLS - wc_width - 3;

        attrset(body_attr());
        draw_text_clipped(0, 1, title, body_attr(), title_width);
        draw_text_clipped(0, COLS - wc_width - 1, wc,
                          body_attr(), wc_width);
    }

    row = geo.top_pad;
    for (int li = 0; li < line_count && row < geo.bottom; li++) {
        int rows = visual_rows_for_line(lines[li]);
        int skip_rows;

        if (logical_row + rows <= top) {
            logical_row += rows;
            continue;
        }

        skip_rows = top - logical_row;
        if (skip_rows < 0)
            skip_rows = 0;

        draw_line_wrapped_from(&row, geo.left, li, lines[li], skip_rows,
                               geo.bottom);
        logical_row += rows;
    }

    if (!distraction_free) {
        attrset(chrome_attr());
        mvhline(LINES - 1, 0, ' ', COLS);

        draw_text_clipped(LINES - 1, 1, current_footer_text(),
                          chrome_attr(), COLS - 2);
    }

    cursor_screen_pos(&cr, &cc);
    attrset(body_attr());
    move(cr, geo.left + cc);
    if (update) {
        refresh();
        /*
         * A terminal block cursor masks the reverse-video cell beneath it.
         * While extending a selection this made its active-end character look
         * unselected; a one-character selection looked like it had not begun
         * at all until keyboard repeat moved the cursor again.
         */
        set_cursor_visibility(editor_cursor_visibility());
    }
}

static void draw_screen(void)
{
    BodyGeometry geo;
    int single_special_window = 0;

    if (buffer_system_ready && layout_root >= 0 &&
        layout_nodes[layout_root].used &&
        layout_nodes[layout_root].kind == LAYOUT_LEAF) {
        int window_index = layout_nodes[layout_root].window_index;

        single_special_window =
            window_index >= 0 && window_index < MAX_EDITOR_WINDOWS &&
            editor_windows[window_index].used &&
            editor_windows[window_index].kind == EDITOR_WINDOW_BUFFER_SHELF;
        if (!single_special_window)
            pane_rendering = 0;
    }
    if (buffer_system_ready && layout_root >= 0 &&
        layout_nodes[layout_root].used &&
        (single_special_window ||
         layout_nodes[layout_root].kind != LAYOUT_LEAF)) {
        draw_workspace_screen();
        return;
    }

    /*
     * Keep the terminal cursor hidden while repainting. Otherwise ncurses can
     * briefly expose it at intermediate draw positions during vertical scroll,
     * producing the side-blink / eyeblink jump.
     */
    set_cursor_visibility(0);

    char title[700];
    char wc[64];
    char status[512];
    int body_ready;
    int cr;
    int cc;

    keep_cursor_visible();
    geo = body_geometry();

    ensure_screen_storage(geo.body_width);
    body_ready = ensure_body_window(geo.left, geo.body_width, geo.bottom);
    if ((!distraction_free && LINES < 2) ||
        !screen_cache_geometry_matches(geo.left) ||
        !body_cells_valid(screen_cells, geo.body_width)) {
        draw_screen_impl(body_ready ? 0 : 1);
        capture_screen_cache(geo.left);
        if (body_ready) {
            sync_body_window_from_stdscr();
            cursor_screen_pos(&cr, &cc);
            refresh_windowed_screen(cr, cc, geo.left);
            set_cursor_visibility(editor_cursor_visibility());
        }
        return;
    }

    build_desired_body_cells(geo.body_width);
    for (int row = geo.top_pad; row < geo.bottom; row++)
        repaint_changed_body_row(row, geo.left, geo.body_width);

    format_screen_chrome(title, sizeof(title), wc, sizeof(wc),
                         status, sizeof(status));

    if (!distraction_free &&
        (strcmp(title, screen_cache_title) != 0 ||
         strcmp(wc, screen_cache_wc) != 0)) {
        int wc_width = (int)strlen(wc);
        int title_width = COLS - wc_width - 3;

        attrset(body_attr());
        mvhline(0, 0, ' ', COLS);
        draw_text_clipped(0, 1, title, body_attr(), title_width);
        draw_text_clipped(0, COLS - wc_width - 1, wc,
                          body_attr(), wc_width);
    }

    if (!distraction_free && strcmp(status, screen_cache_status) != 0) {
        attrset(chrome_attr());
        mvhline(LINES - 1, 0, ' ', COLS);
        draw_text_clipped(LINES - 1, 1, status,
                          chrome_attr(), COLS - 2);
    }

    snprintf(screen_cache_title, sizeof(screen_cache_title), "%s", title);
    snprintf(screen_cache_wc, sizeof(screen_cache_wc), "%s", wc);
    snprintf(screen_cache_status, sizeof(screen_cache_status), "%s", status);
    screen_cache_top = top;

    cursor_screen_pos(&cr, &cc);
    if (body_ready) {
        refresh_windowed_screen(cr, cc, geo.left);
    } else {
        attrset(body_attr());
        move(cr, geo.left + cc);
        refresh();
    }
    set_cursor_visibility(editor_cursor_visibility());
}

static void clamp_cursor(void)
{
    if (cy < 0)
        cy = 0;
    if (cy >= line_count)
        cy = line_count - 1;
    if (cx < 0)
        cx = 0;
    if (cx > (int)strlen(lines[cy]))
        cx = (int)strlen(lines[cy]);
}

static int document_cursor_index(void)
{
    int index = cx;

    for (int i = 0; i < cy; i++)
        index += (int)strlen(lines[i]) + 1;

    return index;
}

static void trace_space_insert(void)
{
    int render_y = 0;
    int render_x = 0;
    int wrapped_row = 0;
    WrapRow row;

    if (!env_enabled("SW_TRACE_SPACE"))
        return;

    cursor_visual_pos(&render_y, &render_x);
    if (layout_row_for_cursor(&row))
        wrapped_row = row.doc_row;
    else
        wrapped_row = render_y;

    fprintf(stderr,
            "cursor_index=%d cursor_line=%d cursor_column=%d "
            "render_x=%d render_y=%d desired_x=%d line_length=%d "
            "wrapped_row=%d\n",
            document_cursor_index(), cy, cx, render_x, render_y, goal_col,
            (int)strlen(lines[cy]), wrapped_row);
    fflush(stderr);
}

static int insert_char(int ch)
{
    int len = (int)strlen(lines[cy]);
    char text[2];

    if (len >= MAX_LINE - 2) {
        set_status("Line is full");
        return 0;
    }

    maybe_save_typing_undo();
    text[0] = (char)ch;
    text[1] = '\0';
    if (!replace_range_recorded(cy, cx, cy, cx, text))
        return 0;

    mark_edit();
    if (ch == ' ')
        trace_space_insert();
    return 1;
}

static int insert_printable_key(int ch)
{
    if (!isprint((unsigned char)ch))
        return 0;
    if (!insert_char(ch))
        return 0;

    (void)request_typewriter_sound(typewriter_sound_for_printable(ch));
    return 1;
}

static int newline(void)
{
    int own_group = 0;
    int inserted = 0;

    if (undo_group_depth == 0) {
        break_undo_burst();
        begin_undo_group();
        own_group = 1;
    }

    if (replace_range_recorded(cy, cx, cy, cx, "\n")) {
        mark_edit();
        inserted = 1;
    }

    if (own_group)
        end_undo_group();
    return inserted;
}

static int delete_selection(void);

static int backspace(void)
{
    int own_group = 0;
    int deleted = 0;

    break_undo_burst();

    if (selecting) {
        return delete_selection();
    }

    if (cx == 0 && cy == 0)
        return 0;

    if (undo_group_depth == 0) {
        begin_undo_group();
        own_group = 1;
    }

    if (cx > 0) {
        if (!replace_range_recorded(cy, cx - 1, cy, cx, ""))
            goto done;
    } else {
        int prev_len = (int)strlen(lines[cy - 1]);

        if (prev_len + (int)strlen(lines[cy]) >= MAX_LINE - 1) {
            set_status("Joined line would be too long");
            goto done;
        }

        if (!replace_range_recorded(cy - 1, prev_len, cy, 0, ""))
            goto done;
    }

    mark_edit();
    deleted = 1;
done:
    if (own_group)
        end_undo_group();
    return deleted;
}

static int delete_forward(void)
{
    int own_group = 0;
    int deleted = 0;

    break_undo_burst();

    if (selecting) {
        return delete_selection();
    }

    if (cx == (int)strlen(lines[cy]) && cy == line_count - 1)
        return 0;

    if (undo_group_depth == 0) {
        begin_undo_group();
        own_group = 1;
    }

    if (cx < (int)strlen(lines[cy])) {
        if (!replace_range_recorded(cy, cx, cy, cx + 1, ""))
            goto done;
    } else {
        if (strlen(lines[cy]) + strlen(lines[cy + 1]) >= MAX_LINE - 1) {
            set_status("Joined line would be too long");
            goto done;
        }

        if (!replace_range_recorded(cy, (int)strlen(lines[cy]), cy + 1, 0, ""))
            goto done;
    }

    mark_edit();
    deleted = 1;
done:
    if (own_group)
        end_undo_group();
    return deleted;
}

static int delete_selection(void)
{
    int sy;
    int sx;
    int ey;
    int ex;
    int own_group = 0;

    if (!selecting)
        return 0;

    break_undo_burst();
    ordered_selection(&sy, &sx, &ey, &ex);

    if (sy == ey && sx == ex) {
        clear_selection();
        return 0;
    }

    if (undo_group_depth == 0) {
        begin_undo_group();
        own_group = 1;
    }

    if (!replace_range_recorded(sy, sx, ey, ex, ""))
        goto done;

    goal_col = -1;
    clear_cursor_affinity();
    clear_selection();
    clamp_cursor();
    keep_cursor_visible();
    mark_edit();
    if (own_group)
        end_undo_group();
    return 1;
done:
    if (own_group)
        end_undo_group();
    return 0;
}

static int keyboard_backspace(void)
{
    if (!backspace())
        return 0;
    (void)request_typewriter_sound(TYPEWRITER_SOUND_DELETE);
    return 1;
}

static int keyboard_delete_forward(void)
{
    if (!delete_forward())
        return 0;
    (void)request_typewriter_sound(TYPEWRITER_SOUND_DELETE);
    return 1;
}

static int keyboard_newline(void)
{
    if (selecting)
        (void)delete_selection();
    if (!newline())
        return 0;
    (void)request_typewriter_sound(TYPEWRITER_SOUND_ENTER);
    return 1;
}

static int keyboard_tab(void)
{
    if (selecting)
        (void)delete_selection();
    if (!insert_char('\t'))
        return 0;
    (void)request_typewriter_sound(TYPEWRITER_SOUND_KEY);
    return 1;
}


static int detect_clipboard_backend(void)
{
    const char *wayland = getenv("WAYLAND_DISPLAY");
    const char *x11 = getenv("DISPLAY");

    if (clip_backend != CLIP_BACKEND_UNKNOWN)
        return clip_backend;
#ifdef __APPLE__
    if (ssp_command_available("/usr/bin/pbcopy") &&
        ssp_command_available("/usr/bin/pbpaste"))
        return clip_backend = CLIP_BACKEND_MACOS;
#endif
    if (wayland && *wayland &&
        ssp_command_available("wl-copy") &&
        ssp_command_available("wl-paste"))
        return clip_backend = CLIP_BACKEND_WL;

    if (x11 && *x11 && ssp_command_available("xclip"))
        return clip_backend = CLIP_BACKEND_XCLIP;

    if (x11 && *x11 && ssp_command_available("xsel"))
        return clip_backend = CLIP_BACKEND_XSEL;

    return clip_backend = CLIP_BACKEND_NONE;
}

static void warn_no_system_clipboard_once(void)
{
    if (!clip_warned) {
#ifdef __APPLE__
        set_status("macOS clipboard commands are unavailable.");
#else
        set_status("No system clipboard tool. Install wl-clipboard, xclip, or xsel.");
#endif
        clip_warned = 1;
    }
}

static void write_system_clipboard(const char *text)
{
    char tmpname[] = "/tmp/simplewords-clip-XXXXXX";
    char *argv[6] = {0};
    int fd;
    FILE *fp;

    if (!text)
        return;

    fd = mkstemp(tmpname);
    if (fd < 0) {
        warn_no_system_clipboard_once();
        return;
    }

    fp = fdopen(fd, "w");
    if (!fp) {
        close(fd);
        unlink(tmpname);
        warn_no_system_clipboard_once();
        return;
    }

    fputs(text, fp);
    fclose(fp);

    switch (detect_clipboard_backend()) {
    case CLIP_BACKEND_WL:
        argv[0] = "wl-copy";
        argv[1] = "--type";
        argv[2] = "text/plain";
        break;
    case CLIP_BACKEND_XCLIP:
        argv[0] = "xclip";
        argv[1] = "-selection";
        argv[2] = "clipboard";
        break;
    case CLIP_BACKEND_XSEL:
        argv[0] = "xsel";
        argv[1] = "--clipboard";
        argv[2] = "--input";
        break;
#ifdef __APPLE__
    case CLIP_BACKEND_MACOS:
        argv[0] = "/usr/bin/pbcopy";
        break;
#endif
    default:
        unlink(tmpname);
        warn_no_system_clipboard_once();
        return;
    }

    if (!ssp_detach_argv_with_input_file(tmpname, argv))
        warn_no_system_clipboard_once();
    unlink(tmpname);
}

static char *read_system_clipboard(void)
{
    char *argv[6] = {0};
    char *buf = NULL;

    switch (detect_clipboard_backend()) {
    case CLIP_BACKEND_WL:
        argv[0] = "wl-paste";
        argv[1] = "--no-newline";
        break;
    case CLIP_BACKEND_XCLIP:
        argv[0] = "xclip";
        argv[1] = "-selection";
        argv[2] = "clipboard";
        argv[3] = "-o";
        break;
    case CLIP_BACKEND_XSEL:
        argv[0] = "xsel";
        argv[1] = "--clipboard";
        argv[2] = "--output";
        break;
#ifdef __APPLE__
    case CLIP_BACKEND_MACOS:
        argv[0] = "/usr/bin/pbpaste";
        break;
#endif
    default:
        warn_no_system_clipboard_once();
        return NULL;
    }

    if (!ssp_capture_argv(argv, &buf, SYSTEM_CLIPBOARD_LIMIT,
                          SYSTEM_CLIPBOARD_TIMEOUT_MS)) {
        set_status("Clipboard read failed, timed out, or was too large");
        return NULL;
    }
    return buf;
}


static void copy_selection(void)
{
    int sy;
    int sx;
    int ey;
    int ex;
    size_t cap = 1;

    if (!selecting)
        return;

    ordered_selection(&sy, &sx, &ey, &ex);
    for (int y = sy; y <= ey; y++)
        cap += strlen(lines[y]) + 1;

    free(clip);
    clip = calloc(cap, 1);
    if (!clip)
        exit(1);

    for (int y = sy; y <= ey; y++) {
        int start = y == sy ? sx : 0;
        int end = y == ey ? ex : (int)strlen(lines[y]);

        if (end > start)
            strncat(clip, lines[y] + start, (size_t)(end - start));
        if (y != ey)
            strcat(clip, "\n");
    }

    write_system_clipboard(clip);
    set_status("Copied");
}

static void cut_selection(void)
{
    if (!selecting)
        return;
    copy_selection();
    delete_selection();
    set_status("Cut");
}

static char *normalize_pasted_text(const char *text)
{
    size_t length = strlen(text);
    char *normalized = xmalloc(length + 1);
    size_t out = 0;

    for (size_t i = 0; i < length; i++) {
        if (text[i] == '\r') {
            normalized[out++] = '\n';
            if (i + 1 < length && text[i + 1] == '\n')
                i++;
        } else {
            normalized[out++] = text[i];
        }
    }
    normalized[out] = '\0';
    return normalized;
}

static int insert_pasted_text(const char *text)
{
    char *normalized;
    int sy = cy;
    int sx = cx;
    int ey = cy;
    int ex = cx;
    int inserted;

    if (!text || !*text)
        return 0;

    normalized = normalize_pasted_text(text);
    if (selecting)
        ordered_selection(&sy, &sx, &ey, &ex);

    break_undo_burst();
    begin_undo_group();

    if ((int)strlen(normalized) > config.text_width)
        reset_wrap_cache();

    inserted = replace_range_recorded(sy, sx, ey, ex, normalized);
    if (inserted) {
        clear_selection();
        mark_edit();
    }

    end_undo_group();
    break_undo_burst();
    free(normalized);
    return inserted;
}

static void paste_clipboard(void)
{
    char *system_clip = read_system_clipboard();
    const char *text = NULL;

    if (system_clip && system_clip[0])
        text = system_clip;
    else if (clip && clip[0])
        text = clip;

    if (!text) {
        if (detect_clipboard_backend() == CLIP_BACKEND_NONE)
            warn_no_system_clipboard_once();
        else
            set_status("Clipboard empty");
        free(system_clip);
        return;
    }

    if (insert_pasted_text(text))
        set_status("Pasted");

    free(system_clip);
}

static void move_left(int extend)
{
    int doc_row;
    int col;

    break_undo_burst();
    goal_col = -1;

    if (extend)
        begin_selection_if_needed();
    else
        clear_selection();

    cursor_visual_pos(&doc_row, &col);
    if (col == 0 && doc_row > 0) {
        WrapRow previous;

        if (layout_row_for_doc_row(doc_row - 1, &previous) &&
            previous.line == cy && previous.cursor_end == cx) {
            set_cursor_affinity(cy, cx, previous.doc_row,
                                previous.visual_width);
            return;
        }
    }

    clear_cursor_affinity();
    if (cx > 0)
        cx--;
    else if (cy > 0) {
        cy--;
        cx = (int)strlen(lines[cy]);
    }
}

static void move_right(int extend)
{
    int affinity_doc_row;
    int affinity_col;

    break_undo_burst();
    goal_col = -1;

    if (extend)
        begin_selection_if_needed();
    else
        clear_selection();

    if (active_cursor_affinity(&affinity_doc_row, &affinity_col)) {
        WrapRow current;
        WrapRow next;

        if (layout_row_for_doc_row(affinity_doc_row, &current) &&
            affinity_col == current.visual_width &&
            current.line == cy && current.cursor_end == cx &&
            layout_row_for_doc_row(affinity_doc_row + 1, &next) &&
            next.line == cy && next.cursor_start == cx) {
            set_cursor_affinity(cy, cx, next.doc_row, 0);
            return;
        }
    }

    clear_cursor_affinity();
    if (cx < (int)strlen(lines[cy]))
        cx++;
    else if (cy < line_count - 1) {
        cy++;
        cx = 0;
    }
}

static void move_visual_line(int dir, int extend)
{
    int doc_row;
    int col;
    int target_doc_row;
    int total_rows;
    int out_line;
    int out_index;
    int affinity_col;

    break_undo_burst();
    if (extend)
        begin_selection_if_needed();
    else
        clear_selection();

    cursor_visual_pos(&doc_row, &col);
    if (goal_col < 0)
        goal_col = col;

    total_rows = document_visual_rows();
    target_doc_row = doc_row + dir;

    if (target_doc_row < 0) {
        if (extend)
            cx = 0;
        return;
    }
    if (target_doc_row >= total_rows) {
        if (extend)
            cx = (int)strlen(lines[cy]);
        return;
    }

    if (visual_to_pos_with_affinity(target_doc_row, goal_col,
                                    &out_line, &out_index, &affinity_col)) {
        cy = out_line;
        cx = out_index;
        set_cursor_affinity(cy, cx, target_doc_row, affinity_col);
    }
}

static void move_visual_home(int extend)
{
    WrapRow row;

    break_undo_burst();
    goal_col = -1;

    if (extend)
        begin_selection_if_needed();
    else
        clear_selection();

    if (layout_row_for_cursor(&row)) {
        cy = row.line;
        cx = row.cursor_start;
        set_cursor_affinity(cy, cx, row.doc_row, 0);
    }
}

static void move_visual_end(int extend)
{
    WrapRow row;

    break_undo_burst();
    goal_col = -1;

    if (extend)
        begin_selection_if_needed();
    else
        clear_selection();

    if (layout_row_for_cursor(&row)) {
        cy = row.line;
        cx = row.cursor_end;
        set_cursor_affinity(cy, cx, row.doc_row, row.visual_width);
    }
}

static void move_page(int dir, int extend)
{
    int page = body_geometry().visible_rows;

    if (page < 1)
        page = 1;
    for (int i = 0; i < page; i++)
        move_visual_line(dir, extend);
}

typedef struct {
    int code;
    int action;
    const char *capability;
} KeyMapping;

static KeyMapping runtime_keys[16];
static int runtime_key_count = 0;

static void add_terminfo_key(const char *capability, int action)
{
    const char *sequence = tigetstr((char *)capability);
    int code;

    if (!sequence || sequence == (char *)-1 || !*sequence)
        return;
    code = key_defined(sequence);
    if (code <= 0)
        return;

    for (int i = 0; i < runtime_key_count; i++) {
        if (runtime_keys[i].code == code && runtime_keys[i].action == action)
            return;
    }
    if (runtime_key_count >= (int)(sizeof(runtime_keys) / sizeof(runtime_keys[0])))
        return;

    runtime_keys[runtime_key_count++] = (KeyMapping){code, action, capability};
}

static void discover_modified_navigation(void)
{
    runtime_key_count = 0;
    add_terminfo_key("kUP", KEY_EXTEND_UP);
    add_terminfo_key("kDN", KEY_EXTEND_DOWN);
    add_terminfo_key("kLFT", KEY_SLEFT);
    add_terminfo_key("kRIT", KEY_SRIGHT);
    add_terminfo_key("kPRV", KEY_EXTEND_PAGE_UP);
    add_terminfo_key("kNXT", KEY_EXTEND_PAGE_DOWN);

    /* Some descriptions expose shifted vertical arrows as the standardized
     * scroll-reverse/scroll-forward capabilities. */
    add_terminfo_key("kri", KEY_EXTEND_UP);
    add_terminfo_key("kind", KEY_EXTEND_DOWN);
}

static int normalize_terminfo_key(int ch)
{
    for (int i = 0; i < runtime_key_count; i++) {
        if (runtime_keys[i].code == ch)
            return runtime_keys[i].action;
    }
    return ch;
}

static int shifted_modifier(int modifier)
{
    return modifier > 0 && ((modifier - 1) & 1) != 0;
}

static int parse_modified_csi(const char *sequence)
{
    int first;
    int modifier;
    char final;

    if (sscanf(sequence, "[1;%d%c", &modifier, &final) == 2 &&
        shifted_modifier(modifier)) {
        if (final == 'A') return KEY_EXTEND_UP;
        if (final == 'B') return KEY_EXTEND_DOWN;
        if (final == 'C') return KEY_SRIGHT;
        if (final == 'D') return KEY_SLEFT;
    }
    if (sscanf(sequence, "O1;%d%c", &modifier, &final) == 2 &&
        shifted_modifier(modifier)) {
        if (final == 'A') return KEY_EXTEND_UP;
        if (final == 'B') return KEY_EXTEND_DOWN;
        if (final == 'C') return KEY_SRIGHT;
        if (final == 'D') return KEY_SLEFT;
    }
    if (sscanf(sequence, "[%d;%d%c", &first, &modifier, &final) == 3 &&
        shifted_modifier(modifier)) {
        if (final == '~' && first == 5) return KEY_EXTEND_PAGE_UP;
        if (final == '~' && first == 6) return KEY_EXTEND_PAGE_DOWN;
        if (final == 'u' && first == 57352) return KEY_EXTEND_UP;
        if (final == 'u' && first == 57353) return KEY_EXTEND_DOWN;
        if (final == 'u' && first == 57354) return KEY_EXTEND_PAGE_UP;
        if (final == 'u' && first == 57355) return KEY_EXTEND_PAGE_DOWN;
    }
    return 0;
}

static void append_bracketed_paste_bytes(char **buffer, size_t *length,
                                         size_t *capacity,
                                         const char *bytes, size_t count,
                                         int *too_large)
{
    size_t required;
    size_t new_capacity;

    if (*too_large || count == 0)
        return;
    if (count > SYSTEM_CLIPBOARD_LIMIT - *length) {
        *too_large = 1;
        return;
    }

    required = *length + count + 1;
    if (required > *capacity) {
        new_capacity = *capacity ? *capacity * 2 : 4096;
        while (new_capacity < required)
            new_capacity *= 2;
        if (new_capacity > SYSTEM_CLIPBOARD_LIMIT + 1u)
            new_capacity = SYSTEM_CLIPBOARD_LIMIT + 1u;
        *buffer = xrealloc(*buffer, new_capacity);
        *capacity = new_capacity;
    }

    memcpy(*buffer + *length, bytes, count);
    *length += count;
}

static char *capture_bracketed_paste(void)
{
    static const char end_marker[] = BRACKETED_PASTE_END;
    const size_t marker_length = sizeof(end_marker) - 1;
    char *buffer = NULL;
    size_t length = 0;
    size_t capacity = 0;
    size_t matched = 0;
    int complete = 0;
    int too_large = 0;
    int idle_ticks = 0;

    /* Inside a paste, escape sequences are document text except for the final
     * bracketed-paste delimiter.  Turn off keypad decoding so ncurses gives us
     * the original bytes instead of translating any text that happens to look
     * like a function key. */
    (void)keypad(stdscr, FALSE);
    timeout(250);

    while (!complete && idle_ticks < BRACKETED_PASTE_IDLE_TICKS) {
        int ch = getch();
        char byte;

        if (ch == ERR) {
            if (terminal_input_disconnected(STDIN_FILENO))
                terminate_requested = SIGHUP;
            if (terminate_requested)
                break;
            idle_ticks++;
            continue;
        }
        idle_ticks = 0;
        if (ch < 0 || ch > UCHAR_MAX)
            continue;

        byte = (char)(unsigned char)ch;
        if (byte == end_marker[matched]) {
            matched++;
            if (matched == marker_length)
                complete = 1;
            continue;
        }

        if (matched > 0) {
            append_bracketed_paste_bytes(&buffer, &length, &capacity,
                                         end_marker, matched, &too_large);
            matched = 0;
        }
        if (byte == end_marker[0]) {
            matched = 1;
        } else {
            append_bracketed_paste_bytes(&buffer, &length, &capacity,
                                         &byte, 1, &too_large);
        }
    }

    (void)keypad(stdscr, TRUE);
    timeout(250);

    if (!complete) {
        free(buffer);
        set_status("Paste interrupted before terminal delimiter");
        return NULL;
    }
    if (too_large) {
        free(buffer);
        set_status("Paste is larger than 16 MiB");
        return NULL;
    }

    if (!buffer)
        buffer = xmalloc(1);
    buffer[length] = '\0';
    return buffer;
}

static int read_editor_key(void)
{
    char sequence[32];
    int len = 0;
    int ch = getch();

    if (ch == ERR && terminal_input_disconnected(STDIN_FILENO))
        terminate_requested = SIGHUP;
    if (ch != 27)
        return normalize_terminfo_key(ch);

    timeout(25);
    ch = getch();
    if (ch != '[' && ch != 'O') {
        if (ch != ERR)
            ungetch(ch);
        timeout(250);
        return 27;
    }

    sequence[len++] = (char)ch;
    while (len < (int)sizeof(sequence) - 1) {
        ch = getch();
        if (ch == ERR)
            break;
        if (ch < 0 || ch > UCHAR_MAX)
            break;
        sequence[len++] = (char)ch;
        if ((ch >= '@' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '~')
            break;
    }
    sequence[len] = '\0';
    timeout(250);

    if (strcmp(sequence, BRACKETED_PASTE_BEGIN) == 0) {
        free(pending_bracketed_paste);
        pending_bracketed_paste = capture_bracketed_paste();
        return KEY_BRACKETED_PASTE;
    }

    ch = parse_modified_csi(sequence);
    if (ch)
        return ch;

    set_status("Unknown terminal key sequence ignored");
    return ERR;
}

static unsigned long long path_hash(const char *s)
{
    unsigned long long h = 1469598103934665603ULL;

    while (s && *s) {
        h ^= (unsigned char)*s++;
        h *= 1099511628211ULL;
    }

    return h;
}

static int snprintf_ok(int n, size_t size)
{
    return n >= 0 && (size_t)n < size;
}

static int format_string(char *out, size_t outsz, const char *format, ...)
    __attribute__((format(printf, 3, 4)));

static int format_string(char *out, size_t outsz, const char *format, ...)
{
    va_list args;
    int written;

    if (!out || outsz == 0)
        return 0;

    va_start(args, format);
    written = vsnprintf(out, outsz, format, args);
    va_end(args);
    if (written < 0 || (size_t)written >= outsz) {
        out[0] = '\0';
        return 0;
    }
    return 1;
}

static int copy_string(char *out, size_t outsz, const char *source)
{
    size_t len;

    if (!out || outsz == 0)
        return 0;
    if (!source)
        source = "";
    len = strlen(source);
    if (len >= outsz) {
        out[0] = '\0';
        return 0;
    }
    memcpy(out, source, len + 1);
    return 1;
}

static int ensure_dir(const char *path)
{
    struct stat st;

    if (!path || !*path)
        return 0;
    if (stat(path, &st) == 0)
        return S_ISDIR(st.st_mode);
    if (mkdir(path, 0700) == 0)
        return 1;
    return errno == EEXIST && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int mkdirs(const char *path)
{
    char tmp[PATH_MAX];
    size_t len;

    if (!path || !*path)
        return 0;
    if (!snprintf_ok(snprintf(tmp, sizeof(tmp), "%s", path), sizeof(tmp)))
        return 0;

    len = strlen(tmp);
    while (len > 1 && tmp[len - 1] == '/')
        tmp[--len] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (!ensure_dir(tmp))
                return 0;
            *p = '/';
        }
    }

    return ensure_dir(tmp);
}

static int home_path(char *out, size_t outsz, const char *suffix)
{
    const char *home = getenv("HOME");

    if (!out || outsz == 0)
        return 0;
    out[0] = '\0';
    if (!home || !*home || !suffix || !*suffix)
        return 0;

    return snprintf_ok(snprintf(out, outsz, "%s/%s", home, suffix), outsz);
}

static int simplewords_state_dir(char *out, size_t outsz)
{
    if (!home_path(out, outsz, ".local/state/simplewords"))
        return 0;
    return mkdirs(out);
}

static int simplewords_autosave_dir(char *out, size_t outsz)
{
    char state[PATH_MAX];

    if (!simplewords_state_dir(state, sizeof(state)))
        return 0;
    if (!snprintf_ok(snprintf(out, outsz, "%s/autosave", state), outsz))
        return 0;
    return mkdirs(out);
}

static int simplewords_backup_dir(char *out, size_t outsz)
{
    char state[PATH_MAX];

    if (!simplewords_state_dir(state, sizeof(state)))
        return 0;
    if (!snprintf_ok(snprintf(out, outsz, "%s/backups", state), outsz))
        return 0;
    return mkdirs(out);
}

static int simplewords_recovery_dir(char *out, size_t outsz)
{
    char state[PATH_MAX];

    if (!simplewords_state_dir(state, sizeof(state)))
        return 0;
    if (!snprintf_ok(snprintf(out, outsz, "%s/recovery", state), outsz))
        return 0;
    return mkdirs(out);
}

static int simplewords_lock_dir(char *out, size_t outsz)
{
    char state[PATH_MAX];

    if (!simplewords_state_dir(state, sizeof(state)))
        return 0;
    if (!snprintf_ok(snprintf(out, outsz, "%s/locks", state), outsz))
        return 0;
    return mkdirs(out);
}

static void acquire_workspace_lock(void)
{
    char state[PATH_MAX];
    char path[PATH_MAX];

    if (!simplewords_state_dir(state, sizeof(state)) ||
        !format_string(path, sizeof(path), "%s/workspace.lock", state))
        return;
    workspace_lock_fd = open(path, O_CREAT | O_RDWR, 0600);
    if (workspace_lock_fd < 0)
        return;
    if (flock(workspace_lock_fd, LOCK_EX | LOCK_NB) != 0) {
        close(workspace_lock_fd);
        workspace_lock_fd = -1;
        return;
    }

    workspace_session_owner = 1;
    if (ftruncate(workspace_lock_fd, 0) == 0) {
        dprintf(workspace_lock_fd, "%ld\n", (long)getpid());
        (void)fsync(workspace_lock_fd);
    }
}

static void release_workspace_lock(void)
{
    if (workspace_lock_fd >= 0) {
        flock(workspace_lock_fd, LOCK_UN);
        close(workspace_lock_fd);
    }
    workspace_lock_fd = -1;
    workspace_session_owner = 0;
}

static int workspace_socket_file(char *out, size_t outsz)
{
    char state[PATH_MAX];

    if (!simplewords_state_dir(state, sizeof(state)))
        return 0;
    return format_string(out, outsz, "%s/workspace.sock", state);
}

static int write_all_fd(int fd, const void *data, size_t length)
{
    const unsigned char *bytes = data;

    while (length > 0) {
        ssize_t written = write(fd, bytes, length);

        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            return 0;
        bytes += written;
        length -= (size_t)written;
    }
    return 1;
}

static int read_all_fd(int fd, void *data, size_t length)
{
    unsigned char *bytes = data;

    while (length > 0) {
        ssize_t received = read(fd, bytes, length);

        if (received < 0 && errno == EINTR)
            continue;
        if (received <= 0)
            return 0;
        bytes += received;
        length -= (size_t)received;
    }
    return 1;
}

static void encode_u32(unsigned char out[4], uint32_t value)
{
    out[0] = (unsigned char)(value >> 24);
    out[1] = (unsigned char)(value >> 16);
    out[2] = (unsigned char)(value >> 8);
    out[3] = (unsigned char)value;
}

static uint32_t decode_u32(const unsigned char input[4])
{
    return ((uint32_t)input[0] << 24) |
           ((uint32_t)input[1] << 16) |
           ((uint32_t)input[2] << 8) |
           (uint32_t)input[3];
}

static int start_workspace_server(void)
{
    struct sockaddr_un address;
    int flags;

    if (!workspace_session_owner || workspace_server_fd >= 0 ||
        !workspace_socket_file(workspace_socket_path,
                               sizeof(workspace_socket_path)) ||
        strlen(workspace_socket_path) >= sizeof(address.sun_path))
        return 0;

    workspace_server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (workspace_server_fd < 0)
        return 0;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    copy_string(address.sun_path, sizeof(address.sun_path),
                workspace_socket_path);
    unlink(workspace_socket_path);
    if (bind(workspace_server_fd, (struct sockaddr *)&address,
             sizeof(address)) != 0 ||
        chmod(workspace_socket_path, 0600) != 0 ||
        listen(workspace_server_fd, 4) != 0) {
        close(workspace_server_fd);
        workspace_server_fd = -1;
        unlink(workspace_socket_path);
        workspace_socket_path[0] = '\0';
        return 0;
    }
    flags = fcntl(workspace_server_fd, F_GETFL, 0);
    if (flags >= 0)
        (void)fcntl(workspace_server_fd, F_SETFL, flags | O_NONBLOCK);
    return 1;
}

static int claim_workspace_if_available(void)
{
    if (workspace_session_owner)
        return 0;
    acquire_workspace_lock();
    if (!workspace_session_owner)
        return 0;
    (void)start_workspace_server();
    return 1;
}

static void stop_workspace_server(void)
{
    if (workspace_server_fd >= 0)
        close(workspace_server_fd);
    workspace_server_fd = -1;
    if (workspace_socket_path[0])
        unlink(workspace_socket_path);
    workspace_socket_path[0] = '\0';
}

static int forward_files_to_workspace(int argc, char **argv)
{
    struct sockaddr_un address;
    char socket_path[PATH_MAX];
    int fd = -1;
    int ok = 1;

    if (argc < 1 || !workspace_socket_file(socket_path, sizeof(socket_path)) ||
        strlen(socket_path) >= sizeof(address.sun_path))
        return 0;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    copy_string(address.sun_path, sizeof(address.sun_path), socket_path);
    for (int attempt = 0; attempt < 5; attempt++) {
        struct timespec pause = {0, 50000000L};

        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd >= 0 && connect(fd, (struct sockaddr *)&address,
                               sizeof(address)) == 0)
            break;
        if (fd >= 0)
            close(fd);
        fd = -1;
        nanosleep(&pause, NULL);
    }
    if (fd < 0)
        return 0;

    ok = write_all_fd(fd, "SWB1", 4);
    for (int i = 0; ok && i < argc; i++) {
        char canonical[512];
        unsigned char length_bytes[4];
        size_t length;

        if (!canonical_visit_path(argv[i], canonical, sizeof(canonical))) {
            ok = 0;
            break;
        }
        length = strlen(canonical);
        encode_u32(length_bytes, (uint32_t)length);
        ok = write_all_fd(fd, length_bytes, sizeof(length_bytes)) &&
             write_all_fd(fd, canonical, length);
    }
    if (ok) {
        unsigned char end[4] = {0, 0, 0, 0};
        ok = write_all_fd(fd, end, sizeof(end));
    }
    close(fd);
    return ok;
}

static int poll_workspace_requests(void)
{
    int opened = 0;

    if (workspace_server_fd < 0)
        return 0;
    while (1) {
        int client = accept(workspace_server_fd, NULL, NULL);
        struct timeval receive_timeout = {0, 250000};
        char magic[4];

        if (client < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        (void)setsockopt(client, SOL_SOCKET, SO_RCVTIMEO,
                         &receive_timeout, sizeof(receive_timeout));
        if (!read_all_fd(client, magic, sizeof(magic)) ||
            memcmp(magic, "SWB1", 4) != 0) {
            close(client);
            continue;
        }
        while (1) {
            unsigned char length_bytes[4];
            uint32_t length;
            char path[512];

            if (!read_all_fd(client, length_bytes, sizeof(length_bytes)))
                break;
            length = decode_u32(length_bytes);
            if (length == 0)
                break;
            if (length >= sizeof(path) ||
                !read_all_fd(client, path, length))
                break;
            path[length] = '\0';
            visit_file_in_buffer(path);
            opened++;
        }
        close(client);
    }
    if (opened > 0)
        set_status(opened == 1 ? "Opened command-line file in a buffer" :
                                "Opened command-line files in buffers");
    return opened;
}

static void release_document_edit_lock(EditorBuffer *buffer)
{
    if (!buffer)
        return;
    if (buffer->document_lock_fd >= 0) {
        flock(buffer->document_lock_fd, LOCK_UN);
        close(buffer->document_lock_fd);
    }
    buffer->document_lock_fd = -1;
    buffer->lock_blocked = 0;
}

static int ensure_document_edit_lock(void)
{
    EditorBuffer *buffer = &editor_buffers[active_buffer_index];
    char directory[PATH_MAX];
    char lock_path[PATH_MAX];
    int fd;

    if (!buffer_system_ready || !filename[0])
        return 1;
    if (buffer->document_lock_fd >= 0)
        return 1;
    if (!simplewords_lock_dir(directory, sizeof(directory)) ||
        !format_string(lock_path, sizeof(lock_path), "%s/%016llx.lock",
                       directory, path_hash(filename))) {
        set_status("Cannot create document lock; edit cancelled");
        return 0;
    }

    fd = open(lock_path, O_CREAT | O_RDWR, 0600);
    if (fd < 0 || flock(fd, LOCK_EX | LOCK_NB) != 0) {
        if (fd >= 0)
            close(fd);
        buffer->lock_blocked = 1;
        set_status("This file is being edited in another SimpleWords; edit blocked");
        return 0;
    }

    buffer->document_lock_fd = fd;
    buffer->lock_blocked = 0;
    if (ftruncate(fd, 0) == 0) {
        dprintf(fd, "%ld\n%s\n", (long)getpid(), filename);
        (void)fsync(fd);
    }
    return 1;
}

static int regular_file(const char *path)
{
    struct stat st;
    return path && *path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static int copy_file_for_migration(const char *src, const char *dst)
{
    struct stat st;
    int have_stat = stat(src, &st) == 0;
    FILE *in = fopen(src, "rb");
    FILE *out;
    char buf[8192];
    size_t got;
    int ok = 1;

    if (!in)
        return 0;
    out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return 0;
    }

    while ((got = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, got, out) != got) {
            ok = 0;
            break;
        }
    }

    if (ferror(in))
        ok = 0;
    fclose(in);
    if (fclose(out) != 0)
        ok = 0;

    if (ok && have_stat) {
        struct utimbuf times;

        times.actime = st.st_atime;
        times.modtime = st.st_mtime;
        if (utime(dst, &times) != 0)
            ok = 0;
    }

    if (!ok)
        unlink(dst);
    return ok;
}

static void migrate_file_if_safe(const char *src, const char *dst)
{
    if (!regular_file(src) || regular_file(dst))
        return;
    if (rename(src, dst) == 0)
        return;
    if (copy_file_for_migration(src, dst))
        unlink(src);
}

static int backup_existing_document(const char *path)
{
    char dir[PATH_MAX];
    char backup[PATH_MAX];
    const char *base;
    time_t now;
    struct tm tm_now;
    char stamp[32];
    int fd;

    if (!regular_file(path))
        return 1;
    if (!simplewords_backup_dir(dir, sizeof(dir))) {
        errno = EIO;
        return 0;
    }

    base = strrchr(path, '/');
    base = base ? base + 1 : path;
    now = time(NULL);
    if (!localtime_r(&now, &tm_now)) {
        errno = EIO;
        return 0;
    }
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm_now);

    if (!snprintf_ok(snprintf(backup, sizeof(backup),
                              "%s/%016llx-%s.%s.%ld.XXXXXX",
                              dir, path_hash(path), base, stamp, (long)getpid()),
                     sizeof(backup))) {
        errno = ENAMETOOLONG;
        return 0;
    }

    fd = mkstemp(backup);
    if (fd < 0)
        return 0;
    close(fd);
    unlink(backup);

    return copy_file_for_migration(path, backup);
}

static void autosave_path_for(const char *docpath, char *out, size_t outsz)
{
    char dir[PATH_MAX];
    const char *base;

    if (!simplewords_autosave_dir(dir, sizeof(dir))) {
        out[0] = '\0';
        return;
    }

    base = strrchr(docpath, '/');
    base = base ? base + 1 : docpath;

    format_string(out, outsz, "%s/%016llx-%s.autosave",
                  dir, path_hash(docpath), base);
}

static void legacy_hashed_autosave_path_for(const char *docpath, char *out, size_t outsz)
{
    const char *home = getenv("HOME");
    const char *base;

    if (!home) {
        out[0] = '\0';
        return;
    }

    base = strrchr(docpath, '/');
    base = base ? base + 1 : docpath;

    format_string(out, outsz, "%s/writing/autosave/%016llx-%s.autosave",
                  home, path_hash(docpath), base);
}

static void legacy_autosave_path_for(const char *docpath, char *out, size_t outsz)
{
    const char *home = getenv("HOME");
    const char *base;

    if (!home) {
        out[0] = '\0';
        return;
    }

    base = strrchr(docpath, '/');
    base = base ? base + 1 : docpath;

    format_string(out, outsz, "%s/writing/autosave/%s.autosave", home, base);
}

static void untitled_autosave_path_for(const char *name, char *out, size_t outsz)
{
    char dir[PATH_MAX];

    if (!name || !*name || strchr(name, '/')) {
        if (out && outsz)
            out[0] = '\0';
        return;
    }

    if (!simplewords_autosave_dir(dir, sizeof(dir))) {
        out[0] = '\0';
        return;
    }

    format_string(out, outsz, "%s/%s.autosave", dir, name);
}

static void remove_autosaves_for(const char *docpath)
{
    char candidates[3][PATH_MAX];

    autosave_path_for(docpath, candidates[0], sizeof(candidates[0]));
    legacy_hashed_autosave_path_for(docpath, candidates[1], sizeof(candidates[1]));
    legacy_autosave_path_for(docpath, candidates[2], sizeof(candidates[2]));

    for (size_t i = 0; i < 3; i++) {
        if (candidates[i][0])
            unlink(candidates[i]);
    }
}

static void remove_untitled_autosave(const char *name)
{
    char path[PATH_MAX];

    untitled_autosave_path_for(name, path, sizeof(path));
    if (path[0])
        unlink(path);
}

static void clear_pending_recovery(void)
{
    pending_recovery_doc[0] = '\0';
    pending_recovery_autosave[0] = '\0';
}

static void clear_opened_recovery(void)
{
    opened_recovery_doc[0] = '\0';
    opened_recovery_autosave[0] = '\0';
}

static void remember_pending_recovery(const char *docpath, const char *autosave)
{
    if (!docpath || !*docpath || !autosave || !*autosave) {
        clear_pending_recovery();
        return;
    }

    snprintf(pending_recovery_doc, sizeof(pending_recovery_doc), "%s", docpath);
    snprintf(pending_recovery_autosave, sizeof(pending_recovery_autosave), "%s", autosave);
}

static void remember_opened_recovery(const char *docpath, const char *autosave)
{
    if (!docpath || !*docpath || !autosave || !*autosave) {
        clear_opened_recovery();
        return;
    }

    snprintf(opened_recovery_doc, sizeof(opened_recovery_doc), "%s", docpath);
    snprintf(opened_recovery_autosave, sizeof(opened_recovery_autosave), "%s", autosave);
}

static int pending_recovery_for(const char *docpath)
{
    return docpath && *docpath && pending_recovery_doc[0] &&
           strcmp(pending_recovery_doc, docpath) == 0 &&
           pending_recovery_autosave[0];
}

static int opened_recovery_for(const char *docpath)
{
    return docpath && *docpath && opened_recovery_doc[0] &&
           strcmp(opened_recovery_doc, docpath) == 0 &&
           opened_recovery_autosave[0];
}

static int recovery_prompt_active(void)
{
    return pending_recovery_for(filename);
}

static const char *current_footer_text(void)
{
    if (recovery_prompt_active())
        return recovery_footer_text;
    return status_msg[0] ? status_msg : help_text;
}

static int preserved_recovery_path_for(const char *docpath, char *out, size_t outsz)
{
    char dir[PATH_MAX];
    const char *base;

    if (!docpath || !*docpath || !simplewords_recovery_dir(dir, sizeof(dir))) {
        if (out && outsz)
            out[0] = '\0';
        return 0;
    }

    base = strrchr(docpath, '/');
    base = base ? base + 1 : docpath;
    return snprintf_ok(snprintf(out, outsz, "%s/%016llx-%s.recovery",
                                dir, path_hash(docpath), base), outsz);
}

static int preserve_recovery_autosave(const char *docpath, const char *autosave,
                                      char *out, size_t outsz)
{
    char preserved[PATH_MAX];

    if (!regular_file(autosave) ||
        !preserved_recovery_path_for(docpath, preserved, sizeof(preserved))) {
        if (out && outsz)
            out[0] = '\0';
        return 0;
    }

    if (strcmp(autosave, preserved) != 0) {
        if (!copy_file_for_migration(autosave, preserved)) {
            if (out && outsz)
                out[0] = '\0';
            return 0;
        }
        remove_autosaves_for(docpath);
    }

    if (out && outsz)
        snprintf(out, outsz, "%s", preserved);
    return 1;
}

static int file_mtime(const char *path, time_t *out)
{
    struct stat st;

    if (stat(path, &st) != 0)
        return 0;

    *out = st.st_mtime;
    return 1;
}

static void remember_disk_revision(EditorBuffer *buffer, const char *path)
{
    struct stat st;

    if (!buffer)
        return;
    buffer->disk_revision_known = 0;
    if (!path || !*path || stat(path, &st) != 0 || !S_ISREG(st.st_mode))
        return;
    buffer->disk_revision_known = 1;
    buffer->disk_mtime = st.st_mtime;
    buffer->disk_size = st.st_size;
    buffer->disk_device = st.st_dev;
    buffer->disk_inode = st.st_ino;
}

static int disk_revision_changed(const EditorBuffer *buffer, const char *path)
{
    struct stat st;

    if (!buffer || !buffer->disk_revision_known || !path || !*path)
        return 0;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
        return 1;
    return st.st_mtime != buffer->disk_mtime ||
           st.st_size != buffer->disk_size ||
           st.st_dev != buffer->disk_device ||
           st.st_ino != buffer->disk_inode;
}

static FILE *persistence_log_handle(void)
{
    static int initialized = 0;
    static FILE *fp = NULL;
    const char *path;

    if (initialized)
        return fp;

    initialized = 1;
    path = getenv("SIMPLEWORDS_PERSIST_LOG");
    if (!path || !*path)
        return NULL;

    if (strcmp(path, "-") == 0)
        fp = stderr;
    else
        fp = fopen(path, "a");

    if (fp)
        setvbuf(fp, NULL, _IOLBF, 0);
    return fp;
}

static int persistence_logging_enabled(void)
{
    return persistence_log_handle() != NULL;
}

static const char *load_result_name(LoadResult result)
{
    switch (result) {
    case LOAD_RESULT_FAILED:
        return "failed";
    case LOAD_RESULT_DISK:
        return "disk";
    case LOAD_RESULT_AUTOSAVE:
        return "autosave";
    case LOAD_RESULT_NEW:
        return "new";
    }
    return "unknown";
}

static void persistence_log_event(const char *func, const char *fmt, ...)
{
    FILE *fp = persistence_log_handle();
    char stamp[64] = "";
    time_t now;
    struct tm tm_now;
    va_list ap;

    if (!fp)
        return;

    now = time(NULL);
    if (localtime_r(&now, &tm_now))
        strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S %z", &tm_now);
    else
        snprintf(stamp, sizeof(stamp), "%lld", (long long)now);

    fprintf(fp, "%s pid=%ld %s: ", stamp, (long)getpid(), func ? func : "?");
    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);
    fputc('\n', fp);
    fflush(fp);
}

static void real_path_for_log(const char *path, char *out, size_t outsz)
{
    char resolved[PATH_MAX];
    int saved_errno;

    if (!out || outsz == 0)
        return;
    out[0] = '\0';
    if (!path || !*path) {
        snprintf(out, outsz, "(none)");
        return;
    }

    if (realpath(path, resolved)) {
        snprintf(out, outsz, "%s", resolved);
        return;
    }

    saved_errno = errno;
    snprintf(out, outsz, "(realpath failed: %s)", strerror(saved_errno));
}

static void autosave_path_for_log(const char *docpath, char *out, size_t outsz)
{
    const char *home = getenv("HOME");
    const char *base;

    if (!out || outsz == 0)
        return;
    out[0] = '\0';
    if (!home || !*home || !docpath || !*docpath)
        return;

    base = strrchr(docpath, '/');
    base = base ? base + 1 : docpath;
    snprintf(out, outsz, "%s/.local/state/simplewords/autosave/%016llx-%s.autosave",
             home, path_hash(docpath), base);
}

static void persistence_log_state(const char *func, const char *phase, const char *path)
{
    const char *subject = path && *path ? path : filename;
    char filename_real[PATH_MAX];
    char subject_real[PATH_MAX];
    char autosave[PATH_MAX];
    time_t real_mtime = 0;
    time_t autosave_mtime = 0;
    int have_real_mtime = 0;
    int have_autosave_mtime = 0;

    if (!persistence_logging_enabled())
        return;

    real_path_for_log(filename, filename_real, sizeof(filename_real));
    real_path_for_log(subject, subject_real, sizeof(subject_real));
    autosave_path_for_log(subject, autosave, sizeof(autosave));
    if (subject && *subject)
        have_real_mtime = file_mtime(subject, &real_mtime);
    if (autosave[0])
        have_autosave_mtime = file_mtime(autosave, &autosave_mtime);

    persistence_log_event(func,
                          "%s current_filename='%s' current_filename_real='%s' subject_path='%s' subject_real='%s' untitled_name='%s' computed_autosave_path='%s' autosave_exists=%d mtime_real=%lld mtime_autosave=%lld dirty=%d autosave_dirty=%d last_edit_time=%lld line_count=%d cy=%d cx=%d top=%d status='%s'",
                          phase ? phase : "state", filename, filename_real,
                          subject ? subject : "", subject_real, untitled_name,
                          autosave, have_autosave_mtime,
                          have_real_mtime ? (long long)real_mtime : -1LL,
                          have_autosave_mtime ? (long long)autosave_mtime : -1LL,
                          dirty, autosave_dirty, (long long)last_edit_time,
                          line_count, cy, cx, top, status_msg);
}

static void persistence_log_loaded_file(const char *func, const char *path)
{
    persistence_log_event(func, "buffer loaded/replaced from path='%s'", path ? path : "");
    persistence_log_state(func, "after buffer load/replace", path);
}

static void set_dirty_logged(int value, const char *func, int line, const char *reason)
{
    persistence_log_event(func, "assign dirty old=%d new=%d line=%d reason='%s' current_filename='%s' untitled_name='%s'",
                          dirty, value, line, reason ? reason : "", filename, untitled_name);
    dirty = value;
}

static void set_autosave_dirty_logged(int value, const char *func, int line, const char *reason)
{
    persistence_log_event(func, "assign autosave_dirty old=%d new=%d line=%d reason='%s' current_filename='%s' untitled_name='%s'",
                          autosave_dirty, value, line, reason ? reason : "", filename, untitled_name);
    autosave_dirty = value;
}

static void set_last_edit_time_logged(time_t value, const char *func, int line, const char *reason)
{
    persistence_log_event(func, "assign last_edit_time old=%lld new=%lld line=%d reason='%s' current_filename='%s' untitled_name='%s'",
                          (long long)last_edit_time, (long long)value, line,
                          reason ? reason : "", filename, untitled_name);
    last_edit_time = value;
}

static void clear_document(void)
{
    persistence_log_event(__func__, "enter clears buffer line_count=%d", line_count);
    persistence_log_state(__func__, "before clear_document", NULL);
    reset_wrap_cache();
    for (int i = 0; i < line_count; i++)
        free(lines[i]);

    line_count = 0;
    persistence_log_event(__func__, "exit cleared buffer line_count=%d", line_count);
    persistence_log_state(__func__, "after clear_document", NULL);
}

static void free_document_lines(char **doc_lines, int doc_line_count)
{
    for (int i = 0; i < doc_line_count; i++)
        free(doc_lines[i]);
}

static int read_document_lines(const char *path, char **doc_lines, int *doc_line_count)
{
    FILE *fp;
    char buf[MAX_LINE];
    int count = 0;

    *doc_line_count = 0;
    fp = fopen(path, "r");
    if (!fp)
        return 0;

    while (count < MAX_LINES && fgets(buf, sizeof(buf), fp)) {
        size_t len = strlen(buf);
        int complete_line = len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r');

        if (!complete_line && len == sizeof(buf) - 1) {
            fclose(fp);
            free_document_lines(doc_lines, count);
            errno = EOVERFLOW;
            return 0;
        }

        while (len && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
            buf[--len] = '\0';
        doc_lines[count++] = new_line(buf);
    }

    if (count >= MAX_LINES) {
        int ch = fgetc(fp);
        if (ch != EOF) {
            fclose(fp);
            free_document_lines(doc_lines, count);
            errno = EOVERFLOW;
            return 0;
        }
    }

    if (ferror(fp)) {
        int saved_errno = errno ? errno : EIO;
        fclose(fp);
        free_document_lines(doc_lines, count);
        errno = saved_errno;
        return 0;
    }

    if (fclose(fp) != 0) {
        int saved_errno = errno ? errno : EIO;
        free_document_lines(doc_lines, count);
        errno = saved_errno;
        return 0;
    }

    if (count == 0)
        doc_lines[count++] = new_line("");

    *doc_line_count = count;
    return 1;
}

static int read_document_into_buffer(const char *path)
{
    char *new_lines[MAX_LINES];
    int new_line_count = 0;

    persistence_log_event(__func__, "enter path='%s'", path ? path : "");
    persistence_log_state(__func__, "read_document entry", path);
    memset(new_lines, 0, sizeof(new_lines));

    if (!read_document_lines(path, new_lines, &new_line_count)) {
        persistence_log_event(__func__, "exit false read failed path='%s' errno=%d reason='%s'",
                              path ? path : "", errno, strerror(errno));
        persistence_log_state(__func__, "read_document failed without replacing buffer", path);
        return 0;
    }

    persistence_log_event(__func__, "replacing buffer from path='%s'", path ? path : "");
    clear_document();
    for (int i = 0; i < new_line_count; i++)
        lines[i] = new_lines[i];
    line_count = new_line_count;
    reset_wrap_cache();

    persistence_log_loaded_file(__func__, path);
    persistence_log_event(__func__, "exit true path='%s' line_count=%d", path ? path : "", line_count);
    return 1;
}

static int document_is_empty(void);

static int load_autosave_if_newer(const char *docpath)
{
    char candidates[3][PATH_MAX];
    char best[PATH_MAX] = "";
    char new_path[PATH_MAX] = "";
    time_t doc_time = 0;
    time_t best_time = 0;
    int have_doc_time;

    persistence_log_event(__func__, "enter docpath='%s'", docpath ? docpath : "");
    persistence_log_state(__func__, "load_autosave_if_newer entry", docpath);

    autosave_path_for(docpath, candidates[0], sizeof(candidates[0]));
    legacy_hashed_autosave_path_for(docpath, candidates[1], sizeof(candidates[1]));
    legacy_autosave_path_for(docpath, candidates[2], sizeof(candidates[2]));

    persistence_log_event(__func__, "computed candidates current='%s' legacy_hashed='%s' legacy_plain='%s'", candidates[0], candidates[1], candidates[2]);

    if (candidates[0][0])
        copy_string(new_path, sizeof(new_path), candidates[0]);

    for (size_t i = 0; i < 3; i++) {
        time_t candidate_time = 0;

        if (!candidates[i][0]) {
            persistence_log_event(__func__, "candidate[%zu] skipped: empty path", i);
            continue;
        }
        if (!file_mtime(candidates[i], &candidate_time)) {
            persistence_log_event(__func__, "candidate[%zu] missing path='%s'", i, candidates[i]);
            continue;
        }
        persistence_log_event(__func__, "candidate[%zu] exists path='%s' mtime=%lld", i, candidates[i], (long long)candidate_time);
        if (!best[0] || candidate_time > best_time) {
            copy_string(best, sizeof(best), candidates[i]);
            best_time = candidate_time;
        }
    }

    if (!best[0]) {
        persistence_log_event(__func__, "exit false reason='no autosave candidates exist' docpath='%s'", docpath ? docpath : "");
        persistence_log_state(__func__, "load_autosave_if_newer no autosave", docpath);
        return 0;
    }

    persistence_log_event(__func__, "best autosave candidate path='%s' mtime=%lld", best, (long long)best_time);

    if (new_path[0] && strcmp(best, new_path) != 0 && !regular_file(new_path)) {
        persistence_log_event(__func__, "attempting autosave migration from='%s' to='%s'", best, new_path);
        migrate_file_if_safe(best, new_path);
        if (file_mtime(new_path, &best_time)) {
            copy_string(best, sizeof(best), new_path);
            persistence_log_event(__func__, "migration available at new path='%s' mtime=%lld", best, (long long)best_time);
        }
    }

    have_doc_time = file_mtime(docpath, &doc_time);
    persistence_log_event(__func__, "mtime comparison doc_exists=%d doc_mtime=%lld autosave_mtime=%lld", have_doc_time, have_doc_time ? (long long)doc_time : -1LL, (long long)best_time);
    if (have_doc_time && best_time <= doc_time) {
        clear_pending_recovery();
        persistence_log_event(__func__, "exit false reason='autosave is not newer' docpath='%s' autosave='%s'", docpath ? docpath : "", best);
        persistence_log_state(__func__, "load_autosave_if_newer stale autosave", docpath);
        return 0;
    }

    if (have_doc_time) {
        char preserved[PATH_MAX];

        if (preserve_recovery_autosave(docpath, best, preserved, sizeof(preserved))) {
            remember_pending_recovery(docpath, preserved);
            set_status("Recovered draft preserved");
        } else {
            remember_pending_recovery(docpath, best);
            set_status("Recovered draft preserved");
        }
        persistence_log_event(__func__, "exit false reason='newer autosave preserved without replacing existing disk file' docpath='%s' autosave='%s' pending='%s'", docpath ? docpath : "", best, pending_recovery_autosave);
        persistence_log_state(__func__, "load_autosave_if_newer preserved autosave", docpath);
        return 0;
    }

    clear_pending_recovery();
    if (!read_document_into_buffer(best)) {
        persistence_log_event(__func__, "exit false reason='failed to read autosave' autosave='%s'", best);
        persistence_log_state(__func__, "load_autosave_if_newer read failure", docpath);
        return 0;
    }

    SET_DIRTY(1, "persistence recovery");
    SET_AUTOSAVE_DIRTY(0, "persistence recovery");
    SET_LAST_EDIT_TIME(0, "persistence recovery");
    if (document_is_empty())
        clear_status();
    else
        set_status(have_doc_time ? "Recovered newer autosave" :
                                  "Recovered autosave");
    persistence_log_event(__func__, "exit true reason='autosave loaded' autosave='%s' docpath='%s'", best, docpath ? docpath : "");
    persistence_log_state(__func__, "load_autosave_if_newer recovered", docpath);
    return 1;
}

static void remember_directory(const char *path, char *directory, size_t directory_size);
static int containing_directory_exists(const char *path);

static void reset_edit_state_after_load(void)
{
    clear_selection();
    clear_undo_history();
    goal_col = -1;
    clear_cursor_affinity();
}

static void finish_loaded_position(int restore_pos,
                                   int restore_y, int restore_x, int restore_top)
{
    if (restore_pos) {
        cy = restore_y;
        cx = restore_x;
        top = restore_top;
    } else {
        cy = 0;
        cx = 0;
        top = 0;
    }

    clamp_cursor();
    if (top < 0)
        top = 0;
    clamp_top();
    reset_edit_state_after_load();
}

static LoadResult load_file_at_position(const char *path, int recover_autosave, int restore_pos,
                                        int restore_y, int restore_x, int restore_top)
{
    persistence_log_event(__func__, "enter path='%s' recover_autosave=%d restore_pos=%d restore_y=%d restore_x=%d restore_top=%d",
                          path ? path : "", recover_autosave, restore_pos,
                          restore_y, restore_x, restore_top);
    persistence_log_state(__func__, "load_file_at_position entry", path);

    if (!read_document_into_buffer(path)) {
        int open_errno = errno;
        char msg[700];

        clear_document();
        lines[line_count++] = new_line("");
        cy = cx = top = 0;

        if (open_errno == ENOENT) {
            strncpy(filename, path, sizeof(filename) - 1);
            filename[sizeof(filename) - 1] = '\0';
            remember_disk_revision(&editor_buffers[active_buffer_index],
                                   filename);
            snprintf(last_open_file, sizeof(last_open_file), "%s", filename);
            if (containing_directory_exists(filename)) {
                remember_directory(filename, last_open_directory,
                                   sizeof(last_open_directory));
                remember_directory(filename, last_save_directory,
                                   sizeof(last_save_directory));
            }
            if (recover_autosave) {
                int recovered;

                persistence_log_event(__func__, "calling load_autosave_if_newer path='%s' reason='disk file missing'",
                                      path ? path : "");
                recovered = load_autosave_if_newer(path);
                persistence_log_event(__func__, "load_autosave_if_newer returned %d path='%s'",
                                      recovered, path ? path : "");
                if (recovered) {
                    finish_loaded_position(restore_pos, restore_y, restore_x, restore_top);
                    persistence_log_event(__func__, "exit result=%s ultimately_loaded='autosave' path='%s'",
                                          load_result_name(LOAD_RESULT_AUTOSAVE), path ? path : "");
                    persistence_log_state(__func__, "load_file_at_position exit autosave", path);
                    return LOAD_RESULT_AUTOSAVE;
                }
            } else {
                persistence_log_event(__func__, "load_autosave_if_newer not called path='%s' reason='recover_autosave disabled'",
                                      path ? path : "");
            }
            SET_DIRTY(0, "load reset edit state");
            SET_AUTOSAVE_DIRTY(0, "load reset edit state");
            SET_LAST_EDIT_TIME(0, "load reset edit state");
            reset_edit_state_after_load();
            set_status("New file");
            persistence_log_event(__func__, "exit result=%s ultimately_loaded='new empty buffer' path='%s'",
                                  load_result_name(LOAD_RESULT_NEW), path ? path : "");
            persistence_log_state(__func__, "load_file_at_position exit new", path);
            return LOAD_RESULT_NEW;
        }

        snprintf(msg, sizeof(msg),
                 "Open failed: %s: %s",
                 path, strerror(open_errno));
        filename[0] = '\0';
        SET_LAST_EDIT_TIME(0, "open failed reset edit time");
        reset_edit_state_after_load();
        set_status(msg);
        persistence_log_event(__func__, "exit result=%s reason='open failed' path='%s' errno=%d",
                              load_result_name(LOAD_RESULT_FAILED), path ? path : "", open_errno);
        persistence_log_state(__func__, "load_file_at_position exit failed", path);
        return LOAD_RESULT_FAILED;
    }

    strncpy(filename, path, sizeof(filename) - 1);
    filename[sizeof(filename) - 1] = '\0';
    remember_disk_revision(&editor_buffers[active_buffer_index], filename);
    snprintf(last_open_file, sizeof(last_open_file), "%s", filename);
    if (containing_directory_exists(filename)) {
        remember_directory(filename, last_open_directory,
                           sizeof(last_open_directory));
        remember_directory(filename, last_save_directory,
                           sizeof(last_save_directory));
    }

    LoadResult result = LOAD_RESULT_DISK;
    if (recover_autosave) {
        int recovered;

        persistence_log_event(__func__, "calling load_autosave_if_newer path='%s' reason='disk file opened'",
                              path ? path : "");
        recovered = load_autosave_if_newer(path);
        persistence_log_event(__func__, "load_autosave_if_newer returned %d path='%s'",
                              recovered, path ? path : "");
        if (recovered)
            result = LOAD_RESULT_AUTOSAVE;
    } else {
        persistence_log_event(__func__, "load_autosave_if_newer not called path='%s' reason='recover_autosave disabled'",
                              path ? path : "");
    }

    if (result != LOAD_RESULT_AUTOSAVE) {
        SET_DIRTY(0, "persistence state reset");
        SET_AUTOSAVE_DIRTY(0, "persistence state reset");
        SET_LAST_EDIT_TIME(0, "persistence state reset");
    }

    finish_loaded_position(restore_pos, restore_y, restore_x, restore_top);
    persistence_log_event(__func__, "exit result=%s ultimately_loaded='%s' path='%s'",
                          load_result_name(result),
                          result == LOAD_RESULT_AUTOSAVE ? "autosave" : "disk",
                          path ? path : "");
    persistence_log_state(__func__, "load_file_at_position exit", path);
    return result;
}

static void load_file(const char *path)
{
    load_file_at_position(path, 1, 0, 0, 0, 0);
}

static int save_template_for(const char *path, char *tmp, size_t tmpsz)
{
    const char *slash = strrchr(path, '/');
    int written;

    if (!slash)
        written = snprintf(tmp, tmpsz, ".simplewords-save-XXXXXX");
    else if (slash == path)
        written = snprintf(tmp, tmpsz, "/.simplewords-save-XXXXXX");
    else
        written = snprintf(tmp, tmpsz, "%.*s/.simplewords-save-XXXXXX",
                           (int)(slash - path), path);

    if (written < 0 || (size_t)written >= tmpsz) {
        errno = ENAMETOOLONG;
        return 0;
    }

    return 1;
}

static int write_document(const char *path)
{
    char tmp[PATH_MAX];
    struct stat st;
    mode_t mode;

    if (!save_template_for(path, tmp, sizeof(tmp)))
        return 0;

    if (stat(path, &st) == 0) {
        mode = st.st_mode & 0777;
    } else {
        mode_t mask = umask(0);
        umask(mask);
        mode = 0666 & ~mask;
    }

    int fd = mkstemp(tmp);
    if (fd < 0)
        return 0;

    fchmod(fd, mode);

    FILE *fp = fdopen(fd, "w");
    if (!fp) {
        close(fd);
        unlink(tmp);
        return 0;
    }

    int ok = 1;
    for (int i = 0; i < line_count; i++) {
        if (fputs(lines[i], fp) == EOF)
            ok = 0;
        if (i != line_count - 1 && fputc('\n', fp) == EOF)
            ok = 0;
    }

    if (fclose(fp) != 0)
        ok = 0;

    if (ok && rename(tmp, path) == 0)
        return 1;

    {
        int saved_errno = errno;
        unlink(tmp);
        errno = saved_errno;
        return 0;
    }
}

static void expand_user_path(const char *in, char *out, size_t outsz)
{
    const char *home = getenv("HOME");

    if (strncmp(in, "$HOME", 5) == 0 && (in[5] == '\0' || in[5] == '/') && home) {
        snprintf(out, outsz, "%s%s", home, in + 5);
        return;
    }

    if (in[0] == '~' && in[1] == '/' && home) {
        snprintf(out, outsz, "%s/%s", home, in + 2);
    } else {
        snprintf(out, outsz, "%s", in);
    }
}

static void display_user_path(const char *in, char *out, size_t outsz)
{
    const char *home = getenv("HOME");

    if (home && home[0]) {
        size_t home_len = strlen(home);
        if (strncmp(in, home, home_len) == 0 &&
            (in[home_len] == '/' || in[home_len] == '\0')) {
            snprintf(out, outsz, "~%s", in + home_len);
            return;
        }
    }
    snprintf(out, outsz, "%s", in);
}

static void default_open_prompt_path(char *out, size_t outsz)
{
    const char *home = getenv("HOME");

    if (last_open_directory[0]) {
        display_user_path(last_open_directory, out, outsz);
        return;
    }

    if (last_save_directory[0]) {
        display_user_path(last_save_directory, out, outsz);
        return;
    }

    if (home && home[0]) {
        char fallback[PATH_MAX];
        snprintf(fallback, sizeof(fallback), "%s/writing/", home);
        display_user_path(fallback, out, outsz);
        return;
    }

    snprintf(out, outsz, "./");
}

static void default_save_prompt_path(char *out, size_t outsz)
{
    const char *home = getenv("HOME");

    if (filename[0]) {
        display_user_path(filename, out, outsz);
        return;
    }

    if (last_save_directory[0]) {
        display_user_path(last_save_directory, out, outsz);
        return;
    }

    if (home && home[0]) {
        char fallback[PATH_MAX];
        snprintf(fallback, sizeof(fallback), "%s/writing/", home);
        display_user_path(fallback, out, outsz);
        return;
    }

    snprintf(out, outsz, "./");
}

static void remember_directory(const char *path, char *directory, size_t directory_size)
{
    const char *slash = strrchr(path, '/');
    size_t dir_len;

    if (!slash) {
        snprintf(directory, directory_size, "./");
        return;
    }

    dir_len = (size_t)(slash - path + 1);
    if (dir_len >= directory_size)
        dir_len = directory_size - 1;
    memcpy(directory, path, dir_len);
    directory[dir_len] = '\0';
}

static int containing_directory_exists(const char *path)
{
    char directory[PATH_MAX];
    const char *slash = strrchr(path, '/');
    struct stat st;
    size_t dir_len;

    if (!slash)
        return stat(".", &st) == 0 && S_ISDIR(st.st_mode);

    dir_len = slash == path ? 1 : (size_t)(slash - path);
    if (dir_len >= sizeof(directory))
        return 0;
    memcpy(directory, path, dir_len);
    directory[dir_len] = '\0';
    return stat(directory, &st) == 0 && S_ISDIR(st.st_mode);
}

typedef struct {
    char *name;
    int is_dir;
} PathCompletion;

static int compare_path_completions(const void *a, const void *b)
{
    const PathCompletion *pa = a;
    const PathCompletion *pb = b;
    return strcmp(pa->name, pb->name);
}

static void free_path_completions(PathCompletion *items, int count)
{
    for (int i = 0; i < count; i++)
        free(items[i].name);
    free(items);
}

static PathCompletion *path_completions(const char *input, int *count_out,
                                        int *base_len_out, int *error_out)
{
    char dirpart[512];
    char dirpath[PATH_MAX];
    char fullpath[PATH_MAX];
    const char *slash = strrchr(input, '/');
    const char *base = slash ? slash + 1 : input;
    PathCompletion *items = NULL;
    int count = 0;
    int cap = 0;
    DIR *dir;
    struct dirent *entry;

    *error_out = 0;

    if (slash) {
        size_t n = (size_t)(slash - input + 1);
        if (n >= sizeof(dirpart)) {
            *error_out = ENAMETOOLONG;
            return NULL;
        }
        memcpy(dirpart, input, n);
        dirpart[n] = '\0';
        expand_user_path(dirpart, dirpath, sizeof(dirpath));
    } else {
        dirpart[0] = '\0';
        snprintf(dirpath, sizeof(dirpath), ".");
    }

    dir = opendir(dirpath);
    if (!dir) {
        *error_out = errno;
        return NULL;
    }

    while ((entry = readdir(dir)) != NULL) {
        struct stat st;
        int is_dir;
        size_t base_len = strlen(base);

        if (strncmp(entry->d_name, base, base_len) != 0)
            continue;
        if (!base[0] && entry->d_name[0] == '.' &&
            strcmp(entry->d_name, ".") != 0 &&
            strcmp(entry->d_name, "..") != 0)
            continue;

        snprintf(fullpath, sizeof(fullpath), "%s%s%s", dirpath,
                 dirpath[0] && dirpath[strlen(dirpath) - 1] == '/' ? "" : "/",
                 entry->d_name);
        is_dir = stat(fullpath, &st) == 0 && S_ISDIR(st.st_mode);

        if (count == cap) {
            int new_cap = cap ? cap * 2 : 32;
            PathCompletion *grown = realloc(items,
                                             (size_t)new_cap * sizeof(*items));
            if (!grown)
                break;
            items = grown;
            cap = new_cap;
        }

        items[count].name = strdup(entry->d_name);
        if (!items[count].name)
            break;
        items[count].is_dir = is_dir;
        count++;
    }

    closedir(dir);
    qsort(items, (size_t)count, sizeof(*items), compare_path_completions);
    *count_out = count;
    *base_len_out = (int)strlen(base);
    return items;
}

static int complete_path(char *out, size_t outsz, PathCompletion *items,
                         int count, int base_len)
{
    const char *slash = strrchr(out, '/');
    int prefix_len = slash ? (int)(slash - out + 1) : 0;
    int common_len;
    int old_len = (int)strlen(out);

    if (count == 0)
        return 0;

    common_len = (int)strlen(items[0].name);
    for (int i = 1; i < count; i++) {
        int j = 0;
        while (j < common_len && items[0].name[j] == items[i].name[j])
            j++;
        common_len = j;
    }

    if (common_len > base_len && prefix_len + common_len < (int)outsz) {
        memcpy(out + prefix_len, items[0].name, (size_t)common_len);
        out[prefix_len + common_len] = '\0';
    }

    if (count == 1 && items[0].is_dir) {
        int len = (int)strlen(out);
        if (len + 1 < (int)outsz && (len == 0 || out[len - 1] != '/')) {
            out[len] = '/';
            out[len + 1] = '\0';
        }
    }

    return (int)strlen(out) != old_len;
}

static void draw_prompt_footer(const char *prompt, const char *text,
                               int cursor, int *view_start,
                               const char *feedback);

static void draw_path_completions(const char *prompt, const char *path,
                                  int cursor, int *view_start,
                                  PathCompletion *items, int count)
{
    int widest = 1;

    for (int i = 0; i < count; i++) {
        int width = (int)strlen(items[i].name) + (items[i].is_dir ? 1 : 0);
        if (width > widest)
            widest = width;
    }
    widest += 2;

    int cols = COLS / widest;
    int total_rows;
    int visible_rows;
    int pane_start;
    int last;

    if (cols < 1)
        cols = 1;
    total_rows = (count + cols - 1) / cols;
    visible_rows = LINES / 2 - 1;
    if (visible_rows < 1)
        visible_rows = 1;
    if (visible_rows > total_rows)
        visible_rows = total_rows;
    pane_start = LINES - visible_rows - 2;
    if (pane_start < 0)
        pane_start = 0;
    last = visible_rows * cols;
    if (last > count)
        last = count;

    attrset(chrome_attr());
    mvhline(pane_start, 0, ' ', COLS);
    {
        char heading[128];
        snprintf(heading, sizeof(heading),
                 "%d completion%s  Esc returns to path; Esc again cancels%s",
                 count, count == 1 ? "" : "s",
                 last < count ? "  (more below)" : "");
        mvaddnstr(pane_start, 1, heading, COLS - 2);
    }

    attrset(body_attr());
    for (int row = pane_start + 1; row < LINES - 1; row++)
        mvhline(row, 0, ' ', COLS);
    for (int i = 0; i < last; i++) {
        int row = pane_start + 1 + i / cols;
        int col = (i % cols) * widest;
        char label[PATH_MAX];
        snprintf(label, sizeof(label), "%s%s", items[i].name,
                 items[i].is_dir ? "/" : "");
        mvaddnstr(row, col, label, widest - 1);
    }

    draw_prompt_footer(prompt, path, cursor, view_start, NULL);
}

static int is_prompt_tab(int ch)
{
    if (ch == '\t' || ch == 9)
        return 1;
#ifdef KEY_STAB
    if (ch == KEY_STAB)
        return 1;
#endif
#ifdef KEY_CTAB
    if (ch == KEY_CTAB)
        return 1;
#endif
#ifdef KEY_BTAB
    if (ch == KEY_BTAB)
        return 1;
#endif
    return 0;
}

static void prompt_clamp_cursor(int len, int *cursor)
{
    if (!cursor)
        return;
    if (*cursor < 0)
        *cursor = 0;
    if (*cursor > len)
        *cursor = len;
}

static void prompt_adjust_view(int len, int cursor, int editable_cols,
                               int *view_start)
{
    int max_offset;

    if (!view_start)
        return;
    if (*view_start < 0 || *view_start > len)
        *view_start = cursor;
    if (cursor < *view_start)
        *view_start = cursor;
    if (editable_cols <= 0) {
        *view_start = cursor;
        return;
    }

    max_offset = editable_cols - 1;
    if (cursor - *view_start > max_offset)
        *view_start = cursor - max_offset;
}

static int prompt_insert_byte(char *out, size_t outsz, int *len, int *cursor,
                              int ch)
{
    int max_len;

    if (!out || !len || !cursor || outsz == 0 || outsz > (size_t)INT_MAX)
        return 0;

    max_len = (int)outsz - 1;
    if (*len < 0 || *len > max_len)
        *len = (int)strnlen(out, outsz);
    prompt_clamp_cursor(*len, cursor);
    if (*len >= max_len)
        return 0;

    memmove(out + *cursor + 1, out + *cursor,
            (size_t)(*len - *cursor + 1));
    out[*cursor] = (char)ch;
    (*cursor)++;
    (*len)++;
    return 1;
}

static int prompt_backspace(char *out, int *len, int *cursor)
{
    if (!out || !len || !cursor)
        return 0;
    prompt_clamp_cursor(*len, cursor);
    if (*cursor <= 0)
        return 0;

    memmove(out + *cursor - 1, out + *cursor,
            (size_t)(*len - *cursor + 1));
    (*cursor)--;
    (*len)--;
    return 1;
}

static int prompt_delete_forward(char *out, int *len, int *cursor)
{
    if (!out || !len || !cursor)
        return 0;
    prompt_clamp_cursor(*len, cursor);
    if (*cursor >= *len)
        return 0;

    memmove(out + *cursor, out + *cursor + 1,
            (size_t)(*len - *cursor));
    (*len)--;
    return 1;
}

static int prompt_handle_navigation_key(int ch, int len, int *cursor)
{
    if (ch == KEY_LEFT || ch == KEY_SLEFT) {
        if (*cursor > 0)
            (*cursor)--;
        return 1;
    }
    if (ch == KEY_RIGHT || ch == KEY_SRIGHT) {
        if (*cursor < len)
            (*cursor)++;
        return 1;
    }
    if (ch == KEY_HOME || ch == 1) {
        *cursor = 0;
        return 1;
    }
    if (ch == KEY_END || ch == 5) {
        *cursor = len;
        return 1;
    }
    return 0;
}

static int prompt_printable_key(int ch)
{
    if (ch < 0 || ch > UCHAR_MAX)
        return 0;
    return isprint((unsigned char)ch) || (unsigned char)ch >= 0x80;
}

static int append_bracketed_paste_to_prompt(char *out, size_t outsz, int *len,
                                            int *cursor)
{
    int changed = 0;

    if (!pending_bracketed_paste)
        return 0;

    for (const unsigned char *p =
             (const unsigned char *)pending_bracketed_paste;
         *p && *p != '\r' && *p != '\n'; p++) {
        if ((isprint(*p) || *p >= 0x80) &&
            prompt_insert_byte(out, outsz, len, cursor, *p))
            changed = 1;
    }

    free(pending_bracketed_paste);
    pending_bracketed_paste = NULL;
    return changed;
}

static void draw_prompt_footer(const char *prompt, const char *text,
                               int cursor, int *view_start,
                               const char *feedback)
{
    int prompt_len;
    int text_len;
    int editable_cols;
    int prompt_cols;
    int cursor_col;
    int visible_len;

    if (!prompt)
        prompt = "";
    if (!text)
        text = "";

    prompt_len = (int)strlen(prompt);
    text_len = (int)strlen(text);
    editable_cols = COLS - 2 - prompt_len;
    prompt_cols = COLS - 2;
    if (editable_cols < 0)
        editable_cols = 0;
    if (prompt_cols < 0)
        prompt_cols = 0;

    prompt_clamp_cursor(text_len, &cursor);
    prompt_adjust_view(text_len, cursor, editable_cols, view_start);

    attrset(chrome_attr());
    mvhline(LINES - 1, 0, ' ', COLS);
    if (prompt_cols > 0)
        mvaddnstr(LINES - 1, 1, prompt, prompt_cols);
    if (editable_cols > 0)
        mvaddnstr(LINES - 1, 1 + prompt_len, text + *view_start,
                  editable_cols);

    if (feedback && feedback[0]) {
        int feedback_col;

        visible_len = text_len - *view_start;
        if (visible_len < 0)
            visible_len = 0;
        if (visible_len > editable_cols)
            visible_len = editable_cols;
        feedback_col = 1 + prompt_len + visible_len + 2;
        if (feedback_col < COLS - 1)
            mvaddnstr(LINES - 1, feedback_col, feedback,
                      COLS - 1 - feedback_col);
        else if (LINES > 1) {
            mvhline(LINES - 2, 0, ' ', COLS);
            mvaddnstr(LINES - 2, 1, feedback, COLS - 2);
        }
    }

    cursor_col = 1 + prompt_len + cursor - *view_start;
    if (cursor_col < 0)
        cursor_col = 0;
    if (cursor_col >= COLS)
        cursor_col = COLS - 1;
    move(LINES - 1, cursor_col);
}

static int prompt_path(const char *prompt, const char *initial,
                       char *out, size_t outsz,
                       int require_existing_parent)
{
    int len;
    int cursor;
    int view_start = 0;
    int ch;
    int tab_pending = 0;
    int pane_open = 0;
    PathCompletion *items = NULL;
    int count = 0;
    int base_len = 0;
    char completion_feedback[128] = "";

    snprintf(out, outsz, "%s", initial ? initial : "");
    len = (int)strlen(out);
    cursor = len;
    set_cursor_visibility(1);

    while (1) {
        draw_screen();
        if (pane_open) {
            draw_path_completions(prompt, out, cursor, &view_start,
                                  items, count);
        } else {
            draw_prompt_footer(prompt, out, cursor, &view_start,
                               completion_feedback);
        }
        refresh();
        set_cursor_visibility(1);

        do {
            if (terminate_requested) {
                free_path_completions(items, count);
                return 0;
            }
            ch = read_editor_key();
        } while (ch == ERR);
        if (terminate_requested) {
            free_path_completions(items, count);
            return 0;
        }
        if (ch == KEY_BRACKETED_PASTE) {
            if (append_bracketed_paste_to_prompt(out, outsz, &len,
                                                 &cursor)) {
                free_path_completions(items, count);
                items = NULL;
                count = 0;
                pane_open = 0;
                tab_pending = 0;
                completion_feedback[0] = '\0';
            }
            continue;
        }
        if (ch == 27) {
            if (pane_open) {
                pane_open = 0;
                tab_pending = 0;
                free_path_completions(items, count);
                items = NULL;
                count = 0;

                /* The completion pane occupies ordinary editor rows.
                 * Force them to be repainted immediately when leaving it. */
                screen_cache_valid = 0;
                clear();
                draw_screen_impl(0);
                refresh();
                continue;
            }

            free_path_completions(items, count);

            /* Second Esc cancels the path prompt.  Do not leave either
             * the prompt or its former completion pane on screen until
             * some later navigation key happens to trigger a redraw. */
            screen_cache_valid = 0;
            clear();
            draw_screen_impl(0);
            refresh();
            return 0;
        }
        if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
            int accepted = out[0] != '\0';

            if (accepted && require_existing_parent) {
                char expanded[PATH_MAX];

                expand_user_path(out, expanded, sizeof(expanded));
                if (!containing_directory_exists(expanded)) {
                    snprintf(completion_feedback,
                             sizeof(completion_feedback),
                             "Folder does not exist");
                    pane_open = 0;
                    tab_pending = 0;
                    free_path_completions(items, count);
                    items = NULL;
                    count = 0;
                    continue;
                }
            }

            free_path_completions(items, count);
            return accepted;
        }
        if (prompt_handle_navigation_key(ch, len, &cursor)) {
            tab_pending = 0;
            continue;
        }
        if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (prompt_backspace(out, &len, &cursor)) {
                free_path_completions(items, count);
                items = NULL;
                count = 0;
                pane_open = 0;
            }
            completion_feedback[0] = '\0';
            tab_pending = 0;
        } else if (ch == KEY_DC || ch == 4) {
            if (prompt_delete_forward(out, &len, &cursor)) {
                free_path_completions(items, count);
                items = NULL;
                count = 0;
                pane_open = 0;
            }
            completion_feedback[0] = '\0';
            tab_pending = 0;
        } else if (is_prompt_tab(ch)) {
            int changed;
            int completion_errno;

            free_path_completions(items, count);
            count = 0;
            items = path_completions(out, &count, &base_len,
                                     &completion_errno);
            if (completion_errno) {
                snprintf(completion_feedback, sizeof(completion_feedback),
                         "%s", completion_errno == ENOENT || completion_errno == ENOTDIR
                               ? "No such directory" : strerror(completion_errno));
                set_status(completion_feedback);
                pane_open = 0;
                tab_pending = 0;
                continue;
            }
            completion_feedback[0] = '\0';
            changed = complete_path(out, outsz, items, count, base_len);
            len = (int)strlen(out);
            cursor = len;
            if (tab_pending && !changed && count > 0)
                pane_open = 1;
            tab_pending = 1;
        } else if (prompt_printable_key(ch)) {
            if (prompt_insert_byte(out, outsz, &len, &cursor, ch)) {
                free_path_completions(items, count);
                items = NULL;
                count = 0;
                pane_open = 0;
                completion_feedback[0] = '\0';
                tab_pending = 0;
            }
        }
    }
}


static int prompt_string(const char *prompt, char *out, size_t outsz)
{
    int len = 0;
    int cursor = 0;
    int view_start = 0;
    int ch;

    out[0] = '\0';
    set_cursor_visibility(1);

    while (1) {
        draw_screen();
        draw_prompt_footer(prompt, out, cursor, &view_start, NULL);
        refresh();

        do {
            if (terminate_requested)
                return 0;
            ch = read_editor_key();
        } while (ch == ERR);

        if (terminate_requested)
            return 0;
        if (ch == KEY_BRACKETED_PASTE) {
            (void)append_bracketed_paste_to_prompt(out, outsz, &len,
                                                   &cursor);
            continue;
        }
        if (ch == 27)
            return 0;
        if (ch == '\n' || ch == '\r' || ch == KEY_ENTER)
            return out[0] != '\0';
        if (prompt_handle_navigation_key(ch, len, &cursor))
            continue;
        if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            (void)prompt_backspace(out, &len, &cursor);
        } else if (ch == KEY_DC || ch == 4) {
            (void)prompt_delete_forward(out, &len, &cursor);
        } else if (prompt_printable_key(ch)) {
            (void)prompt_insert_byte(out, outsz, &len, &cursor, ch);
        }
    }
}

static int find_text_forward(const char *needle, int start_y, int start_x,
                             int *out_y, int *out_x)
{
    for (int pass = 0; pass < 2; pass++) {
        int first_y = pass == 0 ? start_y : 0;
        int last_y = pass == 0 ? line_count : start_y + 1;

        for (int y = first_y; y < last_y; y++) {
            int x = (pass == 0 && y == start_y) ? start_x : 0;
            char *hit;

            if (x < 0) x = 0;
            if (x > (int)strlen(lines[y])) x = (int)strlen(lines[y]);

            hit = strstr(lines[y] + x, needle);
            if (hit) {
                *out_y = y;
                *out_x = (int)(hit - lines[y]);
                return 1;
            }
        }
    }
    return 0;
}

static char *last_strstr_before(char *haystack, const char *needle, int limit)
{
    char *best = NULL;
    char *hit = haystack;

    while ((hit = strstr(hit, needle))) {
        if ((int)(hit - haystack) > limit)
            break;
        best = hit;
        hit++;
    }

    return best;
}

static int find_text_backward(const char *needle, int start_y, int start_x,
                              int *out_y, int *out_x)
{
    for (int pass = 0; pass < 2; pass++) {
        int first_y = pass == 0 ? start_y : line_count - 1;
        int last_y = pass == 0 ? -1 : start_y - 1;

        for (int y = first_y; y > last_y; y--) {
            int limit = (pass == 0 && y == start_y)
                        ? start_x
                        : (int)strlen(lines[y]);
            char *hit = last_strstr_before(lines[y], needle, limit);

            if (hit) {
                *out_y = y;
                *out_x = (int)(hit - lines[y]);
                return 1;
            }
        }
    }
    return 0;
}

static void repeat_find(int direction)
{
    int fy, fx;

    if (!last_find[0]) {
        set_status("No active find");
        find_mode = 0;
        find_active = 0;
        find_match_y = -1;
        find_match_x = -1;
        find_match_len = 0;
        screen_cache_valid = 0;
        return;
    }

    if (direction > 0) {
        if (!find_text_forward(last_find, cy, cx + 1, &fy, &fx)) {
            set_status("No next match");
            return;
        }
    } else {
        if (!find_text_backward(last_find, cy, cx - 1, &fy, &fx)) {
            set_status("No previous match");
            return;
        }
    }

    cy = fy;
    cx = fx;
    find_match_y = fy;
    find_match_x = fx;
    find_match_len = (int)strlen(last_find);
    goal_col = -1;
    clear_cursor_affinity();
    clear_selection();
    keep_cursor_visible();
    set_status(direction > 0 ? "Next match" : "Previous match");
    find_mode = 1;
    find_active = 1;
    screen_cache_valid = 0;
}

static void find_word_prompt(void)
{
    char needle[256];

    break_undo_burst();
    clear_selection();

    if (!prompt_string("Find: ", needle, sizeof(needle))) {
        set_status("Find cancelled");
        return;
    }

    snprintf(last_find, sizeof(last_find), "%s", needle);
    find_mode = 1;
    repeat_find(1);
}



static void save_session(void);
static void clear_session(void);
static int load_session(void);
static void autosave_file_now(void);
static void flush_recovery_state(void);
static int confirm_quit(void);

static void handle_terminate(int sig)
{
    terminate_requested = sig;
}

static int terminal_input_disconnected(int fd)
{
    struct pollfd input = {fd, POLLIN, 0};
    int result;

    if (fd < 0 || !isatty(fd))
        return 1;
    do {
        result = poll(&input, 1, 0);
    } while (result < 0 && errno == EINTR && !terminate_requested);
    if (result < 0)
        return errno == EBADF;
    return result > 0 &&
           (input.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0;
}

static int save_document_to_path(const char *path)
{
    char target[sizeof(filename)];
    int was_untitled = !filename[0];
    char previous_untitled[sizeof(untitled_name)];

    if (!path || !path[0]) {
        errno = EINVAL;
        return 0;
    }
    if (!snprintf_ok(snprintf(target, sizeof(target), "%s", path),
                     sizeof(target))) {
        errno = ENAMETOOLONG;
        return 0;
    }

    snprintf(previous_untitled, sizeof(previous_untitled), "%s", untitled_name);
    persistence_log_event(__func__, "enter target='%s' current_filename='%s'",
                          target, filename);

    if (buffer_system_ready && dirty && filename[0] &&
        strcmp(filename, target) == 0 &&
        !ensure_document_edit_lock()) {
        errno = EWOULDBLOCK;
        return 0;
    }

    if (!allow_stale_document_write && filename[0] &&
        strcmp(filename, target) == 0 && dirty &&
        disk_revision_changed(&editor_buffers[active_buffer_index], target)) {
        set_status("Save blocked: file changed on disk; use C-x C-w to write deliberately");
        persistence_log_event(__func__,
                              "exit blocked target='%s' reason='disk revision changed'",
                              target);
        errno = ESTALE;
        return 0;
    }

    if (backup_existing_document(target) && write_document(target)) {
        int keep_pending_recovery = pending_recovery_for(target);
        int resolve_opened_recovery = opened_recovery_for(target);

        /* A Save As only becomes the document's identity after the write
         * succeeds.  Keeping this assignment transactional prevents a typo
         * in a directory name from poisoning every later C-x C-s. */
        snprintf(filename, sizeof(filename), "%s", target);
        release_document_edit_lock(&editor_buffers[active_buffer_index]);
        remember_disk_revision(&editor_buffers[active_buffer_index], filename);

        remember_directory(filename, last_save_directory, sizeof(last_save_directory));
        if (!keep_pending_recovery)
            remove_autosaves_for(filename);
        if (resolve_opened_recovery && opened_recovery_autosave[0]) {
            unlink(opened_recovery_autosave);
            clear_opened_recovery();
        }
        if (was_untitled)
            remove_untitled_autosave(previous_untitled);
        if (!keep_pending_recovery)
            clear_pending_recovery();
        SET_DIRTY(0, "persistence state reset");
        SET_AUTOSAVE_DIRTY(0, "persistence state reset");
        SET_LAST_EDIT_TIME(0, "persistence state reset");
        save_session();
        set_status(keep_pending_recovery ? "Recovered draft preserved" : "Saved");
        persistence_log_event(__func__, "exit saved filename='%s'", filename);
        persistence_log_state(__func__, "save_document_to_path exit saved", filename);
        return 1;
    } else {
        int saved_errno = errno;
        char msg[600];

        if (saved_errno == ENOENT || saved_errno == ENOTDIR)
            snprintf(msg, sizeof(msg), "Save failed: folder does not exist");
        else
            snprintf(msg, sizeof(msg), "Save failed: %s",
                     strerror(saved_errno));
        set_status(msg);
        persistence_log_event(__func__, "exit failed target='%s' current_filename='%s' errno=%d reason='%s'",
                              target, filename, saved_errno,
                              strerror(saved_errno));
        persistence_log_state(__func__,
                              "save_document_to_path exit failed", target);
        errno = saved_errno;
        return 0;
    }
}

static int prompt_save_target(char *target, size_t target_size)
{
    char path[512];
    char initial[512];

    default_save_prompt_path(initial, sizeof(initial));
    if (!prompt_path("Save as: ", initial, path, sizeof(path), 1))
        return 0;
    expand_user_path(path, target, target_size);
    return target[0] != '\0';
}

static void save_file(int force_write)
{
    char target[sizeof(filename)];

    persistence_log_event(__func__, "enter force_write=%d", force_write);
    persistence_log_state(__func__, "save_file entry", filename);
    break_undo_burst();
    if (!force_write && filename[0] && !dirty) {
        set_status("No changes to save");
        persistence_log_event(__func__, "exit no-op reason='not dirty' filename='%s'", filename);
        persistence_log_state(__func__, "save_file exit no-op", filename);
        return;
    }

    if (filename[0]) {
        snprintf(target, sizeof(target), "%s", filename);
    } else if (!prompt_save_target(target, sizeof(target))) {
        set_status("Save cancelled");
        persistence_log_event(__func__,
                              "exit cancelled reason='save as prompt cancelled'");
        persistence_log_state(__func__, "save_file exit cancelled", filename);
        return;
    }

    (void)save_document_to_path(target);
}

static void save_file_as(void)
{
    char target[sizeof(filename)];

    persistence_log_event(__func__, "enter current_filename='%s'", filename);
    persistence_log_state(__func__, "save_file_as entry", filename);
    break_undo_burst();
    if (!prompt_save_target(target, sizeof(target))) {
        set_status("Save cancelled");
        persistence_log_event(__func__, "exit cancelled");
        persistence_log_state(__func__, "save_file_as exit cancelled", filename);
        return;
    }

    allow_stale_document_write = 1;
    (void)save_document_to_path(target);
    allow_stale_document_write = 0;
}

static int document_is_empty(void)
{
    return line_count == 1 &&
           lines[0] &&
           lines[0][0] == '\0';
}

static void ensure_autosave_dir(void)
{
    char dir[PATH_MAX];

    simplewords_autosave_dir(dir, sizeof(dir));
}

static void autosave_file_common(int force)
{
    char path[PATH_MAX];
    time_t now;

    persistence_log_event(__func__, "enter force=%d", force);
    persistence_log_state(__func__, "autosave_file_common entry", filename);

    if ((!autosave_dirty && !dirty) || !last_edit_time) {
        persistence_log_event(__func__, "exit skipped reason='clean or no last_edit_time' force=%d dirty=%d autosave_dirty=%d last_edit_time=%lld",
                              force, dirty, autosave_dirty, (long long)last_edit_time);
        return;
    }
    now = time(NULL);
    if (!force && now - last_edit_time < config.autosave_interval) {
        persistence_log_event(__func__, "exit skipped reason='interval not elapsed' elapsed=%lld interval=%d",
                              (long long)(now - last_edit_time), config.autosave_interval);
        return;
    }

    path[0] = '\0';
    if (filename[0]) {
        autosave_path_for(filename, path, sizeof(path));
    } else {
        untitled_autosave_path_for(untitled_name, path, sizeof(path));
    }

    if (!path[0]) {
        persistence_log_event(__func__, "exit skipped reason='no autosave path'");
        return;
    }

    persistence_log_event(__func__, "writing autosave path='%s' force=%d", path, force);
    ensure_autosave_dir();

    if (write_document(path)) {
        autosave_wrote_any = 1;
        SET_AUTOSAVE_DIRTY(0, "autosave written");
        if (!autosaving_all_buffers)
            save_session();
        persistence_log_event(__func__, "exit wrote autosave path='%s'", path);
        persistence_log_state(__func__, "autosave_file_common exit wrote", filename);
    } else {
        persistence_log_event(__func__, "exit failed write autosave path='%s' errno=%d reason='%s'",
                              path, errno, strerror(errno));
        persistence_log_state(__func__, "autosave_file_common exit failed", filename);
    }
}

static void autosave_file(void)
{
    int previous_buffer;
    int previous_window;

    persistence_log_event(__func__, "enter");
    persistence_log_state(__func__, "autosave_file entry", filename);
    if (!buffer_system_ready || autosaving_all_buffers) {
        autosave_file_common(0);
    } else {
        save_active_window_view();
        previous_buffer = active_buffer_index;
        previous_window = active_window_index;
        autosaving_all_buffers = 1;
        autosave_wrote_any = 0;
        for (int i = 0; i < MAX_BUFFERS; i++) {
            if (!editor_buffers[i].used)
                continue;
            activate_buffer_raw(i);
            autosave_file_common(0);
        }
        autosaving_all_buffers = 0;
        activate_buffer_raw(previous_buffer);
        active_window_index = previous_window;
        if (autosave_wrote_any)
            save_session();
    }
    persistence_log_event(__func__, "exit");
    persistence_log_state(__func__, "autosave_file exit", filename);
}

static void autosave_file_now(void)
{
    int previous_buffer;
    int previous_window;

    persistence_log_event(__func__, "enter");
    persistence_log_state(__func__, "autosave_file_now entry", filename);
    if (!buffer_system_ready || autosaving_all_buffers) {
        autosave_file_common(1);
    } else {
        save_active_window_view();
        previous_buffer = active_buffer_index;
        previous_window = active_window_index;
        autosaving_all_buffers = 1;
        autosave_wrote_any = 0;
        for (int i = 0; i < MAX_BUFFERS; i++) {
            if (!editor_buffers[i].used)
                continue;
            activate_buffer_raw(i);
            autosave_file_common(1);
        }
        autosaving_all_buffers = 0;
        activate_buffer_raw(previous_buffer);
        active_window_index = previous_window;
        if (autosave_wrote_any)
            save_session();
    }
    persistence_log_event(__func__, "exit");
    persistence_log_state(__func__, "autosave_file_now exit", filename);
}

static void flush_recovery_state(void)
{
    persistence_log_event(__func__, "enter");
    persistence_log_state(__func__, "flush_recovery_state entry", filename);
    autosave_file_now();
    save_session();
    persistence_log_event(__func__, "exit");
    persistence_log_state(__func__, "flush_recovery_state exit", filename);
}

static void discard_pending_recovery(void)
{
    char doc[PATH_MAX];
    char recovery[PATH_MAX];

    if (!recovery_prompt_active())
        return;

    snprintf(doc, sizeof(doc), "%s", pending_recovery_doc);
    snprintf(recovery, sizeof(recovery), "%s", pending_recovery_autosave);
    if (doc[0])
        remove_autosaves_for(doc);
    if (recovery[0])
        unlink(recovery);
    clear_pending_recovery();
    screen_cache_valid = 0;
    set_status("Recovery discarded");
}

static void open_pending_recovery(void)
{
    char doc[PATH_MAX];
    char recovery[PATH_MAX];

    if (!recovery_prompt_active())
        return;

    if (strlen(pending_recovery_doc) >= sizeof(filename)) {
        set_status("Recovery document path is too long");
        return;
    }

    copy_string(doc, sizeof(doc), pending_recovery_doc);
    copy_string(recovery, sizeof(recovery), pending_recovery_autosave);
    if (dirty)
        flush_recovery_state();
    if (dirty && !confirm_quit()) {
        set_status("Open recovery cancelled");
        return;
    }

    if (!read_document_into_buffer(recovery)) {
        char msg[600];

        snprintf(msg, sizeof(msg), "Recovery open failed: %s", strerror(errno));
        set_status(msg);
        return;
    }

    copy_string(filename, sizeof(filename), doc);
    remember_directory(filename, last_open_directory, sizeof(last_open_directory));
    remember_directory(filename, last_save_directory, sizeof(last_save_directory));
    remember_opened_recovery(doc, recovery);
    clear_pending_recovery();
    SET_DIRTY(1, "recovery draft opened");
    SET_AUTOSAVE_DIRTY(0, "recovery draft opened");
    SET_LAST_EDIT_TIME(0, "recovery draft opened");
    finish_loaded_position(0, 0, 0, 0);
    screen_cache_valid = 0;
    set_status("Recovery draft opened");
}

static void open_file_prompt(void)
{
    char path[512];
    char initial[512];

    break_undo_burst();
    if (dirty)
        flush_recovery_state();

    default_open_prompt_path(initial, sizeof(initial));
    if (!prompt_path("Open: ", initial, path, sizeof(path), 0)) {
        set_status("Open cancelled");
        return;
    }

    char expanded[512];
    expand_user_path(path, expanded, sizeof(expanded));

    if (buffer_system_ready) {
        visit_file_in_buffer(expanded);
        return;
    }

    if (dirty && !confirm_quit()) {
        set_status("Open cancelled");
        return;
    }

    LoadResult result = load_file_at_position(expanded, 1, 0, 0, 0, 0);
    if (result != LOAD_RESULT_FAILED)
        save_session();
    if (result == LOAD_RESULT_DISK && !pending_recovery_for(filename))
        set_status("Opened disk file");
}

static int confirm_quit(void)
{
    int ch;
    int quit = 0;

    persistence_log_event(__func__, "enter dirty=%d", dirty);
    persistence_log_state(__func__, "confirm_quit entry", filename);
    break_undo_burst();
    if (!dirty) {
        persistence_log_event(__func__, "exit true reason='not dirty'");
        persistence_log_state(__func__, "confirm_quit exit clean", filename);
        return 1;
    }

    timeout(-1);
    set_status("Unsaved changes. Continue anyway? y/N");

    while (1) {
        draw_screen();
        ch = read_editor_key();
        if (terminate_requested) {
            quit = 1;
            break;
        }

        if (ch == KEY_BRACKETED_PASTE) {
            free(pending_bracketed_paste);
            pending_bracketed_paste = NULL;
            continue;
        }

        if (ch == 'y' || ch == 'Y') {
            quit = 1;
            break;
        }

        if (ch == 'n' || ch == 'N' || ch == 27 ||
            ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
            quit = 0;
            break;
        }
    }

    timeout(250);
    clear_status();
    persistence_log_event(__func__, "exit result=%d", quit);
    persistence_log_state(__func__, "confirm_quit exit", filename);
    return quit;
}

static int confirm_quit_workspace(void)
{
    int modified = 0;
    int ch;
    char message[240];

    if (!buffer_system_ready)
        return confirm_quit();
    save_active_window_view();
    for (int i = 0; i < MAX_BUFFERS; i++)
        if (editor_buffers[i].used && editor_buffers[i].modified)
            modified++;
    if (!modified)
        return 1;

    snprintf(message, sizeof(message),
             "%d unsaved buffer%s. Quit? recovery copies will be kept. y/N",
             modified, modified == 1 ? "" : "s");
    set_status(message);
    timeout(-1);
    while (1) {
        draw_screen();
        ch = read_editor_key();
        if (terminate_requested || ch == 'y' || ch == 'Y') {
            timeout(250);
            clear_status();
            return 1;
        }
        if (ch == 'n' || ch == 'N' || ch == 27 || ch == '\n' ||
            ch == '\r' || ch == KEY_ENTER) {
            timeout(250);
            clear_status();
            return 0;
        }
    }
}

static void new_blank_buffer(void)
{
    if (buffer_system_ready) {
        create_blank_buffer();
        return;
    }

    persistence_log_event(__func__, "enter");
    persistence_log_state(__func__, "new_blank_buffer entry", filename);
    if (dirty) {
        if (!confirm_quit()) {
            persistence_log_event(__func__, "exit cancelled by confirm_quit");
            persistence_log_state(__func__, "new_blank_buffer exit cancelled", filename);
            return;
        }
        flush_recovery_state();
    }

    persistence_log_event(__func__, "clearing/replacing buffer for new blank line_count=%d", line_count);
    for (int i = 0; i < line_count; i++)
        free(lines[i]);

    line_count = 1;
    lines[0] = new_line("");
    cy = 0;
    cx = 0;
    top = 0;
    goal_col = -1;
    clear_cursor_affinity();
    filename[0] = '\0';
    make_untitled_name();

    SET_DIRTY(0, "new blank buffer");
    SET_AUTOSAVE_DIRTY(0, "new blank buffer");
    SET_LAST_EDIT_TIME(0, "new blank buffer");

    clear_selection();
    clear_undo_history();
    clamp_top();
    clear_session();

    set_status("New blank buffer");
    persistence_log_event(__func__, "exit new blank buffer");
    persistence_log_state(__func__, "new_blank_buffer exit", filename);
}

static void activate_buffer_raw(int index)
{
    if (index < 0 || index >= MAX_BUFFERS || !editor_buffers[index].used)
        return;
    if (index == active_buffer_index)
        return;

    reset_wrap_cache();
    active_buffer_index = index;
    screen_cache_valid = 0;
}

static void mark_active_buffer_used(void)
{
    editor_buffers[active_buffer_index].last_used = ++buffer_use_clock;
}

static void save_active_window_view(void)
{
    EditorWindow *window;

    if (!buffer_system_ready || active_window_index < 0 ||
        active_window_index >= MAX_EDITOR_WINDOWS)
        return;
    window = &editor_windows[active_window_index];
    if (!window->used || window->kind != EDITOR_WINDOW_DOCUMENT ||
        window->buffer_index != active_buffer_index)
        return;

    window->cursor_y = cy;
    window->cursor_x = cx;
    window->view_top = top;
}

static WindowBufferState window_buffer_state_for(int buffer_index)
{
    WindowBufferState state = {buffer_index, 0, 0, 0};

    if (buffer_index >= 0 && buffer_index < MAX_BUFFERS &&
        editor_buffers[buffer_index].used) {
        state.cursor_y = editor_buffers[buffer_index].cursor_y;
        state.cursor_x = editor_buffers[buffer_index].cursor_x;
        state.view_top = editor_buffers[buffer_index].view_top;
    }
    return state;
}

static WindowBufferState current_window_buffer_state(const EditorWindow *window)
{
    WindowBufferState state = {-1, 0, 0, 0};

    if (!window)
        return state;
    state.buffer_index = window->buffer_index;
    state.cursor_y = window->cursor_y;
    state.cursor_x = window->cursor_x;
    state.view_top = window->view_top;
    return state;
}

static void remove_buffer_from_history_stack(WindowBufferState *items,
                                             int *count,
                                             int buffer_index)
{
    int kept = 0;

    if (!items || !count)
        return;
    for (int i = 0; i < *count; i++) {
        if (items[i].buffer_index != buffer_index)
            items[kept++] = items[i];
    }
    *count = kept;
}

static void remove_buffer_from_window_history(EditorWindow *window,
                                              int buffer_index)
{
    if (!window)
        return;
    remove_buffer_from_history_stack(window->previous_buffers,
                                     &window->previous_buffer_count,
                                     buffer_index);
    remove_buffer_from_history_stack(window->next_buffers,
                                     &window->next_buffer_count,
                                     buffer_index);
}

static void push_window_history_state(WindowBufferState *items, int *count,
                                      WindowBufferState state)
{
    if (!items || !count || state.buffer_index < 0 ||
        state.buffer_index >= MAX_BUFFERS ||
        !editor_buffers[state.buffer_index].used)
        return;
    remove_buffer_from_history_stack(items, count, state.buffer_index);
    if (*count >= MAX_BUFFERS) {
        memmove(items, items + 1,
                (MAX_BUFFERS - 1) * sizeof(*items));
        *count = MAX_BUFFERS - 1;
    }
    items[(*count)++] = state;
}

static int find_window_history_state(const EditorWindow *window,
                                     int buffer_index,
                                     WindowBufferState *state)
{
    if (!window)
        return 0;
    for (int i = window->previous_buffer_count - 1; i >= 0; i--) {
        if (window->previous_buffers[i].buffer_index == buffer_index) {
            if (state)
                *state = window->previous_buffers[i];
            return 1;
        }
    }
    for (int i = window->next_buffer_count - 1; i >= 0; i--) {
        if (window->next_buffers[i].buffer_index == buffer_index) {
            if (state)
                *state = window->next_buffers[i];
            return 1;
        }
    }
    return 0;
}

static int pop_window_history_state(WindowBufferState *items, int *count,
                                    int excluded_buffer,
                                    WindowBufferState *state)
{
    while (items && count && *count > 0) {
        WindowBufferState candidate = items[--(*count)];

        if (candidate.buffer_index < 0 ||
            candidate.buffer_index >= MAX_BUFFERS ||
            candidate.buffer_index == excluded_buffer ||
            !editor_buffers[candidate.buffer_index].used)
            continue;
        if (state)
            *state = candidate;
        return 1;
    }
    return 0;
}

static void set_editor_window_buffer_state(EditorWindow *window,
                                           WindowBufferState state)
{
    if (!window)
        return;
    window->kind = EDITOR_WINDOW_DOCUMENT;
    window->buffer_index = state.buffer_index;
    window->cursor_y = state.cursor_y;
    window->cursor_x = state.cursor_x;
    window->view_top = state.view_top;
}

static void load_editor_window(int index)
{
    EditorWindow *window;

    if (index < 0 || index >= MAX_EDITOR_WINDOWS ||
        !editor_windows[index].used)
        return;

    active_window_index = index;
    window = &editor_windows[index];
    if (window->kind == EDITOR_WINDOW_BUFFER_SHELF) {
        screen_cache_valid = 0;
        return;
    }
    activate_buffer_raw(window->buffer_index);
    cy = window->cursor_y;
    cx = window->cursor_x;
    top = window->view_top;
    clamp_cursor();
    clamp_top();
    clear_cursor_affinity();
    mark_active_buffer_used();
    screen_cache_valid = 0;
}

static void select_buffer_in_active_window(int index)
{
    EditorWindow *window;
    WindowBufferState incoming;

    if (index < 0 || index >= MAX_BUFFERS || !editor_buffers[index].used)
        return;
    if (!buffer_system_ready) {
        activate_buffer_raw(index);
        return;
    }

    save_active_window_view();
    window = &editor_windows[active_window_index];
    if (window->kind == EDITOR_WINDOW_DOCUMENT &&
        window->buffer_index == index) {
        load_editor_window(active_window_index);
        return;
    }
    incoming = window_buffer_state_for(index);
    if (find_window_history_state(window, index, &incoming))
        remove_buffer_from_window_history(window, index);
    if (window->kind == EDITOR_WINDOW_DOCUMENT)
        push_window_history_state(window->previous_buffers,
                                  &window->previous_buffer_count,
                                  current_window_buffer_state(window));
    window->next_buffer_count = 0;
    set_editor_window_buffer_state(window, incoming);
    load_editor_window(active_window_index);
}

static int buffer_count(void)
{
    int count = 0;

    for (int i = 0; i < MAX_BUFFERS; i++)
        if (editor_buffers[i].used)
            count++;
    return count;
}

static int buffer_index_for_path(const char *path)
{
    if (!path || !*path)
        return -1;
    for (int i = 0; i < MAX_BUFFERS; i++) {
        if (editor_buffers[i].used && editor_buffers[i].path[0] &&
            strcmp(editor_buffers[i].path, path) == 0)
            return i;
    }
    return -1;
}

static int canonical_visit_path(const char *path, char *out, size_t outsz)
{
    char absolute[PATH_MAX];
    char resolved[PATH_MAX];
    char directory[PATH_MAX];
    char *slash;
    const char *base;

    if (!path || !*path || !out || outsz == 0)
        return 0;
    if (realpath(path, resolved))
        return copy_string(out, outsz, resolved);

    if (path[0] == '/') {
        if (!copy_string(absolute, sizeof(absolute), path))
            return 0;
    } else {
        char cwd[PATH_MAX];

        if (!getcwd(cwd, sizeof(cwd)) ||
            !format_string(absolute, sizeof(absolute), "%s/%s", cwd, path))
            return 0;
    }

    slash = strrchr(absolute, '/');
    if (!slash)
        return copy_string(out, outsz, absolute);
    base = slash + 1;
    if (slash == absolute) {
        copy_string(directory, sizeof(directory), "/");
    } else {
        size_t length = (size_t)(slash - absolute);

        if (length >= sizeof(directory))
            return 0;
        memcpy(directory, absolute, length);
        directory[length] = '\0';
    }

    if (realpath(directory, resolved))
        return format_string(out, outsz, "%s%s%s", resolved,
                             strcmp(resolved, "/") == 0 ? "" : "/", base);
    return copy_string(out, outsz, absolute);
}

static void make_buffer_draft_name(int index)
{
    char base[sizeof(editor_buffers[index].draft_name)];
    int collision = 0;

    make_untitled_name();
    copy_string(base, sizeof(base), untitled_name);
    for (int i = 0; i < MAX_BUFFERS; i++) {
        if (i != index && editor_buffers[i].used &&
            strcmp(editor_buffers[i].draft_name, base) == 0) {
            collision = 1;
            break;
        }
    }
    if (collision) {
        char suffix[16];
        size_t suffix_length;
        size_t base_length;

        snprintf(suffix, sizeof(suffix), "-%02d", index + 1);
        suffix_length = strlen(suffix);
        base_length = strlen(base);
        if (base_length + suffix_length >=
            sizeof(editor_buffers[index].draft_name))
            base_length = sizeof(editor_buffers[index].draft_name) -
                          suffix_length - 1;
        memcpy(untitled_name, base, base_length);
        memcpy(untitled_name + base_length, suffix, suffix_length + 1);
    }
}

static int allocate_buffer_slot(void)
{
    int previous = active_buffer_index;

    for (int index = 0; index < MAX_BUFFERS; index++) {
        EditorBuffer *buffer;

        if (editor_buffers[index].used)
            continue;
        buffer = &editor_buffers[index];
        memset(buffer, 0, sizeof(*buffer));
        buffer->used = 1;
        buffer->text_line_count = 1;
        buffer->text_lines[0] = new_line("");
        buffer->affinity_line = -1;
        buffer->affinity_x = -1;
        buffer->affinity_doc_row = -1;
        buffer->affinity_col = -1;
        buffer->search_match_y = -1;
        buffer->search_match_x = -1;
        buffer->document_lock_fd = -1;
        buffer->last_used = ++buffer_use_clock;
        activate_buffer_raw(index);
        make_buffer_draft_name(index);
        activate_buffer_raw(previous);
        return index;
    }

    set_status("Buffer shelf is full (32 documents)");
    return -1;
}

static void free_buffer_storage(int index)
{
    EditorBuffer *buffer;

    if (index < 0 || index >= MAX_BUFFERS || !editor_buffers[index].used)
        return;
    buffer = &editor_buffers[index];
    release_document_edit_lock(buffer);
    for (int i = 0; i < buffer->text_line_count; i++)
        free(buffer->text_lines[i]);
    for (int i = 0; i < buffer->undo_item_count; i++)
        free_undo_group(&buffer->undo_items[i]);
    for (int i = 0; i < buffer->redo_item_count; i++)
        free_undo_group(&buffer->redo_items[i]);
    free_undo_group(&buffer->pending_undo);
    memset(buffer, 0, sizeof(*buffer));
}

static void initialize_buffer_system(void)
{
    if (buffer_system_ready)
        return;

    if (!lines[0])
        lines[0] = new_line("");
    if (!untitled_name[0])
        make_untitled_name();
    editor_buffers[0].used = 1;
    editor_buffers[0].last_used = ++buffer_use_clock;
    editor_windows[0].used = 1;
    editor_windows[0].buffer_index = 0;
    editor_windows[0].cursor_y = cy;
    editor_windows[0].cursor_x = cx;
    editor_windows[0].view_top = top;
    active_buffer_index = 0;
    active_window_index = 0;
    buffer_system_ready = 1;
}

static void create_blank_buffer(void)
{
    int index;

    if (active_window_is_buffer_shelf())
        close_buffer_shelf_window(active_window_index);
    index = allocate_buffer_slot();

    if (index < 0)
        return;
    select_buffer_in_active_window(index);
    set_status("New draft buffer");
    save_session();
}

static void visit_file_in_buffer(const char *path)
{
    char canonical[sizeof(filename)];
    int existing;
    EditorWindow previous_window;
    int previous_buffer;
    int index;
    LoadResult result;

    if (active_window_is_buffer_shelf())
        close_buffer_shelf_window(active_window_index);

    if (!canonical_visit_path(path, canonical, sizeof(canonical))) {
        set_status("Open failed: path is too long");
        return;
    }

    existing = buffer_index_for_path(canonical);
    if (existing >= 0) {
        select_buffer_in_active_window(existing);
        set_status("Switched to existing buffer");
        save_session();
        return;
    }

    previous_window = editor_windows[active_window_index];
    previous_buffer = active_buffer_index;
    index = allocate_buffer_slot();
    if (index < 0)
        return;
    select_buffer_in_active_window(index);
    result = load_file_at_position(canonical, 1, 0, 0, 0, 0);
    if (result == LOAD_RESULT_FAILED) {
        editor_windows[active_window_index] = previous_window;
        free_buffer_storage(index);
        activate_buffer_raw(previous_buffer);
        load_editor_window(active_window_index);
        return;
    }

    mark_active_buffer_used();
    save_active_window_view();
    save_session();
    if (result == LOAD_RESULT_DISK && !pending_recovery_for(filename))
        set_status("Opened in new buffer");
}

static int most_recent_other_buffer(int excluded)
{
    int best = -1;
    unsigned long long best_use = 0;

    for (int i = 0; i < MAX_BUFFERS; i++) {
        if (!editor_buffers[i].used || i == excluded)
            continue;
        if (best < 0 || editor_buffers[i].last_used > best_use) {
            best = i;
            best_use = editor_buffers[i].last_used;
        }
    }
    return best;
}

static int confirm_kill_buffer(int index)
{
    char shown[160];
    char message[320];

    if (!editor_buffers[index].modified)
        return 1;
    if (editor_buffers[index].path[0]) {
        const char *slash = strrchr(editor_buffers[index].path, '/');
        copy_string(shown, sizeof(shown),
                    slash ? slash + 1 : editor_buffers[index].path);
    } else {
        copy_string(shown, sizeof(shown), editor_buffers[index].draft_name);
    }
    snprintf(message, sizeof(message),
             "Kill unsaved %s? recovery copy stays on disk. y/N", shown);
    set_status(message);
    timeout(-1);
    while (1) {
        int ch;

        draw_screen();
        ch = read_editor_key();
        if (terminate_requested) {
            timeout(250);
            clear_status();
            return 0;
        }
        if (ch == 'y' || ch == 'Y') {
            timeout(250);
            clear_status();
            return 1;
        }
        if (ch == 'n' || ch == 'N' || ch == 27 || ch == '\n' ||
            ch == '\r' || ch == KEY_ENTER) {
            timeout(250);
            clear_status();
            return 0;
        }
    }
}

static void autosave_buffer_now(int index)
{
    int previous = active_buffer_index;

    activate_buffer_raw(index);
    autosave_file_common(1);
    activate_buffer_raw(previous);
}

static void kill_buffer_index(int index)
{
    int windows_to_close[MAX_EDITOR_WINDOWS];
    int windows_to_close_count = 0;
    int live_window_count = 0;
    int fallback;
    int killed_active_buffer;

    if (index < 0 || index >= MAX_BUFFERS || !editor_buffers[index].used)
        return;
    if (!confirm_kill_buffer(index)) {
        set_status("Buffer kept");
        return;
    }

    killed_active_buffer = active_buffer_index == index;
    if (editor_buffers[index].modified)
        autosave_buffer_now(index);
    fallback = most_recent_other_buffer(index);
    if (fallback < 0)
        fallback = allocate_buffer_slot();
    if (fallback < 0)
        return;

    save_active_window_view();
    if (active_buffer_index == index)
        reset_wrap_cache();
    for (int i = 0; i < MAX_EDITOR_WINDOWS; i++)
        if (editor_windows[i].used)
            live_window_count++;
    for (int i = 0; i < MAX_EDITOR_WINDOWS; i++) {
        EditorWindow *window = &editor_windows[i];
        WindowBufferState replacement_state;
        int found_replacement = 0;

        if (!window->used)
            continue;
        if (window->kind == EDITOR_WINDOW_DOCUMENT &&
            window->buffer_index == index) {
            found_replacement = pop_window_history_state(
                window->previous_buffers, &window->previous_buffer_count,
                index, &replacement_state);
            if (!found_replacement)
                found_replacement = pop_window_history_state(
                    window->next_buffers, &window->next_buffer_count,
                    index, &replacement_state);
            if (!found_replacement &&
                live_window_count - windows_to_close_count > 1) {
                windows_to_close[windows_to_close_count++] = i;
                remove_buffer_from_window_history(window, index);
                continue;
            }
            if (!found_replacement)
                replacement_state = window_buffer_state_for(fallback);
            remove_buffer_from_window_history(window, index);
            remove_buffer_from_window_history(
                window, replacement_state.buffer_index);
            set_editor_window_buffer_state(window, replacement_state);
        } else {
            remove_buffer_from_window_history(window, index);
        }
    }
    for (int i = 0; i < windows_to_close_count; i++) {
        int window_index = windows_to_close[i];
        int next_window = remove_editor_window_from_layout(window_index);

        if (next_window >= 0 && active_window_index == window_index)
            active_window_index = next_window;
    }
    free_buffer_storage(index);
    if (killed_active_buffer && active_window_is_buffer_shelf())
        activate_buffer_raw(fallback);
    load_editor_window(active_window_index);
    set_status(windows_to_close_count > 0 ?
               "Buffer killed; empty-history window closed" :
               "Buffer killed; recovery copy retained if needed");
    save_session();
}

static void kill_current_buffer(void)
{
    kill_buffer_index(active_buffer_index);
}

static int allocate_editor_window(void)
{
    for (int i = 0; i < MAX_EDITOR_WINDOWS; i++) {
        if (!editor_windows[i].used) {
            memset(&editor_windows[i], 0, sizeof(editor_windows[i]));
            editor_windows[i].used = 1;
            return i;
        }
    }
    return -1;
}

static int document_window_count(void)
{
    int count = 0;

    for (int i = 0; i < MAX_EDITOR_WINDOWS; i++)
        if (editor_windows[i].used &&
            editor_windows[i].kind == EDITOR_WINDOW_DOCUMENT)
            count++;
    return count;
}

static int buffer_shelf_window_index(void)
{
    for (int i = 0; i < MAX_EDITOR_WINDOWS; i++)
        if (editor_windows[i].used &&
            editor_windows[i].kind == EDITOR_WINDOW_BUFFER_SHELF)
            return i;
    return -1;
}

static int allocate_layout_node(void)
{
    for (int i = 0; i < MAX_LAYOUT_NODES; i++) {
        if (!layout_nodes[i].used) {
            memset(&layout_nodes[i], 0, sizeof(layout_nodes[i]));
            layout_nodes[i].used = 1;
            layout_nodes[i].parent = -1;
            layout_nodes[i].first = -1;
            layout_nodes[i].second = -1;
            layout_nodes[i].window_index = -1;
            layout_nodes[i].ratio = 50;
            return i;
        }
    }
    return -1;
}

static int layout_leaf_for_window(int node_index, int window_index)
{
    LayoutNode *node;
    int found;

    if (node_index < 0 || node_index >= MAX_LAYOUT_NODES ||
        !layout_nodes[node_index].used)
        return -1;
    node = &layout_nodes[node_index];
    if (node->kind == LAYOUT_LEAF)
        return node->window_index == window_index ? node_index : -1;
    found = layout_leaf_for_window(node->first, window_index);
    return found >= 0 ? found :
           layout_leaf_for_window(node->second, window_index);
}

static void assign_layout_rectangles(int node_index, EditorRect rect)
{
    LayoutNode *node;

    if (node_index < 0 || node_index >= MAX_LAYOUT_NODES ||
        !layout_nodes[node_index].used)
        return;
    node = &layout_nodes[node_index];
    if (node->kind == LAYOUT_LEAF) {
        if (node->window_index >= 0 &&
            node->window_index < MAX_EDITOR_WINDOWS)
            editor_window_rects[node->window_index] = rect;
        return;
    }

    if (node->kind == LAYOUT_ABOVE_BELOW) {
        int first_height = rect.height * node->ratio / 100;
        EditorRect first = rect;
        EditorRect second = rect;

        if (first_height < 1)
            first_height = 1;
        if (first_height >= rect.height)
            first_height = rect.height - 1;
        first.height = first_height;
        second.y += first_height;
        second.height -= first_height;
        assign_layout_rectangles(node->first, first);
        assign_layout_rectangles(node->second, second);
    } else if (node->kind == LAYOUT_SIDE_BY_SIDE) {
        int first_width = rect.width * node->ratio / 100;
        EditorRect first = rect;
        EditorRect second = rect;

        if (first_width < 1)
            first_width = 1;
        if (first_width >= rect.width)
            first_width = rect.width - 1;
        first.width = first_width;
        second.x += first_width;
        second.width -= first_width;
        assign_layout_rectangles(node->first, first);
        assign_layout_rectangles(node->second, second);
    }
}

static void recompute_layout_rectangles(void)
{
    EditorRect root = {0, 0, LINES, COLS};

    memset(editor_window_rects, 0, sizeof(editor_window_rects));
    assign_layout_rectangles(layout_root, root);
}

static int split_editor_window_internal(int kind, int use_full_frame_geometry)
{
    int leaf;
    int new_window;
    int first_node;
    int second_node;
    EditorRect rect;

    if (kind != LAYOUT_ABOVE_BELOW && kind != LAYOUT_SIDE_BY_SIDE)
        return -1;
    recompute_layout_rectangles();
    if (use_full_frame_geometry)
        rect = (EditorRect){0, 0, LINES, COLS};
    else
        rect = editor_window_rects[active_window_index];
    if ((kind == LAYOUT_ABOVE_BELOW && rect.height < 12) ||
        (kind == LAYOUT_SIDE_BY_SIDE && rect.width < 48)) {
        set_status(kind == LAYOUT_ABOVE_BELOW ?
                   "Window is too short to split" :
                   "Window is too narrow to split");
        return -1;
    }

    leaf = layout_leaf_for_window(layout_root, active_window_index);
    new_window = allocate_editor_window();
    first_node = allocate_layout_node();
    second_node = allocate_layout_node();
    if (leaf < 0 || new_window < 0 || first_node < 0 || second_node < 0) {
        if (new_window >= 0)
            memset(&editor_windows[new_window], 0,
                   sizeof(editor_windows[new_window]));
        if (first_node >= 0)
            memset(&layout_nodes[first_node], 0,
                   sizeof(layout_nodes[first_node]));
        if (second_node >= 0)
            memset(&layout_nodes[second_node], 0,
                   sizeof(layout_nodes[second_node]));
        set_status("No room for another editor window");
        return -1;
    }

    save_active_window_view();
    editor_windows[new_window] = editor_windows[active_window_index];
    editor_windows[new_window].used = 1;

    layout_nodes[first_node].kind = LAYOUT_LEAF;
    layout_nodes[first_node].parent = leaf;
    layout_nodes[first_node].window_index = active_window_index;
    layout_nodes[second_node].kind = LAYOUT_LEAF;
    layout_nodes[second_node].parent = leaf;
    layout_nodes[second_node].window_index = new_window;

    layout_nodes[leaf].kind = kind;
    layout_nodes[leaf].first = first_node;
    layout_nodes[leaf].second = second_node;
    layout_nodes[leaf].window_index = -1;
    layout_nodes[leaf].ratio = 50;
    screen_cache_valid = 0;
    set_status(kind == LAYOUT_ABOVE_BELOW ?
               "Split above/below" : "Split side by side");
    save_session();
    return new_window;
}

static int split_editor_window(int kind)
{
    int new_window;

    if (kind != LAYOUT_ABOVE_BELOW && kind != LAYOUT_SIDE_BY_SIDE)
        return -1;
    if (buffer_shelf_window_index() >= 0) {
        set_status("Close Buffer List before splitting");
        return -1;
    }
    if (document_window_count() >= 2) {
        int other_window = -1;

        for (int i = 0; i < MAX_EDITOR_WINDOWS; i++) {
            if (i != active_window_index && editor_windows[i].used &&
                editor_windows[i].kind == EDITOR_WINDOW_DOCUMENT) {
                other_window = i;
                break;
            }
        }
        layout_nodes[layout_root].kind = kind;
        layout_nodes[layout_root].ratio = 50;
        distraction_free = 0;
        pane_rendering = 0;
        screen_cache_valid = 0;
        set_status(kind == LAYOUT_ABOVE_BELOW ?
                   "Two windows shown above/below" :
                   "Two windows shown side by side");
        save_session();
        return other_window;
    }
    new_window = split_editor_window_internal(kind, 0);
    if (new_window >= 0) {
        distraction_free = 0;
        pane_rendering = 0;
        screen_cache_valid = 0;
        set_status(kind == LAYOUT_ABOVE_BELOW ?
                   "Split above/below" : "Split side by side");
        save_session();
    }
    return new_window;
}

static int first_window_in_layout(int node_index)
{
    if (node_index < 0 || !layout_nodes[node_index].used)
        return -1;
    if (layout_nodes[node_index].kind == LAYOUT_LEAF)
        return layout_nodes[node_index].window_index;
    return first_window_in_layout(layout_nodes[node_index].first);
}

static int first_document_window_in_layout(int node_index)
{
    int window_index;
    int found;

    if (node_index < 0 || node_index >= MAX_LAYOUT_NODES ||
        !layout_nodes[node_index].used)
        return -1;
    if (layout_nodes[node_index].kind == LAYOUT_LEAF) {
        window_index = layout_nodes[node_index].window_index;
        if (window_index >= 0 && window_index < MAX_EDITOR_WINDOWS &&
            editor_windows[window_index].used &&
            editor_windows[window_index].kind == EDITOR_WINDOW_DOCUMENT)
            return window_index;
        return -1;
    }
    found = first_document_window_in_layout(layout_nodes[node_index].first);
    return found >= 0 ? found :
           first_document_window_in_layout(layout_nodes[node_index].second);
}

static int buffer_shelf_companion_window_index(int shelf_window)
{
    int leaf;
    int parent;
    int sibling;
    int companion;

    if (shelf_window < 0 || shelf_window >= MAX_EDITOR_WINDOWS ||
        !editor_windows[shelf_window].used ||
        editor_windows[shelf_window].kind != EDITOR_WINDOW_BUFFER_SHELF)
        return -1;
    leaf = layout_leaf_for_window(layout_root, shelf_window);
    if (leaf >= 0) {
        parent = layout_nodes[leaf].parent;
        if (parent >= 0) {
            sibling = layout_nodes[parent].first == leaf ?
                      layout_nodes[parent].second :
                      layout_nodes[parent].first;
            companion = first_document_window_in_layout(sibling);
            if (companion >= 0)
                return companion;
        }
    }
    if (active_window_index != shelf_window &&
        active_window_index >= 0 &&
        active_window_index < MAX_EDITOR_WINDOWS &&
        editor_windows[active_window_index].used &&
        editor_windows[active_window_index].kind == EDITOR_WINDOW_DOCUMENT)
        return active_window_index;
    return first_document_window_in_layout(layout_root);
}

static int remove_editor_window_from_layout(int window_index)
{
    int leaf;
    int parent;
    int sibling;
    int grandparent;
    int next_window;
    LayoutNode replacement;

    if (layout_nodes[layout_root].kind == LAYOUT_LEAF)
        return -1;
    leaf = layout_leaf_for_window(layout_root, window_index);
    if (leaf < 0)
        return -1;
    parent = layout_nodes[leaf].parent;
    sibling = layout_nodes[parent].first == leaf ?
              layout_nodes[parent].second : layout_nodes[parent].first;
    next_window = first_window_in_layout(sibling);
    grandparent = layout_nodes[parent].parent;
    replacement = layout_nodes[sibling];
    replacement.parent = grandparent;
    layout_nodes[parent] = replacement;
    if (replacement.kind != LAYOUT_LEAF) {
        layout_nodes[replacement.first].parent = parent;
        layout_nodes[replacement.second].parent = parent;
    }
    memset(&layout_nodes[leaf], 0, sizeof(layout_nodes[leaf]));
    memset(&layout_nodes[sibling], 0, sizeof(layout_nodes[sibling]));
    memset(&editor_windows[window_index], 0,
           sizeof(editor_windows[window_index]));
    return next_window;
}

/*
 * Older SimpleWords builds could turn each accepted Buffer List into another
 * permanent document window.  Keep every buffer, but collapse restored view
 * state to the selected document plus the most recently used other document.
 * Buffer List itself is transient and is never resumed at startup.
 */
static int normalize_restored_workspace_windows(void)
{
    int changed = 0;
    int shelf_window;

    while ((shelf_window = buffer_shelf_window_index()) >= 0) {
        int companion = buffer_shelf_companion_window_index(shelf_window);

        if (layout_nodes[layout_root].kind == LAYOUT_LEAF) {
            int replacement = most_recent_other_buffer(-1);

            if (replacement >= 0)
                set_editor_window_buffer_state(
                    &editor_windows[shelf_window],
                    window_buffer_state_for(replacement));
            changed = 1;
            break;
        }
        if (active_window_index == shelf_window && companion >= 0)
            active_window_index = companion;
        (void)remove_editor_window_from_layout(shelf_window);
        changed = 1;
    }

    if (active_window_index < 0 ||
        active_window_index >= MAX_EDITOR_WINDOWS ||
        !editor_windows[active_window_index].used ||
        editor_windows[active_window_index].kind != EDITOR_WINDOW_DOCUMENT)
        active_window_index = first_document_window_in_layout(layout_root);

    while (document_window_count() > 2) {
        int remove_window = -1;
        unsigned long long oldest_use = ULLONG_MAX;

        for (int i = 0; i < MAX_EDITOR_WINDOWS; i++) {
            int buffer_index;
            unsigned long long last_used;

            if (i == active_window_index || !editor_windows[i].used ||
                editor_windows[i].kind != EDITOR_WINDOW_DOCUMENT)
                continue;
            buffer_index = editor_windows[i].buffer_index;
            last_used = buffer_index >= 0 && buffer_index < MAX_BUFFERS &&
                        editor_buffers[buffer_index].used ?
                        editor_buffers[buffer_index].last_used : 0;
            if (remove_window < 0 || last_used < oldest_use) {
                remove_window = i;
                oldest_use = last_used;
            }
        }
        if (remove_window < 0)
            break;
        (void)remove_editor_window_from_layout(remove_window);
        changed = 1;
    }

    return changed;
}

static void delete_editor_window(void)
{
    int next_window;
    int shelf_window = buffer_shelf_window_index();

    if (shelf_window >= 0) {
        close_buffer_shelf_window(shelf_window);
        set_status("Buffer List closed; document window kept");
        return;
    }

    if (layout_nodes[layout_root].kind == LAYOUT_LEAF) {
        set_status("Only one window");
        return;
    }
    save_active_window_view();
    next_window = remove_editor_window_from_layout(active_window_index);
    if (next_window < 0)
        return;
    load_editor_window(next_window);
    set_status("Window closed; buffer kept");
    save_session();
}

static void delete_other_editor_windows(void)
{
    int shelf_window = buffer_shelf_window_index();
    int kept_window;

    if (shelf_window >= 0)
        close_buffer_shelf_window(shelf_window);
    kept_window = active_window_index;

    save_active_window_view();
    for (int i = 0; i < MAX_EDITOR_WINDOWS; i++) {
        if (i != kept_window)
            memset(&editor_windows[i], 0, sizeof(editor_windows[i]));
    }
    memset(layout_nodes, 0, sizeof(layout_nodes));
    layout_nodes[0].used = 1;
    layout_nodes[0].kind = LAYOUT_LEAF;
    layout_nodes[0].parent = -1;
    layout_nodes[0].first = -1;
    layout_nodes[0].second = -1;
    layout_nodes[0].window_index = kept_window;
    layout_nodes[0].ratio = 50;
    layout_root = 0;
    pane_rendering = 0;
    load_editor_window(kept_window);
    set_status("Other windows closed; buffers kept");
    save_session();
}

static void collect_layout_windows(int node_index, int *items, int *count)
{
    if (node_index < 0 || !layout_nodes[node_index].used ||
        *count >= MAX_EDITOR_WINDOWS)
        return;
    if (layout_nodes[node_index].kind == LAYOUT_LEAF) {
        items[(*count)++] = layout_nodes[node_index].window_index;
        return;
    }
    collect_layout_windows(layout_nodes[node_index].first, items, count);
    collect_layout_windows(layout_nodes[node_index].second, items, count);
}

static void select_other_editor_window(void)
{
    int items[MAX_EDITOR_WINDOWS];
    int count = 0;
    int current = 0;
    int shelf_window = buffer_shelf_window_index();

    if (shelf_window >= 0) {
        int target = active_window_index == shelf_window ?
                     buffer_shelf_companion_window_index(shelf_window) :
                     shelf_window;

        if (target >= 0) {
            save_active_window_view();
            load_editor_window(target);
            set_status(target == shelf_window ?
                       "Buffer List" : "Document window");
            save_session();
        }
        return;
    }

    collect_layout_windows(layout_root, items, &count);
    if (count < 2) {
        set_status("Only one window");
        return;
    }
    for (int i = 0; i < count; i++) {
        if (items[i] == active_window_index) {
            current = i;
            break;
        }
    }
    save_active_window_view();
    load_editor_window(items[(current + 1) % count]);
    set_status("Other window");
    save_session();
}

static void cycle_editor_buffer(int direction)
{
    EditorWindow *window;
    WindowBufferState outgoing;
    WindowBufferState incoming;
    int found;

    if (active_window_is_buffer_shelf()) {
        set_status("Open a document before changing buffer history");
        return;
    }
    if (buffer_count() < 2) {
        set_status("Only one buffer");
        return;
    }

    save_active_window_view();
    window = &editor_windows[active_window_index];
    outgoing = current_window_buffer_state(window);
    if (direction < 0) {
        found = pop_window_history_state(window->previous_buffers,
                                         &window->previous_buffer_count,
                                         window->buffer_index, &incoming);
    } else {
        found = pop_window_history_state(window->next_buffers,
                                         &window->next_buffer_count,
                                         window->buffer_index, &incoming);
    }
    if (!found) {
        int fallback = most_recent_other_buffer(window->buffer_index);

        if (fallback < 0) {
            set_status(direction < 0 ? "No previous buffer" :
                                      "No next buffer");
            return;
        }
        incoming = window_buffer_state_for(fallback);
    }

    remove_buffer_from_window_history(window, incoming.buffer_index);
    if (direction < 0)
        push_window_history_state(window->next_buffers,
                                  &window->next_buffer_count, outgoing);
    else
        push_window_history_state(window->previous_buffers,
                                  &window->previous_buffer_count, outgoing);
    set_editor_window_buffer_state(window, incoming);
    load_editor_window(active_window_index);
    set_status(direction < 0 ? "Previous buffer" : "Next buffer");
    save_session();
}

static int apply_undo_group(const UndoGroup *group, int forward)
{
    if (forward) {
        for (int i = 0; i < group->op_count; i++) {
            const UndoOp *op = &group->ops[i];

            if (!replace_range_raw(op->start.y, op->start.x,
                                   op->old_end.y, op->old_end.x,
                                   op->new_text, NULL))
                return 0;
        }
    } else {
        for (int i = group->op_count - 1; i >= 0; i--) {
            const UndoOp *op = &group->ops[i];

            if (!replace_range_raw(op->start.y, op->start.x,
                                   op->new_end.y, op->new_end.x,
                                   op->old_text, NULL))
                return 0;
        }
    }

    return 1;
}

static void restore_group_cursor(int y, int x, int saved_top)
{
    invalidate_wrap_cache();
    cy = y;
    cx = x;
    top = saved_top;
    goal_col = -1;
    clear_cursor_affinity();
    clamp_cursor();
    clamp_top();
}

static void do_undo(void)
{
    UndoGroup group;

    break_undo_burst();
    if (!undo_count) {
        set_status("Nothing to undo");
        return;
    }

    group = pop_group(undo_stack, &undo_count);
    group.after_cy = cy;
    group.after_cx = cx;
    group.after_top = top;

    persistence_log_event(__func__, "enter op_count=%d", group.op_count);
    persistence_log_state(__func__, "do_undo before apply", filename);
    if (!apply_undo_group(&group, 0)) {
        free_undo_group(&group);
        set_status("Undo failed");
        return;
    }

    restore_group_cursor(group.before_cy, group.before_cx, group.before_top);
    mark_edit();
    push_group(redo_stack, &redo_count, &group);
    set_status("Undo");
    persistence_log_state(__func__, "do_undo after apply", filename);
}

static void do_redo(void)
{
    UndoGroup group;

    break_undo_burst();
    if (!redo_count) {
        set_status("Nothing to redo");
        return;
    }

    group = pop_group(redo_stack, &redo_count);
    group.before_cy = cy;
    group.before_cx = cx;
    group.before_top = top;

    persistence_log_event(__func__, "enter op_count=%d", group.op_count);
    persistence_log_state(__func__, "do_redo before apply", filename);
    if (!apply_undo_group(&group, 1)) {
        free_undo_group(&group);
        set_status("Redo failed");
        return;
    }

    restore_group_cursor(group.after_cy, group.after_cx, group.after_top);
    mark_edit();
    push_group(undo_stack, &undo_count, &group);
    set_status("Redo");
    persistence_log_state(__func__, "do_redo after apply", filename);
}



static int transient_mail_file(const char *path)
{
    const char *base;

    if (!path || !*path)
        return 0;

    base = strrchr(path, '/');
    base = base ? base + 1 : path;

    return strncmp(base, "simplemail-compose-", 19) == 0 ||
           strncmp(base, "simplemail-reply-",   17) == 0;
}

static int session_path(char *out, size_t outsz)
{
    char dir[PATH_MAX];

    if (!simplewords_state_dir(dir, sizeof(dir)))
        return 0;
    return snprintf_ok(snprintf(out, outsz, "%s/session", dir), outsz);
}

static int workspace_session_path(char *out, size_t outsz)
{
    char dir[PATH_MAX];

    if (!simplewords_state_dir(dir, sizeof(dir)))
        return 0;
    return snprintf_ok(snprintf(out, outsz, "%s/workspace", dir), outsz);
}

static void legacy_session_path(char *out, size_t outsz)
{
    if (!home_path(out, outsz, LEGACY_SESSION_FILE) && out && outsz)
        out[0] = '\0';
}

static void migrate_legacy_session(const char *new_path)
{
    static int attempted = 0;
    char old_path[PATH_MAX];

    if (attempted)
        return;
    attempted = 1;

    legacy_session_path(old_path, sizeof(old_path));
    if (!old_path[0])
        return;

    migrate_file_if_safe(old_path, new_path);
}


static void clear_session(void)
{
    char path[PATH_MAX];
    int have_path;

    persistence_log_event(__func__, "enter");
    if (buffer_system_ready && !workspace_session_owner) {
        persistence_log_event(__func__,
                              "exit skipped: another process owns workspace");
        return;
    }
    have_path = buffer_system_ready ?
                workspace_session_path(path, sizeof(path)) :
                session_path(path, sizeof(path));
    if (have_path) {
        persistence_log_event(__func__, "unlink session path='%s'", path);
        unlink(path);
    } else {
        persistence_log_event(__func__, "no session path available");
    }
    persistence_log_event(__func__, "exit");
}

#define SESSION_V2_MAGIC "@simplewords-session-v2"

typedef struct {
    char kind;
    int cursor_y;
    int cursor_x;
    int view_top;
    unsigned long long last_used;
    char identity[512];
} SessionBufferRecord;

typedef struct {
    int kind;
    int window_kind;
    int ratio;
    int buffer_ordinal;
    int cursor_y;
    int cursor_x;
    int view_top;
} SessionLayoutRecord;

typedef struct {
    WindowBufferState previous_buffers[MAX_BUFFERS];
    int previous_buffer_count;
    WindowBufferState next_buffers[MAX_BUFFERS];
    int next_buffer_count;
} SessionWindowHistory;

static int hex_encode_string(const char *input, char *out, size_t outsz)
{
    static const char digits[] = "0123456789abcdef";
    size_t length = input ? strlen(input) : 0;

    if (!out || outsz < length * 2 + 1)
        return 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char value = (unsigned char)input[i];

        out[i * 2] = digits[value >> 4];
        out[i * 2 + 1] = digits[value & 0x0f];
    }
    out[length * 2] = '\0';
    return 1;
}

static int hex_value(int ch)
{
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    if (ch >= 'a' && ch <= 'f')
        return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F')
        return ch - 'A' + 10;
    return -1;
}

static int hex_decode_string(const char *input, char *out, size_t outsz)
{
    size_t length;

    if (!input || !out || outsz == 0)
        return 0;
    length = strlen(input);
    if ((length & 1u) || length / 2 >= outsz)
        return 0;
    for (size_t i = 0; i < length; i += 2) {
        int high = hex_value((unsigned char)input[i]);
        int low = hex_value((unsigned char)input[i + 1]);

        if (high < 0 || low < 0)
            return 0;
        out[i / 2] = (char)((high << 4) | low);
        if (out[i / 2] == '\0')
            return 0;
    }
    out[length / 2] = '\0';
    return 1;
}

static int reachable_layout_node_count(int node_index)
{
    if (node_index < 0 || node_index >= MAX_LAYOUT_NODES ||
        !layout_nodes[node_index].used)
        return 0;
    if (layout_nodes[node_index].kind == LAYOUT_LEAF)
        return 1;
    return 1 + reachable_layout_node_count(layout_nodes[node_index].first) +
           reachable_layout_node_count(layout_nodes[node_index].second);
}

static int write_session_layout(FILE *fp, int node_index,
                                const int *buffer_ordinals,
                                int fallback_ordinal)
{
    LayoutNode *node;

    if (node_index < 0 || node_index >= MAX_LAYOUT_NODES ||
        !layout_nodes[node_index].used)
        return 0;
    node = &layout_nodes[node_index];
    if (node->kind == LAYOUT_LEAF) {
        EditorWindow *window;
        int ordinal;

        if (node->window_index < 0 ||
            node->window_index >= MAX_EDITOR_WINDOWS ||
            !editor_windows[node->window_index].used)
            return 0;
        window = &editor_windows[node->window_index];
        if (window->kind == EDITOR_WINDOW_BUFFER_SHELF)
            return fprintf(fp, "SHELF %d %d\n",
                           buffer_drawer_selected,
                           buffer_drawer_scroll) >= 0;
        ordinal = buffer_ordinals[window->buffer_index];
        if (ordinal < 0)
            ordinal = fallback_ordinal;
        return fprintf(fp, "LEAF %d %d %d %d\n", ordinal,
                       window->cursor_y, window->cursor_x,
                       window->view_top) >= 0;
    }

    if (fprintf(fp, "SPLIT %c %d\n",
                node->kind == LAYOUT_ABOVE_BELOW ? 'A' : 'S',
                node->ratio) < 0)
        return 0;
    return write_session_layout(fp, node->first, buffer_ordinals,
                                fallback_ordinal) &&
           write_session_layout(fp, node->second, buffer_ordinals,
                                fallback_ordinal);
}

static int persisted_history_state_count(const WindowBufferState *states,
                                         int state_count,
                                         const int *buffer_ordinals)
{
    int count = 0;

    for (int i = 0; i < state_count; i++) {
        int index = states[i].buffer_index;

        if (index >= 0 && index < MAX_BUFFERS &&
            buffer_ordinals[index] >= 0)
            count++;
    }
    return count;
}

static int write_session_history_stack(FILE *fp, const char *record_name,
                                       const WindowBufferState *states,
                                       int state_count,
                                       const int *buffer_ordinals)
{
    for (int i = 0; i < state_count; i++) {
        const WindowBufferState *state = &states[i];
        int index = state->buffer_index;
        int ordinal;

        if (index < 0 || index >= MAX_BUFFERS)
            continue;
        ordinal = buffer_ordinals[index];
        if (ordinal < 0)
            continue;
        if (fprintf(fp, "%s %d %d %d %d\n", record_name, ordinal,
                    state->cursor_y, state->cursor_x,
                    state->view_top) < 0)
            return 0;
    }
    return 1;
}

static int write_session_window_histories(FILE *fp, const int *windows,
                                          int window_count_value,
                                          const int *buffer_ordinals)
{
    if (fprintf(fp, "HISTORY_COUNT %d\n", window_count_value) < 0)
        return 0;
    for (int rank = 0; rank < window_count_value; rank++) {
        const EditorWindow *window = &editor_windows[windows[rank]];
        int previous_count = persisted_history_state_count(
            window->previous_buffers, window->previous_buffer_count,
            buffer_ordinals);
        int next_count = persisted_history_state_count(
            window->next_buffers, window->next_buffer_count,
            buffer_ordinals);

        if (fprintf(fp, "HISTORY %d %d %d\n", rank, previous_count,
                    next_count) < 0 ||
            !write_session_history_stack(
                fp, "PREV", window->previous_buffers,
                window->previous_buffer_count, buffer_ordinals) ||
            !write_session_history_stack(
                fp, "NEXT", window->next_buffers,
                window->next_buffer_count, buffer_ordinals))
            return 0;
    }
    return 1;
}

static void save_workspace_session(void)
{
    char path[PATH_MAX];
    char tmp[PATH_MAX];
    int ordinals[MAX_BUFFERS];
    int persisted_buffers[MAX_BUFFERS];
    int persisted_count = 0;
    int windows[MAX_EDITOR_WINDOWS];
    int window_count_value = 0;
    int active_window_rank = 0;
    int layout_count;
    int fd;
    FILE *fp;
    int ok = 1;

    if (!workspace_session_owner)
        return;
    save_active_window_view();
    for (int i = 0; i < MAX_BUFFERS; i++)
        ordinals[i] = -1;
    for (int i = 0; i < MAX_BUFFERS; i++) {
        if (!editor_buffers[i].used ||
            (editor_buffers[i].path[0] &&
             transient_mail_file(editor_buffers[i].path)))
            continue;
        ordinals[i] = persisted_count;
        persisted_buffers[persisted_count++] = i;
    }
    if (persisted_count == 0) {
        clear_session();
        return;
    }
    if (!workspace_session_path(path, sizeof(path)) ||
        !snprintf_ok(snprintf(tmp, sizeof(tmp), "%s.tmp.XXXXXX", path),
                     sizeof(tmp)))
        return;

    collect_layout_windows(layout_root, windows, &window_count_value);
    for (int i = 0; i < window_count_value; i++) {
        if (windows[i] == active_window_index) {
            active_window_rank = i;
            break;
        }
    }
    layout_count = reachable_layout_node_count(layout_root);

    fd = mkstemp(tmp);
    if (fd < 0)
        return;
    fp = fdopen(fd, "w");
    if (!fp) {
        close(fd);
        unlink(tmp);
        return;
    }

    if (fprintf(fp, "%s\nACTIVE_WINDOW %d\nBUFFER_COUNT %d\n",
                SESSION_V2_MAGIC, active_window_rank, persisted_count) < 0)
        ok = 0;
    for (int ordinal = 0; ok && ordinal < persisted_count; ordinal++) {
        int index = persisted_buffers[ordinal];
        EditorBuffer *buffer = &editor_buffers[index];
        const char *identity = buffer->path[0] ?
                               buffer->path : buffer->draft_name;
        char encoded[sizeof(buffer->path) * 2 + 1];

        if (!hex_encode_string(identity, encoded, sizeof(encoded)) ||
            fprintf(fp, "BUFFER %c %d %d %d %llu %s\n",
                    buffer->path[0] ? 'F' : 'U',
                    buffer->cursor_y, buffer->cursor_x, buffer->view_top,
                    buffer->last_used, encoded) < 0)
            ok = 0;
    }
    if (ok && fprintf(fp, "LAYOUT_COUNT %d\n", layout_count) < 0)
        ok = 0;
    if (ok)
        ok = write_session_layout(fp, layout_root, ordinals, 0);
    if (ok)
        ok = write_session_window_histories(fp, windows,
                                            window_count_value, ordinals);
    if (ok && fprintf(fp, "END\n") < 0)
        ok = 0;
    if (ok && fflush(fp) != 0)
        ok = 0;
    if (ok && fsync(fileno(fp)) != 0)
        ok = 0;
    if (fclose(fp) != 0)
        ok = 0;

    if (ok && rename(tmp, path) == 0) {
        persistence_log_event(__func__,
                              "saved workspace buffers=%d windows=%d path='%s'",
                              persisted_count, window_count_value, path);
    } else {
        unlink(tmp);
        persistence_log_event(__func__,
                              "workspace save failed path='%s' errno=%d '%s'",
                              path, errno, strerror(errno));
    }
}

static void save_session(void)
{
    char path[PATH_MAX];
    char tmp[PATH_MAX];
    FILE *fp;
    int fd;

    persistence_log_event(__func__, "enter");
    persistence_log_state(__func__, "save_session entry", filename);

    if (buffer_system_ready) {
        save_workspace_session();
        return;
    }

    if (filename[0] && transient_mail_file(filename)) {
        persistence_log_event(__func__, "exit skipped reason='transient mail file' filename='%s'", filename);
        return;
    }

    if (!filename[0] && document_is_empty()) {
        persistence_log_event(__func__, "clearing session reason='empty untitled buffer'");
        clear_session();
        persistence_log_event(__func__, "exit skipped reason='empty untitled buffer'");
        return;
    }

    if (!session_path(path, sizeof(path))) {
        persistence_log_event(__func__, "exit skipped reason='no session path'");
        return;
    }

    if (!snprintf_ok(snprintf(tmp, sizeof(tmp), "%s.tmp.XXXXXX", path), sizeof(tmp))) {
        persistence_log_event(__func__, "exit skipped reason='session tmp path too long' path='%s'", path);
        return;
    }
    fd = mkstemp(tmp);
    if (fd < 0) {
        persistence_log_event(__func__, "exit failed reason='mkstemp' tmp='%s' errno=%d '%s'",
                              tmp, errno, strerror(errno));
        return;
    }

    fp = fdopen(fd, "w");
    if (!fp) {
        persistence_log_event(__func__, "exit failed reason='fdopen' tmp='%s' errno=%d '%s'",
                              tmp, errno, strerror(errno));
        close(fd);
        unlink(tmp);
        return;
    }

    if (filename[0])
        fprintf(fp, "%s\n%d\n%d\n%d\n", filename, cy, cx, top);
    else
        fprintf(fp, "%s\n%s\n%d\n%d\n%d\n",
                SESSION_UNTITLED_MARKER, untitled_name, cy, cx, top);

    if (fclose(fp) != 0) {
        persistence_log_event(__func__, "exit failed reason='fclose' tmp='%s' errno=%d '%s'",
                              tmp, errno, strerror(errno));
        unlink(tmp);
        return;
    }

    if (rename(tmp, path) == 0) {
        persistence_log_event(__func__, "exit saved session path='%s'", path);
        persistence_log_state(__func__, "save_session exit saved", filename);
    } else {
        persistence_log_event(__func__, "exit failed reason='rename' tmp='%s' path='%s' errno=%d '%s'",
                              tmp, path, errno, strerror(errno));
    }
}

static LoadResult load_untitled_autosave_at_position(const char *name, int restore_pos,
                                                     int restore_y, int restore_x, int restore_top)
{
    char path[PATH_MAX];

    persistence_log_event(__func__, "enter name='%s' restore_pos=%d restore_y=%d restore_x=%d restore_top=%d",
                          name ? name : "", restore_pos, restore_y, restore_x, restore_top);
    persistence_log_state(__func__, "load_untitled_autosave_at_position entry", filename);
    untitled_autosave_path_for(name, path, sizeof(path));
    persistence_log_event(__func__, "computed untitled autosave path='%s'", path);
    if (!path[0] || !read_document_into_buffer(path)) {
        persistence_log_event(__func__, "exit result=%s reason='missing or unreadable untitled autosave' path='%s'",
                              load_result_name(LOAD_RESULT_FAILED), path);
        persistence_log_state(__func__, "load_untitled_autosave_at_position exit failed", path);
        return LOAD_RESULT_FAILED;
    }

    filename[0] = '\0';
    snprintf(untitled_name, sizeof(untitled_name), "%s", name);
    SET_DIRTY(1, "persistence recovery");
    SET_AUTOSAVE_DIRTY(0, "persistence recovery");
    SET_LAST_EDIT_TIME(0, "persistence recovery");
    finish_loaded_position(restore_pos, restore_y, restore_x, restore_top);
    if (document_is_empty())
        clear_status();
    else
        set_status("Recovered untitled autosave");
    persistence_log_event(__func__, "exit result=%s path='%s'",
                          load_result_name(LOAD_RESULT_AUTOSAVE), path);
    persistence_log_state(__func__, "load_untitled_autosave_at_position exit autosave", path);
    return LOAD_RESULT_AUTOSAVE;
}

static int validate_session_layout(const SessionLayoutRecord *records,
                                   int record_count, int *position,
                                   int persisted_buffer_count, int *leaves)
{
    const SessionLayoutRecord *record;

    if (*position < 0 || *position >= record_count)
        return 0;
    record = &records[(*position)++];
    if (record->kind == LAYOUT_LEAF) {
        if ((record->window_kind == EDITOR_WINDOW_DOCUMENT &&
             (record->buffer_ordinal < 0 ||
              record->buffer_ordinal >= persisted_buffer_count)) ||
            (record->window_kind != EDITOR_WINDOW_DOCUMENT &&
             record->window_kind != EDITOR_WINDOW_BUFFER_SHELF) ||
            ++(*leaves) > MAX_EDITOR_WINDOWS)
            return 0;
        return 1;
    }
    if ((record->kind != LAYOUT_ABOVE_BELOW &&
         record->kind != LAYOUT_SIDE_BY_SIDE) ||
        record->ratio < 10 || record->ratio > 90)
        return 0;
    return validate_session_layout(records, record_count, position,
                                   persisted_buffer_count, leaves) &&
           validate_session_layout(records, record_count, position,
                                   persisted_buffer_count, leaves);
}

static int build_restored_layout(const SessionLayoutRecord *records,
                                 int record_count, int *position,
                                 int parent, int *window_ranks,
                                 int *window_rank_count)
{
    const SessionLayoutRecord *record;
    int node_index;

    if (*position < 0 || *position >= record_count)
        return -1;
    record = &records[(*position)++];
    node_index = allocate_layout_node();
    if (node_index < 0)
        return -1;
    layout_nodes[node_index].parent = parent;
    layout_nodes[node_index].kind = record->kind;
    layout_nodes[node_index].ratio = record->ratio;

    if (record->kind == LAYOUT_LEAF) {
        int window_index = allocate_editor_window();

        if (window_index < 0)
            return -1;
        layout_nodes[node_index].window_index = window_index;
        editor_windows[window_index].kind = record->window_kind;
        if (record->window_kind == EDITOR_WINDOW_BUFFER_SHELF) {
            editor_windows[window_index].buffer_index = -1;
            buffer_drawer_selected = record->cursor_y;
            buffer_drawer_scroll = record->view_top;
        } else {
            editor_windows[window_index].buffer_index =
                record->buffer_ordinal;
            editor_windows[window_index].cursor_y = record->cursor_y;
            editor_windows[window_index].cursor_x = record->cursor_x;
            editor_windows[window_index].view_top = record->view_top;
        }
        window_ranks[(*window_rank_count)++] = window_index;
        return node_index;
    }

    layout_nodes[node_index].first = build_restored_layout(
        records, record_count, position, node_index,
        window_ranks, window_rank_count);
    layout_nodes[node_index].second = build_restored_layout(
        records, record_count, position, node_index,
        window_ranks, window_rank_count);
    if (layout_nodes[node_index].first < 0 ||
        layout_nodes[node_index].second < 0)
        return -1;
    return node_index;
}

static void initialize_restored_buffer_slot(int index)
{
    EditorBuffer *buffer = &editor_buffers[index];

    memset(buffer, 0, sizeof(*buffer));
    buffer->used = 1;
    buffer->text_line_count = 1;
    buffer->text_lines[0] = new_line("");
    buffer->affinity_line = -1;
    buffer->affinity_x = -1;
    buffer->affinity_doc_row = -1;
    buffer->affinity_col = -1;
    buffer->search_match_y = -1;
    buffer->search_match_x = -1;
    buffer->document_lock_fd = -1;
}

static int read_session_history_stack(FILE *fp, char *line, size_t line_size,
                                      const char *expected_record,
                                      WindowBufferState *states,
                                      int state_count, int persisted_count)
{
    for (int i = 0; i < state_count; i++) {
        char record_name[8];
        WindowBufferState *state = &states[i];

        if (!fgets(line, line_size, fp) ||
            sscanf(line, "%7s %d %d %d %d", record_name,
                   &state->buffer_index, &state->cursor_y,
                   &state->cursor_x, &state->view_top) != 5 ||
            strcmp(record_name, expected_record) != 0 ||
            state->buffer_index < 0 ||
            state->buffer_index >= persisted_count ||
            state->cursor_y < 0 || state->cursor_x < 0 ||
            state->view_top < 0)
            return 0;
    }
    return 1;
}

static int load_workspace_session(FILE *fp)
{
    SessionBufferRecord buffer_records[MAX_BUFFERS];
    SessionLayoutRecord layout_records[MAX_LAYOUT_NODES];
    SessionWindowHistory window_histories[MAX_EDITOR_WINDOWS];
    char line[2200];
    int active_window_rank;
    int persisted_count;
    int layout_count;
    int history_count = 0;
    int validation_position = 0;
    int leaf_count = 0;
    int windows_by_rank[MAX_EDITOR_WINDOWS];
    int restored_window_count = 0;
    int build_position = 0;
    int normalized_windows;
    unsigned long long greatest_use = buffer_use_clock;

    memset(buffer_records, 0, sizeof(buffer_records));
    memset(layout_records, 0, sizeof(layout_records));
    memset(window_histories, 0, sizeof(window_histories));
    if (!fgets(line, sizeof(line), fp) ||
        sscanf(line, "ACTIVE_WINDOW %d", &active_window_rank) != 1 ||
        !fgets(line, sizeof(line), fp) ||
        sscanf(line, "BUFFER_COUNT %d", &persisted_count) != 1 ||
        persisted_count < 1 || persisted_count > MAX_BUFFERS)
        return 0;

    for (int i = 0; i < persisted_count; i++) {
        char encoded[sizeof(buffer_records[i].identity) * 2 + 1];
        SessionBufferRecord *record = &buffer_records[i];

        if (!fgets(line, sizeof(line), fp) ||
            sscanf(line, "BUFFER %c %d %d %d %llu %1024s",
                   &record->kind, &record->cursor_y, &record->cursor_x,
                   &record->view_top, &record->last_used, encoded) != 6 ||
            (record->kind != 'F' && record->kind != 'U') ||
            record->cursor_y < 0 || record->cursor_x < 0 ||
            record->view_top < 0 ||
            !hex_decode_string(encoded, record->identity,
                               sizeof(record->identity)) ||
            !record->identity[0])
            return 0;
    }

    if (!fgets(line, sizeof(line), fp) ||
        sscanf(line, "LAYOUT_COUNT %d", &layout_count) != 1 ||
        layout_count < 1 || layout_count > MAX_LAYOUT_NODES)
        return 0;
    for (int i = 0; i < layout_count; i++) {
        char orientation;
        SessionLayoutRecord *record = &layout_records[i];

        if (!fgets(line, sizeof(line), fp))
            return 0;
        if (sscanf(line, "SPLIT %c %d", &orientation, &record->ratio) == 2) {
            if (orientation == 'A')
                record->kind = LAYOUT_ABOVE_BELOW;
            else if (orientation == 'S')
                record->kind = LAYOUT_SIDE_BY_SIDE;
            else
                return 0;
        } else if (sscanf(line, "LEAF %d %d %d %d",
                          &record->buffer_ordinal, &record->cursor_y,
                          &record->cursor_x, &record->view_top) == 4) {
            record->kind = LAYOUT_LEAF;
            record->window_kind = EDITOR_WINDOW_DOCUMENT;
            if (record->cursor_y < 0 || record->cursor_x < 0 ||
                record->view_top < 0)
                return 0;
        } else if (sscanf(line, "SHELF %d %d",
                          &record->cursor_y, &record->view_top) == 2) {
            record->kind = LAYOUT_LEAF;
            record->window_kind = EDITOR_WINDOW_BUFFER_SHELF;
            record->buffer_ordinal = -1;
            record->cursor_x = 0;
            if (record->cursor_y < 0 || record->view_top < 0)
                return 0;
        } else {
            return 0;
        }
    }
    if (!validate_session_layout(layout_records, layout_count,
                                 &validation_position, persisted_count,
                                 &leaf_count) ||
        validation_position != layout_count || active_window_rank < 0 ||
        active_window_rank >= leaf_count)
        return 0;
    if (!fgets(line, sizeof(line), fp))
        return 0;
    if (strncmp(line, "END", 3) != 0) {
        if (sscanf(line, "HISTORY_COUNT %d", &history_count) != 1 ||
            history_count != leaf_count ||
            history_count < 1 || history_count > MAX_EDITOR_WINDOWS)
            return 0;
        for (int i = 0; i < history_count; i++) {
            int rank;
            SessionWindowHistory *history = &window_histories[i];

            if (!fgets(line, sizeof(line), fp) ||
                sscanf(line, "HISTORY %d %d %d", &rank,
                       &history->previous_buffer_count,
                       &history->next_buffer_count) != 3 ||
                rank != i || history->previous_buffer_count < 0 ||
                history->previous_buffer_count > MAX_BUFFERS ||
                history->next_buffer_count < 0 ||
                history->next_buffer_count > MAX_BUFFERS ||
                !read_session_history_stack(
                    fp, line, sizeof(line), "PREV",
                    history->previous_buffers,
                    history->previous_buffer_count, persisted_count) ||
                !read_session_history_stack(
                    fp, line, sizeof(line), "NEXT",
                    history->next_buffers,
                    history->next_buffer_count, persisted_count))
                return 0;
        }
        if (!fgets(line, sizeof(line), fp) ||
            strncmp(line, "END", 3) != 0)
            return 0;
    }

    reset_wrap_cache();
    for (int i = 0; i < MAX_BUFFERS; i++)
        if (editor_buffers[i].used)
            free_buffer_storage(i);
    for (int i = 0; i < persisted_count; i++) {
        SessionBufferRecord *record = &buffer_records[i];
        LoadResult result;

        initialize_restored_buffer_slot(i);
        active_buffer_index = i;
        if (record->kind == 'F') {
            result = load_file_at_position(record->identity, 1, 1,
                                           record->cursor_y,
                                           record->cursor_x,
                                           record->view_top);
            if (result == LOAD_RESULT_FAILED) {
                copy_string(filename, sizeof(editor_buffers[i].path),
                            record->identity);
                finish_loaded_position(1, record->cursor_y,
                                       record->cursor_x, record->view_top);
            }
        } else {
            result = load_untitled_autosave_at_position(
                record->identity, 1, record->cursor_y,
                record->cursor_x, record->view_top);
            if (result == LOAD_RESULT_FAILED) {
                copy_string(untitled_name,
                            sizeof(editor_buffers[i].draft_name),
                            record->identity);
                finish_loaded_position(1, record->cursor_y,
                                       record->cursor_x, record->view_top);
            }
        }
        editor_buffers[i].last_used = record->last_used;
        if (record->last_used > greatest_use)
            greatest_use = record->last_used;
    }
    buffer_use_clock = greatest_use;

    memset(editor_windows, 0, sizeof(editor_windows));
    memset(layout_nodes, 0, sizeof(layout_nodes));
    layout_root = build_restored_layout(layout_records, layout_count,
                                        &build_position, -1,
                                        windows_by_rank,
                                        &restored_window_count);
    if (layout_root < 0 || build_position != layout_count ||
        restored_window_count != leaf_count)
        return 0;
    for (int rank = 0; rank < history_count; rank++) {
        EditorWindow *window = &editor_windows[windows_by_rank[rank]];
        SessionWindowHistory *history = &window_histories[rank];

        memcpy(window->previous_buffers, history->previous_buffers,
               sizeof(window->previous_buffers));
        window->previous_buffer_count = history->previous_buffer_count;
        memcpy(window->next_buffers, history->next_buffers,
               sizeof(window->next_buffers));
        window->next_buffer_count = history->next_buffer_count;
    }
    active_window_index = windows_by_rank[active_window_rank];
    normalized_windows = normalize_restored_workspace_windows();
    load_editor_window(active_window_index);
    if (normalized_windows) {
        snprintf(status_msg, sizeof(status_msg),
                 "Workspace restored: %d buffer%s; views reduced to %d",
                 persisted_count, persisted_count == 1 ? "" : "s",
                 document_window_count());
    } else {
        snprintf(status_msg, sizeof(status_msg),
                 "Workspace restored: %d buffer%s, %d window%s",
                 persisted_count, persisted_count == 1 ? "" : "s",
                 document_window_count(),
                 document_window_count() == 1 ? "" : "s");
    }
    status_time = time(NULL);
    return 1;
}

static int load_session(void)
{
    char path[PATH_MAX];
    char filebuf[PATH_MAX];
    int sy = 0;
    int sx = 0;
    int st = 0;
    FILE *fp = NULL;
    LoadResult result;

    persistence_log_event(__func__, "enter");
    persistence_log_state(__func__, "load_session entry", filename);

    if (buffer_system_ready && workspace_session_path(path, sizeof(path))) {
        persistence_log_event(__func__, "workspace path='%s'", path);
        fp = fopen(path, "r");
        if (!fp)
            persistence_log_event(__func__,
                                  "workspace open failed; trying legacy session path='%s' errno=%d '%s'",
                                  path, errno, strerror(errno));
    }

    if (!fp) {
        if (!session_path(path, sizeof(path))) {
            persistence_log_event(__func__,
                                  "exit false reason='no session path'");
            return 0;
        }

        persistence_log_event(__func__, "legacy session path='%s'", path);
        migrate_legacy_session(path);
        fp = fopen(path, "r");
        if (!fp) {
            persistence_log_event(__func__, "exit false reason='session open failed' path='%s' errno=%d '%s'",
                                  path, errno, strerror(errno));
            return 0;
        }
    }

    if (!fgets(filebuf, sizeof(filebuf), fp)) {
        persistence_log_event(__func__, "exit false reason='missing session filename' path='%s'", path);
        fclose(fp);
        return 0;
    }

    filebuf[strcspn(filebuf, "\r\n")] = 0;
    persistence_log_event(__func__, "session target='%s'", filebuf);

    if (strcmp(filebuf, SESSION_V2_MAGIC) == 0) {
        int loaded = load_workspace_session(fp);

        fclose(fp);
        if (!loaded)
            set_status("Saved workspace is damaged; started a blank buffer");
        return loaded;
    }

    if (strcmp(filebuf, SESSION_UNTITLED_MARKER) == 0) {
        char namebuf[sizeof(untitled_name)];

        if (!fgets(namebuf, sizeof(namebuf), fp)) {
            persistence_log_event(__func__, "exit false reason='missing untitled session name'");
            fclose(fp);
            return 0;
        }
        namebuf[strcspn(namebuf, "\r\n")] = 0;
        if (fscanf(fp, "%d\n%d\n%d", &sy, &sx, &st) != 3) {
            persistence_log_event(__func__, "exit false reason='bad untitled session position'");
            fclose(fp);
            return 0;
        }
        fclose(fp);
        if (!namebuf[0]) {
            persistence_log_event(__func__, "exit false reason='empty untitled session name'");
            return 0;
        }

        result = load_untitled_autosave_at_position(namebuf, 1, sy, sx, st);
        persistence_log_event(__func__, "untitled restore result=%s document_empty=%d",
                              load_result_name(result), document_is_empty());
        persistence_log_state(__func__, "load_session untitled exit", filename);
        return result == LOAD_RESULT_AUTOSAVE && !document_is_empty();
    }

    if (fscanf(fp, "%d\n%d\n%d", &sy, &sx, &st) != 3) {
        persistence_log_event(__func__, "exit false reason='bad named session position'");
        fclose(fp);
        return 0;
    }

    fclose(fp);

    if (!filebuf[0]) {
        persistence_log_event(__func__, "exit false reason='empty session target'");
        return 0;
    }

    if (filename[0] && strcmp(filename, filebuf) != 0) {
        persistence_log_event(__func__, "exit false reason='session target does not match requested filename' current='%s' session='%s'",
                              filename, filebuf);
        return 0;
    }

    result = load_file_at_position(filebuf, 1, 1, sy, sx, st);
    persistence_log_event(__func__, "named restore load_file_at_position result=%s document_empty=%d",
                          load_result_name(result), document_is_empty());
    if (result == LOAD_RESULT_AUTOSAVE) {
        persistence_log_state(__func__, "load_session exit named autosave", filebuf);
        return !document_is_empty();
    }
    if (result == LOAD_RESULT_DISK && !document_is_empty()) {
        if (!pending_recovery_for(filename))
            set_status("Session restored");
        persistence_log_event(__func__, "exit true reason='disk session restored'");
        persistence_log_state(__func__, "load_session exit disk restored", filebuf);
        return 1;
    }
    persistence_log_event(__func__, "exit false reason='nothing restored' result=%s document_empty=%d",
                          load_result_name(result), document_is_empty());
    persistence_log_state(__func__, "load_session exit nothing restored", filebuf);
    return 0;
}

/*
 * Command-line and desktop "Open With" launches are workspace operations,
 * not alternate one-file sessions. Restore the accumulated buffer/layout
 * snapshot first, then visit every requested path. Only a genuinely new
 * workspace reuses the initial placeholder for its first file.
 */
static int open_startup_files_additively(int path_count, char **paths)
{
    int restored = load_session();
    int first_to_visit = 0;

    save_active_window_view();
    if (path_count <= 0)
        return restored;

    if (!restored) {
        char canonical[sizeof(filename)];

        persistence_log_event(__func__, "opening first file in new workspace path='%s'",
                              paths[0] ? paths[0] : "");
        if (canonical_visit_path(paths[0], canonical, sizeof(canonical)))
            load_file(canonical);
        else
            load_file(paths[0]);
        mark_active_buffer_used();
        save_active_window_view();
        first_to_visit = 1;
    }

    for (int i = first_to_visit; i < path_count; i++)
        visit_file_in_buffer(paths[i]);

    save_active_window_view();
    save_session();
    return restored;
}

static int word_count_for_buffer(int index)
{
    EditorBuffer *buffer;
    int words = 0;
    int in_word = 0;

    if (index < 0 || index >= MAX_BUFFERS || !editor_buffers[index].used)
        return 0;
    buffer = &editor_buffers[index];
    for (int y = 0; y < buffer->text_line_count; y++) {
        for (unsigned char *p = (unsigned char *)buffer->text_lines[y];
             p && *p; p++) {
            if (isspace(*p)) {
                in_word = 0;
            } else if (!in_word) {
                words++;
                in_word = 1;
            }
        }
        in_word = 0;
    }
    return words;
}

static void buffer_name_for_index(int index, char *out, size_t outsz)
{
    EditorBuffer *buffer;

    if (!out || outsz == 0)
        return;
    out[0] = '\0';
    if (index < 0 || index >= MAX_BUFFERS || !editor_buffers[index].used)
        return;
    buffer = &editor_buffers[index];
    if (buffer->path[0]) {
        const char *slash = strrchr(buffer->path, '/');
        copy_string(out, outsz, slash ? slash + 1 : buffer->path);
    } else {
        copy_string(out, outsz, buffer->draft_name);
    }
}

static void draw_pane_border(EditorRect rect, attr_t attr)
{
    int bottom = rect.y + rect.height - 1;
    int right = rect.x + rect.width - 1;

    if (rect.height < 2 || rect.width < 2)
        return;
    attrset(attr);
    mvhline(rect.y, rect.x, ACS_HLINE, rect.width);
    mvhline(bottom, rect.x, ACS_HLINE, rect.width);
    mvvline(rect.y, rect.x, ACS_VLINE, rect.height);
    mvvline(rect.y, right, ACS_VLINE, rect.height);
    mvaddch(rect.y, rect.x, ACS_ULCORNER | attr);
    mvaddch(rect.y, right, ACS_URCORNER | attr);
    mvaddch(bottom, rect.x, ACS_LLCORNER | attr);
    mvaddch(bottom, right, ACS_LRCORNER | attr);
}

static void draw_editor_pane(int window_index, EditorRect rect, int selected)
{
    EditorWindow *window = &editor_windows[window_index];
    BodyGeometry geo;
    char name[180];
    char title[240];
    char footer[sizeof(status_msg) + 4];
    int row;
    int logical_row = 0;
    attr_t border_attr = selected ? A_BOLD : A_DIM;

    if (!window->used || rect.height < 3 || rect.width < 4)
        return;
    activate_buffer_raw(window->buffer_index);
    cy = window->cursor_y;
    cx = window->cursor_x;
    top = window->view_top;
    clamp_cursor();

    pane_rendering = 1;
    pane_rect = rect;
    keep_cursor_visible();
    window->view_top = top;
    geo = body_geometry();

    for (int y = rect.y; y < rect.y + rect.height && y < LINES; y++) {
        if (y >= 0) {
            attrset(body_attr());
            mvhline(y, rect.x, ' ', rect.width);
        }
    }
    draw_pane_border(rect, border_attr);

    buffer_name_for_index(window->buffer_index, name, sizeof(name));
    snprintf(title, sizeof(title), " %s%s%s%s ", selected ? "● " : "",
             name, dirty ? " *" : "",
             editor_buffers[window->buffer_index].lock_blocked ?
             " [open elsewhere]" : "");
    draw_text_clipped(rect.y, rect.x + 2, title, border_attr,
                      rect.width - 4);

    row = geo.top_pad;
    for (int li = 0; li < line_count && row < geo.bottom; li++) {
        int rows = visual_rows_for_line(lines[li]);
        int skip_rows;

        if (logical_row + rows <= top) {
            logical_row += rows;
            continue;
        }
        skip_rows = top - logical_row;
        if (skip_rows < 0)
            skip_rows = 0;
        draw_line_wrapped_from(&row, geo.left, li, lines[li], skip_rows,
                               geo.bottom);
        logical_row += rows;
    }

    if (selected && (status_msg[0] || recovery_prompt_active())) {
        snprintf(footer, sizeof(footer), " %s ", current_footer_text());
    } else {
        snprintf(footer, sizeof(footer), " %d words · buffer %d/%d ",
                 word_count(), window->buffer_index + 1, buffer_count());
    }
    draw_text_clipped(rect.y + rect.height - 1, rect.x + 2, footer,
                      border_attr, rect.width - 4);
}

static void rebuild_buffer_drawer_order(int prefer_other)
{
    int selected_buffer = -1;

    if (buffer_drawer_selected >= 0 &&
        buffer_drawer_selected < buffer_drawer_count)
        selected_buffer = buffer_drawer_order[buffer_drawer_selected];
    buffer_drawer_count = 0;
    for (int i = 0; i < MAX_BUFFERS; i++) {
        if (editor_buffers[i].used)
            buffer_drawer_order[buffer_drawer_count++] = i;
    }
    for (int i = 1; i < buffer_drawer_count; i++) {
        int item = buffer_drawer_order[i];
        int j = i;

        while (j > 0 &&
               editor_buffers[buffer_drawer_order[j - 1]].last_used <
               editor_buffers[item].last_used) {
            buffer_drawer_order[j] = buffer_drawer_order[j - 1];
            j--;
        }
        buffer_drawer_order[j] = item;
    }

    buffer_drawer_selected = 0;
    if (selected_buffer >= 0) {
        for (int i = 0; i < buffer_drawer_count; i++) {
            if (buffer_drawer_order[i] == selected_buffer) {
                buffer_drawer_selected = i;
                break;
            }
        }
    } else if (prefer_other && buffer_drawer_count > 1 &&
               buffer_drawer_order[0] == active_buffer_index) {
        buffer_drawer_selected = 1;
    }
    if (buffer_drawer_selected >= buffer_drawer_count)
        buffer_drawer_selected = buffer_drawer_count - 1;
    if (buffer_drawer_selected < 0)
        buffer_drawer_selected = 0;
}

static int active_window_is_buffer_shelf(void)
{
    return active_window_index >= 0 &&
           active_window_index < MAX_EDITOR_WINDOWS &&
           editor_windows[active_window_index].used &&
           editor_windows[active_window_index].kind ==
               EDITOR_WINDOW_BUFFER_SHELF;
}

static void draw_buffer_shelf_pane(int window_index, EditorRect rect,
                                   int selected)
{
    int visible;
    int name_width;
    int inner_width;
    attr_t border_attr = selected ? A_BOLD : A_DIM;
    char title[80];
    char header[256];
    char footer[sizeof(status_msg) + 4];

    if (rect.height < 4 || rect.width < 8)
        return;
    rebuild_buffer_drawer_order(0);
    visible = rect.height - 4;
    inner_width = rect.width - 2;
    name_width = inner_width / 3;
    if (name_width < 12)
        name_width = 12;
    if (name_width > 28)
        name_width = 28;

    if (buffer_drawer_selected < buffer_drawer_scroll)
        buffer_drawer_scroll = buffer_drawer_selected;
    if (buffer_drawer_selected >= buffer_drawer_scroll + visible)
        buffer_drawer_scroll = buffer_drawer_selected - visible + 1;
    if (buffer_drawer_scroll < 0)
        buffer_drawer_scroll = 0;

    for (int row = rect.y; row < rect.y + rect.height && row < LINES; row++) {
        if (row >= 0) {
            attrset(body_attr());
            mvhline(row, rect.x, ' ', rect.width);
        }
    }
    draw_pane_border(rect, border_attr);
    snprintf(title, sizeof(title), " %s*Buffer List* ",
             selected ? "● " : "");
    draw_text_clipped(rect.y, rect.x + 2, title, border_attr,
                      rect.width - 4);

    snprintf(header, sizeof(header), "   %-*s %7s  %s",
             name_width, "Name", "Words", "File");
    draw_text_clipped(rect.y + 1, rect.x + 1, header, A_BOLD,
                      inner_width);

    for (int slot = 0; slot < visible; slot++) {
        int order_index = buffer_drawer_scroll + slot;
        int row = rect.y + 2 + slot;
        int index;
        char name[160];
        char detail[PATH_MAX + 240];
        const char *location;
        attr_t attr = body_attr();

        if (order_index >= buffer_drawer_count)
            break;
        index = buffer_drawer_order[order_index];
        buffer_name_for_index(index, name, sizeof(name));
        location = editor_buffers[index].path[0] ?
                   editor_buffers[index].path : "unsaved draft";
        snprintf(detail, sizeof(detail), "%c%c%c %-*.*s %7d  %s",
                 index == active_buffer_index ? '.' : ' ',
                 editor_buffers[index].modified ? '*' : ' ',
                 editor_buffers[index].lock_blocked ? '%' : ' ',
                 name_width, name_width, name,
                 word_count_for_buffer(index), location);
        if (order_index == buffer_drawer_selected)
            attr = selected ? A_REVERSE | A_BOLD : A_BOLD;
        draw_text_clipped(row, rect.x + 1, detail, attr, inner_width);
    }

    if (selected && status_msg[0]) {
        snprintf(footer, sizeof(footer), " %s ", status_msg);
    } else if (selected) {
        snprintf(footer, sizeof(footer),
                 " ↑↓ choose · Enter open · d kill · Esc close ");
    } else {
        snprintf(footer, sizeof(footer), " C-x o to enter ");
    }
    draw_text_clipped(rect.y + rect.height - 1, rect.x + 2, footer,
                      border_attr, rect.width - 4);
    (void)window_index;
}

static void draw_workspace_screen(void)
{
    int selected_window = active_window_index;
    int selected_buffer = active_buffer_index;
    int selected_is_shelf = active_window_is_buffer_shelf();
    int shelf_window = buffer_shelf_window_index();
    int windows[MAX_EDITOR_WINDOWS];
    int window_count_value = 0;
    int cursor_row = 0;
    int cursor_col = 0;
    EditorRect selected_rect;

    set_cursor_visibility(0);
    destroy_body_window();
    save_active_window_view();
    recompute_layout_rectangles();
    erase();

    if (shelf_window >= 0) {
        int document_window = selected_is_shelf ?
                              buffer_shelf_companion_window_index(
                                  shelf_window) : selected_window;
        int document_width = COLS / 2;
        EditorRect document_rect = {0, 0, LINES, document_width};
        EditorRect shelf_rect = {0, document_width, LINES,
                                 COLS - document_width};

        if (document_window < 0)
            document_window = first_document_window_in_layout(layout_root);
        editor_window_rects[document_window] = document_rect;
        editor_window_rects[shelf_window] = shelf_rect;
        draw_editor_pane(document_window, document_rect,
                         !selected_is_shelf);
        draw_buffer_shelf_pane(shelf_window, shelf_rect,
                               selected_is_shelf);
        selected_rect = selected_is_shelf ? shelf_rect : document_rect;
    } else if (distraction_free) {
        EditorRect focus = {0, 0, LINES, COLS};
        draw_editor_pane(selected_window, focus, 1);
        selected_rect = focus;
    } else {
        collect_layout_windows(layout_root, windows, &window_count_value);
        for (int i = 0; i < window_count_value; i++) {
            int window_index = windows[i];

            if (editor_windows[window_index].kind ==
                EDITOR_WINDOW_BUFFER_SHELF) {
                draw_buffer_shelf_pane(
                    window_index, editor_window_rects[window_index],
                    window_index == selected_window);
            } else {
                draw_editor_pane(window_index,
                                 editor_window_rects[window_index],
                                 window_index == selected_window);
            }
        }
        selected_rect = editor_window_rects[selected_window];
    }

    active_window_index = selected_window;
    if (selected_is_shelf) {
        activate_buffer_raw(selected_buffer);
        pane_rendering = 0;
        refresh();
        set_cursor_visibility(0);
        screen_cache_valid = 0;
        return;
    }
    activate_buffer_raw(editor_windows[selected_window].buffer_index);
    cy = editor_windows[selected_window].cursor_y;
    cx = editor_windows[selected_window].cursor_x;
    top = editor_windows[selected_window].view_top;
    pane_rendering = 1;
    pane_rect = selected_rect;
    clamp_cursor();
    clamp_top();
    cursor_screen_pos(&cursor_row, &cursor_col);
    move(cursor_row, body_geometry().left + cursor_col);
    refresh();
    set_cursor_visibility(editor_cursor_visibility());
    screen_cache_valid = 0;
}

static int selected_buffer_from_shelf(void)
{
    rebuild_buffer_drawer_order(0);
    if (buffer_drawer_count < 1 || buffer_drawer_selected < 0 ||
        buffer_drawer_selected >= buffer_drawer_count)
        return -1;
    return buffer_drawer_order[buffer_drawer_selected];
}

static void close_buffer_shelf_window(int shelf_window)
{
    int original_window = active_window_index;

    if (shelf_window < 0 || shelf_window >= MAX_EDITOR_WINDOWS ||
        !editor_windows[shelf_window].used ||
        editor_windows[shelf_window].kind != EDITOR_WINDOW_BUFFER_SHELF)
        return;
    if (shelf_window != active_window_index) {
        save_active_window_view();
        load_editor_window(shelf_window);
    }
    if (layout_nodes[layout_root].kind == LAYOUT_LEAF) {
        int replacement = active_buffer_index;

        if (replacement < 0 || replacement >= MAX_BUFFERS ||
            !editor_buffers[replacement].used)
            replacement = most_recent_other_buffer(-1);
        if (replacement >= 0)
            select_buffer_in_active_window(replacement);
    } else {
        int next_window = remove_editor_window_from_layout(shelf_window);

        if (next_window >= 0)
            load_editor_window(next_window);
    }
    if (original_window != shelf_window && original_window >= 0 &&
        original_window < MAX_EDITOR_WINDOWS &&
        editor_windows[original_window].used)
        load_editor_window(original_window);
    set_status("Buffer List closed");
    save_session();
}

static void dismiss_buffer_shelf_window(void)
{
    close_buffer_shelf_window(active_window_index);
}

static void show_buffer_shelf_window(void)
{
    int existing = buffer_shelf_window_index();
    int shelf_window;

    if (existing >= 0) {
        rebuild_buffer_drawer_order(0);
        set_status(existing == active_window_index ?
                   "Buffer List refreshed" :
                   "Buffer List is at the right; C-x o to enter");
        screen_cache_valid = 0;
        return;
    }

    shelf_window = split_editor_window_internal(LAYOUT_SIDE_BY_SIDE, 1);
    if (shelf_window < 0)
        return;
    editor_windows[shelf_window].previous_buffer_count = 0;
    editor_windows[shelf_window].next_buffer_count = 0;
    editor_windows[shelf_window].kind = EDITOR_WINDOW_BUFFER_SHELF;
    editor_windows[shelf_window].buffer_index = -1;
    editor_windows[shelf_window].cursor_y = 0;
    editor_windows[shelf_window].cursor_x = 0;
    editor_windows[shelf_window].view_top = 0;
    buffer_drawer_selected = -1;
    buffer_drawer_scroll = 0;
    rebuild_buffer_drawer_order(1);
    set_status("Buffer List opened at the right; C-x o to enter");
    screen_cache_valid = 0;
    save_session();
}

static void handle_buffer_shelf_key(int ch)
{
    int index;
    int page;

    rebuild_buffer_drawer_order(0);
    page = editor_window_rects[active_window_index].height - 5;
    if (page < 1)
        page = 1;

    if (ch == KEY_UP) {
        if (buffer_drawer_selected > 0)
            buffer_drawer_selected--;
    } else if (ch == KEY_DOWN) {
        if (buffer_drawer_selected + 1 < buffer_drawer_count)
            buffer_drawer_selected++;
    } else if (ch == KEY_PPAGE) {
        buffer_drawer_selected -= page;
        if (buffer_drawer_selected < 0)
            buffer_drawer_selected = 0;
    } else if (ch == KEY_NPAGE) {
        buffer_drawer_selected += page;
        if (buffer_drawer_selected >= buffer_drawer_count)
            buffer_drawer_selected = buffer_drawer_count - 1;
    } else if (ch == KEY_HOME) {
        buffer_drawer_selected = 0;
    } else if (ch == KEY_END) {
        buffer_drawer_selected = buffer_drawer_count - 1;
    } else if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
        index = selected_buffer_from_shelf();
        if (index >= 0) {
            int shelf_window = active_window_index;

            close_buffer_shelf_window(shelf_window);
            select_buffer_in_active_window(index);
            set_status("Buffer selected");
            save_session();
        }
    } else if (ch == 'd' || ch == 'D' || ch == 'k' || ch == 'K' ||
               ch == KEY_DC) {
        index = selected_buffer_from_shelf();
        if (index >= 0) {
            kill_buffer_index(index);
            rebuild_buffer_drawer_order(0);
        }
    } else if (ch == 's' || ch == 'S') {
        index = selected_buffer_from_shelf();
        if (index >= 0) {
            int shelf_window = active_window_index;

            close_buffer_shelf_window(shelf_window);
            select_buffer_in_active_window(index);
            save_file(0);
        }
    } else if (ch == 'n' || ch == 'N') {
        close_buffer_shelf_window(active_window_index);
        create_blank_buffer();
    } else if (ch == 'o' || ch == 'O') {
        close_buffer_shelf_window(active_window_index);
        open_file_prompt();
    } else if (ch == 'g' || ch == 'G') {
        rebuild_buffer_drawer_order(0);
        set_status("Buffer List refreshed");
    } else if (ch == 27 || ch == 'q' || ch == 'Q' || ch == 7) {
        dismiss_buffer_shelf_window();
    } else if (ch == KEY_RESIZE) {
        destroy_body_window();
        recompute_layout_rectangles();
    } else if (ch == KEY_BRACKETED_PASTE) {
        free(pending_bracketed_paste);
        pending_bracketed_paste = NULL;
        set_status("Buffer List is read-only");
    } else {
        set_status("Buffer List: arrows choose, Enter opens, Esc closes");
    }
    screen_cache_valid = 0;
}

static void init_colors(void)
{
    if (!has_colors())
        return;

    start_color();
    use_default_colors();
}

int main(int argc, char **argv)
{
    int prefix = 0;
    int needs_redraw = 1;
    long long next_workspace_claim_ms = 0;

    setlocale(LC_ALL, "");
    signal(SIGPIPE, SIG_IGN);
    make_untitled_name();
    lines[0] = new_line("");
    initialize_buffer_system();
    acquire_workspace_lock();
    if (workspace_session_owner) {
        (void)start_workspace_server();
    } else if (argc > 1 && !env_enabled("SIMPLEWORDS_NEW_INSTANCE") &&
               forward_files_to_workspace(argc - 1, argv + 1)) {
        free_buffer_storage(0);
        fprintf(stdout, "Opened in the running SimpleWords workspace.\n");
        return 0;
    }
    configure_settle_options();
    load_simplewords_config();
    (void)atexit(stop_typewriter_audio);
    (void)atexit(disable_bracketed_paste);
    {
        char cwd[PATH_MAX];
        persistence_log_event(__func__, "startup argc=%d argv1='%s' home='%s' cwd='%s'",
                              argc, argc > 1 ? argv[1] : "",
                              getenv("HOME") ? getenv("HOME") : "",
                              getcwd(cwd, sizeof(cwd)) ? cwd : "(getcwd failed)");
        persistence_log_state(__func__, "startup initial state", argc > 1 ? argv[1] : filename);
    }
    signal(SIGHUP, handle_terminate);
    signal(SIGTERM, handle_terminate);
    signal(SIGINT, handle_terminate);
    (void)start_typewriter_audio();

    {
        int restored;

        persistence_log_event(__func__,
                              "startup restoring workspace before files owner=%d path_count=%d",
                              workspace_session_owner, argc - 1);
        restored = open_startup_files_additively(argc - 1, argv + 1);
        if (argc == 1 && !workspace_session_owner) {
            set_status(restored ?
                       "Workspace restored; another window owns session updates" :
                       "Another SimpleWords workspace is open; started independently");
        }
    }
    persistence_log_state(__func__, "startup after initial load", argc > 1 ? argv[1] : filename);

    use_extended_names(TRUE);
    initscr();
    set_escdelay(25);
    raw();
    noecho();
    nonl();
    keypad(stdscr, TRUE);
    enable_bracketed_paste();
    discover_modified_navigation();
    timeout(250);
    intrflush(stdscr, FALSE);
    leaveok(stdscr, FALSE);
    scrollok(stdscr, FALSE);
    init_colors();
    wbkgdset(stdscr, (chtype)' ' | body_attr());
    set_cursor_visibility(1);
    last_keypress_ms = monotonic_ms();

    while (1) {
        int ch;
        long long input_wait_started_ms;
        long long now_ms;

        if (terminate_requested) {
            flush_recovery_state();
            break;
        }

        now_ms = monotonic_ms();
        if (!workspace_session_owner && now_ms >= next_workspace_claim_ms) {
            next_workspace_claim_ms = now_ms + 1000;
            if (claim_workspace_if_available()) {
                save_session();
                set_status("This window now owns workspace session updates");
                needs_redraw = 1;
            }
        }

        if (!prefix && poll_workspace_requests())
            needs_redraw = 1;

        if (needs_redraw) {
            draw_screen();
            needs_redraw = 0;
        }
        input_wait_started_ms = monotonic_ms();
        ch = read_editor_key();

        if (ch == ERR) {
            /*
             * If stdin/TTY gets weird or timeout polling returns immediately,
             * don't spin the editor at 100% CPU. Sleep a little on idle ticks.
             */
            if (terminate_requested) {
                flush_recovery_state();
                break;
            }
            if (terminal_input_disconnected(STDIN_FILENO)) {
                terminate_requested = SIGHUP;
                flush_recovery_state();
                break;
            }
            if (!prefix && poll_workspace_requests())
                needs_redraw = 1;
            autosave_file();
            if (status_msg[0] && status_time && time(NULL) - status_time > 4) {
                clear_status();
                needs_redraw = 1;
            }
            if (idle_cursor_enabled && !selecting &&
                !idle_cursor_hidden && last_keypress_ms > 0 &&
                monotonic_ms() - last_keypress_ms >= 750) {
                idle_cursor_hidden = 1;
                set_cursor_visibility(0);
            }

            /* A normal timeout already yielded the CPU while ncurses waited.
             * Back off only when a broken TTY returns ERR immediately; an
             * unconditional sleep here created a recurring 50 ms blind spot
             * for newly arriving input. */
            if (monotonic_ms() - input_wait_started_ms < 10)
                napms(10);
            continue;
        }

        last_keypress_ms = monotonic_ms();
        idle_cursor_hidden = 0;
        needs_redraw = 1;

        if (active_window_is_buffer_shelf() && !prefix) {
            if (ch == 24)
                prefix = 1;
            else
                handle_buffer_shelf_key(ch);
            continue;
        }

        if (ch == KEY_BRACKETED_PASTE &&
            !active_window_is_buffer_shelf() && recovery_prompt_active()) {
            free(pending_bracketed_paste);
            pending_bracketed_paste = NULL;
            set_status("Resolve recovery first: r open recovery, d discard");
            prefix = 0;
            continue;
        }

        if (!active_window_is_buffer_shelf() && recovery_prompt_active()) {
            if (ch == 'r' || ch == 'R') {
                open_pending_recovery();
                prefix = 0;
                continue;
            }
            if (ch == 'd' || ch == 'D') {
                discard_pending_recovery();
                prefix = 0;
                continue;
            }
            set_status("Resolve recovery first: r open recovery, d discard");
            prefix = 0;
            continue;
        }

        if (ch == KEY_BRACKETED_PASTE) {
            if (pending_bracketed_paste) {
                if (pending_bracketed_paste[0]) {
                    clear_status();
                    if (ensure_document_edit_lock() &&
                        insert_pasted_text(pending_bracketed_paste))
                        set_status("Pasted");
                } else {
                    set_status("Paste was empty");
                }
                free(pending_bracketed_paste);
                pending_bracketed_paste = NULL;
            }
            prefix = 0;
            continue;
        }

        if (status_msg[0] && ch != 24)
            clear_status();

        if (prefix) {
            ControlXBufferCommand buffer_command =
                control_x_buffer_command_for_key(ch);

            if (ch == 19) {
                if (active_window_is_buffer_shelf())
                    handle_buffer_shelf_key('s');
                else
                    save_file(0);
            } else if (ch == 6) {
                open_file_prompt();
            } else if (buffer_command == CONTROL_X_BUFFER_COMMAND_NEW) {
                new_blank_buffer();
            } else if (buffer_command == CONTROL_X_BUFFER_COMMAND_LIST) {
                show_buffer_shelf_window();
            } else if (ch == 'n' || ch == 'N') {
                new_blank_buffer();
            } else if (ch == 'k' || ch == 'K') {
                if (active_window_is_buffer_shelf())
                    handle_buffer_shelf_key('k');
                else
                    kill_current_buffer();
            } else if (ch == '2') {
                if (active_window_is_buffer_shelf())
                    set_status("Open a document before splitting this window");
                else
                    (void)split_editor_window(LAYOUT_ABOVE_BELOW);
            } else if (ch == '3') {
                if (active_window_is_buffer_shelf())
                    set_status("Open a document before splitting this window");
                else
                    (void)split_editor_window(LAYOUT_SIDE_BY_SIDE);
            } else if (ch == '0') {
                delete_editor_window();
            } else if (ch == '1') {
                delete_other_editor_windows();
            } else if (ch == 'o' || ch == 'O') {
                select_other_editor_window();
            } else if (ch == KEY_LEFT) {
                cycle_editor_buffer(-1);
            } else if (ch == KEY_RIGHT) {
                cycle_editor_buffer(1);
            } else if (ch == 23) {
                if (active_window_is_buffer_shelf()) {
                    int index = selected_buffer_from_shelf();

                    if (index >= 0) {
                        close_buffer_shelf_window(active_window_index);
                        select_buffer_in_active_window(index);
                        save_file_as();
                    }
                } else {
                    save_file_as();
                }
            } else if (ch == 3) {
                if (confirm_quit_workspace())
                    break;
            } else if (ch == 'u' || ch == 'U') {
                if (active_window_is_buffer_shelf())
                    set_status("Buffer List is read-only");
                else if (!undo_count || ensure_document_edit_lock())
                    do_undo();
            } else if (ch == 'r' || ch == 'R' || ch == 18) {
                if (active_window_is_buffer_shelf())
                    set_status("Buffer List is read-only");
                else if (!redo_count || ensure_document_edit_lock())
                    do_redo();
            } else if (ch == 26) {
                distraction_free = !distraction_free;
                clear_status();
                clamp_top();
                screen_cache_valid = 0;
            } else if (ch == 20) {
                config.typewriter_sound = !config.typewriter_sound;
                if (config.typewriter_sound) {
                    if (start_typewriter_audio())
                        set_status("Typewriter sound on");
                    else
                        set_status("Typewriter sound on (audio unavailable)");
                } else {
                    stop_typewriter_audio();
                    set_status("Typewriter sound off");
                }
                if (!save_typewriter_sound_setting())
                    set_status("Typewriter sound changed; config update failed");
            } else {
                set_status("Unknown C-x command");
            }
            prefix = 0;
            continue;
        }

        if (ch == 19) {
            find_word_prompt();
            prefix = 0;
            continue;
        }

        if (find_active && (ch == 'n' || ch == 'N')) {
            repeat_find(ch == 'N' ? -1 : 1);
            continue;
        }

        if (ch == 27 && find_active) {
            find_mode = 0;
            find_active = 0;
            find_match_y = -1;
            find_match_x = -1;
            find_match_len = 0;
            screen_cache_valid = 0;
            set_status("Find cleared");
            continue;
        }

        if (ch == KEY_RESIZE) {
            destroy_body_window();
            screen_cache_valid = 0;
            clamp_top();
            keep_cursor_visible();
        } else if (ch == 24) {
            prefix = 1;
        } else if (ch == KEY_HOME) {
            move_visual_home(0);
            screen_cache_valid = 0;
        } else if (ch == KEY_END) {
            move_visual_end(0);
            screen_cache_valid = 0;
        } else if (ch == KEY_UP) {
            move_visual_line(-1, 0);
            screen_cache_valid = 0;
        } else if (ch == KEY_DOWN) {
            move_visual_line(1, 0);
            screen_cache_valid = 0;
        } else if (ch == KEY_LEFT) {
            move_left(0);
            screen_cache_valid = 0;
        } else if (ch == KEY_RIGHT) {
            move_right(0);
            screen_cache_valid = 0;
        } else if (ch == KEY_EXTEND_UP || ch == KEY_SR) {
            move_visual_line(-1, 1);
        } else if (ch == KEY_EXTEND_DOWN || ch == KEY_SF) {
            move_visual_line(1, 1);
        } else if (ch == KEY_SLEFT) {
            move_left(1);
        } else if (ch == KEY_SRIGHT) {
            move_right(1);
        } else if (ch == KEY_EXTEND_PAGE_UP || ch == KEY_SPREVIOUS) {
            move_page(-1, 1);
        } else if (ch == KEY_EXTEND_PAGE_DOWN || ch == KEY_SNEXT) {
            move_page(1, 1);
        } else if (ch == KEY_PPAGE) {
            move_page(-1, 0);
        } else if (ch == KEY_NPAGE) {
            move_page(1, 0);
        } else if (ch == 27) {
            /* Alt-w / Meta-w copies selection, Emacs-style. */
            timeout(25);
            ch = getch();
            timeout(250);
            if (ch == 'w' || ch == 'W') {
                copy_selection();
            } else if (ch != ERR) {
                ungetch(ch);
            } else {
                int shelf_window = buffer_shelf_window_index();

                if (shelf_window >= 0)
                    close_buffer_shelf_window(shelf_window);
            }
        } else if (ch == 23) {
            if (!selection_nonempty() || ensure_document_edit_lock())
                cut_selection();
        } else if (ch == 25) {
            if (ensure_document_edit_lock())
                paste_clipboard();
        } else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if ((!selection_nonempty() && cx == 0 && cy == 0) ||
                ensure_document_edit_lock())
                (void)keyboard_backspace();
        } else if (ch == KEY_DC) {
            if ((!selection_nonempty() &&
                 cx == (int)strlen(lines[cy]) && cy == line_count - 1) ||
                ensure_document_edit_lock())
                (void)keyboard_delete_forward();
        } else if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
            if (ensure_document_edit_lock()) {
                set_cursor_visibility(0);
                (void)keyboard_newline();
            }
        } else if (ch == '\t' || ch == 9) {
            if (ensure_document_edit_lock())
                (void)keyboard_tab();
        } else if (isprint((unsigned char)ch)) {
            if (ensure_document_edit_lock()) {
                if (selecting)
                    (void)delete_selection();
                (void)insert_printable_key(ch);
            }
        }

    }

    stop_typewriter_audio();

    if (env_enabled("SIMPLEWORDS_AUTOSAVE_ON_EXIT") && filename[0] && dirty) {
        if (!disk_revision_changed(&editor_buffers[active_buffer_index],
                                   filename) &&
            backup_existing_document(filename) && write_document(filename)) {
            if (!pending_recovery_for(filename))
                remove_autosaves_for(filename);
            SET_DIRTY(0, "load reset edit state");
            SET_AUTOSAVE_DIRTY(0, "load reset edit state");
            SET_LAST_EDIT_TIME(0, "load reset edit state");
        }
    }

    flush_recovery_state();
    persistence_log_state(__func__, "main after final flush", filename);
    stop_workspace_server();
    release_workspace_lock();
    destroy_body_window();
    endwin();
    disable_bracketed_paste();

    if (buffer_system_ready) {
        for (int i = 0; i < MAX_BUFFERS; i++)
            if (editor_buffers[i].used)
                free_buffer_storage(i);
    } else {
        for (int i = 0; i < line_count; i++)
            free(lines[i]);
        clear_undo_history();
    }
    free(clip);
    free(pending_bracketed_paste);
    free(desired_rows);
    free(screen_cells);
    free(desired_cells);

    return 0;
}
