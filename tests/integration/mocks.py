#!/usr/bin/env python3
"""Mock system services for headless integration tests.

Runs on the *session* bus (start shade with SHADE_STATS_BUS=session):
  org.freedesktop.UPower (DisplayDevice), net.connman.iwd (one station),
  org.bluez (one adapter, one device with Battery1), and an MPRIS player
  org.mpris.MediaPlayer2.mock.

Each service lives on its own private connection (iwd and BlueZ both own
"/"). Control them with method calls on org.shade.Test at /test on the
bus name org.shade.Mocks, e.g.
  gdbus call --session -d org.shade.Mocks -o /test -m org.shade.Test.Battery 55 1
"""
import sys

import dbus
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib

dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
PROPS = "org.freedesktop.DBus.Properties"
OM = "org.freedesktop.DBus.ObjectManager"


def conn():
    return dbus.SessionBus(private=True)


class PropObject(dbus.service.Object):
    """An object with a dict of interfaces -> properties."""

    def __init__(self, bus, path, ifaces):
        super().__init__(bus, path)
        self.path = path
        self.ifaces = ifaces

    @dbus.service.method(PROPS, in_signature="ss", out_signature="v")
    def Get(self, iface, name):
        return self.ifaces[iface][name]

    @dbus.service.method(PROPS, in_signature="s", out_signature="a{sv}")
    def GetAll(self, iface):
        return dbus.Dictionary(self.ifaces.get(iface, {}), signature="sv")

    @dbus.service.method(PROPS, in_signature="ssv")
    def Set(self, iface, name, value):
        self.set(iface, {name: value})

    @dbus.service.signal(PROPS, signature="sa{sv}as")
    def PropertiesChanged(self, iface, changed, invalidated):
        pass

    def set(self, iface, changed):
        self.ifaces.setdefault(iface, {}).update(changed)
        self.PropertiesChanged(iface, dbus.Dictionary(changed, signature="sv"), dbus.Array([], signature="s"))


class Manager(dbus.service.Object):
    def __init__(self, bus):
        super().__init__(bus, "/")
        self.objects = {}

    def add(self, obj):
        self.objects[obj.path] = obj
        self.InterfacesAdded(dbus.ObjectPath(obj.path), obj.ifaces)

    def remove(self, path):
        obj = self.objects.pop(path)
        self.InterfacesRemoved(dbus.ObjectPath(path), dbus.Array(list(obj.ifaces), signature="s"))
        obj.remove_from_connection()

    @dbus.service.method(OM, out_signature="a{oa{sa{sv}}}")
    def GetManagedObjects(self):
        return {dbus.ObjectPath(p): o.ifaces for p, o in self.objects.items()}

    @dbus.service.signal(OM, signature="oa{sa{sv}}")
    def InterfacesAdded(self, path, ifaces):
        pass

    @dbus.service.signal(OM, signature="oas")
    def InterfacesRemoved(self, path, ifaces):
        pass


# --- UPower -----------------------------------------------------------------
up_bus = conn()
up_name = dbus.service.BusName("org.freedesktop.UPower", up_bus)
battery = PropObject(up_bus, "/org/freedesktop/UPower/devices/DisplayDevice", {
    "org.freedesktop.UPower.Device": {
        "Type": dbus.UInt32(2), "IsPresent": True, "Percentage": dbus.Double(80.0), "State": dbus.UInt32(2),
    }
})

# --- iwd ----------------------------------------------------------------------
iwd_bus = conn()
iwd_name = dbus.service.BusName("net.connman.iwd", iwd_bus)
iwd = Manager(iwd_bus)


class Station(PropObject):
    @dbus.service.method("net.connman.iwd.Station", out_signature="a(on)")
    def GetOrderedNetworks(self):
        return [(dbus.ObjectPath("/net/connman/iwd/0/3/686f6d65_psk"), dbus.Int16(self.strength))]


station = Station(iwd_bus, "/net/connman/iwd/0/3", {
    "net.connman.iwd.Station": {
        "State": "connected", "Scanning": False,
        "ConnectedNetwork": dbus.ObjectPath("/net/connman/iwd/0/3/686f6d65_psk"),
    }
})
station.strength = -5500
network = PropObject(iwd_bus, "/net/connman/iwd/0/3/686f6d65_psk", {
    "net.connman.iwd.Network": {"Name": "home", "Connected": True, "Type": "psk"}
})
iwd.objects[station.path] = station
iwd.objects[network.path] = network

# --- BlueZ --------------------------------------------------------------------
bz_bus = conn()
bz_name = dbus.service.BusName("org.bluez", bz_bus)
bluez = Manager(bz_bus)
adapter = PropObject(bz_bus, "/org/bluez/hci0", {"org.bluez.Adapter1": {"Powered": True, "Alias": "laptop"}})
device = PropObject(bz_bus, "/org/bluez/hci0/dev_AA_BB", {
    "org.bluez.Device1": {"Alias": "Headphones", "Connected": False, "Paired": True},
    "org.bluez.Battery1": {"Percentage": dbus.Byte(70)},
})
bluez.objects[adapter.path] = adapter
bluez.objects[device.path] = device

