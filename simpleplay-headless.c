#define _POSIX_C_SOURCE 200809L
#include "simpleplay-headless.h"
#include "simpleplay-engine.h"
#include "simpleplay-formats.h"
#include <gio/gunixsocketaddress.h>
#include <glib-unix.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <math.h>
#include <locale.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct Backend Backend;
typedef struct {
    char *name, *last;
    gint64 id;
} Observation;
typedef struct {
    Backend *backend;
    GSocketConnection *connection;
    GSocket *socket;
    GSource *read_source, *write_source;
    GString *input, *output;
    GPtrArray *observations;
    gboolean dead;
} Client;
struct Backend {
    GMainLoop *loop;
    SpEngine *engine;
    SpStatus status;
    SpMediaInfo info;
    GSocketService *service;
    GPtrArray *clients, *playlist;
    char *socket_path, *filename;
    dev_t socket_device;
    ino_t socket_inode;
    int current, exit_code, repeat;
    gboolean idle, quiet, loaded, finished, quitting, paused, decode_video;
    double start, end, last_volume;
};

static void backend_open(Backend *b, int index, double start);
static void event_send(Backend *b, const char *event, const char *reason, const char *error);
static void client_send(Client *c, JsonNode *node);

static JsonNode *node_string(const char *text) { return json_node_init_string(json_node_alloc(), text ? text : ""); }
static JsonNode *node_number(double value) { return json_node_init_double(json_node_alloc(), value); }
static JsonNode *node_bool(gboolean value) { return json_node_init_boolean(json_node_alloc(), value); }
static JsonNode *node_null(void) { return json_node_new(JSON_NODE_NULL); }
static JsonNode *node_object(JsonObject *object) { JsonNode *n = json_node_new(JSON_NODE_OBJECT); json_node_take_object(n, object); return n; }
static JsonNode *node_array(JsonArray *array) { JsonNode *n = json_node_new(JSON_NODE_ARRAY); json_node_take_array(n, array); return n; }

static char *serialize(JsonNode *node)
{
    JsonGenerator *generator = json_generator_new();
    json_generator_set_root(generator, node);
    char *text = json_generator_to_data(generator, NULL);
    g_object_unref(generator);
    return text;
}

static const char *arg_string(JsonArray *array, guint index)
{
    if (index >= json_array_get_length(array)) return NULL;
    JsonNode *node = json_array_get_element(array, index);
    return JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING ? json_node_get_string(node) : NULL;
}

static gboolean arg_number(JsonArray *array, guint index, double *value)
{
    if (index >= json_array_get_length(array)) return FALSE;
    JsonNode *node = json_array_get_element(array, index);
    if (!JSON_NODE_HOLDS_VALUE(node)) return FALSE;
    GType type = json_node_get_value_type(node);
    if (type != G_TYPE_DOUBLE && type != G_TYPE_INT64) return FALSE;
    *value = json_node_get_double(node);
    return isfinite(*value);
}

static gboolean arg_bool(JsonArray *array, guint index, gboolean *value)
{
    if (index >= json_array_get_length(array)) return FALSE;
    JsonNode *node = json_array_get_element(array, index);
    if (!JSON_NODE_HOLDS_VALUE(node)) return FALSE;
    if (json_node_get_value_type(node) == G_TYPE_BOOLEAN) { *value = json_node_get_boolean(node); return TRUE; }
    const char *text = arg_string(array, index);
    if (text && (!strcmp(text, "yes") || !strcmp(text, "true"))) { *value = TRUE; return TRUE; }
    if (text && (!strcmp(text, "no") || !strcmp(text, "false"))) { *value = FALSE; return TRUE; }
    return FALSE;
}

static void snapshot(Backend *b)
{
    guint revision = b->status.revision;
    sp_engine_status(b->engine, &b->status);
    if (revision != b->status.revision) sp_engine_info(b->engine, &b->info);
}

