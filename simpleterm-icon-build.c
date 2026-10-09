/* Build a macOS ICNS from the one source icon using existing GTK libraries. */
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <string.h>

static const struct {
    const char *type;
    int size;
} icns_entries[] = {
    {"icp4", 16}, {"icp5", 32}, {"icp6", 64},
    {"ic07", 128}, {"ic08", 256}, {"ic09", 512}, {"ic10", 1024},
    {"ic11", 32}, {"ic12", 64}, {"ic13", 256}, {"ic14", 512}
};

int main(int argc, char **argv)
{
    if (argc != 3) {
        g_printerr("Usage: %s SOURCE.png OUTPUT.icns\n", argv[0]);
        return 1;
    }
    GError *error = NULL;
    int status = 0;
    GdkPixbuf *source = gdk_pixbuf_new_from_file(argv[1], &error);
    if (!source) {
        g_printerr("Simpleterm icon: %s\n", error->message);
        g_error_free(error);
        return 1;
    }
    GByteArray *icns = g_byte_array_new();
    g_byte_array_append(icns, (const guint8 *)"icns\0\0\0\0", 8);
    for (gsize i = 0; i < G_N_ELEMENTS(icns_entries); ++i) {
        int size = icns_entries[i].size;
        GdkPixbuf *scaled = gdk_pixbuf_scale_simple(source, size, size, GDK_INTERP_HYPER);
        char *png = NULL;
        gsize length = 0;
        gboolean saved = gdk_pixbuf_save_to_buffer(scaled, &png, &length,
            "png", &error, "compression", "9", NULL);
        g_object_unref(scaled);
        if (!saved) goto out;
        guint32 chunk_length = GUINT32_TO_BE((guint32)length + 8);
        g_byte_array_append(icns, (const guint8 *)icns_entries[i].type, 4);
        g_byte_array_append(icns, (const guint8 *)&chunk_length, 4);
        g_byte_array_append(icns, (const guint8 *)png, length);
        g_free(png);
    }
    guint32 file_length = GUINT32_TO_BE(icns->len);
    memcpy(icns->data + 4, &file_length, 4);
    g_file_set_contents(argv[2], (const char *)icns->data, icns->len, &error);

out:
    if (error) {
        status = 1;
        g_printerr("Simpleterm icon: %s\n", error->message);
        g_error_free(error);
    }
    g_byte_array_unref(icns);
    g_object_unref(source);
    return status;
}
