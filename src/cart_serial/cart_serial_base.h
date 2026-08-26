// cart_serial_base.h - DirectPlay 公用层
// ROM 来源协议模型、ROM 缓存、写入合并缓冲、存档延迟同步、统一关闭。
// GBA 与 GB/GBC 平台层 (gba_cart_serial.h / gb_cart_serial.h) 共用本文件。
// 提取自 ChisBread/SkyEmu dev_readfromserial (DirectPlayV0.6)。
#pragma once
#include "sb_types.h"
#include "serial_port.h"

// ROM实时读取协议类型
typedef enum {
  SB_CART_PROTOCOL_NONE = 0,
  SB_CART_PROTOCOL_FILE = 1,
  SB_CART_PROTOCOL_SERIAL = 2,
  SB_CART_PROTOCOL_NET = 3
} sb_cart_protocol_t;


// 串口写入缓存项
typedef struct {
  uint32_t addr;      // 写入地址
  uint8_t data;       // 写入数据
  bool is_rom;        // true=ROM区域(writeRom), false=RAM区域(writeRam)
} sb_cart_write_entry_t;

#define SB_CART_WRITE_BUFFER_SIZE 2048


// DirectPlay 共用状态 (嵌入 gba_scratch_t / gb_scratch_t)
typedef struct {
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
  uint8_t mbc_type;              // GB 平台层: 真实卡带 MBC 类型 (CS_GB_*, 与 SB_MBC_* 同值)
} sb_cart_serial_t;

// ---------------- ROM 缓存 ----------------
static inline bool cs_cache_has(sb_cart_serial_t* cs, size_t off) {
  return cs->rom_cache_valid && cs->rom_cache_valid[off] != 0;
}
static void cs_cache_free(sb_cart_serial_t* cs) {
  if (cs->ram_prefetch_cache) { free(cs->ram_prefetch_cache); cs->ram_prefetch_cache = NULL; }
  if (cs->rom_cache_valid) { free(cs->rom_cache_valid); cs->rom_cache_valid = NULL; }
  // rom_cache_data 所有权在 emu->rom_data, 由模拟器释放
  cs->rom_cache_data = NULL;
  cs->use_realtime_rom = false;
}

// ---------------- 存档延迟同步 ----------------
// 写入发生: 记 pending 并清零稳定计数; 连续 SAVE_SYNC_STABLE_FRAMES 帧不 dirty 才落盘
#define SAVE_SYNC_STABLE_FRAMES 10
static void cs_sync_note_dirty(sb_cart_serial_t* cs) {
  cs->save_sync_pending = true;
  cs->save_sync_stable_frames = 0;
}
// 每帧调用; 返回 true 表示现在应写盘(调用方随后须调 cs_sync_done)
static bool cs_sync_should_write(sb_cart_serial_t* cs, bool is_dirty) {
  if (!cs->save_sync_pending) return false;
  if (is_dirty) { cs->save_sync_stable_frames = 0; return false; }
  if (++cs->save_sync_stable_frames < SAVE_SYNC_STABLE_FRAMES) return false;
  return true;
}
static void cs_sync_done(sb_cart_serial_t* cs) {
  cs->save_sync_pending = false;
  cs->save_sync_stable_frames = 0;
}

// ---------------- FILE 协议辅助 ----------------
// 实时ROM读取辅助函数 - 读取指定范围的字节
static bool cs_read_file_bytes(FILE* file, size_t offset, uint8_t* buffer, size_t size) {
  if (!file) {
    return false;
  }
  
  if (fseek(file, offset, SEEK_SET) != 0) {
    log_printf("Failed to seek to offset %zu in ROM file\n", offset);
    return false;
  }
  
  size_t bytes_read = fread(buffer, 1, size, file);
  if (bytes_read != size) {
    log_printf("Failed to read %zu bytes at offset %zu: read %zu bytes\n", size, offset, bytes_read);
    return false;
  }
  
  return true;
}