static JsonNode *property_get(Backend *b, const char *name)
{
    snapshot(b);
    SpStatus *s = &b->status;
    gboolean idle = s->state == SP_IDLE || s->state == SP_ENDED || s->state == SP_ERROR;
    if (!strcmp(name, "pause")) return node_bool(s->state == SP_PAUSED || b->paused);
    if (!strcmp(name, "idle-active") || !strcmp(name, "core-idle")) return node_bool(idle || (!strcmp(name, "core-idle") && s->state == SP_PAUSED));
    if (!strcmp(name, "eof-reached")) return node_bool(s->state == SP_ENDED);
    if (!strcmp(name, "paused-for-cache")) return node_bool(s->buffering);
    if (!strcmp(name, "time-pos") || !strcmp(name, "playback-time") || !strcmp(name, "time-pos/full")) return idle ? NULL : node_number(s->position);
    if (!strcmp(name, "duration")) return s->duration > 0 ? node_number(s->duration) : NULL;
    if (!strcmp(name, "percent-pos")) return s->duration > 0 ? node_number(100 * s->position / s->duration) : NULL;
    if (!strcmp(name, "volume")) return node_number(s->volume * 100);
    if (!strcmp(name, "mute")) return node_bool(s->volume == 0);
    if (!strcmp(name, "speed")) return node_number(s->rate);
    if (!strcmp(name, "seekable")) return node_bool(b->info.seekable);
    if (!strcmp(name, "playlist-pos")) return node_number(b->current);
    if (!strcmp(name, "playlist-count")) return node_number(b->playlist->len);
    if (!strcmp(name, "path")) return b->filename ? node_string(b->filename) : NULL;
    if (!strcmp(name, "filename")) { g_autofree char *base = b->filename ? g_path_get_basename(b->filename) : NULL; return base ? node_string(base) : NULL; }
    if (!strcmp(name, "media-title") || !strcmp(name, "metadata/icy-title") || !strcmp(name, "metadata/by-key/icy-title")) {
        if (*b->info.stream_title) return node_string(b->info.stream_title);
        if (strcmp(name, "media-title")) return NULL;
        if (*b->info.title) return node_string(b->info.title);
        g_autofree char *base = b->filename ? g_filename_display_basename(b->filename) : NULL;
        return base ? node_string(base) : NULL;
    }
    if (!strcmp(name, "audio-codec-name") || !strcmp(name, "audio-codec")) return node_string(b->info.audio_codec);
    if (!strcmp(name, "video-codec")) return node_string(b->info.video_codec);
    if (!strcmp(name, "width")) return node_number(b->info.width);
    if (!strcmp(name, "height")) return node_number(b->info.height);
    if (!strcmp(name, "metadata")) {
        JsonObject *o = json_object_new();
        json_object_set_string_member(o, "title", b->info.title);
        json_object_set_string_member(o, "artist", b->info.artist);
        json_object_set_string_member(o, "album", b->info.album);
        if (*b->info.stream_title) json_object_set_string_member(o, "icy-title", b->info.stream_title);
        return node_object(o);
    }
    if (!strcmp(name, "audio-params") || !strcmp(name, "audio-out-params")) {
        gboolean output = !strcmp(name, "audio-out-params");
        JsonObject *o = json_object_new();
        json_object_set_int_member(o, "samplerate", output ? s->output_sample_rate : b->info.sample_rate);
        json_object_set_int_member(o, "channel-count", output ? s->output_channels : b->info.channels);
        if (output) json_object_set_string_member(o, "format", "float");
        return node_object(o);
    }
    if (!strcmp(name, "playlist")) {
        JsonArray *list = json_array_new();
        for (guint n = 0; n < b->playlist->len; n++) {
            JsonObject *item = json_object_new();
            json_object_set_string_member(item, "filename", g_ptr_array_index(b->playlist, n));
            json_object_set_boolean_member(item, "current", (int)n == b->current);
            json_array_add_object_element(list, item);
        }
        return node_array(list);
    }
    if (!strcmp(name, "track-list") || !strcmp(name, "aid") || !strcmp(name, "sid")) {
        JsonArray *list = json_array_new();
        int ordinal[2] = {0, 0}, selected[2] = {-1, -1};
        for (int n = 0; n < b->info.n_tracks; n++) {
            SpTrack *t = &b->info.tracks[n];
            int id = ++ordinal[t->type];
            gboolean active = t->index == (t->type == SP_AUDIO ? s->audio_track : s->subtitle_track);
            if (active) selected[t->type] = id;
            JsonObject *item = json_object_new();
            json_object_set_int_member(item, "id", id);
            json_object_set_string_member(item, "type", t->type == SP_AUDIO ? "audio" : "sub");
            json_object_set_string_member(item, "title", t->label);
            json_object_set_boolean_member(item, "selected", active);
            json_array_add_object_element(list, item);
        }
        if (!strcmp(name, "track-list")) return node_array(list);
        json_array_unref(list);
        int id = selected[!strcmp(name, "aid") ? SP_AUDIO : SP_SUBTITLE];
        return id >= 0 ? node_number(id) : node_string("no");
    }
    return NULL;
}

