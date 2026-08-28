// gb_cart_serial.h - GB/GBC 平台层 DirectPlay (真实卡带串口直读)
// 基于 ChisFlash 固件 GB 总线命令族 (与本地 chis-burner-cmd / ChisFlashBurner 协议对齐):
//   0xFA GB 总线写透传: [FA][addr u32 LE][data...] -> ACK 0xAA
//   0xFB GB 总线读透传: [FB][addr u32 LE][len u16 LE] -> 2 字节忽略前缀 + N 字节数据
// 帧格式与 GBA 命令族相同: [u16 LE 帧总长][cmd][payload][2B 占位], 地址一律 u32 小端。
//
// 设计要点:
//  - 模拟器自身已完成 MBC bank 映射(算出线性 ROM 偏移), 本层只负责把线性偏移
//    翻译成"切真实卡带 bank + 读 0x0000/0x4000 窗口", ROM 缓存按线性偏移组织。
//  - 模拟器对 0x0000-0x7FFF 的每次写(即 MBC 寄存器写)原样转发到真实卡带,
//    保证两边 MBC 状态一致。
//  - SRAM 以本地 ram_data 镜像为准: 启动时整块从真实卡带读入, 存档同步时整块回写。
#pragma once
#include "cart_serial_base.h"

// MBC 常量 (与 gb.h 中 SB_MBC_* 同值, 此处独立定义使本文件自包含)
#define CS_GB_NO_MBC 0
#define CS_GB_MBC1   1
#define CS_GB_MBC2   2
#define CS_GB_MBC3   3
#define CS_GB_MBC5   5

// 配置文件可选标志
#define CS_GB_FLAG_NONE            0
#define CS_GB_FLAG_MBC5_SHIFT1     1  // Chis 复制卡: 线性 bank N 需写寄存器 N+1

// GB 总线窗口
#define CS_GB_ROM_LO_WINDOW  0x0000  // bank0 固定窗 0x0000-0x3FFF
#define CS_GB_ROM_HI_WINDOW  0x4000  // 可切窗 0x4000-0x7FFF
#define CS_GB_RAM_WINDOW     0xA000  // SRAM 窗 0xA000-0xBFFF (8KB)
#define CS_GB_BANK_SIZE      0x4000  // 16KB ROM bank
#define CS_GB_RAM_BANK_SIZE  0x2000  // 8KB SRAM bank
#define CS_GB_FETCH_CHUNK    2048    // ROM 未命中时按 2KB 块填充

static void gb_serial_sleep_ms(int ms) {
#ifdef _WIN32
  Sleep(ms);
#else
  usleep(ms * 1000);
#endif
}

static void gb_serial_flush(serial_port_t port) {
#ifdef _WIN32
  PurgeComm(port, PURGE_RXCLEAR | PURGE_TXCLEAR);
#else
  tcflush(port, TCIOFLUSH);
#endif
}

// 发送一帧命令 [u16 LE 总长][cmd][payload][2B 占位]
static bool gb_serial_send_frame(serial_port_t port, uint8_t cmd, const uint8_t* payload, uint16_t payload_len) {
  uint16_t frame_len = 2 + 1 + payload_len + 2;
  uint8_t* frame = (uint8_t*)malloc(frame_len);
  if (!frame) return false;
  frame[0] = frame_len & 0xFF;
  frame[1] = (frame_len >> 8) & 0xFF;
  frame[2] = cmd;
  if (payload_len) memcpy(frame + 3, payload, payload_len);
  frame[3 + payload_len] = 0;
  frame[4 + payload_len] = 0;
  gb_serial_flush(port);
  size_t written = cs_serial_write(port, frame, frame_len);
  free(frame);
  return written == frame_len;
}

// 0xFB: 读 GB 总线 addr 处 len 字节 (响应 = 2 字节忽略前缀 + 数据)
static bool gb_serial_bus_read(serial_port_t port, uint32_t addr, uint8_t* buffer, uint16_t len) {
  if (port == INVALID_SERIAL_PORT || !buffer || len == 0) return false;
  uint8_t payload[6];
  payload[0] = addr & 0xFF; payload[1] = (addr >> 8) & 0xFF;
  payload[2] = (addr >> 16) & 0xFF; payload[3] = (addr >> 24) & 0xFF;
  payload[4] = len & 0xFF; payload[5] = (len >> 8) & 0xFF;
  if (!gb_serial_send_frame(port, 0xFB, payload, 6)) return false;
  uint8_t* response = (uint8_t*)malloc((size_t)len + 2);
  if (!response) return false;
  size_t total = 0;
  int retries = 30;
  bool ok = false;
  while (total < (size_t)len + 2 && retries--) {
    size_t n = cs_serial_read(port, response + total, (size_t)len + 2 - total);
    total += n;
    if (n == 0) gb_serial_sleep_ms(10);
  }
  if (total == (size_t)len + 2) {
    memcpy(buffer, response + 2, len);  // 固件应答前 2 字节为 CRC 占位, 忽略
    ok = true;
  }
  free(response);
  return ok;
}

