#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Refactor ChisBread's inline serial code in src/gba.h into layered
src/cart_serial/ modules (base + GBA platform), preserving behavior."""
import re, os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GBA_H = os.path.join(ROOT, 'src', 'gba.h')
OUT_DIR = os.path.join(ROOT, 'src', 'cart_serial')

with open(GBA_H, encoding='utf-8') as f:
    text = f.read()

def cut(start_marker, end_marker):
    """Remove text[start_marker .. end of line containing end_marker], return it."""
    global text
    i = text.index(start_marker)
    j = text.index(end_marker, i)
    j = text.index('\n', j) + 1
    block = text[i:j]
    text = text[:i] + text[j:]
    return block

def span(start_marker, end_marker):
    """Remove text[start_marker .. line before end_marker], return it."""
    global text
    i = text.index(start_marker)
    j = text.index(end_marker, i)
    j = text.rindex('\n', i, j) + 1
    block = text[i:j]
    text = text[:i] + text[j:]
    return block

def drop_line(marker):
    global text
    i = text.index(marker)
    j = text.index('\n', i) + 1
    text = text[:i] + text[j:]

# ---------------------------------------------------------------- cut blocks
blk_includes = cut('// 串口自动查找所需头文件', '#include <dirent.h>')
drop_line('#endif')  # closes platform #ifdef chain

blk_enum = cut('// ROM实时读取协议类型', '} gba_rom_protocol_t;')

blk_portdef = cut('// 串口端口类型定义', '#define INVALID_SERIAL_PORT -1')
drop_line('#endif')

blk_wentry = cut('// 串口写入缓存项', '#define GBA_SERIAL_WRITE_BUFFER_SIZE 2048')

# log block: up to (not incl.) fwd decl of gba_read_rom_byte
blk_log = span('// 日志输出配置宏', 'static uint8_t gba_read_rom_byte(gba_scratch_t *scratch, size_t offset);')

# scratch struct serial fields -> embedded base struct
i = text.index('  // 实时ROM读取相关字段')
j = text.index('\n', text.index('  bool save_sync_pending;')) + 1
text = text[:i] + '  // DirectPlay 串口直读状态 (GBA/GB/GBC 共用, 见 cart_serial/cart_serial_base.h)\n  sb_cart_serial_t serial;\n' + text[j:]

blk_filehelper = span('// 实时ROM读取辅助函数', '// DTR控制函数')
blk_platform = span('// DTR控制函数', '// 根据Python协议实现串口读取ROM')
blk_gba_cmds = span('// 根据Python协议实现串口读取ROM', 'static bool gba_save_rom_cache')
blk_cache = span('static bool gba_save_rom_cache', 'void gba_unload(gba_t*gba,gba_scratch_t *scratch){')
blk_gba_sync = span('// 串口模式下切换Flash bank', 'bool gba_load_rom(sb_emu_state_t*emu,gba_t* gba, gba_scratch_t *scratch){')

# ------------------------------------------------------------- renames
def renames_common(s):
    s = s.replace('GBA_SERIAL_LOG_', 'CART_SERIAL_LOG_')
    s = s.replace('gba_rom_protocol_t', 'sb_cart_protocol_t')
    s = s.replace('GBA_ROM_PROTOCOL_', 'SB_CART_PROTOCOL_')
    s = s.replace('gba_serial_write_entry_t', 'sb_cart_write_entry_t')
    s = s.replace('GBA_SERIAL_WRITE_BUFFER_SIZE', 'SB_CART_WRITE_BUFFER_SIZE')
    s = s.replace('gba_serial_set_dtr', 'cs_serial_set_dtr')
    s = s.replace('gba_auto_find_serial_port', 'cs_serial_auto_find')
    s = s.replace('gba_open_serial_port', 'cs_serial_open')
    s = s.replace('gba_close_serial_port', 'cs_serial_close')
    s = re.sub(r'\bgba_serial_read\(', 'cs_serial_read(', s)
    s = re.sub(r'\bgba_serial_write\(', 'cs_serial_write(', s)
    s = s.replace('gba_load_realtime_rom_bytes_from_file', 'cs_read_file_bytes')
    s = s.replace('gba_save_rom_cache', 'cs_cache_save')
    s = s.replace('gba_load_rom_cache', 'cs_cache_load')
    return s

FIELD_RE = re.compile(r'scratch->(use_realtime_rom|rom_protocol|realtime_rom_size|'
                      r'rom_source_address|rom_cache_path|rom_cache_data|rom_cache_valid|'
                      r'rom_source_file|rom_source_serial|custom_backup_type|'
                      r'serial_write_buffer|serial_write_count|ram_prefetch_cache|'
                      r'ram_prefetch_start|ram_prefetch_size|ram_prefetch_valid|'
                      r'save_sync_stable_frames|save_sync_pending)\b')

for name in ('blk_includes', 'blk_enum', 'blk_portdef', 'blk_wentry', 'blk_log',
             'blk_filehelper', 'blk_platform', 'blk_gba_cmds', 'blk_gba_sync'):
    globals()[name] = renames_common(globals()[name])

# cache funcs: take sb_cart_serial_t* cs, fields accessed directly
blk_cache = renames_common(blk_cache)
blk_cache = blk_cache.replace('gba_scratch_t *scratch', 'sb_cart_serial_t *cs')
blk_cache = re.sub(r'\bscratch->', 'cs->', blk_cache)

text = renames_common(text)
text = FIELD_RE.sub(r'scratch->serial.\1', text)

# unload call-site fixup in remaining gba.h
text = text.replace('cs_cache_save(scratch,', 'cs_cache_save(&scratch->serial,')

# ---------------------------------------------------------------- assemblies
os.makedirs(OUT_DIR, exist_ok=True)

serial_port_h = f'''// serial_port.h - DirectPlay 基础层: 平台串口 I/O
// 提取自 ChisBread/SkyEmu dev_readfromserial (DirectPlayV0.6), 更名 cs_serial_*。
// 适配 ChisFlash USB 读卡器: VID 0x0483 PID 0x0721, 115200 8N1,
// DTR 翻转复位固件命令缓冲。
#pragma once
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

{blk_includes}

// 日志输出配置宏
{blk_log}

{blk_portdef}

// ---------- 平台串口原语 ----------
{blk_platform}
'''

base_h = f'''// cart_serial_base.h - DirectPlay 公用层
// ROM 来源协议模型、ROM 缓存、写入合并缓冲、存档延迟同步、统一关闭。
// GBA 与 GB/GBC 平台层 (gba_cart_serial.h / gb_cart_serial.h) 共用本文件。
// 提取自 ChisBread/SkyEmu dev_readfromserial (DirectPlayV0.6)。
#pragma once
#include "sb_types.h"
#include "cart_serial/serial_port.h"

{blk_enum}

{blk_wentry}

// DirectPlay 共用状态 (嵌入 gba_scratch_t / gb_scratch_t)
typedef struct {{
  bool use_realtime_rom;         // 直读模式开关
  sb_cart_protocol_t rom_protocol;
  size_t realtime_rom_size;      // ROM 总大小
  char rom_source_address[SB_FILE_PATH_SIZE];
  char rom_cache_path[SB_FILE_PATH_SIZE];
  uint8_t *rom_cache_data;       // ROM 缓存数据
  uint8_t *rom_cache_valid;      // 每字节有效位 (1=已缓存)
  FILE *rom_source_file;         // FILE 协议句柄
  void *rom_source_serial;       // SERIAL 协议 serial_port_t*
  int custom_backup_type;        // -1=自动检测 (平台层自定义语义)
  sb_cart_write_entry_t serial_write_buffer[SB_CART_WRITE_BUFFER_SIZE];
  int serial_write_count;
  uint8_t *ram_prefetch_cache;   // RAM 预读缓存 (2KB)
  uint32_t ram_prefetch_start;
  uint16_t ram_prefetch_size;
  bool ram_prefetch_valid;
  int save_sync_stable_frames;   // 连续未 dirty 帧数计数
  bool save_sync_pending;        // 有待同步的存档数据
}} sb_cart_serial_t;

// ---------------- ROM 缓存 ----------------
static inline bool cs_cache_has(sb_cart_serial_t* cs, size_t off) {{
  return cs->rom_cache_valid && cs->rom_cache_valid[off] != 0;
}}
static void cs_cache_free(sb_cart_serial_t* cs) {{
  if (cs->ram_prefetch_cache) {{ free(cs->ram_prefetch_cache); cs->ram_prefetch_cache = NULL; }}
  if (cs->rom_cache_valid) {{ free(cs->rom_cache_valid); cs->rom_cache_valid = NULL; }}
  // rom_cache_data 所有权在 emu->rom_data, 由模拟器释放
  cs->rom_cache_data = NULL;
  cs->use_realtime_rom = false;
}}

// ---------------- 存档延迟同步 ----------------
// 写入发生: 记 pending 并清零稳定计数; 连续 SAVE_SYNC_STABLE_FRAMES 帧不 dirty 才落盘
#define SAVE_SYNC_STABLE_FRAMES 10
static void cs_sync_note_dirty(sb_cart_serial_t* cs) {{
  cs->save_sync_pending = true;
  cs->save_sync_stable_frames = 0;
}}
// 每帧调用; 返回 true 表示现在应写盘(调用方随后须调 cs_sync_done)
static bool cs_sync_should_write(sb_cart_serial_t* cs, bool is_dirty) {{
  if (!cs->save_sync_pending) return false;
  if (is_dirty) {{ cs->save_sync_stable_frames = 0; return false; }}
  if (++cs->save_sync_stable_frames < SAVE_SYNC_STABLE_FRAMES) return false;
  return true;
}}
static void cs_sync_done(sb_cart_serial_t* cs) {{
  cs->save_sync_pending = false;
  cs->save_sync_stable_frames = 0;
}}

// ---------------- FILE 协议辅助 ----------------
{blk_filehelper}

// ---------------- ROM 缓存落盘/加载 ----------------
{blk_cache}

// ---------------- 统一关闭 ----------------
// 保存 ROM 缓存到磁盘、关闭 FILE/SERIAL 句柄并释放辅助内存。
static void cs_serial_shutdown(sb_cart_serial_t* cs) {{
  if (!cs->use_realtime_rom) return;
  if (cs->rom_cache_data && cs->realtime_rom_size > 0)
    cs_cache_save(cs, cs->rom_cache_data, cs->realtime_rom_size);
  if (cs->rom_source_file) {{ fclose(cs->rom_source_file); cs->rom_source_file = NULL; }}
  if (cs->rom_source_serial) {{
    cs_serial_close(*(serial_port_t*)cs->rom_source_serial);
    free(cs->rom_source_serial);
    cs->rom_source_serial = NULL;
  }}
  cs_cache_free(cs);
}}
'''

gba_serial_h = f'''// gba_cart_serial.h - GBA 平台层 DirectPlay
// ChisFlash 固件 GBA 命令族: 0xF5 写ROM(字地址) / 0xF6 读ROM / 0xF7 写SRAM /
// 0xF8 读SRAM / 0xF9 Flash编程。提取自 ChisBread/SkyEmu dev_readfromserial。
#pragma once
#include "cart_serial/cart_serial_base.h"

{blk_gba_cmds}

{blk_gba_sync}
'''

with open(os.path.join(OUT_DIR, 'serial_port.h'), 'w', encoding='utf-8') as f:
    f.write(serial_port_h)
with open(os.path.join(OUT_DIR, 'cart_serial_base.h'), 'w', encoding='utf-8') as f:
    f.write(base_h)
with open(os.path.join(OUT_DIR, 'gba_cart_serial.h'), 'w', encoding='utf-8') as f:
    f.write(gba_serial_h)

# ------------------------------------------------------- rewire gba.h itself
text = text.replace('#include "gba_bios.h"',
                    '#include "gba_bios.h"\n#include "cart_serial/serial_port.h"\n#include "cart_serial/cart_serial_base.h"', 1)
anchor = '  sb_cart_serial_t serial;\n};\n'
assert text.count(anchor) == 1
text = text.replace(anchor, anchor + '\n#include "cart_serial/gba_cart_serial.h"\n', 1)

with open(GBA_H, 'w', encoding='utf-8') as f:
    f.write(text)
print('refactor ok: serial_port.h / cart_serial_base.h / gba_cart_serial.h written, gba.h rewired')
