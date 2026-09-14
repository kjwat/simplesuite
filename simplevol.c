/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include <ncurses.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include "simpleui.h"

#define ROWS_MAX 512
#define OPTIONS_MAX 2048
#define EFFECTS_MAX 64
#define PRESETS_MAX 128
#define TEXT 512
#define LINE_MAXIMUM 8192

typedef struct {
    int page, muted, is_default;
    char id[32], name[TEXT], title[TEXT], detail[TEXT], channels[TEXT], channel_names[TEXT];
    double volume;
} Row;
typedef struct {
    int page, enabled, active;
    char id[32], kind[16], key[TEXT], label[TEXT];
} Option;
typedef struct {
    char key[32], label[80], unit[16], help[TEXT];
    double value, minimum, maximum, step;
} Effect;
typedef struct {
    Row rows[ROWS_MAX];
    Option options[OPTIONS_MAX];
    Effect effects[EFFECTS_MAX];
    char presets[PRESETS_MAX][80], preset_types[PRESETS_MAX][32];
    int nrows, noptions, neffects, npresets, running, autostart;
    char preset[80], output[TEXT], server[TEXT];
    double in_l, in_r, out_l, out_r, compression, limiting, level_gain, loudness;
} Snapshot;
static Snapshot current, pending;
static const char *pages[] = {"Playback", "Outputs", "Inputs", "Recording", "Cards", "Effects"};
static const char *sections[] = {"playback", "outputs", "inputs", "recording", "cards", "effects"};
static int page, selected[6], top[6], child_in = -1, child_out = -1;
static pid_t child = -1;
static volatile sig_atomic_t stopping;
static char message[TEXT] = "Connecting to audio server...";
static bool message_error;
static int64_t message_until;
static char helper_path[PATH_MAX];

typedef enum {DIALOG_NONE, DIALOG_HELP, DIALOG_CHOICES, DIALOG_NUMBER, DIALOG_SAVE} DialogKind;
static struct {
    DialogKind kind;
    char title[160], command[32], section[32], identity[32], text[TEXT];
    char keys[OPTIONS_MAX][TEXT], labels[OPTIONS_MAX][TEXT];
    int enabled[OPTIONS_MAX], count, selected, top;
    bool replace_input;
} dialog;

static void copy(char *dest, size_t size, const char *source)
{
    if (!size) return;
    if (!source) source = "";
    size_t length = strlen(source);
    if (length >= size) length = size - 1;
    memmove(dest, source, length);
    dest[length] = 0;
}

static void notice(bool error, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    message_error = error;
    message_until = sui_monotonic_ms() + (error ? 15000 : 4000);
}

static void stop_signal(int number)
{
    (void)number;
    stopping = 1;
}

static int page_number(const char *name)
{
    for (int i = 0; i < 6; i++) if (!strcmp(name, sections[i])) return i;
    return -1;
}

static int row_indices(int indices[ROWS_MAX])
{
    int count = 0;
    for (int i = 0; i < current.nrows; i++)
        if (current.rows[i].page == page) indices[count++] = i;
    return count;
}

static Row *selected_row(void)
{
    int indices[ROWS_MAX], count = row_indices(indices);
    if (!count || selected[page] < 0 || selected[page] >= count) return NULL;
    return &current.rows[indices[selected[page]]];
}

static Effect *effect_by_key(const char *key)
{
    for (int i = 0; i < current.neffects; i++)
        if (!strcmp(current.effects[i].key, key)) return &current.effects[i];
    return NULL;
}

static double effect_value(const char *key)
{
    Effect *effect = effect_by_key(key);
    return effect ? effect->value : 0;
}

static void decode(char *text)
{
    char *out = text;
    for (char *in = text; *in; in++) {
        unsigned char c = (unsigned char)*in;
        if (c == '%' && in[1] && in[2] && isxdigit((unsigned char)in[1]) && isxdigit((unsigned char)in[2])) {
            char hex[3] = {in[1], in[2], 0};
            c = (unsigned char)strtoul(hex, NULL, 16);
            in += 2;
        }
        *out++ = (c < 32 || c == 127) ? ' ' : (char)c;
    }
    *out = 0;
}

static void send_command(const char *command, const char *a, const char *b, const char *c, const char *d)
{
    const char *fields[] = {command, a, b, c, d};
    char buffer[4096];
    size_t used = 0;
    for (size_t i = 0; i < 5 && fields[i]; i++) {
        if (i) buffer[used++] = '\t';
        for (const unsigned char *p = (const unsigned char *)fields[i]; *p; p++) {
            if (used + 5 >= sizeof(buffer)) { notice(true, "Command is too long"); return; }
            if (*p == '%' || *p < 32 || *p == 127) {
                snprintf(buffer + used, 4, "%%%02X", *p);
                used += 3;
            } else buffer[used++] = (char)*p;
        }
    }
    buffer[used++] = '\n';
    ssize_t result;
    do { result = write(child_in, buffer, used); } while (result < 0 && errno == EINTR);
    if (result != (ssize_t)used) notice(true, "Audio controller is busy or disconnected");
    else notice(false, "Applying...");
}

