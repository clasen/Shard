#!/usr/bin/env python3
"""CoreMIDI helper (macOS, no dependencies).

  midi_bridge.py list                      list MIDI sources and destinations
  midi_bridge.py test <dest>               play a short phrase on a destination
  midi_bridge.py bridge <source> <dest>    forward everything from a source to a destination
  midi_bridge.py monitor <source>          print incoming messages (to find a controller's CCs)

<source>/<dest> are case-insensitive substrings of the endpoint name, e.g. "nano" or "obsidian".
"""
import ctypes
import ctypes.util
import sys
import time

cm = ctypes.cdll.LoadLibrary("/System/Library/Frameworks/CoreMIDI.framework/CoreMIDI")
cf = ctypes.cdll.LoadLibrary(ctypes.util.find_library("CoreFoundation"))

cf.CFStringCreateWithCString.restype = ctypes.c_void_p
cf.CFStringCreateWithCString.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_uint32]
cf.CFStringGetCString.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_long, ctypes.c_uint32]
cf.CFRelease.argtypes = [ctypes.c_void_p]
UTF8 = 0x08000100

READ_PROC = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p)

cm.MIDIGetNumberOfSources.restype = ctypes.c_ulong
cm.MIDIGetNumberOfDestinations.restype = ctypes.c_ulong
cm.MIDIGetSource.restype = ctypes.c_uint32
cm.MIDIGetSource.argtypes = [ctypes.c_ulong]
cm.MIDIGetDestination.restype = ctypes.c_uint32
cm.MIDIGetDestination.argtypes = [ctypes.c_ulong]
cm.MIDIObjectGetStringProperty.argtypes = [ctypes.c_uint32, ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)]
cm.MIDIClientCreate.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint32)]
cm.MIDIOutputPortCreate.argtypes = [ctypes.c_uint32, ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint32)]
cm.MIDIInputPortCreate.argtypes = [ctypes.c_uint32, ctypes.c_void_p, READ_PROC, ctypes.c_void_p,
                                   ctypes.POINTER(ctypes.c_uint32)]
cm.MIDIPortConnectSource.argtypes = [ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p]
cm.MIDISend.argtypes = [ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p]
cm.MIDIPacketListInit.restype = ctypes.c_void_p
cm.MIDIPacketListInit.argtypes = [ctypes.c_void_p]
cm.MIDIPacketListAdd.restype = ctypes.c_void_p
cm.MIDIPacketListAdd.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_void_p, ctypes.c_uint64,
                                 ctypes.c_ulong, ctypes.c_char_p]

PROP_NAME = ctypes.c_void_p.in_dll(cm, "kMIDIPropertyDisplayName")  # includes the device name


def cfstr(s):
    return cf.CFStringCreateWithCString(None, s.encode(), UTF8)


def name_of(obj):
    ref = ctypes.c_void_p()
    if cm.MIDIObjectGetStringProperty(obj, PROP_NAME, ctypes.byref(ref)) != 0 or not ref.value:
        return "?"
    buf = ctypes.create_string_buffer(256)
    cf.CFStringGetCString(ref, buf, 256, UTF8)
    cf.CFRelease(ref)
    return buf.value.decode(errors="replace")


def endpoints(kind):
    count = cm.MIDIGetNumberOfSources() if kind == "src" else cm.MIDIGetNumberOfDestinations()
    get = cm.MIDIGetSource if kind == "src" else cm.MIDIGetDestination
    return [(get(i), name_of(get(i))) for i in range(count)]


def find(kind, needle):
    for ref, name in endpoints(kind):
        if needle.lower() in name.lower():
            return ref, name
    sys.exit(f"no {'source' if kind == 'src' else 'destination'} matching '{needle}'")


client = ctypes.c_uint32()
cm.MIDIClientCreate(cfstr("midi_bridge"), None, None, ctypes.byref(client))


def send(port, dest, data):
    buf = ctypes.create_string_buffer(1024)
    pkt = cm.MIDIPacketListInit(buf)
    cm.MIDIPacketListAdd(buf, 1024, pkt, 0, len(data), bytes(data))
    cm.MIDISend(port, dest, buf)


def packets(pktlist):
    """Yields the raw bytes of every MIDIPacket in a MIDIPacketList (4-byte packed layout)."""
    base = pktlist
    count = ctypes.c_uint32.from_address(base).value
    p = base + 4
    for _ in range(count):
        length = ctypes.c_uint16.from_address(p + 8).value
        yield ctypes.string_at(p + 10, length)
        p = (p + 10 + length + 3) & ~3


def describe(msg):
    out = []
    i = 0
    while i < len(msg):
        st = msg[i]
        kind = st & 0xF0
        size = 2 if kind in (0xC0, 0xD0) else (1 if st >= 0xF0 else 3)
        m = msg[i:i + size]
        ch = (st & 0x0F) + 1
        if kind == 0x90 and len(m) == 3 and m[2]:
            out.append(f"ch{ch} note on  {m[1]:3d} vel {m[2]}")
        elif kind in (0x80, 0x90) and len(m) == 3:
            out.append(f"ch{ch} note off {m[1]:3d}")
        elif kind == 0xB0 and len(m) == 3:
            out.append(f"ch{ch} CC {m[1]:3d} = {m[2]}")
        elif kind == 0xE0 and len(m) == 3:
            out.append(f"ch{ch} pitch bend {((m[2] << 7) | m[1]) - 8192}")
        else:
            out.append(" ".join(f"{b:02x}" for b in m))
        i += size
    return "; ".join(out)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    cmd = sys.argv[1]
    if cmd == "list":
        print("sources:")
        for _, n in endpoints("src"):
            print("  ", n)
        print("destinations:")
        for _, n in endpoints("dst"):
            print("  ", n)
        return

    out_port = ctypes.c_uint32()
    cm.MIDIOutputPortCreate(client, cfstr("out"), ctypes.byref(out_port))

    if cmd == "test":
        dest, name = find("dst", sys.argv[2])
        print("playing on", name)
        for note in (57, 60, 64, 69, 72, 69, 64, 60):
            send(out_port, dest, [0x90, note, 100])
            time.sleep(0.18)
            send(out_port, dest, [0x80, note, 0])
        send(out_port, dest, [0x90, 45, 90])
        send(out_port, dest, [0x90, 57, 80])
        for v in range(20, 110, 6):
            send(out_port, dest, [0xB0, 74, v])
            time.sleep(0.05)
        time.sleep(0.6)
        send(out_port, dest, [0x80, 45, 0])
        send(out_port, dest, [0x80, 57, 0])
        return

    if cmd in ("bridge", "monitor"):
        src, src_name = find("src", sys.argv[2])
        dest = None
        if cmd == "bridge":
            dest, dest_name = find("dst", sys.argv[3])
            print(f"forwarding '{src_name}' -> '{dest_name}' (Ctrl+C to stop)")
        else:
            print(f"monitoring '{src_name}' (Ctrl+C to stop)")

        def on_read(pktlist, _ref, _conn):
            if dest is not None:
                cm.MIDISend(out_port, dest, pktlist)
            else:
                for msg in packets(pktlist):
                    print(describe(msg), flush=True)

        global _keep  # the callback must outlive this function
        _keep = READ_PROC(on_read)
        in_port = ctypes.c_uint32()
        cm.MIDIInputPortCreate(client, cfstr("in"), _keep, None, ctypes.byref(in_port))
        cm.MIDIPortConnectSource(in_port, src, None)
        try:
            while True:
                time.sleep(1)
        except KeyboardInterrupt:
            pass
        return

    sys.exit(__doc__)


if __name__ == "__main__":
    main()
