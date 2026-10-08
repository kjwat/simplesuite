#!/usr/bin/env python3
"""Exercise the real libnm/curses binary on a private, simulated system bus.

No host network devices, connections, credentials, or service state are used.
Requires dbus-run-session, python3-dbus, and python3-gi.
"""
import copy
import fcntl
import os
import pty
import struct
import subprocess
import sys
import termios
import time
import uuid

import dbus
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib

NM = "org.freedesktop.NetworkManager"
ROOT = "/org/freedesktop/NetworkManager"
DEV = NM + ".Device"
WIFI = DEV + ".Wireless"
AP = NM + ".AccessPoint"
CON = NM + ".Settings.Connection"
ACTIVE = NM + ".Connection.Active"
PROPS = "org.freedesktop.DBus.Properties"
OM = "org.freedesktop.DBus.ObjectManager"
objects = {}


def paths(items=()):
    return dbus.Array([dbus.ObjectPath(item) for item in items], signature="o")


class Object(dbus.service.Object):
    def __init__(self, bus, path, properties):
        super().__init__(bus, path)
        self.path = path
        self.properties = properties
        objects[path] = self

    @dbus.service.method(PROPS, in_signature="s", out_signature="a{sv}")
    def GetAll(self, interface):
        return self.properties.get(interface, {})

    @dbus.service.method(PROPS, in_signature="ss", out_signature="v")
    def Get(self, interface, name):
        return self.properties[interface][name]

    @dbus.service.signal(PROPS, signature="sa{sv}as")
    def PropertiesChanged(self, interface, changed, invalidated):
        pass

    def change(self, interface, **values):
        self.properties[interface].update(values)
        self.PropertiesChanged(interface, values, [])


class Profile(Object):
    def __init__(self, bus, number, ssid, key="wpa-psk", band=None):
        self.settings = {
            "connection": {
                "id": "Renamed saved connection",
                "uuid": str(uuid.uuid4()),
                "type": "802-11-wireless",
                "autoconnect": True,
            },
            "802-11-wireless": {"ssid": dbus.ByteArray(ssid), "mode": "infrastructure"},
            "802-11-wireless-security": {"key-mgmt": key, "psk-flags": dbus.UInt32(2)},
            "ipv4": {"method": "auto"},
            "ipv6": {"method": "auto"},
        }
        if band:
            self.settings["802-11-wireless"]["band"] = band
        super().__init__(bus, ROOT + f"/Settings/{number}", {
            CON: {"Unsaved": False, "Flags": dbus.UInt32(0)},
        })

    @dbus.service.method(CON, out_signature="a{sa{sv}}")
    def GetSettings(self):
        return self.settings


class Settings(Object):
    @dbus.service.method(NM + ".Settings", out_signature="ao")
    def ListConnections(self):
        return self.properties[NM + ".Settings"]["Connections"]


class Device(Object):
    def change(self, interface, **values):
        if interface == DEV and "State" in values:
            # libnm reads state from StateReason; both properties move together.
            values.setdefault("StateReason", (values["State"], dbus.UInt32(0)))
        super().change(interface, **values)

    @dbus.service.method(DEV, async_callbacks=("reply_handler", "error_handler"))
    def Disconnect(self, reply_handler, error_handler):
        manager = self.manager
        manager.disconnects.append(self.path)
        if manager.case == "disconnect-failure" and len(manager.disconnects) == 1:
            raise dbus.exceptions.DBusException(
                "Disconnect denied", name=NM + ".PermissionDenied")
        self.change(DEV, Autoconnect=False, State=dbus.UInt32(110))
        manager.disconnect_pending = True
        if manager.case == "disconnect-quit":
            manager.disconnect_reply = reply_handler
        else:
            reply_handler()

    @dbus.service.method(WIFI, in_signature="a{sv}")
    def RequestScan(self, options):
        self.manager.scans.append(options)
        raise dbus.exceptions.DBusException(
            "Scan already in progress", name=NM + ".Device.NotAllowed")

    @dbus.service.method(WIFI, out_signature="ao")
    def GetAllAccessPoints(self):
        return self.properties[WIFI]["AccessPoints"]