static void observation_free(Observation *o) { g_free(o->name); g_free(o->last); g_free(o); }

static void observations_send(Client *c)
{
    for (guint n = 0; n < c->observations->len && !c->dead; n++) {
        Observation *o = g_ptr_array_index(c->observations, n);
        JsonNode *value = property_get(c->backend, o->name);
        if (!value) value = node_null();
        g_autofree char *serialized = serialize(value);
        if (g_strcmp0(serialized, o->last)) {
            g_free(o->last); o->last = g_strdup(serialized);
            JsonObject *event = json_object_new();
            json_object_set_string_member(event, "event", "property-change");
            json_object_set_string_member(event, "name", o->name);
            json_object_set_int_member(event, "id", o->id);
            json_object_set_member(event, "data", json_node_copy(value));
            JsonNode *node = node_object(event); client_send(c, node); json_node_free(node);
        }
        json_node_free(value);
    }
}

static const char *property_set(Backend *b, const char *name, JsonArray *args, guint index)
{
    double number;
    gboolean boolean;
    if (!strcmp(name, "volume") && arg_number(args, index, &number)) sp_engine_volume(b->engine, number / 100);
    else if (!strcmp(name, "pause") && arg_bool(args, index, &boolean)) { b->paused = boolean; sp_engine_pause(b->engine, boolean); }
    else if (!strcmp(name, "speed") && arg_number(args, index, &number) && number >= 0.5 && number <= 2) sp_engine_rate(b->engine, number);
    else if ((!strcmp(name, "time-pos") || !strcmp(name, "playback-time")) && arg_number(args, index, &number)) sp_engine_seek(b->engine, number);
    else if (!strcmp(name, "mute") && arg_bool(args, index, &boolean)) {
        if (boolean && b->status.volume > 0) { b->last_volume = b->status.volume; sp_engine_volume(b->engine, 0); }
        else if (!boolean && b->status.volume == 0) sp_engine_volume(b->engine, b->last_volume > 0 ? b->last_volume : 0.8);
    } else if (!strcmp(name, "playlist-pos") && arg_number(args, index, &number) && number >= 0 && number < b->playlist->len) backend_open(b, number, 0);
    else if (!strcmp(name, "aid") || !strcmp(name, "sid")) {
        SpTrackType type = !strcmp(name, "aid") ? SP_AUDIO : SP_SUBTITLE;
        const char *text = arg_string(args, index);
        if (type == SP_SUBTITLE && text && !strcmp(text, "no")) sp_engine_subtitle(b->engine, -1);
        else if (arg_number(args, index, &number)) {
            int ordinal = 0; gboolean found = FALSE;
            for (int n = 0; n < b->info.n_tracks; n++) {
                SpTrack *track = &b->info.tracks[n];
                if (track->type != type || ++ordinal != (int)number) continue;
                if (type == SP_AUDIO) sp_engine_audio(b->engine, track->index);
                else sp_engine_subtitle(b->engine, track->index);
                found = TRUE; break;
            }
            if (!found) return "invalid parameter";
        } else return "invalid parameter";
    } else return "property unavailable";
    return "success";
}

static void backend_open(Backend *b, int index, double start)
{
    if (index < 0 || (guint)index >= b->playlist->len) return;
    if (b->filename && !b->finished) event_send(b, "end-file", "stop", NULL);
    g_free(b->filename);
    const char *source = g_ptr_array_index(b->playlist, index);
    b->filename = g_str_has_prefix(source, "file://") ? g_filename_from_uri(source, NULL, NULL) : g_strdup(source);
    if (!b->filename) b->filename = g_strdup(source);
    b->current = index; b->loaded = b->finished = FALSE;
    memset(&b->info, 0, sizeof(b->info));
    event_send(b, "start-file", NULL, NULL);
    sp_engine_open(b->engine, b->filename, start, b->paused);
}

