#import <Foundation/Foundation.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int trash_path(const char *path)
{
    @autoreleasepool {
        NSString *text = [NSString stringWithUTF8String:path ?: ""];
        NSURL *url;
        NSError *error = nil;

        if (!text.length)
            return 2;
        url = [NSURL fileURLWithPath:text];
        if ([[NSFileManager defaultManager] trashItemAtURL:url
                                          resultingItemURL:nil
                                                     error:&error])
            return 0;
        fprintf(stderr, "simplefiles: trash failed: %s\n",
                error.localizedDescription.UTF8String ?: strerror(errno));
        return 1;
    }
}

static void empty_directory(NSURL *directory, int *removed, int *failed)
{
    NSFileManager *manager = NSFileManager.defaultManager;
    NSError *listing_error = nil;
    NSArray<NSURL *> *children =
        [manager contentsOfDirectoryAtURL:directory
              includingPropertiesForKeys:nil
                                 options:0
                                   error:&listing_error];

    if (!children) {
        if (listing_error.code != NSFileReadNoSuchFileError)
            (*failed)++;
        return;
    }
    for (NSURL *child in children) {
        NSError *error = nil;

        if ([manager removeItemAtURL:child error:&error])
            (*removed)++;
        else
            (*failed)++;
    }
}

static int empty_trash(void)
{
    @autoreleasepool {
        NSFileManager *manager = NSFileManager.defaultManager;
        NSURL *home_trash = [manager.homeDirectoryForCurrentUser
            URLByAppendingPathComponent:@".Trash" isDirectory:YES];
        NSArray<NSURL *> *volumes =
            [manager mountedVolumeURLsIncludingResourceValuesForKeys:nil
                                                             options:0];
        NSString *relative =
            [NSString stringWithFormat:@".Trashes/%lu",
                                       (unsigned long)getuid()];
        int removed = 0;
        int failed = 0;

        empty_directory(home_trash, &removed, &failed);
        for (NSURL *volume in volumes) {
            if ([volume.path isEqualToString:@"/"])
                continue;
            empty_directory([volume URLByAppendingPathComponent:relative
                                                     isDirectory:YES],
                            &removed, &failed);
        }
        if (failed)
            return removed ? 2 : 1;
        return 0;
    }
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        puts("simplefiles-macos-helper 1.0.0");
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "--trash") == 0)
        return trash_path(argv[2]);
    if (argc == 2 && strcmp(argv[1], "--empty-trash") == 0)
        return empty_trash();
    fprintf(stderr,
            "usage: simplefiles-macos-helper --trash PATH | --empty-trash | --version\n");
    return 2;
}
