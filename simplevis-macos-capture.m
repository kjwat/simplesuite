#import <CoreAudio/CoreAudio.h>
#import <Foundation/Foundation.h>

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define OUTPUT_RATE 44100.0
#define OUTPUT_BUFFER_SAMPLES 4096

typedef struct {
    AudioStreamBasicDescription format;
    double source_position;
} CaptureContext;

static volatile sig_atomic_t stopped;

static AudioObjectPropertyAddress property_address(
    AudioObjectPropertySelector selector)
{
    AudioObjectPropertyAddress address = {
        selector,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    return address;
}

static void stop_capture(int signal_number)
{
    (void)signal_number;
    stopped = 1;
}

static float sample_at(const AudioBufferList *data,
                       const AudioStreamBasicDescription *format,
                       UInt32 frame)
{
    UInt32 channels = format->mChannelsPerFrame;
    int noninterleaved =
        (format->mFormatFlags & kAudioFormatFlagIsNonInterleaved) != 0;
    double sum = 0.0;
    UInt32 used = 0;

    if (!data || channels == 0)
        return 0.0f;
    if (noninterleaved) {
        UInt32 buffers = data->mNumberBuffers < channels
                           ? data->mNumberBuffers : channels;
        for (UInt32 channel = 0; channel < buffers && channel < 2; channel++) {
            const Float32 *samples = data->mBuffers[channel].mData;
            if (samples) {
                sum += samples[frame];
                used++;
            }
        }
    } else if (data->mNumberBuffers > 0 && data->mBuffers[0].mData) {
        const Float32 *samples = data->mBuffers[0].mData;
        UInt32 mixed = channels < 2 ? channels : 2;
        for (UInt32 channel = 0; channel < mixed; channel++) {
            sum += samples[(size_t)frame * channels + channel];
            used++;
        }
    }
    return used ? (float)(sum / used) : 0.0f;
}

static void write_samples(const int16_t *samples, size_t count)
{
    const unsigned char *data = (const unsigned char *)samples;
    size_t bytes = count * sizeof(*samples);

    while (bytes > 0) {
        ssize_t written = write(STDOUT_FILENO, data, bytes);

        if (written > 0) {
            data += written;
            bytes -= (size_t)written;
            continue;
        }
        if (written < 0 && errno == EINTR)
            continue;
        /*
         * Visualization is live-only. If its pipe is momentarily full, drop
         * this tail rather than blocking the Core Audio real-time thread.
         */
        break;
    }
}

static OSStatus capture_io(AudioObjectID device,
                           const AudioTimeStamp *now,
                           const AudioBufferList *input,
                           const AudioTimeStamp *input_time,
                           AudioBufferList *output,
                           const AudioTimeStamp *output_time,
                           void *client_data)
{
    CaptureContext *context = client_data;
    int16_t converted[OUTPUT_BUFFER_SAMPLES];
    size_t converted_count = 0;
    UInt32 frames;
    UInt32 channels_in_first_buffer;
    double step;

    (void)device;
    (void)now;
    (void)input_time;
    (void)output;
    (void)output_time;
    if (!context || !input || input->mNumberBuffers == 0 ||
        context->format.mBytesPerFrame == 0 ||
        !(context->format.mFormatFlags & kAudioFormatFlagIsFloat) ||
        context->format.mBitsPerChannel != 32)
        return kAudioHardwareNoError;

    channels_in_first_buffer = input->mBuffers[0].mNumberChannels;
    if (channels_in_first_buffer == 0)
        return kAudioHardwareNoError;
    frames = input->mBuffers[0].mDataByteSize /
             (channels_in_first_buffer * sizeof(Float32));
    step = context->format.mSampleRate / OUTPUT_RATE;
    if (step <= 0.0)
        return kAudioHardwareNoError;

    while (context->source_position < frames) {
        UInt32 frame = (UInt32)context->source_position;
        float value = sample_at(input, &context->format, frame);

        if (value > 1.0f)
            value = 1.0f;
        else if (value < -1.0f)
            value = -1.0f;
        converted[converted_count++] =
            (int16_t)lrintf(value * (value < 0.0f ? 32768.0f : 32767.0f));
        if (converted_count == OUTPUT_BUFFER_SAMPLES) {
            write_samples(converted, converted_count);
            converted_count = 0;
        }
        context->source_position += step;
    }
    context->source_position -= frames;
    if (converted_count)
        write_samples(converted, converted_count);
    return kAudioHardwareNoError;
}

static OSStatus get_tap_property(AudioObjectID tap,
                                 AudioObjectPropertySelector selector,
                                 void *value, UInt32 size)
{
    AudioObjectPropertyAddress address = property_address(selector);

    return AudioObjectGetPropertyData(tap, &address, 0, NULL, &size, value);
}

static OSStatus attach_tap(AudioObjectID aggregate, CFStringRef tap_uid)
{
    AudioObjectPropertyAddress address =
        property_address(kAudioAggregateDevicePropertyTapList);
    UInt32 size = 0;
    CFArrayRef current = NULL;
    CFMutableArrayRef updated;
    OSStatus status;

    status = AudioObjectGetPropertyDataSize(aggregate, &address, 0, NULL,
                                            &size);
    if (status != noErr)
        return status;
    status = AudioObjectGetPropertyData(aggregate, &address, 0, NULL,
                                        &size, &current);
    if (status != noErr)
        return status;
    updated = current
                ? CFArrayCreateMutableCopy(kCFAllocatorDefault, 0, current)
                : CFArrayCreateMutable(kCFAllocatorDefault, 0,
                                       &kCFTypeArrayCallBacks);
    if (!updated)
        return kAudioHardwareUnspecifiedError;
    if (!CFArrayContainsValue(updated, CFRangeMake(0, CFArrayGetCount(updated)),
                              tap_uid)) {
        CFArrayAppendValue(updated, tap_uid);
        size += (UInt32)sizeof(CFStringRef);
    }
    {
        CFArrayRef replacement = updated;
        status = AudioObjectSetPropertyData(aggregate, &address, 0, NULL,
                                            size, &replacement);
    }
    CFRelease(updated);
    return status;
}

static void print_status(const char *operation, OSStatus status)
{
    UInt32 code = CFSwapInt32HostToBig((UInt32)status);
    char text[5];

    memcpy(text, &code, 4);
    text[4] = '\0';
    for (int i = 0; i < 4; i++) {
        if (text[i] < 32 || text[i] > 126)
            text[i] = '?';
    }
    fprintf(stderr, "simplevis: %s failed (OSStatus %d, '%s')\n",
            operation, (int)status, text);
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        CATapDescription *description;
        AudioObjectID tap = kAudioObjectUnknown;
        AudioObjectID aggregate = kAudioObjectUnknown;
        AudioDeviceIOProcID io_proc = NULL;
        AudioStreamBasicDescription format;
        CFStringRef tap_uid = NULL;
        CaptureContext context;
        OSStatus status;
        int flags;

        if (argc == 2 && strcmp(argv[1], "--version") == 0) {
            puts("simplevis-macos-capture 1.0.0");
            return 0;
        }
        if (argc != 1) {
            fprintf(stderr, "usage: simplevis-macos-capture [--version]\n");
            return 2;
        }
        if (@available(macOS 14.2, *)) {
            description = [[CATapDescription alloc]
                initStereoGlobalTapButExcludeProcesses:@[]];
        } else {
            fprintf(stderr,
                    "simplevis: native system-audio capture requires macOS 14.2 or newer\n");
            return 2;
        }
        description.name = @"SimpleVis system audio";
        description.privateTap = YES;
        description.muteBehavior = CATapUnmuted;
        description.processRestoreEnabled = YES;

        status = AudioHardwareCreateProcessTap(description, &tap);
        if (status != noErr) {
            print_status("creating system audio tap", status);
            return 1;
        }
        memset(&format, 0, sizeof(format));
        status = get_tap_property(tap, kAudioTapPropertyFormat,
                                  &format, sizeof(format));
        if (status != noErr) {
            print_status("reading tap format", status);
            goto cleanup;
        }
        status = get_tap_property(tap, kAudioTapPropertyUID,
                                  &tap_uid, sizeof(tap_uid));
        if (status != noErr || !tap_uid) {
            print_status("reading tap identifier", status);
            goto cleanup;
        }
        {
            NSString *uid = NSUUID.UUID.UUIDString;
            NSDictionary *aggregate_description = @{
                (__bridge NSString *)kAudioAggregateDeviceNameKey:
                    @"SimpleVis private capture",
                (__bridge NSString *)kAudioAggregateDeviceUIDKey: uid,
                (__bridge NSString *)kAudioAggregateDeviceIsPrivateKey: @YES,
                (__bridge NSString *)kAudioAggregateDeviceTapAutoStartKey: @YES
            };

            status = AudioHardwareCreateAggregateDevice(
                (__bridge CFDictionaryRef)aggregate_description, &aggregate);
        }
        if (status != noErr) {
            print_status("creating private aggregate device", status);
            goto cleanup;
        }
        status = attach_tap(aggregate, tap_uid);
        if (status != noErr) {
            print_status("attaching tap to aggregate device", status);
            goto cleanup;
        }

        memset(&context, 0, sizeof(context));
        context.format = format;
        flags = fcntl(STDOUT_FILENO, F_GETFL, 0);
        if (flags >= 0)
            (void)fcntl(STDOUT_FILENO, F_SETFL, flags | O_NONBLOCK);
        signal(SIGTERM, stop_capture);
        signal(SIGINT, stop_capture);
        signal(SIGPIPE, stop_capture);

        status = AudioDeviceCreateIOProcID(aggregate, capture_io, &context,
                                           &io_proc);
        if (status != noErr) {
            print_status("creating audio callback", status);
            goto cleanup;
        }
        status = AudioDeviceStart(aggregate, io_proc);
        if (status != noErr) {
            print_status("starting system audio capture", status);
            goto cleanup;
        }
        while (!stopped)
            pause();
        status = noErr;

cleanup:
        if (aggregate != kAudioObjectUnknown && io_proc) {
            (void)AudioDeviceStop(aggregate, io_proc);
            (void)AudioDeviceDestroyIOProcID(aggregate, io_proc);
        }
        if (aggregate != kAudioObjectUnknown)
            (void)AudioHardwareDestroyAggregateDevice(aggregate);
        if (tap != kAudioObjectUnknown)
            (void)AudioHardwareDestroyProcessTap(tap);
        return status == noErr ? 0 : 1;
    }
}