class Agents(Object):
    destination = None
    registration_reply = None
    registrations = 0

    @dbus.service.method(NM + ".AgentManager", in_signature="su", sender_keyword="sender",
                         async_callbacks=("reply_handler", "error_handler"))
    def RegisterWithCapabilities(self, identifier, capabilities, reply_handler, error_handler, sender=None):
        assert identifier == "simplenet"
        self.destination = sender
        self.registrations += 1
        if self.case.startswith("registration-") and self.registrations == 1:
            self.registration_reply = reply_handler
        else:
            reply_handler()

    @dbus.service.method(NM + ".AgentManager", in_signature="s", sender_keyword="sender",
                         async_callbacks=("reply_handler", "error_handler"))
    def Register(self, identifier, reply_handler, error_handler, sender=None):
        self.RegisterWithCapabilities(identifier, 0, reply_handler, error_handler, sender=sender)

    @dbus.service.method(NM + ".AgentManager")
    def Unregister(self):
        self.destination = None


class ObjectManager(dbus.service.Object):
    @dbus.service.method(OM, out_signature="a{oa{sa{sv}}}")
    def GetManagedObjects(self):
        return {path: obj.properties for path, obj in objects.items()}

    @dbus.service.signal(OM, signature="oa{sa{sv}}")
    def InterfacesAdded(self, path, interfaces):
        pass

    @dbus.service.signal(OM, signature="oas")
    def InterfacesRemoved(self, path, interfaces):
        pass


