#import <AppKit/AppKit.h>
#include <string.h>

/* NSWorkspace reads the current desktop without AppleScript automation access. */
char *simplevis_macos_wallpaper(void) {
    @autoreleasepool {
        NSScreen *screen = NSScreen.screens.firstObject;
        NSURL *url = screen ? [NSWorkspace.sharedWorkspace desktopImageURLForScreen:screen] : nil;
        return [url isFileURL] ? strdup(url.path.fileSystemRepresentation) : NULL;
    }
}