static JsonNode *command_run(Client *c, JsonNode *request)
{
    Backend *b = c->backend;
    const char *error = "invalid parameter";
    JsonNode *data = NULL;
    JsonObject *response = json_object_new();
    if (!JSON_NODE_HOLDS_OBJECT(request)) goto done;
    JsonObject *object = json_node_get_object(request);
    JsonNode *request_id = json_object_get_member(object, "request_id");
    if (request_id) json_object_set_member(response, "request_id", json_node_copy(request_id));
    else json_object_set_int_member(response, "request_id", 0);
    JsonNode *command = json_object_get_member(object, "command");
    if (!command || !JSON_NODE_HOLDS_ARRAY(command)) goto done;
    JsonArray *args = json_node_get_array(command);
    const char *name = arg_string(args, 0), *property = arg_string(args, 1);
    if (!name) goto done;
    snapshot(b);
    if ((!strcmp(name, "get_property") || !strcmp(name, "get_property_string")) && property) {
        data = property_get(b, property); error = data ? "success" : "property unavailable";
        if (data && !strcmp(name, "get_property_string") && (!JSON_NODE_HOLDS_VALUE(data) || json_node_get_value_type(data) != G_TYPE_STRING)) {
            g_autofree char *text = serialize(data); json_node_free(data); data = node_string(text);
        }
    } else if (!strcmp(name, "set_property") && property) error = property_set(b, property, args, 2);
    else if (!strcmp(name, "cycle") && property && (!strcmp(property, "pause") || !strcmp(property, "mute"))) {
        JsonArray *values = json_array_new();
        json_array_add_boolean_element(values, !strcmp(property, "pause") ? b->status.state != SP_PAUSED : b->status.volume != 0);
        error = property_set(b, property, values, 0); json_array_unref(values);
    } else if (!strcmp(name, "add") && property) {
        double amount;
        JsonNode *value = property_get(b, property);
        if (value && JSON_NODE_HOLDS_VALUE(value) && arg_number(args, 2, &amount) && (json_node_get_value_type(value) == G_TYPE_DOUBLE || json_node_get_value_type(value) == G_TYPE_INT64)) {
            JsonArray *values = json_array_new(); json_array_add_double_element(values, json_node_get_double(value) + amount);
            error = property_set(b, property, values, 0); json_array_unref(values);
        }
        if (value) json_node_free(value);
    } else if (!strcmp(name, "seek")) {
        double position;
        if (arg_number(args, 1, &position) && b->info.seekable) {
            const char *mode = arg_string(args, 2);
            if (mode && strstr(mode, "percent")) position *= b->status.duration / 100;
            if (!mode || !strstr(mode, "absolute")) position += b->status.position;
            sp_engine_seek(b->engine, position); error = "success";
        } else error = "property unavailable";
    } else if (!strcmp(name, "loadfile") && property) {
        const char *mode = arg_string(args, 2);
        gboolean append = mode && g_str_has_prefix(mode, "append");
        if (!append) { g_ptr_array_set_size(b->playlist, 0); b->current = -1; b->end = 0; }
        if (b->playlist->len < 4000) {
            g_ptr_array_add(b->playlist, g_strdup(property));
            if (!append || b->status.state == SP_IDLE || (mode && !strcmp(mode, "append-play"))) backend_open(b, b->playlist->len - 1, 0);
            error = "success";
        }
    } else if (!strcmp(name, "stop")) {
        if (!b->finished && b->filename) event_send(b, "end-file", "stop", NULL);
        b->finished = TRUE; sp_engine_stop(b->engine); error = "success";
    } else if (!strcmp(name, "playlist-next") || !strcmp(name, "playlist-prev")) {
        int index = b->current + (!strcmp(name, "playlist-next") ? 1 : -1);
        if (index >= 0 && (guint)index < b->playlist->len) { b->end = 0; backend_open(b, index, 0); error = "success"; }
    } else if (!strcmp(name, "playlist-clear")) {
        char *current = b->filename ? g_strdup(b->filename) : NULL;
        g_ptr_array_set_size(b->playlist, 0);
        if (current) { g_ptr_array_add(b->playlist, current); b->current = 0; } else b->current = -1;
        error = "success";
    } else if (!strcmp(name, "observe_property") && arg_string(args, 2) && c->observations->len < 100) {
        double id;
        if (arg_number(args, 1, &id) && id >= 0 && id <= G_MAXINT32 && floor(id) == id) {
            Observation *o = g_new0(Observation, 1); o->id = id; o->name = g_strdup(arg_string(args, 2));
            g_ptr_array_add(c->observations, o); error = "success";
        }
    } else if (!strcmp(name, "unobserve_property")) {
        double id;
        if (arg_number(args, 1, &id)) {
            for (guint n = 0; n < c->observations->len; ) {
                Observation *o = g_ptr_array_index(c->observations, n);
                if (o->id == (gint64)id) g_ptr_array_remove_index(c->observations, n); else n++;
            }
            error = "success";
        }
    } else if (!strcmp(name, "quit")) {
        error = "success"; b->quitting = TRUE; event_send(b, "shutdown", NULL, NULL); g_main_loop_quit(b->loop);
    } else error = "command not found";
done:
    json_object_set_string_member(response, "error", error);
    if (data) json_object_set_member(response, "data", data);
    return node_object(response);
}

