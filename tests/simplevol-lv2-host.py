"""Small offline LV2 test host. Loads the actual installed/bundled plugins.

Uses the published LV2 C ABI through ctypes; no third-party Python packages.
This is test code and is not part of the audio service.
"""
import ctypes as C
import math
import os
from pathlib import Path
import re


class Feature(C.Structure):
    _fields_ = [("URI", C.c_char_p), ("data", C.c_void_p)]


MapFunction = C.CFUNCTYPE(C.c_uint32, C.c_void_p, C.c_char_p)


class Map(C.Structure):
    _fields_ = [("handle", C.c_void_p), ("map", MapFunction)]


class Descriptor(C.Structure):
    pass


Instantiate = C.CFUNCTYPE(C.c_void_p, C.POINTER(Descriptor), C.c_double, C.c_char_p, C.POINTER(C.POINTER(Feature)))
Connect = C.CFUNCTYPE(None, C.c_void_p, C.c_uint32, C.c_void_p)
Activate = C.CFUNCTYPE(None, C.c_void_p)
Run = C.CFUNCTYPE(None, C.c_void_p, C.c_uint32)
Descriptor._fields_ = [("URI", C.c_char_p), ("instantiate", Instantiate), ("connect_port", Connect),
                      ("activate", Activate), ("run", Run), ("deactivate", Activate),
                      ("cleanup", Activate), ("extension_data", C.c_void_p)]


def plugin_directory():
    paths = os.environ.get("LV2_PATH", "").split(":") + ["/usr/lib/lv2", "/usr/local/lib/lv2", "/usr/lib64/lv2"]
    for base in paths:
        if base and (Path(base) / "lsp-plugins.lv2/compressor_stereo.ttl").is_file():
            return Path(base) / "lsp-plugins.lv2"
    raise RuntimeError("Install lsp-plugins-lv2 >= 1.2.21 to run DSP tests")


class Plugin:
    def __init__(self, name, controls=None, rate=48000, block=256):
        self.rate, self.block = rate, block
        self.directory = plugin_directory()
        self.library = C.CDLL(str(self.directory / "lsp-plugins-lv2.so"))
        self.library.lv2_descriptor.argtypes = [C.c_uint32]
        self.library.lv2_descriptor.restype = C.POINTER(Descriptor)
        for i in range(4096):
            descriptor = self.library.lv2_descriptor(i)
            if not descriptor:
                raise RuntimeError("Plugin not found: " + name)
            if descriptor.contents.URI.decode() == "http://lsp-plug.in/plugins/lv2/" + name:
                self.descriptor = descriptor
                break
        self.urids = {}
        def map_uri(_handle, uri):
            return self.urids.setdefault(uri, len(self.urids) + 1)
        self.mapper_callback = MapFunction(map_uri)
        self.mapper = Map(None, self.mapper_callback)
        self.feature = Feature(b"http://lv2plug.in/ns/ext/urid#map", C.cast(C.pointer(self.mapper), C.c_void_p))
        self.features = (C.POINTER(Feature) * 2)(C.pointer(self.feature), None)
        self.handle = descriptor.contents.instantiate(descriptor, rate, (str(self.directory) + "/").encode(), self.features)
        if not self.handle:
            raise RuntimeError("Cannot instantiate " + name)
        self.ports = {}
        self.atom_inputs, self.atom_outputs = [], []
        text = (self.directory / (name + ".ttl")).read_text()
        for part in re.split(r'(?=\n\s+a lv2:(?:Input|Output)Port)', text)[1:]:
            symbol = re.search(r'lv2:symbol\s+"([^\"]+)"', part)[1]
            index = int(re.search(r'lv2:index\s+(\d+)', part)[1])
            default = re.search(r'lv2:default\s+([-+0-9.eE]+)', part)
            if "lv2:AudioPort" in part[:100]:
                data = (C.c_float * block)()
            elif "atom:AtomPort" in part[:100]:
                data = (C.c_uint32 * 16384)()
                if "lv2:InputPort" in part[:70]:
                    self.atom_inputs.append(data)
                else:
                    self.atom_outputs.append(data)
            else:
                data = C.c_float(float(default[1]) if default else 0)
            self.ports[symbol] = data
            self.descriptor.contents.connect_port(self.handle, index, C.cast(C.pointer(data), C.c_void_p))
        self.sequence_type = map_uri(None, b"http://lv2plug.in/ns/ext/atom#Sequence")
        for key, value in (controls or {}).items():
            self.set(key, value)
        if self.descriptor.contents.activate:
            self.descriptor.contents.activate(self.handle)

    def set(self, key, value):
        self.ports[key].value = value

    def process(self, left, right=None):
        if right is None:
            right = left
        output_l, output_r = [], []
        for start in range(0, len(left), self.block):
            count = min(self.block, len(left) - start)
            for i in range(count):
                self.ports["in_l"][i] = left[start + i]
                self.ports["in_r"][i] = right[start + i]
            for data in self.atom_inputs:
                data[0], data[1], data[2], data[3] = 8, self.sequence_type, 0, 0
            for data in self.atom_outputs:
                data[0], data[1], data[2], data[3] = C.sizeof(data) - 8, self.sequence_type, 0, 0
            self.descriptor.contents.run(self.handle, count)
            output_l.extend(self.ports["out_l"][:count])
            output_r.extend(self.ports["out_r"][:count])
        return output_l, output_r

    def close(self):
        if self.handle:
            if self.descriptor.contents.deactivate:
                self.descriptor.contents.deactivate(self.handle)
            self.descriptor.contents.cleanup(self.handle)
            self.handle = None

    def __enter__(self):
        return self

    def __exit__(self, *_args):
        self.close()


def sine(seconds, amplitude, frequency=1000, rate=48000):
    return [amplitude * math.sin(2 * math.pi * frequency * i / rate) for i in range(int(seconds * rate))]


def rms(signal):
    return math.sqrt(sum(x * x for x in signal) / max(1, len(signal)))


def db(value):
    return 20 * math.log10(max(1e-10, value))