class Manager(Object):
    def __init__(self, bus, case):
        self.bus, self.case = bus, case
        self.object_manager = ObjectManager(bus, "/org/freedesktop")
        self.calls, self.answers, self.errors = [], [], []
        self.disconnects = []
        self.scans = []
        self.disconnect_pending = False
        self.disconnect_reply = None
        self.profile = None
        self.active = None
        self.activation_reply = None
        self.ssid = b"caf\xe9"  # Display conversion must never become identity.
        super().__init__(bus, ROOT, {NM: {
            "Version": "1.52.1", "State": dbus.UInt32(20),
            "NetworkingEnabled": True, "WirelessEnabled": True,
            "WirelessHardwareEnabled": True, "Startup": False,
            "Devices": paths([ROOT + "/Devices/1"]),
            "AllDevices": paths([ROOT + "/Devices/1"]),
            "ActiveConnections": paths(), "PrimaryConnection": dbus.ObjectPath("/"),
            "ActivatingConnection": dbus.ObjectPath("/"),
        }})
        self.ap = Object(bus, ROOT + "/AccessPoint/1", {AP: {
            "Ssid": dbus.ByteArray(self.ssid), "HwAddress": "02:00:00:00:00:01",
            "Flags": dbus.UInt32(1), "WpaFlags": dbus.UInt32(0),
            "RsnFlags": dbus.UInt32(0x108), "Mode": dbus.UInt32(2),
            "Frequency": dbus.UInt32(2412), "Strength": dbus.Byte(90),
            "MaxBitrate": dbus.UInt32(54000), "LastSeen": dbus.Int32(1),
        }})
        self.device = Device(bus, ROOT + "/Devices/1", {
            DEV: {
                "Interface": "wlan-test", "IpInterface": "wlan-test",
                "DeviceType": dbus.UInt32(2), "Managed": True, "Autoconnect": True,
                "State": dbus.UInt32(30), "StateReason": (dbus.UInt32(30), dbus.UInt32(0)),
                "ActiveConnection": dbus.ObjectPath("/"),
                "AvailableConnections": paths(),  # nmtui uses all saved profiles.
                "Driver": "fixture", "Capabilities": dbus.UInt32(1),
                "Ip4Config": dbus.ObjectPath("/"), "Ip6Config": dbus.ObjectPath("/"),
            },
            WIFI: {
                "HwAddress": "02:00:00:00:01:00", "PermHwAddress": "02:00:00:00:01:00",
                "Mode": dbus.UInt32(2), "WirelessCapabilities": dbus.UInt32(0x7fff),
                "AccessPoints": paths([self.ap.path]),
                "ActiveAccessPoint": dbus.ObjectPath("/"), "LastScan": dbus.Int64(1),
            },
        })
        self.device.manager = self
        profiles = []
        if case not in ("new", "cancel", "scan", "ownership", "lifecycle"):
            # Same SSID but wrong band/security precede the valid saved profile.
            profiles = [Profile(bus, 1, self.ssid, band="a"),
                        Profile(bus, 2, self.ssid, key="wpa-eap"),
                        Profile(bus, 3, self.ssid)]
            self.profile = profiles[-1]
        self.settings = Settings(bus, ROOT + "/Settings", {NM + ".Settings": {
            "Connections": paths(p.path for p in profiles),
            "Hostname": "fixture", "CanModify": True,
        }})
        self.agents = Agents(bus, ROOT + "/AgentManager", {})
        self.agents.case = case

    def InterfacesAdded(self, path, interfaces):
        self.object_manager.InterfacesAdded(path, interfaces)

    @dbus.service.method(NM, out_signature="a{ss}")
    def GetPermissions(self):
        return {NM + ".network-control": "yes", NM + ".settings.modify.system": "yes"}

    @dbus.service.method(NM, out_signature="ao")
    def GetDevices(self):
        return paths([self.device.path])

    @dbus.service.method(NM, out_signature="ao")
    def GetAllDevices(self):
        return self.GetDevices()

    def begin(self, device, ap):
        assert str(device) == self.device.path
        assert str(ap) in self.device.properties[WIFI]["AccessPoints"]
        assert self.agents.destination
        if self.active:
            self.active.remove_from_connection()
        self.active = Object(self.bus, ROOT + f"/ActiveConnection/{len(self.calls)}", {ACTIVE: {
            "Connection": dbus.ObjectPath(self.profile.path),
            "SpecificObject": dbus.ObjectPath(ap),
            "Id": self.profile.settings["connection"]["id"],
            "Uuid": self.profile.settings["connection"]["uuid"],
            "Type": "802-11-wireless", "Devices": paths([self.device.path]),
            "State": dbus.UInt32(1), "StateFlags": dbus.UInt32(0),
            "Default": False, "Default6": False, "Vpn": False,
            "Ip4Config": dbus.ObjectPath("/"), "Ip6Config": dbus.ObjectPath("/"),
        }})
        self.InterfacesAdded(self.active.path, self.active.properties)
        self.change(NM, ActiveConnections=paths([self.active.path]))
        self.device.change(DEV, State=dbus.UInt32(60), Autoconnect=True,
                           ActiveConnection=dbus.ObjectPath(self.active.path))
        GLib.idle_add(self.ask)
        return dbus.ObjectPath(self.active.path)

    @dbus.service.method(NM, in_signature="ooo", out_signature="o",
                         async_callbacks=("reply_handler", "error_handler"))
    def ActivateConnection(self, profile, device, ap, reply_handler, error_handler):
        assert str(profile) == self.profile.path
        self.calls.append(("activate", str(profile), str(ap)))
        active = self.begin(device, ap)
        if self.case in ("dismiss", "activation-navigation", "activation-quit") and len(self.calls) == 1:
            self.activation_reply = lambda: reply_handler(active)
        else:
            reply_handler(active)

    @dbus.service.method(NM, in_signature="a{sa{sv}}oo", out_signature="oo")
    def AddAndActivateConnection(self, partial, device, ap):
        assert not partial, "New settings must be completed by NetworkManager, as in nmtui"
        assert self.profile is None, "A saved profile must not be replaced"
        self.calls.append(("add", str(ap)))
        self.profile = Profile(self.bus, 4, self.ssid)
        self.InterfacesAdded(self.profile.path, self.profile.properties)
        self.settings.change(NM + ".Settings", Connections=paths([self.profile.path]))
        return dbus.ObjectPath(self.profile.path), self.begin(device, ap)

    def ask(self, retry=False):
        proxy = self.bus.get_object(self.agents.destination,
                                   "/org/freedesktop/NetworkManager/SecretAgent",
                                   introspect=False)
        method = proxy.get_dbus_method("GetSecrets", NM + ".SecretAgent")
        settings = dbus.Dictionary({key: dbus.Dictionary(value, signature="sv")
                                    for key, value in self.profile.settings.items()},
                                   signature="sa{sv}")
        method(settings, dbus.ObjectPath(self.profile.path),
               "802-11-wireless-security", dbus.Array([], signature="s"),
               dbus.UInt32(3 if retry else 1),
               reply_handler=self.answered, error_handler=lambda error: self.errors.append(error))
        return False

    def answered(self, secrets):
        answer = str(secrets["802-11-wireless-security"]["psk"])
        self.answers.append(answer)
        if answer == "wrong":
            GLib.idle_add(self.ask, True)
        elif self.case not in ("slow", "state-navigation"):
            self.finish()

    def finish(self):
        self.device.change(DEV, State=dbus.UInt32(100))
        self.device.change(WIFI, ActiveAccessPoint=self.active.properties[ACTIVE]["SpecificObject"])
        self.active.change(ACTIVE, State=dbus.UInt32(2))

    def finish_disconnect(self):
        self.active.change(ACTIVE, State=dbus.UInt32(4))
        self.device.change(DEV, State=dbus.UInt32(30),
                           ActiveConnection=dbus.ObjectPath("/"))
        self.device.change(WIFI, ActiveAccessPoint=dbus.ObjectPath("/"))
        self.change(NM, ActiveConnections=paths())
        self.disconnect_pending = False