static void client_flush(Client *c)
{
    while (c->output->len && !c->dead) {
        GError *error = NULL;
        gssize written = g_socket_send(c->socket, c->output->str, c->output->len, NULL, &error);
        if (written > 0) g_string_erase(c->output, 0, written);
        else {
            if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK)) c->dead = TRUE;
            g_clear_error(&error); break;
        }
    }
}

static gboolean client_write(GSocket *socket, GIOCondition condition, gpointer opaque)
{
    (void)socket; (void)condition;
    Client *c = opaque; client_flush(c);
    if (c->dead || !c->output->len) { g_source_unref(c->write_source); c->write_source = NULL; return G_SOURCE_REMOVE; }
    return G_SOURCE_CONTINUE;
}

static void client_send(Client *c, JsonNode *node)
{
    if (c->dead) return;
    g_autofree char *text = serialize(node);
    if (c->output->len + strlen(text) > 256 * 1024) { c->dead = TRUE; return; }
    g_string_append(c->output, text); g_string_append_c(c->output, '\n'); client_flush(c);
    if (c->output->len && !c->dead && !c->write_source) {
        c->write_source = g_socket_create_source(c->socket, G_IO_OUT | G_IO_ERR | G_IO_HUP, NULL);
        g_source_set_callback(c->write_source, G_SOURCE_FUNC(client_write), c, NULL);
        g_source_attach(c->write_source, NULL);
    }
}

static gboolean client_read(GSocket *socket, GIOCondition condition, gpointer opaque)
{
    Client *c = opaque;
    if (condition & G_IO_ERR) { c->dead = TRUE; return G_SOURCE_REMOVE; }
    char buffer[8192];
    int budget = 65536;
    while (!c->dead && budget > 0) {
        GError *error = NULL;
        gssize count = g_socket_receive(socket, buffer, sizeof(buffer), NULL, &error);
        if (count <= 0) {
            if (!count || !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK)) c->dead = TRUE;
            g_clear_error(&error); break;
        }
        g_string_append_len(c->input, buffer, count);
        budget -= count;
        if (c->input->len > 65536) { c->dead = TRUE; break; }
        char *newline;
        while ((newline = memchr(c->input->str, '\n', c->input->len)) && !c->dead) {
            gsize length = newline - c->input->str;
            JsonParser *parser = json_parser_new();
            if (json_parser_load_from_data(parser, c->input->str, length, NULL)) {
                JsonNode *response = command_run(c, json_parser_get_root(parser)); client_send(c, response); json_node_free(response);
                observations_send(c);
            } else {
                JsonObject *o = json_object_new(); json_object_set_string_member(o, "error", "invalid JSON");
                JsonNode *response = node_object(o); client_send(c, response); json_node_free(response);
            }
            g_object_unref(parser); g_string_erase(c->input, 0, length + 1);
            if (c->backend->quitting) break;
        }
        if (c->backend->quitting) break;
    }
    return c->dead ? G_SOURCE_REMOVE : G_SOURCE_CONTINUE;
}

