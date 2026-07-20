#define SIMPLEWORDS_TYPEWRITER_TEST
#define main simplewords_program_main
#include "../simplewords.c"
#undef main

#include <assert.h>

static const char *const bundled_sounds[TYPEWRITER_SOUND_COUNT] = {
    "assets/simplewords-typewriter.wav",
    "assets/simplewords-typewriter-alt.wav",
    "assets/simplewords-typewriter-space.wav",
    "assets/simplewords-typewriter-enter.wav",
    "assets/simplewords-typewriter-delete.wav"
};

static void write_text_file(const char *path, const char *text)
{
    FILE *fp = fopen(path, "w");

    assert(fp);
    assert(fputs(text, fp) >= 0);
    assert(fclose(fp) == 0);
}

static void reset_test_document(void)
{
    for (int i = 0; i < line_count; i++) {
        free(lines[i]);
        lines[i] = NULL;
    }
    clear_undo_history();

    line_count = 1;
    lines[0] = new_line("");
    cy = 0;
    cx = 0;
    top = 0;
    dirty = 0;
    autosave_dirty = 0;
    selecting = 0;
    goal_col = -1;
    invalidate_wrap_cache();
}

static unsigned int request_total(void)
{
    unsigned int total = 0;

    for (unsigned int i = 0; i < TYPEWRITER_SOUND_COUNT; i++)
        total += typewriter_audio_test_requests[i];
    return total;
}

static void reset_test_requests(void)
{
    memset(typewriter_audio_test_requests, 0,
           sizeof(typewriter_audio_test_requests));
}

static int audio_samples_are_clear(void)
{
    for (unsigned int i = 0; i < TYPEWRITER_SOUND_COUNT; i++) {
        if (typewriter_audio.samples[i].pcm_frames ||
            typewriter_audio.samples[i].frame_count != 0)
            return 0;
    }
    return 1;
}

static void assert_bundled_samples_decode_at_native_rate(void)
{
    for (unsigned int i = 0; i < TYPEWRITER_SOUND_COUNT; i++) {
        ma_decoder_config decoder_config =
            ma_decoder_config_init(ma_format_f32,
                                   TYPEWRITER_AUDIO_CHANNELS, 0);
        ma_uint64 frame_count = 0;
        void *frames = NULL;

        assert(ma_decode_file(bundled_sounds[i], &decoder_config,
                              &frame_count, &frames) == MA_SUCCESS);
        assert(frames != NULL);
        assert(frame_count > 0);
        assert(decoder_config.format == ma_format_f32);
        assert(decoder_config.channels == TYPEWRITER_AUDIO_CHANNELS);
        assert(decoder_config.sampleRate == 44100);
        ma_free(frames, NULL);
    }
}

static void assert_different_samples_mix_with_overlapping_tails(void)
{
    TypewriterAudio audio = {0};
    ma_device device = {0};
    float key[] = {
        0.5f, -0.5f,
        0.25f, -0.25f,
        0.125f, -0.125f
    };
    float alt[] = {
        0.25f, 0.25f,
        0.125f, 0.125f,
        0.0625f, 0.0625f
    };
    float output[4];
    float tail[2];

    audio.samples[TYPEWRITER_SOUND_KEY].pcm_frames = key;
    audio.samples[TYPEWRITER_SOUND_KEY].frame_count = 3;
    audio.samples[TYPEWRITER_SOUND_KEY_ALT].pcm_frames = alt;
    audio.samples[TYPEWRITER_SOUND_KEY_ALT].frame_count = 3;
    audio.volume = 0.5f;
    audio.event_queue[0] = TYPEWRITER_SOUND_KEY;
    audio.event_queue[1] = TYPEWRITER_SOUND_KEY_ALT;
    atomic_init(&audio.event_head, 2);
    atomic_init(&audio.event_tail, 0);
    device.pUserData = &audio;

    typewriter_audio_callback(&device, output, NULL, 2);
    assert(output[0] == 0.375f);
    assert(output[1] == -0.125f);
    assert(output[2] == 0.1875f);
    assert(output[3] == -0.0625f);
    assert(atomic_load(&audio.event_tail) == 2);

    typewriter_audio_callback(&device, tail, NULL, 1);
    assert(tail[0] == 0.09375f);
    assert(tail[1] == -0.03125f);
}

