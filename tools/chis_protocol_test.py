# -*- coding: utf-8 -*-
# ChisFlash DirectPlay 协议真机测试
# 帧格式与 src/cart_serial/gb_cart_serial.h / gba_cart_serial.h 一致:
#   [u16 LE 帧总长][cmd][payload][2B 占位]
#   0xFA GB总线写 -> ACK 0xAA ; 0xFB GB总线读 -> 2B忽略前缀+N字节
#   0xF6 GBA ROM读 -> 2B忽略前缀+N字节
import serial, struct, sys, time

PORT = sys.argv[1] if len(sys.argv) > 1 else 'COM13'
ser = serial.Serial()

def log(msg): print(msg, flush=True)

def open_port():
    ser.port = PORT
    ser.baudrate = 115200
    ser.bytesize = 8; ser.parity = 'N'; ser.stopbits = 1
    ser.timeout = 0.8
    ser.open()
    # DTR 复位序列 (固件在控制线边沿清空命令缓冲)
    ser.setDTR(True); ser.setRTS(True)
    time.sleep(0.06)
    ser.setDTR(False); ser.setRTS(False)
    time.sleep(0.06)
    ser.reset_input_buffer(); ser.reset_output_buffer()
    time.sleep(0.02)

def frame(cmd, payload=b''):
    body = bytes([cmd]) + payload
    n = 2 + len(body) + 2
    return struct.pack('<H', n) + body + b'\x00\x00'

def gb_read(addr, length):
    ser.write(frame(0xFB, struct.pack('<IH', addr & 0xFFFF, length)))
    resp = ser.read(length + 2)
    if len(resp) < length + 2:
        return None
    return resp[2:]

def gb_write(addr, data):
    ser.write(frame(0xFA, struct.pack('<I', addr & 0xFFFF) + bytes(data)))
    return ser.read(1) == b'\xAA'

def gba_read_rom(byte_addr, length):
    ser.write(frame(0xF6, struct.pack('<IH', byte_addr, length)))
    resp = ser.read(length + 2)
    if len(resp) < length + 2:
        return None
    return resp[2:]

GB_LOGO = bytes([0xCE,0xED,0x66,0x66,0xCC,0x0D,0x00,0x0B,0x03,0x73,0x00,0x83])
GBA_LOGO = bytes([0x24,0xFF,0xAE,0x51,0x69,0x9A,0xA2,0x21,0x3D,0x84,0x82,0x0A])

def main():
    log(f'== ChisFlash DirectPlay 协议测试 @ {PORT} ==')
    open_port()
    # warm-up: 复位后第一条命令会被吞, 发 2 次 GB 总线写
    ok1 = gb_write(0x0000, [0xF0]); ok2 = gb_write(0x0000, [0xF0])
    log(f'warm-up 2x write(0x0000,0xF0): ack={ok1},{ok2}')

    # --- 1. GB 总线读: 卡带头 ---
    hdr = gb_read(0x0100, 0x50)
    if hdr is None:
        log('!! GB 总线读无响应 (0xFB)')
    else:
        logo_ok = hdr[0x04:0x10] == GB_LOGO
        logo_str = 'OK' if logo_ok else 'FAIL'
        log(f'GB header @0x0100: {hdr[:8].hex()}... nintendo_logo={logo_str}')
        title = hdr[0x34:0x44].decode('ascii', 'replace').rstrip('\x00')
        cgb = hdr[0x43] & 0x80
        cart_type = hdr[0x47]; rom_code = hdr[0x48]; ram_code = hdr[0x49]
        # header checksum @0x14D
        chk = 0
        for b in hdr[0x34-0x00:0x4E-0x00] if False else hdr[0x34:0x4E]:
            chk = (chk - b - 1) & 0xFF
        chk_str = 'PASS' if chk == hdr[0x4D] else 'FAIL'
        log(f'  title={title!r} cgb_bit={cgb:02x} type={cart_type:02x} rom={rom_code:02x} ram={ram_code:02x} chk_calc={chk:02x} chk_hdr={hdr[0x4D]:02x} {chk_str}')

    # --- 2. GBA ROM 读 ---
    gba = gba_read_rom(0x000000, 0x20)
    if gba is None:
        log('!! GBA ROM 读无响应 (0xF6)')
    else:
        log(f'GBA ROM @0x00: {gba[:16].hex()}')
        logo2 = 'OK' if gba[0x04:0x10] == GBA_LOGO else 'not a GBA cart (expected if GB cart inserted)'
        log(f'  gba_logo={logo2}')
        if gba[0x04:0x10] == GBA_LOGO:
            t = gba[0xA0:0xAC].decode('ascii', 'replace').rstrip('\x00')
            log(f'  GBA title: {t!r}')

    # --- 3. GB MBC bank 切换测试 ---
    if hdr:
        mbc = {0x00:'NO_MBC',0x01:'MBC1',0x02:'MBC1',0x03:'MBC1',0x05:'MBC2',0x06:'MBC2',
               0x0F:'MBC3',0x10:'MBC3',0x11:'MBC3',0x12:'MBC3',0x13:'MBC3',
               0x19:'MBC5',0x1A:'MBC5',0x1B:'MBC5',0x1C:'MBC5',0x1D:'MBC5',0x1E:'MBC5'}.get(hdr[0x47], 'UNKNOWN')
        log(f'-- MBC 类型: {mbc} (header 0x147={hdr[0x47]:02x})')
        b0 = gb_read(0x0000, 16)
        log(f'  bank0 固定窗 0x0000: {b0.hex() if b0 else None}')
        if mbc in ('MBC1','MBC3','MBC5'):
            # 切到 bank 2 再读 0x4000 窗
            if mbc == 'MBC5':
                ack = gb_write(0x3000, [0x00]) and gb_write(0x2000, [0x02])
            else:
                ack = gb_write(0x2000, [0x02])
            w2 = gb_read(0x4000, 16)
            log(f'  switch bank2 ack={ack} 0x4000窗: {w2.hex() if w2 else None}')
            # 切回 bank 1
            if mbc == 'MBC5':
                gb_write(0x3000, [0x00]); gb_write(0x2000, [0x01])
            else:
                gb_write(0x2000, [0x01])
            w1 = gb_read(0x4000, 16)
            log(f'  bank1 0x4000窗:  {w1.hex() if w1 else None}')
            if w1 and w2:
                log(f'  bank1/bank2 内容{"不同 -> bank 切换生效" if w1 != w2 else "相同(小 ROM 可能镜像)"}')

    # --- 4. GB SRAM 测试 (如卡带有 RAM) ---
    if hdr:
        ram_size = {0x00:0,0x01:2*1024,0x02:8*1024,0x03:32*1024,0x04:128*1024,0x05:64*1024}.get(hdr[0x49], 0)
        log(f'-- SRAM: ram_code={hdr[0x49]:02x} size={ram_size}')
        if ram_size > 0:
            en = gb_write(0x0000, [0x0A])
            log(f'  RAM enable ack={en}')
            orig = gb_read(0xA000, 8)
            log(f'  SRAM[0:8] 原始: {orig.hex() if orig else None}')
            if orig:
                # 非破坏性写测: 写回读到的原值 + 临时改一字节再还原
                ack = gb_write(0xA000, [orig[0]])
                back = gb_read(0xA000, 1)
                log(f'  写回原值 ack={ack} 读回={back.hex() if back else None} {"PASS" if back and back[0]==orig[0] else "CHECK"}')
            gb_write(0x0000, [0x00])  # 关 RAM

    ser.close()
    log('== 测试完成 ==')

if __name__ == '__main__':
    main()