// 0xFA: 向 GB 总线 addr 写 data (响应 = ACK 0xAA)
static bool gb_serial_bus_write(serial_port_t port, uint32_t addr, const uint8_t* data, uint16_t len) {
  if (port == INVALID_SERIAL_PORT || !data || len == 0) return false;
  uint16_t payload_len = 4 + len;
  uint8_t* payload = (uint8_t*)malloc(payload_len);
  if (!payload) return false;
  payload[0] = addr & 0xFF; payload[1] = (addr >> 8) & 0xFF;
  payload[2] = (addr >> 16) & 0xFF; payload[3] = (addr >> 24) & 0xFF;
  memcpy(payload + 4, data, len);
  bool ok = gb_serial_send_frame(port, 0xFA, payload, payload_len);
  free(payload);
  if (!ok) return false;
  uint8_t ack = 0;
  int retries = 30;
  size_t n = 0;
  while (retries--) {
    n = cs_serial_read(port, &ack, 1);
    if (n == 1) break;
    gb_serial_sleep_ms(10);
  }
  return n == 1 && ack == 0xAA;
}

static bool gb_serial_bus_write1(serial_port_t port, uint32_t addr, uint8_t value) {
  return gb_serial_bus_write(port, addr, &value, 1);
}

// 把线性 bank N 映射到真实卡带 0x4000 窗口 (N>=1)
static bool gb_serial_switch_rom_bank(sb_cart_serial_t* cs, uint8_t mbc, unsigned bank) {
  serial_port_t port = *(serial_port_t*)cs->rom_source_serial;
  if (mbc == CS_GB_MBC5) {
    if (cs->custom_backup_type == CS_GB_FLAG_MBC5_SHIFT1) bank += 1;  // Chis 复制卡特性
    if (!gb_serial_bus_write1(port, 0x3000, (bank >> 8) & 0xFF)) return false;
    return gb_serial_bus_write1(port, 0x2000, bank & 0xFF);
  }
  if (mbc == CS_GB_MBC3) return gb_serial_bus_write1(port, 0x2000, bank & 0x7F);
  if (mbc == CS_GB_MBC2) return gb_serial_bus_write1(port, 0x2100, bank & 0x0F);
  if (mbc == CS_GB_MBC1) {
    if (bank >= 32) {  // 高 bank: mode1 + 0x4000 提供高 2 位
      uint8_t mode = 1, hi = (bank >> 5) & 3, lo = bank & 0x1F;
      if (!gb_serial_bus_write1(port, 0x6000, mode)) return false;
      if (!gb_serial_bus_write1(port, 0x4000, hi)) return false;
      return gb_serial_bus_write1(port, 0x2000, lo);
    }
    uint8_t mode = 0, zero = 0, lo = bank & 0x1F;
    if (!gb_serial_bus_write1(port, 0x6000, mode)) return false;
    if (!gb_serial_bus_write1(port, 0x4000, zero)) return false;
    return gb_serial_bus_write1(port, 0x2000, lo);
  }
  // NO_MBC: 32KB 直映射, bank1 固定在 0x4000 窗
  return mbc == CS_GB_NO_MBC && bank == 1;
}

static bool gb_serial_ram_enable(sb_cart_serial_t* cs, bool enable) {
  serial_port_t port = *(serial_port_t*)cs->rom_source_serial;
  return gb_serial_bus_write1(port, 0x0000, enable ? 0x0A : 0x00);
}

