#ifndef SIMPLEVIS_WALLPAPER_H
#define SIMPLEVIS_WALLPAPER_H

typedef struct {
    double r, g, b;
} RGBColor;

/* Nonblocking: desktop discovery and image decoding run on one worker. */
int simplevis_wallpaper_color(RGBColor *color);
/* Prime the first result before curses paints; later reads remain nonblocking. */
int simplevis_wallpaper_prepare(int timeout_ms);
/* -1 while the first query is pending, 0 unavailable, 1 available. */
int simplevis_wallpaper_status(RGBColor *color);
void simplevis_wallpaper_shutdown(void);

/* The original SimpleWM palette reader, also used by the desktop worker. */
int simplevis_simplewm_accent(RGBColor *color);

#endif