static void commit_snapshot(void)
{
    char identities[5][32] = {{0}};
    for (int p = 0; p < 5; p++) {
        int index = 0;
        for (int i = 0; i < current.nrows; i++) {
            if (current.rows[i].page != p) continue;
            if (index++ == selected[p]) { copy(identities[p], sizeof(identities[p]), current.rows[i].id); break; }
        }
    }
    current = pending;
    for (int p = 0; p < 5; p++) {
        int count = 0, found = -1;
        for (int i = 0; i < current.nrows; i++) {
            if (current.rows[i].page != p) continue;
            if (!strcmp(current.rows[i].id, identities[p])) found = count;
            count++;
        }
        if (found >= 0) selected[p] = found;
        else if (selected[p] >= count) selected[p] = count > 0 ? count - 1 : 0;
    }
}

static void parse_line(char *line)
{
    char *fields[16];
    int count = 0;
    fields[count++] = line;
    for (char *p = line; *p; p++) {
        if (*p == '\t') {
            *p = 0;
            if (count < 16) fields[count++] = p + 1;
        }
    }
    for (int i = 0; i < count; i++) decode(fields[i]);
    if (!strcmp(fields[0], "BEGIN")) {
        memset(&pending, 0, sizeof(pending));
        pending.in_l = pending.in_r = pending.out_l = pending.out_r = pending.loudness = -99;
    } else if (!strcmp(fields[0], "END")) commit_snapshot();
    else if (!strcmp(fields[0], "MESSAGE") && count >= 3) notice(!strcmp(fields[1], "error"), "%s", fields[2]);
    else if (!strcmp(fields[0], "INFO") && count >= 6) {
        pending.running = atoi(fields[1]); pending.autostart = atoi(fields[2]);
        copy(pending.preset, sizeof(pending.preset), fields[3]);
        copy(pending.output, sizeof(pending.output), fields[4]);
        copy(pending.server, sizeof(pending.server), fields[5]);
    } else if (!strcmp(fields[0], "ROW") && count >= 11 && pending.nrows < ROWS_MAX) {
        Row *row = &pending.rows[pending.nrows++];
        row->page = page_number(fields[1]);
        copy(row->id, sizeof(row->id), fields[2]); copy(row->name, sizeof(row->name), fields[3]);
        copy(row->title, sizeof(row->title), fields[4]); copy(row->detail, sizeof(row->detail), fields[5]);
        row->volume = atof(fields[6]); row->muted = atoi(fields[7]); row->is_default = atoi(fields[8]);
        copy(row->channels, sizeof(row->channels), fields[9]); copy(row->channel_names, sizeof(row->channel_names), fields[10]);
    } else if (!strcmp(fields[0], "OPTION") && count >= 8 && pending.noptions < OPTIONS_MAX) {
        Option *option = &pending.options[pending.noptions++];
        option->page = page_number(fields[1]);
        copy(option->id, sizeof(option->id), fields[2]); copy(option->kind, sizeof(option->kind), fields[3]);
        copy(option->key, sizeof(option->key), fields[4]); copy(option->label, sizeof(option->label), fields[5]);
        option->enabled = atoi(fields[6]); option->active = atoi(fields[7]);
    } else if (!strcmp(fields[0], "FX") && count >= 9 && pending.neffects < EFFECTS_MAX) {
        Effect *effect = &pending.effects[pending.neffects++];
        copy(effect->key, sizeof(effect->key), fields[1]); copy(effect->label, sizeof(effect->label), fields[2]);
        effect->value = atof(fields[3]); effect->minimum = atof(fields[4]); effect->maximum = atof(fields[5]); effect->step = atof(fields[6]);
        copy(effect->unit, sizeof(effect->unit), fields[7]); copy(effect->help, sizeof(effect->help), fields[8]);
    } else if (!strcmp(fields[0], "PRESET") && count >= 3 && pending.npresets < PRESETS_MAX) {
        copy(pending.presets[pending.npresets], sizeof(pending.presets[0]), fields[1]);
        copy(pending.preset_types[pending.npresets++], sizeof(pending.preset_types[0]), fields[2]);
    } else if (!strcmp(fields[0], "METER") && count >= 3) {
        double value = atof(fields[2]);
        if (!isfinite(value)) return;
        if (!strcmp(fields[1], "input_l")) pending.in_l = value;
        else if (!strcmp(fields[1], "input_r")) pending.in_r = value;
        else if (!strcmp(fields[1], "output_l")) pending.out_l = value;
        else if (!strcmp(fields[1], "output_r")) pending.out_r = value;
        else if (!strcmp(fields[1], "compression")) pending.compression = value;
        else if (!strcmp(fields[1], "limiting")) pending.limiting = value;
        else if (!strcmp(fields[1], "level_gain")) pending.level_gain = value;
        else if (!strcmp(fields[1], "loudness")) pending.loudness = value;
    }
}

