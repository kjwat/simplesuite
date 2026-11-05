/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../simplebright-backend.h"
#include "../simplebright-filter.h"
#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>

static void capabilities(const char *methods, int toggle, int temperature)
{
    char xml[2048];
    snprintf(xml, sizeof(xml), "<node><interface name='org.kde.KWin.NightLight'>%s</interface></node>", methods);
    int actual_toggle, actual_temp;
    sb_dbus_capabilities(xml, "org.kde.KWin.NightLight", &actual_toggle, &actual_temp);
    assert(actual_toggle == toggle && actual_temp == temperature);
}
int main(void)
{
    assert(sb_percent(0, 65535) == 0);
    assert(sb_percent(65535, 65535) == 100);
    assert(sb_percent(UINT_MAX, UINT_MAX) == 100);
    assert(sb_raw(1, 1) == 1);
    assert(sb_raw(0, 65535) > 0);
    assert(sb_raw(100, UINT_MAX) == UINT_MAX);
    for (unsigned max = 1; max < 10000; max += 17) {
        unsigned previous = 0;
        for (int percent = 1; percent <= 100; percent++) {
            unsigned raw = sb_raw(percent, max);
            assert(raw >= previous && raw >= 1 && raw <= max);
            previous = raw;
        }
    }
    double neutral[3], previous[3] = {0};
    sb_temperature_rgb(6500, neutral);
    for (int c = 0; c < 3; c++) assert(fabs(neutral[c] - 1) < 1e-10);
    for (int k = 1000; k <= 6500; k += 50) {
        double rgb[3]; sb_temperature_rgb(k, rgb);
        for (int c = 0; c < 3; c++) {
            assert(isfinite(rgb[c]) && rgb[c] >= previous[c] && rgb[c] <= 1);
            previous[c] = rgb[c];
        }
        assert(rgb[0] == 1); /* Temperature never changes the brightness multiplier. */
    }
    capabilities("", 0, 0);
    capabilities("<method name='inhibit'><arg type='u' direction='out'/></method><method name='uninhibit'><arg type='u' direction='in'/></method>", 1, 0);
    capabilities("<method name='preview'><arg type='u' direction='in'/></method><method name='stopPreview'/>", 0, 1);
    capabilities("<method name='preview'><arg type='s' direction='in'/></method><method name='stopPreview'/>", 0, 0);
    capabilities("<method name='inhibit'><arg type='s' direction='out'/></method><method name='uninhibit'><arg type='u' direction='in'/></method>", 0, 0);
    puts("OK brightness bounds, low-resolution devices, gamma neutrality/monotonicity, independent D-Bus capabilities");
    return 0;
}
