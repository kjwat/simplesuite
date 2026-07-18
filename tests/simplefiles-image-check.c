#define main simplefiles_program_main
#include "../simplefiles.c"
#undef main

#include <assert.h>

static void write_test_ppm(int fd)
{
    static const unsigned char ppm[] = {
        'P', '6', '\n', '2', ' ', '2', '\n', '2', '5', '5', '\n',
        255, 0, 0,     0, 255, 0,
        0, 0, 255,     255, 255, 255
    };
    size_t written = 0;

    while (written < sizeof(ppm)) {
        ssize_t count = write(fd, ppm + written, sizeof(ppm) - written);
        assert(count > 0);
        written += (size_t)count;
    }
}

int main(void)
{
    char path[] = "/tmp/simplefiles-image-test.XXXXXX";
    static const unsigned char red_pixel[] = {255, 0, 0, 255};
    static const unsigned char man[] = {'M', 'a', 'n'};
    char encoded[8];
    size_t encoded_size;
    ImageFileStamp stamp;
    size_t frame_size = 0;
    int fd = mkstemp(path);

    assert(fd >= 0);
    write_test_ppm(fd);
    close(fd);

    assert(path_is_image_file(path));
    assert(image_file_stamp(path, &stamp));
    assert(image_frame_size(2, 2, &frame_size));
    assert(frame_size == 2U * 2U * IMAGE_PREVIEW_CHANNELS);
    assert(!image_frame_size(0, 2, &frame_size));

    static const char da_sixel[] = "\033[?65;1;4;9c";
    static const char da_plain[] = "\033[?65;1;9c";
    static const char da_truncated[] = "\033[?4";
    char kitty_ok[64];
    int reports_sixel = 0;
    int kitty_ok_length = snprintf(kitty_ok, sizeof(kitty_ok),
                                   "\033_Gi=%u;OK\033\\",
                                   IMAGE_PREVIEW_KITTY_ID);
    assert(terminal_parse_da1(da_sixel, sizeof(da_sixel) - 1,
                              &reports_sixel));
    assert(reports_sixel);
    assert(terminal_parse_da1(da_plain, sizeof(da_plain) - 1,
                              &reports_sixel));
    assert(!reports_sixel);
    assert(!terminal_parse_da1(da_truncated,
                               sizeof(da_truncated) - 1,
                               &reports_sixel));
    assert(kitty_ok_length > 0 && kitty_ok_length < (int)sizeof(kitty_ok));
    assert(terminal_kitty_query_succeeded(kitty_ok,
                                          (size_t)kitty_ok_length));
    assert(!terminal_kitty_query_succeeded(da_plain,
                                            sizeof(da_plain) - 1));

    encoded_size = image_base64_encode(man, sizeof(man), encoded);
    encoded[encoded_size] = '\0';
    assert(strcmp(encoded, "TWFu") == 0);

    GString *sixel = build_sixel_frame(red_pixel, 1, 1);
    assert(sixel);
    assert(g_str_has_prefix(sixel->str, "\033P0;1;0q"));
    assert(g_str_has_suffix(sixel->str, "\033\\"));
    g_string_free(sixel, TRUE);

    terminal_graphics_protocol = TERMINAL_GRAPHICS_NONE;
    assert(!preview_image(NULL, path, 80, 24));
    assert(image_worker_pid == -1);

    if (ssp_command_available("ffmpeg")) {
        terminal_graphics_protocol = TERMINAL_GRAPHICS_SIXEL;
        assert(start_image_worker(path, 2, 2, &stamp));
        for (int i = 0; i < 300 && image_worker_pid > 0; i++) {
            if (check_background_image())
                break;
            usleep(10000);
        }
        assert(image_worker_pid == -1);
        assert(image_ready_pixels);
        assert(image_ready_bytes == frame_size);
        assert(strcmp(image_ready_path, path) == 0);
        for (size_t i = 3; i < image_ready_bytes;
             i += IMAGE_PREVIEW_CHANNELS)
            assert(image_ready_pixels[i] == 255);

        clear_image_preview();
        terminal_graphics_protocol = TERMINAL_GRAPHICS_KITTY;
        assert(start_image_worker(path, 2, 2, &stamp));
        for (int i = 0; i < 300 && image_worker_pid > 0; i++) {
            if (check_background_image())
                break;
            usleep(10000);
        }
        assert(image_worker_pid == -1);
        assert(image_ready_pixels);
        assert(image_ready_format == IMAGE_DATA_PNG);
        assert(image_ready_bytes >= 8);
        assert(memcmp(image_ready_pixels, "\x89PNG\r\n\x1a\n", 8) == 0);
    }

    clear_image_preview();
    terminal_graphics_protocol = TERMINAL_GRAPHICS_NONE;
    unlink(path);
    return 0;
}