// SRAM bank 切换 (8KB/bank)
static bool gb_serial_switch_ram_bank(sb_cart_serial_t* cs, uint8_t mbc, unsigned bank) {
  serial_port_t port = *(serial_port_t*)cs->rom_source_serial;
  switch (mbc) {
    case CS_GB_MBC1: {  // mode0 下 0x4000 选 RAM bank 低 2 位
      uint8_t mode = 0;
      if (!gb_serial_bus_write1(port, 0x6000, mode)) return false;
      return gb_serial_bus_write1(port, 0x4000, bank & 3);
    }
    case CS_GB_MBC3: return gb_serial_bus_write1(port, 0x4000, bank & 0x07);
    case CS_GB_MBC5: return gb_serial_bus_write1(port, 0x4000, bank & 0xFF);
    default: return bank == 0;  // MBC2 无 bank
  }
}

// 模拟器 MBC 寄存器写 (0x0000-0x7FFF) 原样转发, 保持真实卡带 MBC 状态与模拟器一致
static void gb_serial_forward_bus_write(sb_cart_serial_t* cs, uint32_t addr, uint8_t value) {
  if (!cs || cs->rom_protocol != SB_CART_PROTOCOL_SERIAL || !cs->rom_source_serial) return;
  serial_port_t port = *(serial_port_t*)cs->rom_source_serial;
  gb_serial_bus_write1(port, addr & 0x7FFF, value);
}

// ROM 按线性偏移读取 (带缓存, 未命中按 2KB 块从真实卡带补填)
static uint8_t gb_serial_read_rom_byte(sb_cart_serial_t* cs, size_t offset) {
  if (!cs->rom_cache_valid || offset >= cs->realtime_rom_size) return 0xFF;
  if (cs_cache_has(cs, offset)) return cs->rom_cache_data[offset];
  serial_port_t port = *(serial_port_t*)cs->rom_source_serial;
  size_t bank = offset / CS_GB_BANK_SIZE;
  size_t window_off = offset % CS_GB_BANK_SIZE;
  // 块对齐, 且不跨 16KB 窗口
  size_t chunk_start = (offset / CS_GB_FETCH_CHUNK) * CS_GB_FETCH_CHUNK;
  size_t window_end = bank * CS_GB_BANK_SIZE + CS_GB_BANK_SIZE;
  size_t len = CS_GB_FETCH_CHUNK;
  if (chunk_start + len > window_end) len = window_end - chunk_start;
  if (chunk_start + len > cs->realtime_rom_size) len = cs->realtime_rom_size - chunk_start;
  uint32_t bus_addr;
  bool ok = false;
  if (bank == 0) {
    bus_addr = CS_GB_ROM_LO_WINDOW + (uint32_t)(chunk_start % CS_GB_BANK_SIZE);  // bank0 固定窗
    ok = gb_serial_bus_read(port, bus_addr, cs->rom_cache_data + chunk_start, (uint16_t)len);
  } else {
    if (gb_serial_switch_rom_bank(cs, cs->mbc_type, (unsigned)bank)) {
      bus_addr = CS_GB_ROM_HI_WINDOW + (uint32_t)(chunk_start % CS_GB_BANK_SIZE);
      ok = gb_serial_bus_read(port, bus_addr, cs->rom_cache_data + chunk_start, (uint16_t)len);
    }
  }
  if (ok) {
    for (size_t i = 0; i < len; ++i) cs->rom_cache_valid[chunk_start + i] = 1;
  } else {
    log_printf("[GB Serial] ROM fill failed @ bank %zu offset 0x%zx\n", bank, chunk_start);
    return 0xFF;
  }
  return cs->rom_cache_data[offset];
}

// 启动时把真实卡带 SRAM 整块镜像到本地 ram_data (真实卡带为存档权威来源)
static bool gb_serial_mirror_sram(sb_cart_serial_t* cs, uint8_t mbc, uint8_t* ram_data, int ram_size) {
  if (!ram_data || ram_size <= 0) return true;
  serial_port_t port = *(serial_port_t*)cs->rom_source_serial;
  if (!gb_serial_ram_enable(cs, true)) return false;
  bool ok = true;
  for (int off = 0; off < ram_size && ok; ) {
    int bank = off / CS_GB_RAM_BANK_SIZE;
    int win_off = off % CS_GB_RAM_BANK_SIZE;
    int len = CS_GB_RAM_BANK_SIZE - win_off;
    if (len > 4096) len = 4096;
    if (off + len > ram_size) len = ram_size - off;
    // 注意: RAM bank 寄存器跨 MCU 复位保持(读卡器不断卡带电源), 必须
    // 无条件显式切换到目标 bank, 包括 bank 0 (真机实测教训)
    if (!gb_serial_switch_ram_bank(cs, mbc, (unsigned)bank)) { ok = false; break; }
    ok = gb_serial_bus_read(port, CS_GB_RAM_WINDOW + (uint32_t)win_off, ram_data + off, (uint16_t)len);
    off += len;
  }
  if (ok) log_printf("[GB Serial] Mirrored %d bytes SRAM from cartridge\n", ram_size);
  else log_printf("[GB Serial] WARNING: SRAM mirror failed\n");
  return ok;
}

