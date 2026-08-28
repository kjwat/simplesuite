#define SIMPLEWORDS_TYPEWRITER_TEST
#define main simplewords_program_main
#include "../simplewords.c"
#undef main

#include <assert.h>

static unsigned long long random_state = 0x6a09e667f3bcc909ULL;

static unsigned int random_u32(void)
{
    random_state ^= random_state >> 12;
    random_state ^= random_state << 25;
    random_state ^= random_state >> 27;
    return (unsigned int)((random_state * 2685821657736338717ULL) >> 32);
}

static void inspect_layout(int node_index, int parent,
                           unsigned char *seen_nodes,
                           unsigned char *seen_windows, int *leaves)
{
    LayoutNode *node;

    assert(node_index >= 0 && node_index < MAX_LAYOUT_NODES);
    assert(!seen_nodes[node_index]);
    seen_nodes[node_index] = 1;
    node = &layout_nodes[node_index];
    assert(node->used && node->parent == parent);
    if (node->kind == LAYOUT_LEAF) {
        EditorWindow *window;

        assert(node->window_index >= 0 &&
               node->window_index < MAX_EDITOR_WINDOWS);
        assert(!seen_windows[node->window_index]);
        seen_windows[node->window_index] = 1;
        window = &editor_windows[node->window_index];
        assert(window->used);
        if (window->kind == EDITOR_WINDOW_DOCUMENT) {
            assert(window->buffer_index >= 0 &&
                   window->buffer_index < MAX_BUFFERS);
            assert(editor_buffers[window->buffer_index].used);
        } else {
            assert(window->kind == EDITOR_WINDOW_BUFFER_SHELF);
            assert(window->buffer_index == -1);
        }
        (*leaves)++;
        return;
    }
    assert(node->kind == LAYOUT_ABOVE_BELOW ||
           node->kind == LAYOUT_SIDE_BY_SIDE);
    assert(node->ratio >= 10 && node->ratio <= 90);
    inspect_layout(node->first, node_index, seen_nodes, seen_windows, leaves);
    inspect_layout(node->second, node_index, seen_nodes, seen_windows, leaves);
}

static void inspect_state(void)
{
    unsigned char seen_nodes[MAX_LAYOUT_NODES] = {0};
    unsigned char seen_windows[MAX_EDITOR_WINDOWS] = {0};
    int leaves = 0;
    int used_buffers = 0;
    int used_windows = 0;

    assert(active_buffer_index >= 0 && active_buffer_index < MAX_BUFFERS);
    assert(editor_buffers[active_buffer_index].used);
    assert(active_window_index >= 0 &&
           active_window_index < MAX_EDITOR_WINDOWS);
    assert(editor_windows[active_window_index].used);
    for (int i = 0; i < MAX_BUFFERS; i++) {
        EditorBuffer *buffer = &editor_buffers[i];

        if (!buffer->used)
            continue;
        used_buffers++;
        assert(buffer->text_line_count >= 1 &&
               buffer->text_line_count <= MAX_LINES);
        for (int y = 0; y < buffer->text_line_count; y++) {
            assert(buffer->text_lines[y]);
            assert(strlen(buffer->text_lines[y]) < MAX_LINE);
        }
        assert(buffer->cursor_y >= 0 &&
               buffer->cursor_y < buffer->text_line_count);
        assert(buffer->cursor_x >= 0 &&
               buffer->cursor_x <=
                   (int)strlen(buffer->text_lines[buffer->cursor_y]));
        assert(buffer->undo_item_count >= 0 &&
               buffer->undo_item_count <= UNDO_DEPTH);
        assert(buffer->redo_item_count >= 0 &&
               buffer->redo_item_count <= UNDO_DEPTH);
        assert(undo_stack_retained_bytes(buffer->undo_items,
                                         buffer->undo_item_count) <=
               UNDO_BYTE_LIMIT);
        assert(undo_stack_retained_bytes(buffer->redo_items,
                                         buffer->redo_item_count) <=
               UNDO_BYTE_LIMIT);
    }
    assert(used_buffers >= 1 && used_buffers <= MAX_BUFFERS);
    inspect_layout(layout_root, -1, seen_nodes, seen_windows, &leaves);
    for (int i = 0; i < MAX_EDITOR_WINDOWS; i++) {
        if (!editor_windows[i].used)
            continue;
        used_windows++;
        assert(seen_windows[i]);
        assert(editor_windows[i].previous_buffer_count >= 0 &&
               editor_windows[i].previous_buffer_count <= MAX_BUFFERS);
        assert(editor_windows[i].next_buffer_count >= 0 &&
               editor_windows[i].next_buffer_count <= MAX_BUFFERS);
    }
    assert(leaves == used_windows);
    assert(document_window_count() >= 1);
}

