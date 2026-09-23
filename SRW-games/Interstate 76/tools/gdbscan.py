import gdb
base = int(gdb.parse_and_eval("*(int*)&loc_5DACC8")) & 0xffffffff
n = int(gdb.parse_and_eval("*(int*)&loc_5DACCC"))
inf = gdb.selected_inferior()
print("count", n)
for i in range(n + 1):
    rec = bytes(inf.read_memory(base + 48 * i, 48))
    name = rec[:16].split(b'\0')[0]
    if name in (b'gh45.gdf', b'm10.msn', b'01sbu01.wav'):
        import struct
        print(i, name, [hex(x) for x in struct.unpack('<8I', rec[16:48])])
