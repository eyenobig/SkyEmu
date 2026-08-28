# -*- coding: utf-8 -*-
# 完整模拟 gb_serial_mirror_sram + gb_serial_sync_ram_to_cart 的真机验证
# (读整块 SRAM -> 用 C 同款路径回写相同数据 -> 重读校验, 无破坏性)
import serial, struct, time, sys

PORT = sys.argv[1] if len(sys.argv) > 1 else 'COM13'
ser = serial.Serial()
ser.port = PORT; ser.baudrate = 115200
ser.bytesize = 8; ser.parity = 'N'; ser.stopbits = 1; ser.timeout = 2.0

def frame(cmd, payload=b''):
    body = bytes([cmd]) + payload
    return struct.pack('<H', 2 + len(body) + 2) + body + b'\x00\x00'

def gb_read(addr, length):
    ser.write(frame(0xFB, struct.pack('<IH', addr & 0xFFFF, length)))
    resp = ser.read(length + 2)
    return resp[2:] if len(resp) >= length + 2 else None

def gb_write(addr, data):
    ser.write(frame(0xFA, struct.pack('<I', addr & 0xFFFF) + bytes(data)))
    return ser.read(1) == b'\xAA'

# 与 gb_cart_serial.h 一致的常量
RAM_WINDOW = 0xA000
RAM_BANK_SIZE = 0x2000
MBC5 = 5

def ram_enable(en):
    return gb_write(0x0000, [0x0A if en else 0x00])

def switch_ram_bank(bank):  # MBC5: 0x4000
    return gb_write(0x4000, [bank & 0xFF])

def mirror_sram(ram_size):  # 对应 gb_serial_mirror_sram: 4KB 读块
    out = bytearray()
    off = 0
    while off < ram_size:
        bank = off // RAM_BANK_SIZE
        win = off % RAM_BANK_SIZE
        ln = min(4096, RAM_BANK_SIZE - win, ram_size - off)
        if not switch_ram_bank(bank): return None  # 无条件切换(含bank0)
        d = gb_read(RAM_WINDOW + win, ln)
        if d is None: return None
        out += d; off += ln
    return bytes(out)

def sync_ram_to_cart(data):  # 对应 gb_serial_sync_ram_to_cart: 1KB 写块
    off = 0
    while off < len(data):
        bank = off // RAM_BANK_SIZE
        win = off % RAM_BANK_SIZE
        ln = min(1024, RAM_BANK_SIZE - win, len(data) - off)
        if not switch_ram_bank(bank): return False  # 无条件切换(含bank0)
        if not gb_write(RAM_WINDOW + win, data[off:off+ln]): return False
        off += ln
    return True

ser.open()
ser.setDTR(True); ser.setRTS(True); time.sleep(0.06)
ser.setDTR(False); ser.setRTS(False); time.sleep(0.06)
ser.reset_input_buffer(); ser.reset_output_buffer(); time.sleep(0.02)
gb_write(0x0000, [0xF0]); gb_write(0x0000, [0xF0])  # warm-up

hdr = gb_read(0x0100, 0x50)
title = hdr[0x34:0x44].decode('ascii', 'replace').rstrip('\x00')
ram_size = {0x00:0,0x01:2*1024,0x02:8*1024,0x03:32*1024,0x04:128*1024,0x05:64*1024}.get(hdr[0x49], 0)
print(f'卡带: {title!r} type={hdr[0x47]:02x} ram_size={ram_size//1024}KB')
if ram_size == 0:
    print('!! 无 SRAM, 无法测试'); ser.close(); sys.exit(1)

print('-- 1. 启动镜像: 读整块 SRAM (gb_serial_mirror_sram 同款路径)')
if not ram_enable(True): print('!! RAM enable 失败'); sys.exit(1)
t0 = time.time()
orig = mirror_sram(ram_size)
t1 = time.time()
print(f'   读 {len(orig) if orig else 0} 字节, {"OK" if orig else "FAIL"}, 耗时 {t1-t0:.2f}s')
print(f'   前16字节: {orig[:16].hex()}')

print('-- 2. 存档回写: 整块写回相同数据 (gb_serial_sync_ram_to_cart 同款路径)')
t0 = time.time()
ok = sync_ram_to_cart(orig)
t1 = time.time()
print(f'   回写 {"OK" if ok else "FAIL"}, 耗时 {t1-t0:.2f}s')

print('-- 3. 重读校验')
back = mirror_sram(ram_size)
same = (back == orig)
print(f'   校验 {"PASS - 写入的数据完整读回" if same else "FAIL"}')
if not same:
    diff = [i for i in range(len(orig)) if orig[i] != back[i]]
    print(f'   差异字节数: {len(diff)}, 首个: @{diff[0]:#x}: {orig[diff[0]]:02x} != {back[diff[0]]:02x}')

ram_enable(False)
ser.close()
print('== 测试完成 ==')