static int random_buffer(void)
{
    int start = (int)(random_u32() % MAX_BUFFERS);

    for (int offset = 0; offset < MAX_BUFFERS; offset++) {
        int index = (start + offset) % MAX_BUFFERS;

        if (editor_buffers[index].used)
            return index;
    }
    return active_buffer_index;
}

static void select_shelf_buffer(int index)
{
    rebuild_buffer_drawer_order(0);
    for (int i = 0; i < buffer_drawer_count; i++) {
        if (buffer_drawer_order[i] == index) {
            buffer_drawer_selected = i;
            return;
        }
    }
}

static void state_machine_stress(void)
{
    for (int step = 0; step < 200000; step++) {
        int operation = (int)(random_u32() % 15u);
        int shelf;
        int index;

        switch (operation) {
        case 0:
            (void)split_editor_window(LAYOUT_ABOVE_BELOW);
            break;
        case 1:
            (void)split_editor_window(LAYOUT_SIDE_BY_SIDE);
            break;
        case 2:
            select_other_editor_window();
            break;
        case 3:
            create_blank_buffer();
            break;
        case 4:
            cycle_editor_buffer((random_u32() & 1u) ? 1 : -1);
            break;
        case 5:
            show_buffer_shelf_window();
            break;
        case 6:
            shelf = buffer_shelf_window_index();
            if (shelf >= 0)
                close_buffer_shelf_window(shelf);
            break;
        case 7:
            shelf = buffer_shelf_window_index();
            if (shelf >= 0) {
                load_editor_window(shelf);
                select_shelf_buffer(random_buffer());
                handle_buffer_shelf_key('\n');
            }
            break;
        case 8:
            delete_editor_window();
            break;
        case 9:
            delete_other_editor_windows();
            break;
        case 10:
            index = random_buffer();
            editor_buffers[index].modified = 0;
            kill_buffer_index(index);
            break;
        case 11:
            (void)normalize_restored_workspace_windows();
            load_editor_window(active_window_index);
            break;
        case 12:
            if (!active_window_is_buffer_shelf())
                select_buffer_in_active_window(random_buffer());
            break;
        case 13:
            LINES = 1 + (int)(random_u32() % 60u);
            COLS = 1 + (int)(random_u32() % 200u);
            recompute_layout_rectangles();
            break;
        default:
            save_active_window_view();
            break;
        }
        inspect_state();
    }
}

static void reset_workspace(void)
{
    for (int i = 0; i < MAX_BUFFERS; i++)
        if (editor_buffers[i].used)
            free_buffer_storage(i);
    memset(editor_buffers, 0, sizeof(editor_buffers));
    memset(editor_windows, 0, sizeof(editor_windows));
    memset(layout_nodes, 0, sizeof(layout_nodes));
    active_buffer_index = 0;
    active_window_index = 0;
    layout_root = 0;
    buffer_system_ready = 0;
    initialize_restored_buffer_slot(0);
    make_untitled_name();
    editor_windows[0].used = 1;
    editor_windows[0].kind = EDITOR_WINDOW_DOCUMENT;
    editor_windows[0].buffer_index = 0;
    layout_nodes[0].used = 1;
    layout_nodes[0].kind = LAYOUT_LEAF;
    layout_nodes[0].parent = -1;
    layout_nodes[0].first = -1;
    layout_nodes[0].second = -1;
    layout_nodes[0].window_index = 0;
    layout_nodes[0].ratio = 50;
    initialize_buffer_system();
    reset_wrap_cache();
}

