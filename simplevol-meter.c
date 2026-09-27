/* SPDX-License-Identifier: GPL-3.0-or-later
 * SimpleVol control-port meter tap. No audio modification, locks, allocation,
 * file operations, or system calls in run(). The controller owns the private
 * shared-memory file. A sequence counter makes its snapshots consistent.
 *
 * The declarations below describe the public LADSPA 1.1 binary interface.
 * No implementation from another plugin is included.
 */
#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

typedef float LADSPA_Data;
typedef void *LADSPA_Handle;
typedef int LADSPA_PortDescriptor;
typedef int LADSPA_Properties;
typedef struct { int HintDescriptor; float LowerBound, UpperBound; } LADSPA_PortRangeHint;
typedef struct LADSPA_Descriptor LADSPA_Descriptor;
struct LADSPA_Descriptor {
    unsigned long UniqueID;
    const char *Label;
    LADSPA_Properties Properties;
    const char *Name, *Maker, *Copyright;
    unsigned long PortCount;
    const LADSPA_PortDescriptor *PortDescriptors;
    const char *const *PortNames;
    const LADSPA_PortRangeHint *PortRangeHints;
    void *ImplementationData;
    LADSPA_Handle (*instantiate)(const LADSPA_Descriptor *, unsigned long);
    void (*connect_port)(LADSPA_Handle, unsigned long, LADSPA_Data *);
    void (*activate)(LADSPA_Handle);
    void (*run)(LADSPA_Handle, unsigned long);
    void (*run_adding)(LADSPA_Handle, unsigned long);
    void (*set_run_adding_gain)(LADSPA_Handle, LADSPA_Data);
    void (*deactivate)(LADSPA_Handle);
    void (*cleanup)(LADSPA_Handle);
};

enum {NVALUES = 8, NPORTS = 12, MAGIC = 0x53564d31};
typedef struct {
    _Atomic uint32_t magic, sequence, values[NVALUES];
} Shared;
_Static_assert(sizeof(Shared) == 40, "Meter layout must be 40 bytes");
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "Meter needs lock-free 32-bit atomics");
typedef struct { float *ports[NPORTS]; Shared *shared; } Meter;

static LADSPA_Handle instantiate(const LADSPA_Descriptor *descriptor, unsigned long rate)
{
    (void)descriptor; (void)rate;
    Meter *meter = calloc(1, sizeof(*meter));
    if (!meter) return NULL;
    const char *path = getenv("SIMPLEVOL_METER_PATH");
    if (!path || !*path) { free(meter); return NULL; }
    int fd = open(path, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    struct stat st;
    if (fd < 0) { free(meter); return NULL; }
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != getuid() || st.st_size != (off_t)sizeof(Shared)) {
        close(fd); free(meter); return NULL;
    }
    meter->shared = mmap(NULL, sizeof(Shared), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (meter->shared == MAP_FAILED) { free(meter); return NULL; }
    atomic_store(&meter->shared->magic, MAGIC);
    return meter;
}

static void connect_port(LADSPA_Handle instance, unsigned long port, LADSPA_Data *data)
{
    Meter *meter = instance;
    if (port < NPORTS) meter->ports[port] = data;
}

static void run_meter(LADSPA_Handle instance, unsigned long samples)
{
    Meter *meter = instance;
    /* Older PipeWire hosts require audio ports, even on a control meter.
     * Pass the final stereo signal through without changing its samples. */
    for (int channel = 0; channel < 2; channel++) {
        float *input = meter->ports[NVALUES + channel];
        float *output = meter->ports[NVALUES + 2 + channel];
        if (output && output != input) {
            if (input) memcpy(output, input, samples * sizeof(float));
            else memset(output, 0, samples * sizeof(float));
        }
    }
    atomic_fetch_add_explicit(&meter->shared->sequence, 1, memory_order_seq_cst);
    for (int i = 0; i < NVALUES; i++) {
        float value = meter->ports[i] ? *meter->ports[i] : 0;
        uint32_t bits;
        if (!isfinite(value)) value = 0;
        memcpy(&bits, &value, sizeof(bits));
        atomic_store_explicit(&meter->shared->values[i], bits, memory_order_seq_cst);
    }
    atomic_fetch_add_explicit(&meter->shared->sequence, 1, memory_order_seq_cst);
}

static void cleanup(LADSPA_Handle instance)
{
    Meter *meter = instance;
    munmap(meter->shared, sizeof(Shared));
    free(meter);
}

const LADSPA_Descriptor *ladspa_descriptor(unsigned long index)
{
    static const LADSPA_PortDescriptor ports[NPORTS] = {5, 5, 5, 5, 5, 5, 5, 5, 9, 9, 10, 10};
    static const char *const names[NPORTS] = {"Input L", "Input R", "Output L", "Output R", "Compression", "Limiting", "Level Gain", "Loudness", "In L", "In R", "Out L", "Out R"};
    static const LADSPA_PortRangeHint hints[NPORTS] = {{0}};
    static const LADSPA_Descriptor descriptor = {
        .UniqueID = 0, .Label = "simplevol_meter", .Properties = 4,
        .Name = "SimpleVol meter tap", .Maker = "SimpleSuite contributors",
        .Copyright = "GPL-3.0-or-later", .PortCount = NPORTS,
        .PortDescriptors = ports, .PortNames = names, .PortRangeHints = hints,
        .instantiate = instantiate, .connect_port = connect_port,
        .run = run_meter, .cleanup = cleanup,
    };
    return index ? NULL : &descriptor;
}