// ---------------- ROM 缓存落盘/加载 ----------------
static bool cs_cache_save(sb_cart_serial_t *cs, const uint8_t* data, size_t size) {
  if (!cs->use_realtime_rom || !data || size == 0 || !cs->rom_cache_valid) {
    return false;
  }
  
  FILE* file = fopen(cs->rom_cache_path, "wb");
  if (!file) {
    log_printf("Failed to open cache file for writing: %s\n", cs->rom_cache_path);
    return false;
  }
  
  // 写入文件头：魔数 + 版本 + ROM大小
  uint32_t magic = 0x43484147; // "GACH" (GBA Cache)
  uint32_t version = 1;
  uint32_t rom_size = (uint32_t)size;
  
  fwrite(&magic, sizeof(magic), 1, file);
  fwrite(&version, sizeof(version), 1, file);
  fwrite(&rom_size, sizeof(rom_size), 1, file);
  
  // 写入ROM数据
  size_t data_written = fwrite(data, 1, size, file);
  if (data_written != size) {
    log_printf("Failed to write ROM data to cache\n");
    fclose(file);
    return false;
  }
  
  // 写入valid标志位
  size_t valid_written = fwrite(cs->rom_cache_valid, 1, size, file);
  if (valid_written != size) {
    log_printf("Failed to write valid flags to cache\n");
    fclose(file);
    return false;
  }
  
  fclose(file);
  
  // 统计已缓存的字节数
  size_t cached_bytes = 0;
  for (size_t i = 0; i < size; i++) {
    if (cs->rom_cache_valid[i]) cached_bytes++;
  }
  
  log_printf("Successfully saved ROM cache to: %s (%zu/%zu bytes cached)\n", 
         cs->rom_cache_path, cached_bytes, size);
  return true;
}

static bool cs_cache_load(sb_cart_serial_t *cs, uint8_t* buffer, size_t size) {
  if (!cs->use_realtime_rom || !cs->rom_cache_valid) {
    return false;
  }
  
  FILE* file = fopen(cs->rom_cache_path, "rb");
  if (!file) {
    log_printf("Cache file not found: %s\n", cs->rom_cache_path);
    return false;
  }
  
  // 读取并验证文件头
  uint32_t magic = 0, version = 0, rom_size = 0;
  
  if (fread(&magic, sizeof(magic), 1, file) != 1 || magic != 0x43484147) {
    log_printf("Invalid cache file: bad magic number\n");
    fclose(file);
    return false;
  }
  
  if (fread(&version, sizeof(version), 1, file) != 1 || version != 1) {
    log_printf("Invalid cache file: unsupported version %u\n", version);
    fclose(file);
    return false;
  }
  
  if (fread(&rom_size, sizeof(rom_size), 1, file) != 1 || rom_size != size) {
    log_printf("Cache file size mismatch: expected %zu, got %u\n", size, rom_size);
    fclose(file);
    return false;
  }
  
  // 读取ROM数据
  size_t data_read = fread(buffer, 1, size, file);
  if (data_read != size) {
    log_printf("Failed to read ROM data from cache: read %zu/%zu bytes\n", data_read, size);
    fclose(file);
    return false;
  }
  
  // 读取valid标志位
  size_t valid_read = fread(cs->rom_cache_valid, 1, size, file);
  if (valid_read != size) {
    log_printf("Failed to read valid flags from cache: read %zu/%zu bytes\n", valid_read, size);
    fclose(file);
    return false;
  }
  
  fclose(file);
  
  // 统计已缓存的字节数
  size_t cached_bytes = 0;
  for (size_t i = 0; i < size; i++) {
    if (cs->rom_cache_valid[i]) cached_bytes++;
  }
  
  log_printf("Successfully loaded ROM cache from: %s (%zu/%zu bytes cached)\n", 
         cs->rom_cache_path, cached_bytes, size);
  return true;
}



// ---------------- 统一关闭 ----------------
// 保存 ROM 缓存到磁盘、关闭 FILE/SERIAL 句柄并释放辅助内存。
// 幂等: 部分初始化状态(加载失败路径)也可安全调用。
static void cs_serial_shutdown(sb_cart_serial_t* cs) {
  if (cs->use_realtime_rom && cs->rom_cache_data && cs->realtime_rom_size > 0)
    cs_cache_save(cs, cs->rom_cache_data, cs->realtime_rom_size);
  if (cs->rom_source_file) { fclose(cs->rom_source_file); cs->rom_source_file = NULL; }
  if (cs->rom_source_serial) {
    cs_serial_close(*(serial_port_t*)cs->rom_source_serial);
    free(cs->rom_source_serial);
    cs->rom_source_serial = NULL;
  }
  cs_cache_free(cs);
}