static char *flatten_document(void)
{
    size_t length = 1;
    char *flat;
    char *out;

    for (int y = 0; y < line_count; y++)
        length += strlen(lines[y]) + (y + 1 < line_count ? 1u : 0u);
    flat = xmalloc(length);
    out = flat;
    for (int y = 0; y < line_count; y++) {
        size_t part = strlen(lines[y]);

        memcpy(out, lines[y], part);
        out += part;
        if (y + 1 < line_count)
            *out++ = '\n';
    }
    *out = '\0';
    return flat;
}

static EditPos offset_position(size_t offset)
{
    EditPos position = {0, 0};

    for (int y = 0; y < line_count; y++) {
        size_t length = strlen(lines[y]);

        if (offset <= length) {
            position.y = y;
            position.x = (int)offset;
            return position;
        }
        offset -= length + 1;
    }
    position.y = line_count - 1;
    position.x = (int)strlen(lines[position.y]);
    return position;
}

static void free_models(char **models, int *count)
{
    while (*count > 0)
        free(models[--(*count)]);
}

static void reset_differential(char **model, char **undo_models,
                               int *undo_count_value, char **redo_models,
                               int *redo_count_value)
{
    for (int y = 0; y < line_count; y++)
        free(lines[y]);
    clear_undo_history();
    line_count = 1;
    lines[0] = new_line("");
    cy = cx = top = 0;
    clear_selection();
    free(*model);
    *model = xstrdup_local("");
    free_models(undo_models, undo_count_value);
    free_models(redo_models, redo_count_value);
}

static void differential_undo_stress(void)
{
    char *undo_models[UNDO_DEPTH] = {0};
    char *redo_models[UNDO_DEPTH] = {0};
    int undo_model_count = 0;
    int redo_model_count = 0;
    char *model = xstrdup_local("");

    reset_workspace();
    for (int step = 0; step < 50000; step++) {
        unsigned int choice = random_u32() % 100u;

        if (strlen(model) > 2000 || undo_model_count >= UNDO_DEPTH - 1)
            reset_differential(&model, undo_models, &undo_model_count,
                               redo_models, &redo_model_count);
        if (choice < 70) {
            size_t old_length = strlen(model);
            size_t first = random_u32() % (old_length + 1);
            size_t second = random_u32() % (old_length + 1);
            char replacement[64];
            size_t replacement_length = random_u32() % 50u;
            char *expected;
            EditPos start;
            EditPos end;

            if (first > second) {
                size_t swap = first;
                first = second;
                second = swap;
            }
            start = offset_position(first);
            end = offset_position(second);
            for (size_t i = 0; i < replacement_length; i++) {
                unsigned int value = random_u32() % 20u;
                replacement[i] = value == 0 ? '\n' :
                                 value == 1 ? '\t' :
                                 (char)('a' + random_u32() % 26u);
            }
            replacement[replacement_length] = '\0';
            expected = xmalloc(first + replacement_length +
                               old_length - second + 1);
            memcpy(expected, model, first);
            memcpy(expected + first, replacement, replacement_length);
            memcpy(expected + first + replacement_length, model + second,
                   old_length - second + 1);
            begin_undo_group();
            assert(replace_range_recorded(start.y, start.x, end.y, end.x,
                                          replacement));
            mark_edit();
            end_undo_group();
            undo_models[undo_model_count++] = model;
            model = expected;
            free_models(redo_models, &redo_model_count);
        } else if (choice < 85 && undo_model_count > 0) {
            redo_models[redo_model_count++] = model;
            model = undo_models[--undo_model_count];
            undo_models[undo_model_count] = NULL;
            do_undo();
        } else if (choice >= 85 && redo_model_count > 0) {
            undo_models[undo_model_count++] = model;
            model = redo_models[--redo_model_count];
            redo_models[redo_model_count] = NULL;
            do_redo();
        }
        {
            char *actual = flatten_document();
            assert(strcmp(actual, model) == 0);
            free(actual);
        }
        assert(undo_count == undo_model_count);
        assert(redo_count == redo_model_count);
        inspect_state();
    }
    free(model);
    free_models(undo_models, &undo_model_count);
    free_models(redo_models, &redo_model_count);
}

