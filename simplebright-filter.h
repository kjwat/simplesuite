/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SIMPLEBRIGHT_FILTER_H
#define SIMPLEBRIGHT_FILTER_H
#include <stddef.h>
void sb_temperature_rgb(int kelvin, double rgb[3]);
int sb_filter_probe(void);
int sb_filter_open(char *error, size_t size);
int sb_filter_apply(int kelvin, char *error, size_t size);
int sb_filter_fd(void);
int sb_filter_dispatch(char *error, size_t size);
void sb_filter_close(void);
#endif