static void client_free(Client *c)
{
    if (c->read_source) { g_source_destroy(c->read_source); g_source_unref(c->read_source); }
    if (c->write_source) { g_source_destroy(c->write_source); g_source_unref(c->write_source); }
    g_io_stream_close(G_IO_STREAM(c->connection), NULL, NULL); g_object_unref(c->connection);
    g_string_free(c->input, TRUE); g_string_free(c->output, TRUE); g_ptr_array_unref(c->observations); g_free(c);
}

static gboolean incoming(GSocketService *service, GSocketConnection *connection, GObject *source, Backend *b)
{
    (void)service; (void)source;
    if (b->clients->len >= 32) return FALSE;
    Client *c = g_new0(Client, 1); c->backend = b;
    c->connection = g_object_ref(connection); c->socket = g_socket_connection_get_socket(connection);
    g_socket_set_blocking(c->socket, FALSE);
    c->input = g_string_new(NULL); c->output = g_string_new(NULL);
    c->observations = g_ptr_array_new_with_free_func((GDestroyNotify)observation_free);
    c->read_source = g_socket_create_source(c->socket, G_IO_IN | G_IO_HUP | G_IO_ERR, NULL);
    g_source_set_callback(c->read_source, G_SOURCE_FUNC(client_read), c, NULL); g_source_attach(c->read_source, NULL);
    g_ptr_array_add(b->clients, c);
    return TRUE;
}

static void event_send(Backend *b, const char *event, const char *reason, const char *error)
{
    JsonObject *o = json_object_new(); json_object_set_string_member(o, "event", event);
    if (reason) json_object_set_string_member(o, "reason", reason);
    if (error && *error) json_object_set_string_member(o, "file_error", error);
    JsonNode *node = node_object(o);
    for (guint n = 0; n < b->clients->len; n++) client_send(g_ptr_array_index(b->clients, n), node);
    json_node_free(node);
}

static gboolean backend_tick(gpointer opaque)
{
    Backend *b = opaque; snapshot(b);
    SpFrame *frame = sp_engine_frame(b->engine); sp_frame_unref(frame);
    if (!b->loaded && (b->status.state == SP_PLAYING || b->status.state == SP_PAUSED)) {
        b->loaded = TRUE; event_send(b, "file-loaded", NULL, NULL);
    }
    gboolean clipped = b->end > 0 && b->status.position >= b->end;
    if (!b->finished && (clipped || b->status.state == SP_ENDED || b->status.state == SP_ERROR)) {
        gboolean failed = b->status.state == SP_ERROR;
        b->finished = TRUE; event_send(b, "end-file", failed ? "error" : "eof", failed ? b->status.error : NULL);
        if (failed && !b->quiet) g_printerr("SimplePlay: %s\n", b->status.error);
        int next = b->repeat == 1 ? b->current : b->current + 1;
        if ((guint)next >= b->playlist->len && b->repeat == 2) next = 0;
        if (next >= 0 && (guint)next < b->playlist->len) { b->end = 0; backend_open(b, next, 0); }
        else if (!b->idle) { b->exit_code = failed ? 1 : 0; g_main_loop_quit(b->loop); }
        else if (clipped) sp_engine_stop(b->engine);
    }
    for (guint n = 0; n < b->clients->len; ) {
        Client *c = g_ptr_array_index(b->clients, n);
        if (c->dead) g_ptr_array_remove_index(b->clients, n);
        else { observations_send(c); n++; }
    }
    return G_SOURCE_CONTINUE;
}

static gboolean signal_quit(gpointer opaque) { Backend *b = opaque; g_main_loop_quit(b->loop); return G_SOURCE_CONTINUE; }

static const char *option_value(int argc, char **argv, int *index, const char *name)
{
    const char *arg = argv[*index]; gsize length = strlen(name);
    if (!strncmp(arg, name, length) && arg[length] == '=') return arg + length + 1;
    if (!strcmp(arg, name) && *index + 1 < argc) return argv[++*index];
    return NULL;
}

static gboolean number_parse(const char *text, double *value)
{
    char *end; *value = g_ascii_strtod(text, &end);
    return *text && !*end && isfinite(*value);
}