static void workspace_persistence_stress(void)
{
    char path[PATH_MAX];

    reset_workspace();
    workspace_session_owner = 1;
    strcpy(lines[0], "workspace state");
    mark_edit();
    assert(autosave_file_common(1));
    assert(save_workspace_session());
    assert(workspace_session_path(path, sizeof(path)));
    for (int iteration = 0; iteration < 100; iteration++) {
        FILE *fp = fopen(path, "r");
        char magic[128];

        assert(fp);
        assert(fgets(magic, sizeof(magic), fp));
        assert(strncmp(magic, SESSION_V2_MAGIC,
                       strlen(SESSION_V2_MAGIC)) == 0);
        assert(load_workspace_session(fp));
        assert(fclose(fp) == 0);
        inspect_state();
        assert(save_workspace_session());
    }
}

static void damaged_session_stress(void)
{
    static const char valid[] =
        "ACTIVE_WINDOW 0\n"
        "BUFFER_COUNT 1\n"
        "BUFFER U 0 0 0 1 6472616674\n"
        "LAYOUT_COUNT 1\n"
        "LEAF 0 0 0 0\n"
        "HISTORY_COUNT 1\n"
        "HISTORY 0 0 0\n"
        "END\n";
    size_t length = strlen(valid);

    for (int step = 0; step < 25000; step++) {
        char mutated[sizeof(valid)];
        FILE *fp;
        int changes = 1 + (int)(random_u32() % 8u);

        memcpy(mutated, valid, sizeof(valid));
        for (int i = 0; i < changes; i++)
            mutated[random_u32() % length] =
                (char)(1 + random_u32() % 126u);
        fp = tmpfile();
        assert(fp);
        assert(fwrite(mutated, 1, length, fp) == length);
        rewind(fp);
        (void)load_workspace_session(fp);
        assert(fclose(fp) == 0);
        inspect_state();
    }
}

static void remove_tree(const char *path)
{
    DIR *directory = opendir(path);

    if (directory) {
        struct dirent *entry;

        while ((entry = readdir(directory)) != NULL) {
            char child[PATH_MAX];

            if (strcmp(entry->d_name, ".") == 0 ||
                strcmp(entry->d_name, "..") == 0)
                continue;
            assert(format_string(child, sizeof(child), "%s/%s", path,
                                 entry->d_name));
            remove_tree(child);
        }
        assert(closedir(directory) == 0);
        assert(rmdir(path) == 0);
    } else {
        assert(unlink(path) == 0);
    }
}

int main(void)
{
    char home[] = "/tmp/simplewords-state-test.XXXXXX";

    assert(mkdtemp(home));
    assert(setenv("HOME", home, 1) == 0);
    assert(setlocale(LC_ALL, "C.UTF-8"));
    LINES = 30;
    COLS = 120;
    make_untitled_name();
    lines[0] = new_line("");
    initialize_buffer_system();
    state_machine_stress();
    differential_undo_stress();
    workspace_persistence_stress();
    damaged_session_stress();
    workspace_session_owner = 0;
    reset_workspace();
    free_buffer_storage(0);
    reset_wrap_cache();
    free(wrap_cache);
    wrap_cache = NULL;
    remove_tree(home);
    return 0;
}