// 存档同步: 把本地 ram_data 整块回写真实卡带 SRAM
static void gb_serial_sync_ram_to_cart(sb_cart_serial_t* cs, uint8_t mbc, const uint8_t* ram_data, int ram_size) {
  if (!cs || cs->rom_protocol != SB_CART_PROTOCOL_SERIAL || !cs->rom_source_serial) return;
  if (!ram_data || ram_size <= 0) return;
  serial_port_t port = *(serial_port_t*)cs->rom_source_serial;
  if (!gb_serial_ram_enable(cs, true)) {
    log_printf("[GB Serial] ERROR: cannot enable RAM for save sync\n");
    return;
  }
  bool ok = true;
  for (int off = 0; off < ram_size && ok; ) {
    int bank = off / CS_GB_RAM_BANK_SIZE;
    int win_off = off % CS_GB_RAM_BANK_SIZE;
    int len = CS_GB_RAM_BANK_SIZE - win_off;
    if (len > 1024) len = 1024;
    if (off + len > ram_size) len = ram_size - off;
    // 注意: RAM bank 寄存器跨 MCU 复位保持(读卡器不断卡带电源), 必须
    // 无条件显式切换到目标 bank, 包括 bank 0 (真机实测教训)
    if (!gb_serial_switch_ram_bank(cs, mbc, (unsigned)bank)) { ok = false; break; }
    ok = gb_serial_bus_write(port, CS_GB_RAM_WINDOW + (uint32_t)win_off, ram_data + off, (uint16_t)len);
    off += len;
  }
  if (ok) log_printf("[GB Serial] Save synced back to cartridge (%d bytes)\n", ram_size);
  else log_printf("[GB Serial] WARNING: save sync to cartridge failed\n");
}

// 读卡带头 0x0100-0x014F (bank0 固定窗), 用于自动识别 ROM 大小
static bool gb_serial_detect_rom_size(sb_cart_serial_t* cs, size_t* rom_size_out) {
  serial_port_t port = *(serial_port_t*)cs->rom_source_serial;
  uint8_t header[0x50];
  if (!gb_serial_bus_read(port, 0x0100, header, sizeof(header))) return false;
  static const size_t sizes[] = {
    32*1024, 64*1024, 128*1024, 256*1024, 512*1024, 1024*1024, 2*1024*1024,
    4*1024*1024, 8*1024*1024, 0, 0, 0, 1.1*1024*1024, 1.2*1024*1024, 1.5*1024*1024
  };
  uint8_t code = header[0x148 - 0x100];
  size_t size = (code < sizeof(sizes)/sizeof(sizes[0])) ? sizes[code] : 0;
  if (!size) { log_printf("[GB Serial] Bad ROM size code 0x%02x\n", code); return false; }
  *rom_size_out = size;
  log_printf("[GB Serial] Cart type 0x%02x, ROM size %zu KB, title %.11s\n",
             header[0x147 - 0x100], size / 1024, (char*)header + 0x34);
  return true;
}

// 判断 rom 数据是否为 GB/GBC 直读配置文件 ("READREALTIME\n" 开头, 扩展名 .gb/.gbc)
static bool gb_is_realtime_config(const uint8_t* rom_data, size_t rom_size) {
  if (!rom_data || rom_size < 13) return false;
  return memcmp(rom_data, "READREALTIME\n", 13) == 0;
}