static void assert_event_queue_drops_when_full(void)
{
    float frame[] = {0.0f, 0.0f};

    memset(&typewriter_audio, 0, sizeof(typewriter_audio));
    atomic_init(&typewriter_audio.event_head, 0);
    atomic_init(&typewriter_audio.event_tail, 0);
    typewriter_audio.samples[TYPEWRITER_SOUND_KEY].pcm_frames = frame;
    typewriter_audio.samples[TYPEWRITER_SOUND_KEY].frame_count = 1;
    typewriter_audio.device_initialized = 1;
    config.typewriter_sound = 1;

    for (unsigned int i = 0; i < TYPEWRITER_AUDIO_QUEUE_SIZE - 1; i++)
        assert(request_typewriter_sound(TYPEWRITER_SOUND_KEY));
    assert(!request_typewriter_sound(TYPEWRITER_SOUND_KEY));

    typewriter_audio.device_initialized = 0;
    typewriter_audio.samples[TYPEWRITER_SOUND_KEY].pcm_frames = NULL;
    typewriter_audio.samples[TYPEWRITER_SOUND_KEY].frame_count = 0;
    clear_typewriter_audio_samples();
}

static void assert_partial_scheme_fallback(void)
{
    TypewriterAudio audio = {0};
    float frame[] = {0.0f, 0.0f};
    const TypewriterSample *main_sample;

    audio.samples[TYPEWRITER_SOUND_KEY].pcm_frames = frame;
    audio.samples[TYPEWRITER_SOUND_KEY].frame_count = 1;
    main_sample = &audio.samples[TYPEWRITER_SOUND_KEY];

    assert(typewriter_sample_for_sound(&audio, TYPEWRITER_SOUND_KEY_ALT) ==
           main_sample);
    assert(typewriter_sample_for_sound(&audio, TYPEWRITER_SOUND_DELETE) ==
           main_sample);
    assert(typewriter_sample_for_sound(&audio, TYPEWRITER_SOUND_SPACE) ==
           NULL);
    assert(typewriter_sample_for_sound(&audio, TYPEWRITER_SOUND_ENTER) ==
           NULL);
}

static void assert_keyboard_event_routing(void)
{
    const char *alternate_keys = "aAeEiInNoOsStTuU";
    unsigned int before;

    typewriter_audio_test_mode = 1;
    reset_test_requests();
    config.typewriter_sound = 1;
    reset_test_document();

    assert(insert_printable_key('b'));
    assert(typewriter_audio_test_requests[TYPEWRITER_SOUND_KEY] == 1);
    assert(request_total() == 1);

    for (const char *p = alternate_keys; *p; p++)
        assert(insert_printable_key((unsigned char)*p));
    assert(typewriter_audio_test_requests[TYPEWRITER_SOUND_KEY_ALT] == 16);

    assert(insert_printable_key(' '));
    assert(typewriter_audio_test_requests[TYPEWRITER_SOUND_SPACE] == 1);
    assert(!insert_printable_key(KEY_LEFT));

    before = request_total();
    move_left(0);                         /* navigation */
    assert(request_total() == before);
    assert(keyboard_delete_forward());
    assert(typewriter_audio_test_requests[TYPEWRITER_SOUND_DELETE] == 1);
    assert(keyboard_backspace());
    assert(typewriter_audio_test_requests[TYPEWRITER_SOUND_DELETE] == 2);
    assert(keyboard_newline());
    assert(typewriter_audio_test_requests[TYPEWRITER_SOUND_ENTER] == 1);
    assert(keyboard_tab());
    assert(typewriter_audio_test_requests[TYPEWRITER_SOUND_KEY] == 2);

    before = request_total();
    (void)insert_char('g');               /* program-generated text */
    (void)newline();                      /* program-generated newline */

    free(clip);
    clip = xstrdup_local("pasted text\nwith a newline");
    assert(unsetenv("WAYLAND_DISPLAY") == 0);
    assert(unsetenv("DISPLAY") == 0);
    paste_clipboard();                    /* paste */
    do_undo();                            /* command */
    assert(request_total() == before);

    reset_test_document();
    before = request_total();
    assert(!keyboard_backspace());        /* no actual deletion */
    assert(!keyboard_delete_forward());   /* no actual deletion */
    assert(request_total() == before);

    memset(lines[0], 'x', MAX_LINE - 1);
    lines[0][MAX_LINE - 1] = '\0';
    cy = 0;
    cx = MAX_LINE - 1;
    assert(!insert_printable_key('z'));   /* failed insertion */
    assert(request_total() == before);

    typewriter_audio_test_mode = 0;
}

