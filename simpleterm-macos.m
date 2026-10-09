#import <AppKit/AppKit.h>
#include <gio/gio.h>
#include "simpleterm-macos.h"

void simpleterm_macos_set_icon(const char *resource_path)
{
    @autoreleasepool {
        GBytes *bytes = g_resources_lookup_data(resource_path, G_RESOURCE_LOOKUP_FLAGS_NONE, NULL);
        if (!bytes) return;
        gsize length = 0;
        const void *data = g_bytes_get_data(bytes, &length);
        NSImage *image = [[NSImage alloc] initWithData:[NSData dataWithBytes:data length:length]];
        if (image) [[NSApplication sharedApplication] setApplicationIconImage:image];
        [image release];
        g_bytes_unref(bytes);
    }
}
