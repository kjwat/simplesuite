/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SIMPLEBRIGHT_BACKEND_H
#define SIMPLEBRIGHT_BACKEND_H
#include <stddef.h>
#define SB_DEVICES 32
#define SB_TEXT 512
typedef struct {
    char id[160], name[256], backend[32];
    unsigned current, maximum;
    int bus, percent;
} SbDevice;
typedef struct {
    int enabled, kelvin, can_toggle, can_temperature;
    char backend[80], error[SB_TEXT];
} SbNight;
int sb_scan(SbDevice devices[SB_DEVICES], char *error, size_t size);
int sb_read(SbDevice *device, char *error, size_t size);
int sb_set(SbDevice *device, int percent, char *error, size_t size);
int sb_night_request(int command, int kelvin, SbNight *night);
int sb_night_daemon(void);
int sb_percent(unsigned value, unsigned maximum);
unsigned sb_raw(int percent, unsigned maximum);
int sb_saved_temperature(void);
void sb_save_temperature(int kelvin);
/* Capability checks are separate for toggle and temperature. */
void sb_dbus_capabilities(const char *xml, const char *interface, int *toggle, int *temperature);
#endif