static void assert_multiline_paste_is_one_edit(void)
{
    char *text;
    size_t capacity = 32768;
    size_t length = 0;

    text = malloc(capacity);
    assert(text);
    for (int i = 0; i < 500; i++) {
        int written = snprintf(text + length, capacity - length,
                               "Gutenberg line %d\r\n", i);

        assert(written > 0);
        assert((size_t)written < capacity - length);
        length += (size_t)written;
    }

    reset_test_document();
    strcpy(lines[0], "before after");
    cy = 0;
    cx = 7;

    assert(insert_pasted_text(text));
    assert(line_count == 501);
    assert(strcmp(lines[0], "before Gutenberg line 0") == 0);
    assert(strcmp(lines[499], "Gutenberg line 499") == 0);
    assert(strcmp(lines[500], "after") == 0);
    assert(undo_count == 1);
    assert(undo_stack[0].op_count == 1);

    do_undo();
    assert(line_count == 1);
    assert(strcmp(lines[0], "before after") == 0);
    assert(cy == 0);
    assert(cx == 7);
    free(text);
}

static void assert_bracketed_paste_capture(void)
{
    static const char terminal_input[] =
        "\033[200~first line\r\nsecond line\033[201~q";
    FILE *input = tmpfile();
    FILE *output = tmpfile();
    SCREEN *screen;

    assert(input);
    assert(output);
    assert(fwrite(terminal_input, 1, sizeof(terminal_input) - 1, input) ==
           sizeof(terminal_input) - 1);
    rewind(input);
    assert(setenv("TERM", "xterm-256color", 1) == 0);

    screen = newterm(NULL, output, input);
    assert(screen);
    set_term(screen);
    raw();
    noecho();
    nonl();
    keypad(stdscr, TRUE);
    timeout(250);

    assert(read_editor_key() == KEY_BRACKETED_PASTE);
    assert(pending_bracketed_paste);
    assert(strcmp(pending_bracketed_paste,
                  "first line\r\nsecond line") == 0);
    assert(read_editor_key() == 'q');

    free(pending_bracketed_paste);
    pending_bracketed_paste = NULL;
    endwin();
    delscreen(screen);
    fclose(output);
    fclose(input);
}

