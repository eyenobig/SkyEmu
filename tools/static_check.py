import re, sys
root = sys.argv[1] if len(sys.argv) > 1 else '.'
files = sys.argv[2:] if len(sys.argv) > 2 else ['src/gba.h','src/gb.h','src/main.c',
         'src/cart_serial/serial_port.h','src/cart_serial/cart_serial_base.h',
         'src/cart_serial/gba_cart_serial.h','src/cart_serial/gb_cart_serial.h']
ok = True
for f in files:
    s = open(f, encoding='utf-8').read()
    s2 = re.sub(r'//[^\n]*', '', s)
    s2 = re.sub(r'/\*.*?\*/', '', s2, flags=re.S)
    s2 = re.sub(r'"(?:\\.|[^"\\])*"', '""', s2)
    s2 = re.sub(r"'(?:\\.|[^'\\])*'", "''", s2)
    b, p, k = s2.count('{')-s2.count('}'), s2.count('(')-s2.count(')'), s2.count('[')-s2.count(']')
    print('%s: braces %+d parens %+d brackets %+d' % (f, b, p, k))
    if b or p or k: ok = False

defs = {}
for f in files:
    s = open(f, encoding='utf-8').read()
    for m in re.finditer(r'\b(?:static\s+(?:inline|FORCE_INLINE)?\s*)?(?:bool|void|uint8_t|size_t|char\*?|serial_port_t|int)\s+(\w+)\s*\(', s):
        defs.setdefault(m.group(1), []).append(f)
needed = ['gb_is_realtime_config','gb_serial_directplay_load','gb_serial_read_rom_byte',
 'gb_serial_forward_bus_write','gb_serial_mirror_sram','gb_serial_sync_ram_to_cart',
 'cs_serial_shutdown','cs_sync_note_dirty','cs_sync_should_write','cs_sync_done',
 'cs_serial_open','cs_serial_close','cs_serial_read','cs_serial_write','cs_cache_has',
 'gba_read_rom_byte','gba_buffer_serial_write','gba_flush_serial_writes',
 'gba_serial_sync_sram_backup','gba_serial_sync_flash_backup','gba_serial_load_sram_backup',
 'gba_serial_load_flash_backup','gba_serial_read_ram','gba_serial_write_ram','log_printf']
for sym in needed:
    status = 'OK' if sym in defs else 'MISSING'
    print('%s: %s %s' % (sym, status, defs.get(sym, [])))
    if sym not in defs: ok = False
print('ALL OK' if ok else 'ISSUES FOUND')
