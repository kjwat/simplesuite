#import <CoreWLAN/CoreWLAN.h>
#include <IOKit/ps/IOPowerSources.h>
#include <IOKit/ps/IOPSKeys.h>

#include "simplestats-macos.h"

static int percent_from_rssi(NSInteger rssi)
{
    NSInteger percent = ((rssi + 90) * 100) / 60;

    if (percent < 0)
        return 0;
    if (percent > 100)
        return 100;
    return (int)percent;
}

int simplestats_macos_battery_percent(void)
{
    CFTypeRef snapshot = IOPSCopyPowerSourcesInfo();
    CFArrayRef sources;
    int result = -1;

    if (!snapshot)
        return -1;
    sources = IOPSCopyPowerSourcesList(snapshot);
    if (!sources) {
        CFRelease(snapshot);
        return -1;
    }

    for (CFIndex i = 0; i < CFArrayGetCount(sources); i++) {
        CFTypeRef source = CFArrayGetValueAtIndex(sources, i);
        CFDictionaryRef description =
            IOPSGetPowerSourceDescription(snapshot, source);
        CFNumberRef current;
        CFNumberRef maximum;
        int current_value = 0;
        int maximum_value = 0;

        if (!description)
            continue;
        current = CFDictionaryGetValue(description, CFSTR(kIOPSCurrentCapacityKey));
        maximum = CFDictionaryGetValue(description, CFSTR(kIOPSMaxCapacityKey));
        if (current && maximum &&
            CFNumberGetValue(current, kCFNumberIntType, &current_value) &&
            CFNumberGetValue(maximum, kCFNumberIntType, &maximum_value) &&
            maximum_value > 0) {
            result = current_value * 100 / maximum_value;
            break;
        }
    }

    CFRelease(sources);
    CFRelease(snapshot);
    return result;
}

int simplestats_macos_wifi_strength(void)
{
    @autoreleasepool {
        CWInterface *interface = CWWiFiClient.sharedWiFiClient.interface;
        NSInteger rssi;

        if (!interface || interface.powerOn == NO)
            return -1;
        rssi = interface.rssiValue;
        if (rssi >= 0 || rssi < -120)
            return -1;
        return percent_from_rssi(rssi);
    }
}