static void headless_help(void)
{
    g_print("Usage: simpleplay --headless [OPTIONS] [FILE/URL…]\n"
        "       simpleplay-core [OPTIONS] [FILE/URL…]\n\n"
        "Audio playback with no GTK initialization, display server, or session bus.\n"
        "  --input-ipc-server=PATH   Private Unix socket, newline-delimited mpv-style JSON\n"
        "  --idle[=yes/no]           Stay alive to receive loadfile commands\n"
        "  --volume=0…130           Initial volume (default 80)\n"
        "  --start=SECONDS          Start position, also usable for CUE tracks\n"
        "  --end=SECONDS            Stop at an absolute media position\n"
        "  --pause                  Start paused\n"
        "  --vo=null                Decode and discard video (default: audio only)\n"
        "  --ao=null                Decode without an audio output device\n"
        "  --network-timeout=N      Read timeout in seconds (default 15)\n"
        "  --input-format=NAME      Force an FFmpeg demuxer, including raw PCM\n"
        "  --sample-rate=N --channels=N  Raw audio parameters (default 48000 / 2)\n"
        "  --quiet                  Suppress playback errors on stderr\n"
        "  --formats / --check-formats\n"
        "  --help / --version\n\n"
        "Commands: loadfile, get_property, get_property_string, set_property, seek,\n"
        "cycle pause/mute, add, observe_property, unobserve_property, playlist-next,\n"
        "playlist-prev, playlist-clear, stop, quit. See SIMPLEPLAY.md for the API.\n");
}

