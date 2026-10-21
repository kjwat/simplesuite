#ifndef SIMPLEPDF_STATE_H
#define SIMPLEPDF_STATE_H
#include <glib.h>
typedef struct {
    int page, rotation, sizing, width, height;
    double scale, x, y;
} PdfPosition;
gboolean pdf_state_load(const char *uri, PdfPosition *position);
gboolean pdf_state_save(const char *uri, const PdfPosition *position, GError **error);
#endif