static bool read_updates(void)
{
    static char line[LINE_MAXIMUM];
    static size_t used;
    static bool overflow;
    char buffer[16384];
    bool changed = false;
    for (;;) {
        ssize_t length = read(child_out, buffer, sizeof(buffer));
        if (length == 0) {
            notice(true, "Audio controller exited. Restart simplevol; use --doctor for details.");
            close(child_out); child_out = -1;
            return true;
        }
        if (length < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (ssize_t i = 0; i < length; i++) {
            if (buffer[i] == '\n') {
                line[used] = 0;
                if (!overflow) parse_line(line);
                used = 0; overflow = false; changed = true;
            } else if (used + 1 < sizeof(line)) line[used++] = buffer[i];
            else overflow = true;
        }
    }
    return changed;
}

static void line_text(int y, int x, int width, const char *format, ...)
{
    char text[2048];
    va_list args;
    if (y < 0 || y >= LINES || x < 0 || x >= COLS || width <= 0) return;
    va_start(args, format); vsnprintf(text, sizeof(text), format, args); va_end(args);
    mvaddnstr(y, x, text, width < COLS - x ? width : COLS - x);
}

static void bar(int y, int x, int width, double value, double minimum, double maximum)
{
    if (width < 3) return;
    double proportion = (value - minimum) / (maximum - minimum);
    if (!isfinite(proportion)) proportion = 0;
    int filled = (int)round(fmax(0, fmin(1, proportion)) * (width - 2));
    mvaddch(y, x, '[');
    for (int i = 0; i < width - 2; i++) addch(i < filled ? '=' : ' ');
    addch(']');
}

static void keep_visible(int count, int height)
{
    if (selected[page] >= count) selected[page] = count ? count - 1 : 0;
    if (selected[page] < 0) selected[page] = 0;
    if (selected[page] < top[page]) top[page] = selected[page];
    if (selected[page] >= top[page] + height) top[page] = selected[page] - height + 1;
    if (top[page] < 0) top[page] = 0;
}

static void draw_mixer(void)
{
    int indices[ROWS_MAX], count = row_indices(indices);
    int height = (LINES - 9) / 3;
    if (height < 1) height = 1;
    keep_visible(count, height);
    if (!count) {
        line_text(6, 3, COLS - 6, "%s", page == 0 ? "No applications are playing audio." : page == 3 ? "No applications are recording audio." : "No devices available on this page.");
        return;
    }
    for (int n = top[page]; n < count && n < top[page] + height; n++) {
        Row *row = &current.rows[indices[n]];
        int y = 5 + (n - top[page]) * 3;
        if (n == selected[page]) attron(A_REVERSE);
        mvhline(y, 1, ' ', COLS - 2);
        line_text(y, 2, COLS - (page == 4 ? 4 : 21), "%c %s", row->is_default ? '*' : ' ', row->title);
        if (page != 4) line_text(y, COLS - 18, 17, "%s %6.1f%%", row->muted ? "MUTED" : "     ", row->volume);
        if (n == selected[page]) attroff(A_REVERSE);
        line_text(y + 1, 4, COLS - 8, "%s", row->detail);
        if (page != 4) bar(y + 2, 4, COLS - 8 > 74 ? 74 : COLS - 8, row->volume, 0, 150);
    }
}

static void format_effect(char *dest, size_t size, Effect *effect)
{
    if (!strcmp(effect->unit, "bool")) copy(dest, size, effect->value ? "ON" : "off");
    else if (!strcmp(effect->unit, "choice")) {
        const char *choices[] = {"None", "2x", "4x", "8x"};
        int choice = (int)effect->value;
        copy(dest, size, choice >= 0 && choice < 4 ? choices[choice] : "?");
    } else snprintf(dest, size, "%g %s", effect->value, effect->unit);
}

static void draw_meter(int y, int x, int width, const char *name, double value)
{
    line_text(y, x, width, "%s", name);
    bar(y + 1, x, width - 10, value, -60, 0);
    line_text(y + 1, x + width - 9, 9, "%6.1f dB", value);
}

static void draw_effects(void)
{
    int split = COLS >= 110 ? 55 : COLS;
    int height = LINES - 10;
    keep_visible(current.neffects, height);
    for (int i = top[page]; i < current.neffects && i < top[page] + height; i++) {
        Effect *effect = &current.effects[i];
        char value[64];
        format_effect(value, sizeof(value), effect);
        int y = 5 + i - top[page];
        if (!strcmp(effect->unit, "bool")) attron(A_BOLD);
        if (i == selected[page]) attron(A_REVERSE);
        mvhline(y, 1, ' ', split - 2);
        line_text(y, 3, split - 19, "%s", effect->label);
        line_text(y, split - 16, 14, "%12s", value);
        attroff(A_REVERSE | A_BOLD);
    }
    if (current.neffects && selected[page] < current.neffects)
        line_text(LINES - 5, 2, COLS - 4, "%s", current.effects[selected[page]].help);
    if (split == COLS) return;
    int x = split + 3, width = COLS - x - 3;
    if (width > 75) width = 75;
    attron(A_BOLD);
    line_text(5, x, width, "SIGNAL");
    attroff(A_BOLD);
    line_text(6, x, width, "Input -> EQ -> Compressor -> Leveling -> Limiter");
    line_text(8, x, width, "EQ: %s", current.preset);
    int gap = width / 10;
    for (int band = 0; band < 10; band++) {
        char key[16]; snprintf(key, sizeof(key), "eq%d", band);
        double value = effect_value(key);
        int yy = 12 - (int)round(value / 4);
        if (yy < 9) yy = 9;
        if (yy > 15) yy = 15;
        mvaddch(12, x + band * gap, '-');
        mvaddch(yy, x + band * gap, 'o');
        const char *names[] = {"31", "63", "125", "250", "500", "1k", "2k", "4k", "8k", "16k"};
        line_text(16, x + band * gap, gap, "%s", names[band]);
    }
    if (LINES >= 30) {
        draw_meter(19, x, width, "Input L / R", fmax(current.in_l, current.in_r));
        draw_meter(22, x, width, "Output L / R", fmax(current.out_l, current.out_r));
    }
    if (LINES >= 37) {
        line_text(26, x, width, "Compressor reduction  %6.1f dB", current.compression);
        line_text(27, x, width, "Limiter reduction     %6.1f dB", current.limiting);
        line_text(28, x, width, "Leveling gain         %+6.1f dB", current.level_gain);
        line_text(29, x, width, "Leveling output       %6.1f LUFS", current.loudness);
        line_text(31, x, width, "Meters refresh twice per second.");
    }
}

static void draw_dialog(void)
{
    if (dialog.kind == DIALOG_NONE) return;
    int width = COLS - 6, height;
    if (width > 92) width = 92;
    if (dialog.kind == DIALOG_HELP) height = LINES - 4 < 23 ? LINES - 4 : 23;
    else if (dialog.kind == DIALOG_CHOICES) height = dialog.count + 5;
    else height = 7;
    if (height > LINES - 4) height = LINES - 4;
    int y = (LINES - height) / 2, x = (COLS - width) / 2;
    for (int i = 0; i < height; i++) mvhline(y + i, x, ' ', width);
    attron(A_REVERSE);
    mvhline(y, x, ' ', width);
    line_text(y, x + 2, width - 4, "%s", dialog.title);
    attroff(A_REVERSE);
    if (dialog.kind == DIALOG_HELP) {
        const char *help[] = {
            "1-6 / Tab       Playback, outputs, inputs, recording, cards, effects",
            "Up/Down  j/k    Select a stream, device, or effect",
            "Left/Right h/l  Adjust volume by 2% or adjust the selected effect",
            "+ / -          Adjust volume or effect; PageUp/PageDown scroll",
            "Space / m      Mute a stream/device or toggle an effect",
            "Enter          Set an exact value; choose a card profile",
            "d              Make the selected output/input the default",
            "r              Move playback/recording to a different device",
            "p              Choose a port/profile; on Effects, choose a preset",
            "c              Adjust individual channel levels",
            "P / S          Load a preset / save the complete effects chain",
            "o              Choose where processed audio is played",
            "e              Start/stop background effects and restore routing",
            "b              Bypass all effects for an A/B comparison",
            "a              Toggle start at login (independent of running now)",
            "q              Close this panel; background effects keep running",
            "",
            "Effects process stereo playback. Input and recording pages control",
            "microphone volume/routing. * marks the default device.",
            "Preset changes are saved automatically. No startup is enabled by default.",
        };
        for (size_t i = 0; i < sizeof(help) / sizeof(help[0]) && (int)i < height - 3; i++)
            line_text(y + 2 + (int)i, x + 2, width - 4, "%s", help[i]);
    } else if (dialog.kind == DIALOG_CHOICES) {
        int shown = height - 4;
        if (dialog.selected < dialog.top) dialog.top = dialog.selected;
        if (dialog.selected >= dialog.top + shown) dialog.top = dialog.selected - shown + 1;
        for (int i = dialog.top; i < dialog.count && i < dialog.top + shown; i++) {
            int yy = y + 2 + i - dialog.top;
            if (i == dialog.selected) attron(A_REVERSE);
            if (!dialog.enabled[i]) attron(A_DIM);
            mvhline(yy, x + 1, ' ', width - 2);
            line_text(yy, x + 2, width - 4, "%s%s", dialog.labels[i], dialog.enabled[i] ? "" : " (unavailable)");
            attroff(A_REVERSE | A_DIM);
        }
    } else {
        line_text(y + 2, x + 2, width - 4, "> %s", dialog.text);
        line_text(y + 3, x + 2, width - 4, "Enter applies. Esc cancels.");
    }
    line_text(y + height - 1, x + 2, width - 4, "Esc: close");
}

static void draw(void)
{
    erase();
    if (COLS < 56 || LINES < 16) {
        line_text(0, 0, COLS, "simplevol needs at least 56 columns and 16 rows.");
        line_text(2, 0, COLS, "Resize the terminal, or press q to close.");
        refresh(); return;
    }
    attron(A_BOLD);
    line_text(0, 2, COLS - 4, "simplevol   effects: %s%s   login: %s", current.running ? "RUNNING" : "off",
              effect_value("bypass") ? " [BYPASS]" : "", current.autostart ? "ON" : "off");
    attroff(A_BOLD);
    int x = 2;
    for (int i = 0; i < 6; i++) {
        char name[32]; snprintf(name, sizeof(name), "%d %s", i + 1, pages[i]);
        if (i == page) attron(A_REVERSE);
        line_text(2, x, COLS - x, "%s", name);
        if (i == page) attroff(A_REVERSE);
        x += (int)strlen(name) + 2;
    }
    mvhline(3, 1, ACS_HLINE, COLS - 2);
    if (page == 5) draw_effects(); else draw_mixer();
    if (message_until > sui_monotonic_ms() || !current.server[0]) {
        if (message_error) attron(A_BOLD);
        line_text(LINES - 4, 2, COLS - 4, "%s", message);
        attroff(A_BOLD);
    } else if (page != 5) {
        Row *row = selected_row();
        line_text(LINES - 4, 2, COLS - 4, "%s", row ? row->name : current.server);
    }
    mvhline(LINES - 3, 1, ACS_HLINE, COLS - 2);
    line_text(LINES - 2, 2, COLS - 4, "Arrows: select/adjust  Space: mute/toggle  Enter: edit  ?: help  q: close");
    line_text(LINES - 1, 2, COLS - 4, "e: effects  b: bypass  a: login  P: presets  S: save  o: effects output");
    draw_dialog();
    refresh();
}

static void new_dialog(DialogKind kind, const char *title, const char *command, const char *section, const char *identity)
{
    memset(&dialog, 0, sizeof(dialog));
    dialog.kind = kind;
    copy(dialog.title, sizeof(dialog.title), title); copy(dialog.command, sizeof(dialog.command), command);
    copy(dialog.section, sizeof(dialog.section), section); copy(dialog.identity, sizeof(dialog.identity), identity);
    dialog.replace_input = true;
}

static void choice(const char *key, const char *label, int enabled)
{
    if (dialog.count == OPTIONS_MAX) return;
    copy(dialog.keys[dialog.count], sizeof(dialog.keys[0]), key);
    copy(dialog.labels[dialog.count], sizeof(dialog.labels[0]), label);
    dialog.enabled[dialog.count++] = enabled;
}

static void choose_presets(void)
{
    new_dialog(DIALOG_CHOICES, "Presets - EQ presets keep your dynamics settings", "preset", "", "");
    for (int i = 0; i < current.npresets; i++) {
        char label[160]; snprintf(label, sizeof(label), "%s  [%s]", current.presets[i], current.preset_types[i]);
        choice(current.presets[i], label, 1);
    }
}

static void choose_routes(bool effects)
{
    Row *row = selected_row();
    if (!effects && (!row || (page != 0 && page != 3))) { notice(true, "Select a playback or recording stream first"); return; }
    new_dialog(DIALOG_CHOICES, effects ? "Effects output device" : "Route stream to", effects ? "output" : "route",
               effects ? "" : sections[page], effects ? "" : row->id);
    int target = effects || page == 0 ? 1 : 2;
    for (int i = 0; i < current.nrows; i++) {
        Row *candidate = &current.rows[i];
        if (candidate->page != target) continue;
        if ((effects || (row && !strcmp(row->name, "simplevol.output"))) && !strcmp(candidate->name, "simplevol.effects")) continue;
        choice(candidate->name, candidate->title, 1);
    }
}

static void choose_ports(void)
{
    Row *row = selected_row();
    if (!row || (page != 1 && page != 2 && page != 4)) { notice(true, "Select an output, input, or sound card first"); return; }
    new_dialog(DIALOG_CHOICES, page == 4 ? "Sound card profile" : "Device port", page == 4 ? "profile" : "port", sections[page], row->id);
    for (int i = 0; i < current.noptions; i++) {
        Option *option = &current.options[i];
        if (option->page != page || strcmp(option->id, row->id)) continue;
        char label[TEXT + 8]; snprintf(label, sizeof(label), "%s%s", option->active ? "* " : "  ", option->label);
        choice(option->key, label, option->enabled);
    }
}

static void exact_value(void)
{
    if (page == 5) {
        if (!current.neffects) return;
        Effect *effect = &current.effects[selected[page]];
        if (!strcmp(effect->unit, "bool")) {
            send_command("set", effect->key, effect->value ? "0" : "1", NULL, NULL); return;
        }
        char title[160]; snprintf(title, sizeof(title), "%s (%g to %g %s)", effect->label, effect->minimum, effect->maximum, effect->unit);
        new_dialog(DIALOG_NUMBER, title, "set", effect->key, "");
        snprintf(dialog.text, sizeof(dialog.text), "%g", effect->value);
    } else if (page == 4) choose_ports();
    else {
        Row *row = selected_row(); if (!row) return;
        new_dialog(DIALOG_NUMBER, "Volume (0 to 150%)", "volume", sections[page], row->id);
        snprintf(dialog.text, sizeof(dialog.text), "%.1f", row->volume);
    }
}

static void channels(void)
{
    Row *row = selected_row();
    if (!row || page == 4) return;
    new_dialog(DIALOG_CHOICES, "Choose channel to set its volume", "channel-select", sections[page], row->id);
    char names[TEXT], levels[TEXT]; copy(names, sizeof(names), row->channel_names); copy(levels, sizeof(levels), row->channels);
    char *save1, *save2, *name = strtok_r(names, ",", &save1), *level = strtok_r(levels, ",", &save2);
    for (int i = 0; name && level; i++) {
        char key[64], label[TEXT]; snprintf(key, sizeof(key), "%d:%s", i, level);
        snprintf(label, sizeof(label), "%s  %.1f%%", name, atof(level)); choice(key, label, 1);
        name = strtok_r(NULL, ",", &save1); level = strtok_r(NULL, ",", &save2);
    }
}

static void adjust(int direction)
{
    if (page == 5) {
        if (!current.neffects) return;
        Effect *effect = &current.effects[selected[page]];
        send_command("step", effect->key, direction > 0 ? "1" : "-1", NULL, NULL);
    } else {
        Row *row = selected_row();
        if (row && page != 4) send_command("adjust", sections[page], row->id, direction > 0 ? "2" : "-2", NULL);
    }
}

static void dialog_key(int key)
{
    if (key == 27) { dialog.kind = DIALOG_NONE; return; }
    if (dialog.kind == DIALOG_HELP) { if (key == '?' || key == 'q' || key == '\n') dialog.kind = DIALOG_NONE; return; }
    if (dialog.kind == DIALOG_CHOICES) {
        if (key == KEY_UP || key == 'k') { if (dialog.selected > 0) dialog.selected--; }
        else if (key == KEY_DOWN || key == 'j') { if (dialog.selected + 1 < dialog.count) dialog.selected++; }
        else if (key == KEY_NPAGE) { dialog.selected += 8; if (dialog.selected >= dialog.count) dialog.selected = dialog.count - 1; }
        else if (key == KEY_PPAGE) { dialog.selected -= 8; if (dialog.selected < 0) dialog.selected = 0; }
        else if ((key == '\n' || key == KEY_ENTER) && dialog.count) {
            if (!dialog.enabled[dialog.selected]) { notice(true, "That option is unavailable"); return; }
            const char *value = dialog.keys[dialog.selected];
            if (!strcmp(dialog.command, "channel-select")) {
                char title[160], index[32], level[64];
                const char *colon = strchr(value, ':');
                if (!colon) return;
                snprintf(index, sizeof(index), "%.*s", (int)(colon - value), value);
                copy(level, sizeof(level), colon + 1);
                snprintf(title, sizeof(title), "Channel %s volume (0 to 150%%)", index);
                dialog.kind = DIALOG_NUMBER; copy(dialog.title, sizeof(dialog.title), title);
                copy(dialog.command, sizeof(dialog.command), "channel");
                copy(dialog.keys[0], sizeof(dialog.keys[0]), index);
                copy(dialog.text, sizeof(dialog.text), level); dialog.replace_input = true;
                return;
            }
            if (!strcmp(dialog.command, "preset") || !strcmp(dialog.command, "output")) send_command(dialog.command, value, NULL, NULL, NULL);
            else send_command(dialog.command, dialog.section, dialog.identity, value, NULL);
            dialog.kind = DIALOG_NONE;
        }
        return;
    }
    if (key == '\n' || key == KEY_ENTER) {
        if (!dialog.text[0]) return;
        if (dialog.kind == DIALOG_NUMBER) {
            char *end;
            double value = strtod(dialog.text, &end);
            if (*end || !isfinite(value)) { notice(true, "Enter a finite number"); return; }
        }
        if (dialog.kind == DIALOG_SAVE) send_command("save", dialog.text, NULL, NULL, NULL);
        else if (!strcmp(dialog.command, "set")) send_command("set", dialog.section, dialog.text, NULL, NULL);
        else if (!strcmp(dialog.command, "channel")) {
            char value[TEXT + 40]; snprintf(value, sizeof(value), "%s:%s", dialog.keys[0], dialog.text);
            send_command("channel", dialog.section, dialog.identity, value, NULL);
        } else send_command(dialog.command, dialog.section, dialog.identity, dialog.text, NULL);
        dialog.kind = DIALOG_NONE;
    } else if (key == KEY_BACKSPACE || key == 127 || key == 8) {
        size_t length = strlen(dialog.text);
        if (length) dialog.text[length - 1] = 0;
        dialog.replace_input = false;
    } else if (key == 21) {
        dialog.text[0] = 0; dialog.replace_input = false;
    } else if (key >= 32 && key <= 255) {
        if (dialog.replace_input) dialog.text[0] = 0;
        dialog.replace_input = false;
        size_t length = strlen(dialog.text);
        if (length + 1 < sizeof(dialog.text)) { dialog.text[length] = (char)key; dialog.text[length + 1] = 0; }
    }
}

static void handle_key(int key)
{
    if (dialog.kind != DIALOG_NONE) { dialog_key(key); return; }
    if (key == 'q' || key == 27) { stopping = 1; return; }
    if (key >= '1' && key <= '6') { page = key - '1'; return; }
    if (key == '\t') { page = (page + 1) % 6; return; }
    if (key == KEY_BTAB) { page = (page + 5) % 6; return; }
    int indices[ROWS_MAX];
    int count = page == 5 ? current.neffects : row_indices(indices);
    if (key == KEY_UP || key == 'k') { if (selected[page] > 0) selected[page]--; }
    else if (key == KEY_DOWN || key == 'j') { if (selected[page] + 1 < count) selected[page]++; }
    else if (key == KEY_NPAGE) { selected[page] += 10; if (selected[page] >= count) selected[page] = count ? count - 1 : 0; }
    else if (key == KEY_PPAGE) { selected[page] -= 10; if (selected[page] < 0) selected[page] = 0; }
    else if (key == KEY_HOME) selected[page] = 0;
    else if (key == KEY_END) selected[page] = count ? count - 1 : 0;
    else if (key == KEY_LEFT || key == 'h' || key == '-') adjust(-1);
    else if (key == KEY_RIGHT || key == 'l' || key == '+' || key == '=') adjust(1);
    else if (key == '\n' || key == KEY_ENTER) exact_value();
    else if (key == ' ' || key == 'm') {
        if (page == 5) {
            if (current.neffects && !strcmp(current.effects[selected[page]].unit, "bool")) exact_value();
            else notice(false, "Use Left/Right or Enter to adjust this control");
        } else {
            Row *row = selected_row(); if (row && page != 4) send_command("mute", sections[page], row->id, NULL, NULL);
        }
    } else if (key == 'd') {
        Row *row = selected_row();
        if (row && (page == 1 || page == 2)) send_command("default", sections[page], row->id, NULL, NULL);
    } else if (key == 'r') choose_routes(false);
    else if (key == 'o') choose_routes(true);
    else if (key == 'P' || (key == 'p' && page == 5)) choose_presets();
    else if (key == 'p') choose_ports();
    else if (key == 'c' && page != 5) channels();
    else if (key == 'S') new_dialog(DIALOG_SAVE, "Save complete effects chain as", "save", "", "");
    else if (key == 'e') send_command("effects", NULL, NULL, NULL, NULL);
    else if (key == 'a') send_command("autostart", NULL, NULL, NULL, NULL);
    else if (key == 'b') send_command("bypass", NULL, NULL, NULL, NULL);
    else if (key == '?') new_dialog(DIALOG_HELP, "SimpleVol controls", "", "", "");
}

static int locate_helper(void)
{
    char executable[PATH_MAX];
    ssize_t length = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    if (length > 0) {
        executable[length] = 0;
        char *slash = strrchr(executable, '/');
        if (slash) {
            *slash = 0;
            int written = snprintf(helper_path, sizeof(helper_path), "%s/simplevol-audio", executable);
            if (written > 0 && (size_t)written < sizeof(helper_path) && access(helper_path, R_OK) == 0) return 1;
        }
    }
    fprintf(stderr, "simplevol: install simplevol-audio beside the simplevol executable\n");
    return 0;
}

static int start_bridge(void)
{
    int input[2], output[2];
    if (pipe(input) < 0) return 0;
    if (pipe(output) < 0) { close(input[0]); close(input[1]); return 0; }
    child = fork();
    if (child == 0) {
        dup2(input[0], STDIN_FILENO); dup2(output[1], STDOUT_FILENO);
        close(input[0]); close(input[1]); close(output[0]); close(output[1]);
        int nullfd = open("/dev/null", O_WRONLY);
        if (nullfd >= 0) { dup2(nullfd, STDERR_FILENO); close(nullfd); }
        execlp("python3", "python3", helper_path, "--bridge", (char *)NULL);
        _exit(127);
    }
    close(input[0]); close(output[1]);
    if (child < 0) { close(input[1]); close(output[0]); return 0; }
    child_in = input[1]; child_out = output[0];
    fcntl(child_in, F_SETFL, fcntl(child_in, F_GETFL) | O_NONBLOCK);
    fcntl(child_out, F_SETFL, fcntl(child_out, F_GETFL) | O_NONBLOCK);
    return 1;
}

int main(int argc, char **argv)
{
    if (!locate_helper()) return 1;
    if (argc > 1) {
        if (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h")) {
            puts("simplevol - terminal audio mixer and effects\n\n"
                 "Run without arguments for the mixer. Keys: 1-6 pages, ? help.\n"
                 "  --start / --stop          Start/stop background effects\n"
                 "  --daemon                 Run effects in the foreground\n"
                 "  --autostart on|off        Start effects at login\n"
                 "  --status / --doctor      Inspect audio and dependencies\n"
                 "  --preset NAME            Load EQ or a saved chain\n"
                 "  --save-preset NAME       Save all effect settings\n"
                 "  --set CONTROL VALUE      Set an effect control\n"
                 "  --bypass on|off           Compare processed and dry audio\n"
                 "  --version                Show version\n\n"
                 "Closing the mixer leaves background effects running.");
            return 0;
        }
        char **args = calloc((size_t)argc + 2, sizeof(*args));
        if (!args) return 1;
        args[0] = "python3"; args[1] = helper_path;
        for (int i = 1; i < argc; i++) args[i + 1] = argv[i];
        execvp(args[0], args);
        perror("simplevol: python3"); free(args); return 1;
    }
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        fprintf(stderr, "simplevol: use a terminal, or use --status / --doctor\n"); return 1;
    }
    if (!start_bridge()) { perror("simplevol: controller"); return 1; }
    signal(SIGPIPE, SIG_IGN); signal(SIGTERM, stop_signal); signal(SIGINT, stop_signal);
    setlocale(LC_ALL, "");
    initscr(); cbreak(); noecho(); keypad(stdscr, TRUE); curs_set(0);
    set_escdelay(SUI_ESCAPE_DELAY_MS);
    timeout(50);
    current.in_l = current.in_r = current.out_l = current.out_r = current.loudness = -99;
    bool dirty = true;
    int64_t redraw = 0;
    while (!stopping) {
        if (read_updates()) dirty = true;
        int key = getch();
        if (key != ERR) { if (key != KEY_RESIZE) handle_key(key); dirty = true; }
        if (dirty || sui_monotonic_ms() >= redraw) { draw(); dirty = false; redraw = sui_monotonic_ms() + 500; }
    }
    endwin(); close(child_in); close(child_out);
    int status;
    int64_t deadline = sui_monotonic_ms() + 300;
    while (waitpid(child, &status, WNOHANG) == 0 && sui_monotonic_ms() < deadline) sui_sleep_ms(10);
    if (waitpid(child, &status, WNOHANG) == 0) { kill(child, SIGTERM); waitpid(child, &status, 0); }
    return 0;
}