// GB/GBC DirectPlay 总入口:
// 解析配置 -> 打开串口 -> (可选)从卡带头自动识别 ROM 大小 -> 分配缓存并预读头部。
// 配置文件格式 (扩展名 .gb 或 .gbc, 内容为文本):
//   READREALTIME
//   <ROM大小字节数, 0=从卡带头自动识别>
//   SERIAL
//   <串口地址 或 AUTO>
//   [MBC5_BANK_SHIFT1]   <- 可选: Chis 复制卡 bank 偏移特性
// 成功后 *rom_data_out 指向 ROM 缓冲(所有权移交调用方), *rom_size_out 为大小。
static bool gb_serial_directplay_load(const uint8_t* config, size_t config_len,
                                      const char* save_base_path, sb_cart_serial_t* cs,
                                      uint8_t** rom_data_out, size_t* rom_size_out) {
  // 逐行解析 (兼容 \r\n)
  char text[512];
  size_t n = config_len < sizeof(text) - 1 ? config_len : sizeof(text) - 1;
  memcpy(text, config, n);
  text[n] = '\0';
  for (size_t i = 0; i < n; ++i) if (text[i] == '\r') text[i] = '\n';
  char* lines[8] = {0};
  int line_count = 0;
  for (char* tok = strtok(text, "\n"); tok && line_count < 8; tok = strtok(NULL, "\n"))
    lines[line_count++] = tok;
  if (line_count < 4 || strcmp(lines[0], "READREALTIME") != 0) {
    log_printf("[GB Serial] Invalid realtime config\n");
    return false;
  }
  size_t rom_size = strtoul(lines[1], NULL, 0);
  if (strcmp(lines[2], "SERIAL") != 0) {
    log_printf("[GB Serial] Unsupported protocol: %s (only SERIAL)\n", lines[2]);
    return false;
  }
  const char* address = lines[3];
  int flags = CS_GB_FLAG_NONE;
  for (int i = 4; i < line_count; ++i) {
    if (strcmp(lines[i], "MBC5_BANK_SHIFT1") == 0) flags |= CS_GB_FLAG_MBC5_SHIFT1;
  }
  log_printf("[GB Serial] DirectPlay config: size=%zu addr=%s flags=0x%x\n", rom_size, address, flags);

  serial_port_t port = cs_serial_open(address);
  if (port == INVALID_SERIAL_PORT) {
    log_printf("[GB Serial] Failed to open serial port: %s\n", address);
    return false;
  }
  cs->rom_source_serial = malloc(sizeof(serial_port_t));
  if (!cs->rom_source_serial) { cs_serial_close(port); return false; }
  *(serial_port_t*)cs->rom_source_serial = port;
  cs->rom_protocol = SB_CART_PROTOCOL_SERIAL;
  cs->custom_backup_type = flags;
  cs->serial_write_count = 0;

  // 固件怪癖: 复位后第一条命令会被吞, 先 warm-up (对齐 chis-burner-cmd 的 gbc_warm_up)
  uint8_t warm = 0xF0;
  gb_serial_bus_write(port, 0x0000, &warm, 1);
  gb_serial_bus_write(port, 0x0000, &warm, 1);

  if (rom_size == 0) {
    if (!gb_serial_detect_rom_size(cs, &rom_size)) {
      cs_serial_shutdown(cs);
      return false;
    }
  }
  if (rom_size > 8*1024*1024) {
    log_printf("[GB Serial] ROM size %zu too large\n", rom_size);
    cs_serial_shutdown(cs);
    return false;
  }
  cs->rom_cache_data = (uint8_t*)malloc(rom_size);
  cs->rom_cache_valid = (uint8_t*)calloc(rom_size, 1);
  if (!cs->rom_cache_data || !cs->rom_cache_valid) {
    log_printf("[GB Serial] Failed to allocate ROM cache (%zu bytes)\n", rom_size);
    free(cs->rom_cache_data); cs->rom_cache_data = NULL;
    cs_serial_shutdown(cs);
    return false;
  }
  memset(cs->rom_cache_data, 0xFF, rom_size);
  snprintf(cs->rom_cache_path, SB_FILE_PATH_SIZE, "%s_rom_cache.gb", save_base_path);
  cs->realtime_rom_size = rom_size;
  cs->use_realtime_rom = true;

  // 预读卡带头 0x0100-0x014F 到缓存, 让 sb_load_rom 的常规头解析直接命中
  if (!gb_serial_bus_read(port, 0x0100, cs->rom_cache_data + 0x0100, 0x50)) {
    log_printf("[GB Serial] Failed to prefetch cart header\n");
    free(cs->rom_cache_data); cs->rom_cache_data = NULL;
    cs_serial_shutdown(cs);
    return false;
  }
  memset(cs->rom_cache_valid + 0x0100, 1, 0x50);

  *rom_data_out = cs->rom_cache_data;
  *rom_size_out = rom_size;
  log_printf("[GB Serial] DirectPlay ready: %zu KB ROM\n", rom_size / 1024);
  return true;
}
