# -*- coding: utf-8 -*-
# ChisFlash MBC5 bank 映射实测: 完整 dump 64KB 并与 gb_check.gb 分页比对
import serial, struct, sys, time, hashlib

PORT = 'COM13'
REF = r'Z:\Project\testrom\gb_check.gb'

ser = serial.Serial()
ser.port = PORT; ser.baudrate = 115200
ser.bytesize = 8; ser.parity = 'N'; ser.stopbits = 1; ser.timeout = 0.8

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

def read_range(addr, total, chunk=1024):
    out = b''
    while len(out) < total:
        d = gb_read(addr + len(out), min(chunk, total - len(out)))
        if d is None: return None
        out += d
    return out

def md5(b): return hashlib.md5(b).hexdigest()[:12]

ser.open()
ser.setDTR(True); ser.setRTS(True); time.sleep(0.06)
ser.setDTR(False); ser.setRTS(False); time.sleep(0.06)
ser.reset_input_buffer(); ser.reset_output_buffer(); time.sleep(0.02)
gb_write(0x0000, [0xF0]); gb_write(0x0000, [0xF0])  # warm-up

# bank0 固定窗
win0 = read_range(0x0000, 0x4000)
print(f'0x0000 窗 (bank0): {len(win0)} bytes md5={md5(win0)}')

# reg 0..3 在 0x4000 窗
pages = {}
for reg in range(4):
    gb_write(0x3000, [0x00])
    ack = gb_write(0x2000, [reg])
    w = read_range(0x4000, 0x4000)
    pages[reg] = w
    print(f'reg={reg} 0x4000窗: ack={ack} md5={md5(w) if w else None}')

# 参考文件分页
ref = open(REF, 'rb').read()
ref_pages = {i: ref[i*0x4000:(i+1)*0x4000] for i in range(len(ref)//0x4000)}
print(f'参考 {REF.split(chr(92))[-1]}: {len(ref)} bytes')
for i, p in ref_pages.items():
    print(f'  ref page{i}: md5={md5(p)}')

print('== 匹配结果 ==')
print(f'0x0000窗 == ref page0: {win0 == ref_pages[0]}')
for reg in range(4):
    match = [i for i, p in ref_pages.items() if p == pages[reg]]
    print(f'reg={reg} 0x4000窗 == ref page{match}')

ser.close()