def run_case(binary, case):
    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    bus = dbus.SessionBus()
    name = dbus.service.BusName(NM, bus=bus)
    manager = Manager(bus, case)
    context = GLib.MainContext.default()
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
    os.set_blocking(master, False)
    env = dict(os.environ, TERM="xterm", LIBNM_USE_SESSION_BUS="1",
               DBUS_SYSTEM_BUS_ADDRESS=os.environ["DBUS_SESSION_BUS_ADDRESS"])
    command = [binary, "-i", "wlan-test"]
    if case == "ownership":
        command += ["-b", "wpa"]
    child = subprocess.Popen(command, stdin=slave, stdout=slave, stderr=slave, env=env)
    os.close(slave)
    output = bytearray()

    def pump():
        while context.pending():
            context.iteration(False)
        try:
            output.extend(os.read(master, 65536))
        except (BlockingIOError, OSError):
            pass

    def until(predicate, seconds=8):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            pump()
            if predicate():
                return
            if child.poll() is not None:
                break
            time.sleep(0.01)
        raise AssertionError(f"{case}: timed out; exit={child.poll()}, calls={manager.calls}, "
                             f"errors={manager.errors}, output={bytes(output)[-3000:]!r}")

    def click(x=5, y=5):
        # SGR mouse press/release on a network row (terminal coordinates are 1-based).
        os.write(master, f"\x1b[<0;{x};{y}M\x1b[<0;{x};{y}m".encode())

    def settle():
        for _ in range(40):
            pump()
            time.sleep(0.01)

    try:
        if case == "ownership":
            until(lambda: child.poll() is not None)
            assert child.returncode == 1
            assert b"NetworkManager manages wlan-test" in output
            return
        until(lambda: b"access points known" in output)
        responsive = ("registration-navigation", "registration-quit", "activation-navigation",
                      "activation-quit", "state-navigation", "disconnect-navigation", "disconnect-quit")
        if case in responsive:
            points = [manager.ap.path]
            for number in range(2, 16):
                other = Object(bus, ROOT + f"/AccessPoint/{number}",
                               {AP: copy.deepcopy(manager.ap.properties[AP])})
                other.properties[AP].update(HwAddress=f"02:00:00:00:00:{number:02x}",
                                            Strength=dbus.Byte(91 - number))
                manager.InterfacesAdded(other.path, other.properties)
                points.append(other.path)
            manager.device.change(WIFI, AccessPoints=paths(points))
            settle()

            def navigate_pending():
                scans = len(manager.scans)
                # Down, page down/up, rescan, mouse to row three, then Up.
                # A subsequent activation must target AP 2, including after
                # a live refresh sorts the original AP to the end of the list.
                os.write(master, b"\x1bOB\x1b[6~\x1b[5~r")
                until(lambda: len(manager.scans) > scans, seconds=1)
                click(y=7)
                settle()
                os.write(master, b"\x1bOA")
                settle()
                manager.ap.change(AP, Strength=dbus.Byte(1))
                settle()

            def quit_pending():
                os.write(master, b"q")
                until(lambda: child.poll() is not None, seconds=1)
                assert child.returncode == 0
                assert b"CRITICAL" not in output, bytes(output)[-3000:]

            os.write(master, b"\n")
            if case.startswith("registration-"):
                until(lambda: manager.agents.registration_reply is not None)
                late_reply = manager.agents.registration_reply
                if case == "registration-quit":
                    quit_pending()
                    assert not manager.calls
                    return
                navigate_pending()
                assert not manager.calls
                os.write(master, b"\x1b")
                until(lambda: b"Stopped waiting" in output, seconds=1)
                os.write(master, b"\n")
                until(lambda: len(manager.calls) == 1)
                assert manager.calls[0][-1] == points[1]
                late_reply()  # Completion from a dismissed agent cannot touch the new operation.
                until(lambda: b"Authenticate:" in output)
                os.write(master, b"correct-password\n")
                until(lambda: b"Connected to" in output)
                quit_pending()
                return
            if case.startswith("activation-"):
                until(lambda: manager.activation_reply is not None)
                late_reply = manager.activation_reply
                if case == "activation-quit":
                    quit_pending()
                    assert len(manager.calls) == 1 and not manager.disconnects
                    assert b"Authenticate:" not in output
                    return
                navigate_pending()
                assert len(manager.calls) == 1
                os.write(master, b"\x1b")
                until(lambda: b"Stopped waiting" in output, seconds=1)
                os.write(master, b"\n")
                until(lambda: len(manager.calls) == 2)
                assert manager.calls[1][-1] == points[1]
                late_reply()  # The old callback must safely finish after a new request started.
                until(lambda: b"Authenticate:" in output)
                os.write(master, b"correct-password\n")
                until(lambda: b"Connected to" in output)
                quit_pending()
                return
            until(lambda: b"Authenticate:" in output)
            os.write(master, b"correct-password\n")
            until(lambda: manager.answers == ["correct-password"])
            if case == "state-navigation":
                navigate_pending()
                assert len(manager.calls) == 1
                manager.finish()
                until(lambda: b"Connected to" in output)
            else:
                until(lambda: b"Connected to" in output)
                until(lambda: manager.agents.destination is None)
                settle()
                os.write(master, b"\n")
                until(lambda: manager.disconnect_pending)
                if case == "disconnect-quit":
                    until(lambda: manager.disconnect_reply is not None)
                    quit_pending()
                    assert len(manager.calls) == len(manager.disconnects) == 1
                    return
                navigate_pending()
                assert len(manager.calls) == len(manager.disconnects) == 1
                manager.finish_disconnect()
                until(lambda: b"ed from" in output)
            until(lambda: manager.agents.destination is None)
            output.clear()
            os.write(master, b"\n")
            until(lambda: len(manager.calls) == 2)
            assert manager.calls[1][-1] == points[1]
            until(lambda: b"Authenticate:" in output)
            os.write(master, b"\x1b")
            until(lambda: bool(manager.errors))
            quit_pending()
            return
        if case == "lifecycle":
            manager.change(NM, WirelessEnabled=False)
            until(lambda: b"Wi-Fi is disabled" in output)
            manager.change(NM, WirelessEnabled=True)
            until(lambda: b"Wi-Fi is available" in output)
            old_device = manager.device
            manager.object_manager.InterfacesRemoved(old_device.path, [DEV, WIFI])
            manager.change(NM, Devices=paths(), AllDevices=paths())
            objects.pop(old_device.path)
            old_device.remove_from_connection()
            until(lambda: b"No managed Wi-Fi adapter" in output)
            output.clear()
            manager.device = Device(bus, ROOT + "/Devices/2", old_device.properties)
            manager.device.manager = manager
            manager.InterfacesAdded(manager.device.path, manager.device.properties)
            manager.change(NM, Devices=paths([manager.device.path]),
                           AllDevices=paths([manager.device.path]))
            until(lambda: b"Wi-Fi is available" in output)
            os.write(master, b"q")
            until(lambda: child.poll() is not None)
            assert child.returncode == 0 and not manager.calls
            return
        if case == "selection":
            other = Object(bus, ROOT + "/AccessPoint/2", {AP: dict(manager.ap.properties[AP])})
            other.properties[AP].update(HwAddress="02:00:00:00:00:02", Strength=dbus.Byte(100))
            manager.InterfacesAdded(other.path, other.properties)
            manager.device.change(WIFI, AccessPoints=paths([manager.ap.path, other.path]))
            for _ in range(40):
                pump()
                time.sleep(0.01)
        if case == "scan":
            os.write(master, b"r")
            until(lambda: b"Scan request failed" in output)
        if case == "mouse":
            click()
        else:
            os.write(master, b"\n")
        if case == "dismiss":
            until(lambda: manager.activation_reply is not None)
            os.write(master, b"\x1b")
            for _ in range(60):
                pump()
                time.sleep(0.01)
            manager.activation_reply()
            until(lambda: b"Stopped waiting" in output)
            until(lambda: manager.agents.destination is None and bool(manager.errors))
            assert manager.errors[0].get_dbus_name() in (
                NM + ".SecretAgent.AgentCanceled", "org.freedesktop.DBus.Error.UnknownMethod",
                "org.freedesktop.DBus.Error.UnknownObject"), manager.errors[0]
            assert b"Authenticate:" not in output and not manager.answers
            assert len(manager.calls) == 1
            assert manager.active.properties[ACTIVE]["State"] == 1
            os.write(master, b"q")
            until(lambda: child.poll() is not None)
            assert child.returncode == 0
            return
        until(lambda: b"Authenticate:" in output)
        assert len(manager.calls) == 1
        assert manager.calls[0][-1] == manager.ap.path
        if case == "cancel":
            os.write(master, b"\x1b")
            until(lambda: bool(manager.errors))
            assert "UserCanceled" in manager.errors[0].get_dbus_name()
            assert not manager.answers
        else:
            if case == "saved":
                os.write(master, b"wrong\n")
                until(lambda: manager.answers == ["wrong"])
                # Wait until the first request was answered and the retry prompt was drawn.
                until(lambda: manager.agents.destination is not None)
                for _ in range(30):
                    pump()
                    time.sleep(0.01)
            os.write(master, b"correct-password\n")
            until(lambda: "correct-password" in manager.answers)
            if case == "slow":
                started = time.monotonic()
                until(lambda: time.monotonic() - started > 36, seconds=40)
                assert manager.agents.destination is not None, "35-second activation cutoff returned"
                manager.finish()
            until(lambda: b"Connected to" in output)
            assert len(manager.calls) == 1
            assert manager.profile.settings["802-11-wireless"]["ssid"] == manager.ssid
            assert manager.profile.settings["connection"]["autoconnect"]
            until(lambda: manager.agents.destination is None)
            if case in ("disconnect", "disconnect-failure", "disconnect-dismiss", "mouse"):
                settings_before = copy.deepcopy(manager.profile.settings)
                profiles_before = list(manager.settings.properties[NM + ".Settings"]["Connections"])
                settle()
                output.clear()
                if case == "mouse":
                    # Headers, empty rows, and the footer must not activate a network.
                    click(y=2)
                    click(y=8)
                    click(y=24)
                    settle()
                    assert not manager.disconnects and len(manager.calls) == 1
                    click()
                else:
                    os.write(master, b"\n")
                until(lambda: bool(manager.disconnects))
                if case == "disconnect-failure":
                    until(lambda: b"Could not disconnect" in output)
                    assert manager.device.properties[DEV]["State"] == 100
                    assert manager.device.properties[DEV]["Autoconnect"]
                    assert b"ed from" not in output
                    output.clear()
                    os.write(master, b"\n")
                until(lambda: manager.disconnect_pending)
                settle()
                # curses can retain "Disconnect" and redraw only "ed from ...".
                assert b"ed from" not in output, "Request accepted before device disconnected"
                assert len(manager.calls) == 1 and manager.agents.destination is None
                assert not manager.device.properties[DEV]["Autoconnect"]
                if case == "disconnect-dismiss":
                    os.write(master, b"\x1b")
                    until(lambda: b"Stopped waiting" in output)
                manager.finish_disconnect()
                if case != "disconnect-dismiss":
                    until(lambda: b"ed from" in output)
                settle()
                assert manager.profile.settings == settings_before
                assert list(manager.settings.properties[NM + ".Settings"]["Connections"]) == profiles_before
                output.clear()
                if case == "mouse":
                    click()
                else:
                    os.write(master, b"\n")
                until(lambda: len(manager.calls) == 2)
                until(lambda: b"Authenticate:" in output)
                assert manager.calls[1] == manager.calls[0], "Reconnect must reuse the saved profile"
                assert manager.device.properties[DEV]["Autoconnect"]
                os.write(master, b"\x1b")
                until(lambda: bool(manager.errors))
            if case == "saved":
                # No scan occurs here: losing association must immediately remove
                # the UI's active marker and allow a new activation.
                manager.device.change(DEV, State=dbus.UInt32(30), ActiveConnection=dbus.ObjectPath("/"))
                manager.device.change(WIFI, ActiveAccessPoint=dbus.ObjectPath("/"))
                manager.change(NM, ActiveConnections=paths())
                for _ in range(40):
                    pump()
                    time.sleep(0.01)
                os.write(master, b"\n")
                until(lambda: len(manager.calls) == 2)
                os.write(master, b"\x1b")
                until(lambda: bool(manager.errors))
        os.write(master, b"q")
        until(lambda: child.poll() is not None)
        assert child.returncode == 0
    finally:
        if child.poll() is None:
            child.terminate()
            try:
                child.wait(timeout=3)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()
        os.close(master)
        del name


if __name__ == "__main__":
    if len(sys.argv) == 4 and sys.argv[1] == "--case":
        run_case(os.path.abspath(sys.argv[3]), sys.argv[2])
        print(f"simplenet NM integration: {sys.argv[2]} passed")
    else:
        for scenario in ("saved", "new", "cancel", "dismiss", "scan", "ownership", "selection", "lifecycle",
                         "disconnect", "disconnect-failure", "disconnect-dismiss", "mouse",
                         "registration-navigation", "registration-quit", "activation-navigation", "activation-quit",
                         "state-navigation", "disconnect-navigation", "disconnect-quit", "slow"):
            subprocess.run(["dbus-run-session", "--", sys.executable, __file__,
                            "--case", scenario, os.path.abspath(sys.argv[1])],
                           check=True, timeout=65)
