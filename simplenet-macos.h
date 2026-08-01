#ifndef SIMPLENET_MACOS_H
#define SIMPLENET_MACOS_H

#include <stddef.h>

typedef struct {
    char ssid[128];
    char bssid[32];
    int channel;
    int signal;
    int active;
    int secured;
    int enterprise;
    char security[96];
} SimpleNetMacAccessPoint;

enum {
    SIMPLENET_MACOS_CONNECT_FAILED = 0,
    SIMPLENET_MACOS_CONNECT_OK = 1,
    SIMPLENET_MACOS_PASSWORD_REQUIRED = 2,
    SIMPLENET_MACOS_ENTERPRISE_UNSUPPORTED = 3,
    SIMPLENET_MACOS_CONNECTED_NOT_SAVED = 4
};

int simplenet_macos_interface(char *name, size_t name_size,
                              char *ssid, size_t ssid_size,
                              char *bssid, size_t bssid_size,
                              int *powered);
int simplenet_macos_scan(SimpleNetMacAccessPoint *points, int maximum,
                         char *error, size_t error_size);
int simplenet_macos_connect(const char *ssid, const char *bssid,
                            const char *password, int use_saved_password,
                            char *error, size_t error_size);
int simplenet_macos_prefer_network(const char *ssid, const char *bssid,
                                   char *error, size_t error_size);

#endif
