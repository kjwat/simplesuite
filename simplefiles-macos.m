#import <DiskArbitration/DiskArbitration.h>
#import <Foundation/Foundation.h>
#import <IOKit/IOKitLib.h>
#import <IOKit/storage/IOMedia.h>

#include <stdio.h>
#include <string.h>

#include "simplefiles-macos.h"

static void copy_string(char *destination, size_t size, CFStringRef source)
{
    if (!destination || size == 0)
        return;
    destination[0] = '\0';
    if (source)
        (void)CFStringGetCString(source, destination, (CFIndex)size,
                                 kCFStringEncodingUTF8);
}

static int dictionary_boolean(CFDictionaryRef dictionary, CFStringRef key,
                              int fallback)
{
    CFTypeRef value = dictionary ? CFDictionaryGetValue(dictionary, key) : NULL;

    if (!value || CFGetTypeID(value) != CFBooleanGetTypeID())
        return fallback;
    return CFBooleanGetValue(value) ? 1 : 0;
}

static void copy_uuid(char *destination, size_t size, CFTypeRef value)
{
    CFStringRef text = NULL;

    if (!destination || size == 0)
        return;
    destination[0] = '\0';
    if (!value)
        return;
    if (CFGetTypeID(value) == CFStringGetTypeID()) {
        copy_string(destination, size, value);
        return;
    }
    if (CFGetTypeID(value) == CFUUIDGetTypeID())
        text = CFUUIDCreateString(kCFAllocatorDefault, value);
    if (text) {
        copy_string(destination, size, text);
        CFRelease(text);
    }
}

int simplefiles_macos_list_volumes(SimpleFilesMacVolume *volumes, int maximum)
{
    @autoreleasepool {
        DASessionRef session;
        io_iterator_t iterator = IO_OBJECT_NULL;
        kern_return_t result;
        int count = 0;

        if (!volumes || maximum <= 0)
            return 0;
        session = DASessionCreate(kCFAllocatorDefault);
        if (!session)
            return 0;
        result = IOServiceGetMatchingServices(
            kIOMainPortDefault, IOServiceMatching(kIOMediaClass), &iterator);
        if (result != KERN_SUCCESS) {
            CFRelease(session);
            return 0;
        }

        io_object_t media;

        while ((media = IOIteratorNext(iterator)) != IO_OBJECT_NULL) {
            DADiskRef disk =
                DADiskCreateFromIOMedia(kCFAllocatorDefault, session, media);
            CFDictionaryRef description =
                disk ? DADiskCopyDescription(disk) : NULL;

            if (count >= maximum) {
                if (disk)
                    CFRelease(disk);
                IOObjectRelease(media);
                break;
            }
            if (description) {
                CFStringRef bsd_name = CFDictionaryGetValue(
                    description, kDADiskDescriptionMediaBSDNameKey);
                CFStringRef volume_name = CFDictionaryGetValue(
                    description, kDADiskDescriptionVolumeNameKey);
                CFStringRef volume_kind = CFDictionaryGetValue(
                    description, kDADiskDescriptionVolumeKindKey);
                CFURLRef volume_path = CFDictionaryGetValue(
                    description, kDADiskDescriptionVolumePathKey);
                CFTypeRef volume_uuid = CFDictionaryGetValue(
                    description, kDADiskDescriptionVolumeUUIDKey);
                int internal = dictionary_boolean(
                    description, kDADiskDescriptionDeviceInternalKey, 1);
                int removable = dictionary_boolean(
                    description, kDADiskDescriptionMediaRemovableKey, 0);
                int ejectable = dictionary_boolean(
                    description, kDADiskDescriptionMediaEjectableKey, 0);
                int whole = dictionary_boolean(
                    description, kDADiskDescriptionMediaWholeKey, 0);

                if (!whole && bsd_name && (volume_name || volume_path) &&
                    (!internal || removable || ejectable)) {
                    SimpleFilesMacVolume *record = &volumes[count];
                    char bsd[PATH_MAX] = "";

                    memset(record, 0, sizeof(*record));
                    copy_string(bsd, sizeof(bsd), bsd_name);
                    copy_string(record->name, sizeof(record->name), volume_name);
                    if (!record->name[0] && volume_path) {
                        NSString *last =
                            [(NSURL *)volume_path lastPathComponent];
                        snprintf(record->name, sizeof(record->name), "%s",
                                 last.UTF8String ?: bsd);
                    }
                    snprintf(record->device, sizeof(record->device),
                             "/dev/%s", bsd);
                    snprintf(record->id, sizeof(record->id),
                             "device:%s", record->device);
                    copy_uuid(record->uuid, sizeof(record->uuid), volume_uuid);
                    if (volume_path)
                        (void)CFURLGetFileSystemRepresentation(
                            volume_path, true, (UInt8 *)record->mount_path,
                            sizeof(record->mount_path));
                    record->mounted = record->mount_path[0] != '\0';
                    record->can_mount = !record->mounted && volume_kind != NULL;
                    record->removable = 1;
                    if (record->name[0] && record->device[5] != '\0')
                        count++;
                }
                CFRelease(description);
            }
            if (disk)
                CFRelease(disk);
            IOObjectRelease(media);
        }
        IOObjectRelease(iterator);
        CFRelease(session);
        return count;
    }
}