int sp_headless_main(int argc, char **argv)
{
    setlocale(LC_ALL, "");
    Backend b = {.current = -1};
    b.playlist = g_ptr_array_new_with_free_func(g_free);
    b.clients = g_ptr_array_new_with_free_func((GDestroyNotify)client_free);
    double volume = 80, timeout = 15;
    double sample_rate = 48000, channels = 2;
    char *input_format = NULL;
    gboolean idle_explicit = FALSE;
    int result = 0;
    for (int n = 1; n < argc; n++) {
        const char *arg = argv[n], *value;
        if (!strcmp(arg, "--")) { while (++n < argc) g_ptr_array_add(b.playlist, g_strdup(argv[n])); break; }
        if (*arg != '-') g_ptr_array_add(b.playlist, g_strdup(arg));
        else if (!strcmp(arg, "--help") || !strcmp(arg, "-h")) { headless_help(); goto cleanup; }
        else if (!strcmp(arg, "--version")) { g_print("SimplePlay core 0.1.0 · FFmpeg\n"); goto cleanup; }
        else if (!strcmp(arg, "--formats") || !strcmp(arg, "--check-formats")) {
            gboolean complete; g_autofree char *report = sp_formats_report(!strcmp(arg, "--formats"), &complete);
            g_print("%s", report); result = !strcmp(arg, "--check-formats") && !complete ? 1 : 0; goto cleanup;
        } else if ((value = option_value(argc, argv, &n, "--input-ipc-server"))) b.socket_path = g_canonicalize_filename(value, NULL);
        else if ((value = option_value(argc, argv, &n, "--volume"))) { if (!number_parse(value, &volume) || volume < 0 || volume > 130) goto invalid; }
        else if ((value = option_value(argc, argv, &n, "--start"))) { if (!number_parse(value, &b.start) || b.start < 0) goto invalid; }
        else if ((value = option_value(argc, argv, &n, "--end"))) { if (!number_parse(value, &b.end) || b.end < 0) goto invalid; }
        else if ((value = option_value(argc, argv, &n, "--network-timeout"))) { if (!number_parse(value, &timeout) || timeout < 1 || timeout > 3600) goto invalid; }
        else if ((value = option_value(argc, argv, &n, "--input-format"))) { g_free(input_format); input_format = g_strdup(value); }
        else if ((value = option_value(argc, argv, &n, "--sample-rate"))) { if (!number_parse(value, &sample_rate) || sample_rate < 1000 || sample_rate > 768000) goto invalid; }
        else if ((value = option_value(argc, argv, &n, "--channels"))) { if (!number_parse(value, &channels) || channels < 1 || channels > 32) goto invalid; }
        else if (!strcmp(arg, "--idle") || !strcmp(arg, "--idle=yes")) { b.idle = TRUE; idle_explicit = TRUE; }
        else if (!strcmp(arg, "--idle=no")) { b.idle = FALSE; idle_explicit = TRUE; }
        else if (!strcmp(arg, "--keep-open=yes")) b.idle = TRUE;
        else if (!strcmp(arg, "--pause") || !strcmp(arg, "--pause=yes")) b.paused = TRUE;
        else if (!strcmp(arg, "--vo=null")) b.decode_video = TRUE;
        else if (!strcmp(arg, "--ao=null")) g_setenv("SDL_AUDIODRIVER", "dummy", TRUE);
        else if (!strcmp(arg, "--quiet") || !strcmp(arg, "--really-quiet") || !strcmp(arg, "-q")) b.quiet = TRUE;
        else if (!strcmp(arg, "--loop-file=inf") || !strcmp(arg, "--loop-file=yes")) b.repeat = 1;
        else if (!strcmp(arg, "--loop-playlist=inf") || !strcmp(arg, "--loop-playlist=yes")) b.repeat = 2;
        else if (!strcmp(arg, "--headless") || !strcmp(arg, "--no-video") || !strcmp(arg, "--vid=no") ||
                 !strcmp(arg, "--no-audio-display") || !strcmp(arg, "--audio-display=no") || !strcmp(arg, "--force-window=no") ||
                 !strcmp(arg, "--no-terminal") || !strcmp(arg, "--terminal=no") || !strcmp(arg, "--pause=no") ||
                 !strcmp(arg, "--no-config") || !strcmp(arg, "--keep-open=no") ||
                 !strcmp(arg, "--cache=yes") || !strcmp(arg, "--cache=no")) { /* Accepted frontend compatibility flags. */ }
        else { invalid: g_printerr("SimplePlay: invalid option: %s\n", arg); result = 2; goto cleanup; }
    }
    if (!b.playlist->len && b.socket_path && !idle_explicit) b.idle = TRUE;
    if ((!b.playlist->len && !b.idle) || (b.end && b.end <= b.start)) { headless_help(); result = 2; goto cleanup; }
    signal(SIGPIPE, SIG_IGN);
    b.loop = g_main_loop_new(NULL, FALSE);
    b.engine = sp_engine_new(); sp_engine_video_enabled(b.engine, b.decode_video); sp_engine_volume(b.engine, volume / 100); sp_engine_timeout(b.engine, timeout);
    if (input_format) sp_engine_input_format(b.engine, input_format, sample_rate, channels);
    if (b.socket_path) {
        GError *error = NULL;
        GSocketAddress *address = g_unix_socket_address_new(b.socket_path);
        b.service = g_socket_service_new();
        mode_t previous_mask = umask(0077);
        gboolean bound = g_socket_listener_add_address(G_SOCKET_LISTENER(b.service), address, G_SOCKET_TYPE_STREAM,
                                                       G_SOCKET_PROTOCOL_DEFAULT, NULL, NULL, &error);
        umask(previous_mask); g_object_unref(address);
        if (!bound) { g_printerr("SimplePlay IPC: %s\n", error->message); g_error_free(error); result = 1; goto cleanup; }
        struct stat info;
        if (lstat(b.socket_path, &info) == 0) { b.socket_device = info.st_dev; b.socket_inode = info.st_ino; }
        if (g_chmod(b.socket_path, 0600) < 0) { g_printerr("SimplePlay IPC: cannot make the socket private\n"); result = 1; goto cleanup; }
        g_signal_connect(b.service, "incoming", G_CALLBACK(incoming), &b); g_socket_service_start(b.service);
    }
    guint timer = g_timeout_add(25, backend_tick, &b);
    guint term = g_unix_signal_add(SIGTERM, signal_quit, &b), interrupt = g_unix_signal_add(SIGINT, signal_quit, &b);
    if (b.playlist->len) backend_open(&b, 0, b.start);
    g_main_loop_run(b.loop);
    g_source_remove(timer); g_source_remove(term); g_source_remove(interrupt);
    result = b.exit_code;
cleanup:
    if (b.service) { g_socket_service_stop(b.service); g_object_unref(b.service); }
    g_ptr_array_unref(b.clients);
    sp_engine_free(b.engine);
    if (b.loop) g_main_loop_unref(b.loop);
    if (b.socket_inode) {
        struct stat info;
        if (lstat(b.socket_path, &info) == 0 && info.st_ino == b.socket_inode && info.st_dev == b.socket_device && S_ISSOCK(info.st_mode)) g_unlink(b.socket_path);
    }
    g_ptr_array_unref(b.playlist); g_free(b.filename); g_free(b.socket_path);
    g_free(input_format);
    return result;
}