int main(void)
{
    char home[] = "/tmp/simplewords-typewriter-test.XXXXXX";
    char config_dir[PATH_MAX];
    char app_config_dir[PATH_MAX];
    char config_path[PATH_MAX];
    char sound_path[PATH_MAX];
    char missing_path[PATH_MAX];
    char expected[PATH_MAX];
    char *saved_home = getenv("HOME") ? strdup(getenv("HOME")) : NULL;

    assert(mkdtemp(home));
    assert(snprintf(config_dir, sizeof(config_dir), "%s/.config", home) > 0);
    assert(snprintf(app_config_dir, sizeof(app_config_dir),
                    "%s/simplewords", config_dir) > 0);
    assert(snprintf(config_path, sizeof(config_path), "%s/config",
                    app_config_dir) > 0);
    assert(snprintf(sound_path, sizeof(sound_path), "%s/key.wav", home) > 0);
    assert(snprintf(missing_path, sizeof(missing_path), "%s/missing.wav",
                    home) > 0);
    assert(mkdir(config_dir, 0700) == 0);
    assert(mkdir(app_config_dir, 0700) == 0);
    assert(setenv("HOME", home, 1) == 0);

    /* A missing config preserves the disabled defaults. */
    load_simplewords_config();
    assert(config.typewriter_sound == 0);
    assert(config.typewriter_sound_volume == 70);
    assert(snprintf(expected, sizeof(expected),
                    "%s/.local/share/simplesuite/simplewords-typewriter.wav",
                    home) > 0);
    assert(strcmp(config.typewriter_sound_file, expected) == 0);
    assert(snprintf(expected, sizeof(expected),
                    "%s/.local/share/simplesuite/simplewords-typewriter-alt.wav",
                    home) > 0);
    assert(strcmp(config.typewriter_sound_alt_file, expected) == 0);
    assert(snprintf(expected, sizeof(expected),
                    "%s/.local/share/simplesuite/simplewords-typewriter-space.wav",
                    home) > 0);
    assert(strcmp(config.typewriter_sound_space_file, expected) == 0);
    assert(snprintf(expected, sizeof(expected),
                    "%s/.local/share/simplesuite/simplewords-typewriter-enter.wav",
                    home) > 0);
    assert(strcmp(config.typewriter_sound_enter_file, expected) == 0);
    assert(snprintf(expected, sizeof(expected),
                    "%s/.local/share/simplesuite/simplewords-typewriter-delete.wav",
                    home) > 0);
    assert(strcmp(config.typewriter_sound_delete_file, expected) == 0);

    write_text_file(config_path,
                    "typewriter_sound=true\n"
                    "typewriter_sound_file=$HOME/key.wav\n"
                    "typewriter_sound_alt_file=~/alt.wav\n"
                    "typewriter_sound_space_file=$HOME/space.wav\n"
                    "typewriter_sound_enter_file=~/enter.wav\n"
                    "typewriter_sound_delete_file=$HOME/delete.wav\n"
                    "typewriter_sound_volume=800\n");
    load_simplewords_config();
    assert(config.typewriter_sound == 1);
    assert(config.typewriter_sound_volume == 100);
    assert(strcmp(config.typewriter_sound_file, sound_path) == 0);
    assert(snprintf(expected, sizeof(expected), "%s/alt.wav", home) > 0);
    assert(strcmp(config.typewriter_sound_alt_file, expected) == 0);
    assert(snprintf(expected, sizeof(expected), "%s/space.wav", home) > 0);
    assert(strcmp(config.typewriter_sound_space_file, expected) == 0);
    assert(snprintf(expected, sizeof(expected), "%s/enter.wav", home) > 0);
    assert(strcmp(config.typewriter_sound_enter_file, expected) == 0);
    assert(snprintf(expected, sizeof(expected), "%s/delete.wav", home) > 0);
    assert(strcmp(config.typewriter_sound_delete_file, expected) == 0);

    write_text_file(config_path,
                    "typewriter_sound=true\n"
                    "typewriter_sound_file=~/key.wav\n"
                    "typewriter_sound_volume=-20\n");
    load_simplewords_config();
    assert(strcmp(config.typewriter_sound_file, sound_path) == 0);
    assert(config.typewriter_sound_volume == 0);

    /* Runtime toggles persist without disturbing the other settings. */
    config.typewriter_sound = 0;
    assert(save_typewriter_sound_setting());
    load_simplewords_config();
    assert(config.typewriter_sound == 0);
    assert(config.typewriter_sound_volume == 0);
    assert(strcmp(config.typewriter_sound_file, sound_path) == 0);

    /* Disabled mode allocates no samples and opens no audio device. */
    config.typewriter_sound = 0;
    assert(start_typewriter_audio() == 0);
    assert(typewriter_audio.device_initialized == 0);
    assert(audio_samples_are_clear());

    /* Missing and invalid main WAV files remain silent no-ops. */
    config.typewriter_sound = 1;
    snprintf(config.typewriter_sound_file,
             sizeof(config.typewriter_sound_file), "%s", missing_path);
    assert(start_typewriter_audio() == 0);
    assert(typewriter_audio.device_initialized == 0);
    assert(audio_samples_are_clear());

    write_text_file(sound_path, "not a WAV\n");
    snprintf(config.typewriter_sound_file,
             sizeof(config.typewriter_sound_file), "%s", sound_path);
    assert(start_typewriter_audio() == 0);
    assert(typewriter_audio.device_initialized == 0);
    assert(audio_samples_are_clear());

    assert_bundled_samples_decode_at_native_rate();
    assert_different_samples_mix_with_overlapping_tails();
    assert_partial_scheme_fallback();
    assert_event_queue_drops_when_full();
    assert_keyboard_event_routing();
    assert_multiline_paste_is_one_edit();
    assert_bracketed_paste_capture();

    stop_typewriter_audio();
    reset_test_document();
    free(lines[0]);
    lines[0] = NULL;
    free(clip);
    clip = NULL;

    unlink(config_path);
    unlink(sound_path);
    rmdir(app_config_dir);
    rmdir(config_dir);
    rmdir(home);

    if (saved_home) {
        assert(setenv("HOME", saved_home, 1) == 0);
        free(saved_home);
    } else {
        assert(unsetenv("HOME") == 0);
    }
    return 0;
}