# --- MPRIS --------------------------------------------------------------------
mp_bus = conn()
mp_name = [dbus.service.BusName("org.mpris.MediaPlayer2.mock", mp_bus)]
PLAYER = "org.mpris.MediaPlayer2.Player"


def metadata(title, artist, art, length, url=""):
    m = {
        "mpris:trackid": dbus.ObjectPath("/track/%d" % (abs(hash(title)) % 100000)),
        "xesam:title": title, "xesam:artist": dbus.Array([artist], signature="s"),
        "mpris:length": dbus.Int64(length),
    }
    if art:
        m["mpris:artUrl"] = art
    if url:
        m["xesam:url"] = url
    return dbus.Dictionary(m, signature="sv")


class Player(PropObject):
    calls = []

    def call(self, name):
        Player.calls.append(name)

    @dbus.service.method(PLAYER)
    def PlayPause(self):
        self.call("PlayPause")
        st = self.ifaces[PLAYER]["PlaybackStatus"]
        self.set(PLAYER, {"PlaybackStatus": "Paused" if st == "Playing" else "Playing"})

    @dbus.service.method(PLAYER)
    def Next(self):
        self.call("Next")

    @dbus.service.method(PLAYER)
    def Previous(self):
        self.call("Previous")

    @dbus.service.method(PLAYER, in_signature="ox")
    def SetPosition(self, track, pos):
        self.call("SetPosition %d" % pos)
        self.ifaces[PLAYER]["Position"] = dbus.Int64(pos)
        self.Seeked(dbus.Int64(pos))

    @dbus.service.method(PLAYER, in_signature="x")
    def Seek(self, off):
        self.call("Seek %d" % off)

    @dbus.service.signal(PLAYER, signature="x")
    def Seeked(self, pos):
        pass

    def Set(self, iface, name, value):  # noqa: N802 (records calls too)
        self.call("Set %s %s" % (name, value))
        self.set(iface, {name: value})


Player.Set = dbus.service.method(PROPS, in_signature="ssv")(Player.Set)
player = Player(mp_bus, "/org/mpris/MediaPlayer2", {
    "org.mpris.MediaPlayer2": {"Identity": "Mock Player"},
    PLAYER: {
        "PlaybackStatus": "Playing", "LoopStatus": "None", "Shuffle": False, "Rate": dbus.Double(1.0),
        "Metadata": metadata("First Song", "Some Artist", sys.argv[1] if len(sys.argv) > 1 else "", 200_000_000),
        "Position": dbus.Int64(30_000_000),
        "CanGoNext": True, "CanGoPrevious": True, "CanPlay": True, "CanPause": True, "CanSeek": True, "CanControl": True,
    },
})

# --- control ------------------------------------------------------------------
ctl_bus = conn()
ctl_name = dbus.service.BusName("org.shade.Mocks", ctl_bus)


class Control(dbus.service.Object):
    @dbus.service.method("org.shade.Test", in_signature="du")
    def Battery(self, pct, state):
        battery.set("org.freedesktop.UPower.Device", {"Percentage": dbus.Double(pct), "State": dbus.UInt32(state)})

    @dbus.service.method("org.shade.Test", in_signature="b")
    def Wifi(self, connected):
        if connected:
            station.set("net.connman.iwd.Station", {"State": "connected",
                        "ConnectedNetwork": dbus.ObjectPath(network.path)})
        else:
            station.set("net.connman.iwd.Station", {"State": "disconnected"})

    @dbus.service.method("org.shade.Test", in_signature="b")
    def BtPower(self, on):
        adapter.set("org.bluez.Adapter1", {"Powered": on})

    @dbus.service.method("org.shade.Test", in_signature="b")
    def BtConnect(self, on):
        device.set("org.bluez.Device1", {"Connected": on})

    @dbus.service.method("org.shade.Test")
    def BtRemove(self):
        bluez.remove(device.path)

    @dbus.service.method("org.shade.Test", in_signature="ssss")
    def Track(self, title, artist, art, url):
        player.ifaces[PLAYER]["Position"] = dbus.Int64(0)
        player.set(PLAYER, {"Metadata": metadata(title, artist, art, 180_000_000, url)})

    @dbus.service.method("org.shade.Test", in_signature="s")
    def Status(self, status):
        player.set(PLAYER, {"PlaybackStatus": status})

    @dbus.service.method("org.shade.Test", out_signature="as")
    def Calls(self):
        return dbus.Array(Player.calls, signature="s")

    @dbus.service.method("org.shade.Test")
    def QuitPlayer(self):
        mp_name.clear()
        player.remove_from_connection()
        mp_bus.release_name("org.mpris.MediaPlayer2.mock")


Control(ctl_bus, "/test")
print("mocks ready", flush=True)
GLib.MainLoop().run()
