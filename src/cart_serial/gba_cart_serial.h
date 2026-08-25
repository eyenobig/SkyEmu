// gba_cart_serial.h - GBA 平台层 DirectPlay
// ChisFlash 固件 GBA 命令族: 0xF5 写ROM(字地址) / 0xF6 读ROM / 0xF7 写SRAM /
// 0xF8 读SRAM / 0xF9 Flash编程。提取自 ChisBread/SkyEmu dev_readfromserial。
#pragma once
#include "cart_serial/cart_serial_base.h"

// 根据Python协议实现串口读取ROM
// Python代码: cmd.extend(struct.pack("<H", 2 + 1 + 4 + 2 + 2))
//            cmd.append(0xf6)
//            cmd.extend(struct.pack("<I", addr_word << 1))
//            cmd.extend(struct.pack("<H", length_byte))
//            cmd.extend([0, 0])
static bool gba_serial_read_rom(serial_port_t port, uint32_t addr_word, uint8_t* buffer, uint16_t length_byte) {
  // 构建命令包: readRom(addr_word, length_byte)
  // 注意：Python中addr_word是字地址，传输时需要<<1转为字节地址
  uint8_t cmd[11];
  uint16_t cmd_len = 11; // 2(长度) + 1(命令) + 4(地址) + 2(长度) + 2(填充) = 11
  
  // 填充命令总长度(小端序)
  cmd[0] = cmd_len & 0xFF;
  cmd[1] = (cmd_len >> 8) & 0xFF;
  
  // 命令码 0xf6 (读ROM)
  cmd[2] = 0xf6;
  
  // 地址：将字地址转为字节地址(addr_word << 1)，小端序
  uint32_t byte_addr = addr_word << 1;
  cmd[3] = byte_addr & 0xFF;
  cmd[4] = (byte_addr >> 8) & 0xFF;
  cmd[5] = (byte_addr >> 16) & 0xFF;
  cmd[6] = (byte_addr >> 24) & 0xFF;
  
  // 数据长度(小端序)
  cmd[7] = length_byte & 0xFF;
  cmd[8] = (length_byte >> 8) & 0xFF;
  
  // 填充字节
  cmd[9] = 0;
  cmd[10] = 0;
  
  // 打印详细的命令信息
  // printf("\n[Serial] === Read ROM Command ===\n");
  // printf("[Serial] addr_word=0x%08x, byte_addr=0x%08x, length=%u bytes\n", 
  //        addr_word, byte_addr, length_byte);
  // printf("[Serial] Command bytes (11): ");
  // for (int i = 0; i < 11; i++) {
  //   printf("%02x ", cmd[i]);
  // }
  // printf("\n");
  // printf("[Serial] Breakdown:\n");
  // printf("[Serial]   Length: %02x %02x (%u)\n", cmd[0], cmd[1], cmd_len);
  // printf("[Serial]   Command: %02x\n", cmd[2]);
  // printf("[Serial]   Address: %02x %02x %02x %02x (0x%08x)\n", 
  //        cmd[3], cmd[4], cmd[5], cmd[6], byte_addr);
  // printf("[Serial]   Data Length: %02x %02x (%u)\n", cmd[7], cmd[8], length_byte);
  // printf("[Serial]   Padding: %02x %02x\n", cmd[9], cmd[10]);
  
  // 清空串口缓冲区
#ifndef _WIN32
  tcflush(port, TCIOFLUSH);
#else
  PurgeComm(port, PURGE_RXCLEAR | PURGE_TXCLEAR);
#endif
  
  // 发送命令
  // printf("[Serial] Sending command...\n");
  size_t written = cs_serial_write(port, cmd, 11);
  if (written != 11) {
    log_printf("[Serial] ERROR: Failed to write command (wrote %zu/11 bytes)\n", written);
    return false;
  }
  // printf("[Serial] Command sent successfully (%zu bytes)\n", written);
  
  
  // 读取响应 (length_byte + 2)
  size_t response_size = length_byte + 2;
  uint8_t* response = (uint8_t*)malloc(response_size);
  if (!response) {
    log_printf("[Serial] ERROR: Failed to allocate %zu bytes for response\n", response_size);
    return false;
  }
  
  // 尝试一次性读取所有数据(类似Python的ser.read(size))
  size_t total_read = 0;
  int retry_count = 0;
  const int max_retries = 30; // 减少重试次数，但增加每次等待时间
  
  while (total_read < response_size && retry_count < max_retries) {
    size_t read_count = cs_serial_read(port, response + total_read, response_size - total_read);
    if (read_count > 0) {
      total_read += read_count;
      // printf("[Serial] Read %zu bytes, total: %zu/%zu (%.1f%%)\n", 
      //        read_count, total_read, response_size, (total_read * 100.0) / response_size);
      // 如果还没读完，稍微等待一下
      if (total_read < response_size) {
#ifdef _WIN32
        Sleep(1);
#else
        usleep(1000);
#endif
      }
    } else {
      retry_count++;
      if (retry_count == 1 || retry_count % 5 == 0) {
        log_printf("[Serial] Waiting for data... retry %d, received: %zu/%zu bytes\n", 
               retry_count, total_read, response_size);
      }
      // 每次重试等待更长时间
#ifdef _WIN32
      Sleep(10);
#else
      usleep(10000);
#endif
    }
  }
  
  if (total_read < response_size) {
    log_printf("[Serial] ERROR: Failed to read complete response (read %zu/%zu bytes after %d retries)\n", 
           total_read, response_size, retry_count);
    if (total_read > 0) {
      log_printf("[Serial] Received data: ");
      for (size_t i = 0; i < (total_read < 32 ? total_read : 32); i++) {
        log_printf("%02x ", response[i]);
      }
      if (total_read > 32) printf("...");
      log_printf("\n");
    }
    free(response);
    return false;
  }
  
  // printf("[Serial] Response received successfully\n");
  // printf("[Serial] First 16 bytes: ");
  // for (size_t i = 0; i < (response_size < 16 ? response_size : 16); i++) {
  //   printf("%02x ", response[i]);
  // }
  // if (response_size > 16) printf("...");
  // printf("\n");
  
  // 跳过前2字节，复制数据
  memcpy(buffer, response + 2, length_byte);
  free(response);
  
  return true;
}

// 刷新串口写入缓存 - 合并连续地址的写入(ROM和RAM分别合并)
static void gba_flush_serial_writes(gba_scratch_t *scratch) {
  if (!scratch || scratch->serial_write_count == 0) {
    return;
  }
  
  static int flush_count = 0;
  if (flush_count < 10 || flush_count % 100 == 0) {
    log_printf("[Serial] Flushing write buffer #%d: %d writes pending\n", 
           flush_count, scratch->serial_write_count);
  }
  flush_count++;
  
  serial_port_t port = *(serial_port_t*)scratch->rom_source_serial;
  
  // 合并连续地址的写入(保持原始顺序，ROM和RAM分别合并)
  int i = 0;
  static int batch_count = 0;
  
  while (i < scratch->serial_write_count) {
    sb_cart_write_entry_t *start = &scratch->serial_write_buffer[i];
    
    // 查找连续的写入(必须是同类型且地址连续)
    int count = 1;
    while (i + count < scratch->serial_write_count) {
      sb_cart_write_entry_t *next = &scratch->serial_write_buffer[i + count];
      // 关键检查：同类型(ROM/RAM)且地址连续
      if (next->is_rom == start->is_rom && 
          next->addr == start->addr + count) {
        count++;
      } else {
        break;  // 遇到不同类型或地址不连续，停止合并
      }
    }
    
    // 如果找到多个连续的写入，合并发送
    if (count > 1) {
      // 分配数据缓冲区
      uint8_t *data = (uint8_t*)malloc(count);
      if (data) {
        for (int j = 0; j < count; j++) {
          data[j] = scratch->serial_write_buffer[i + j].data;
        }
        
        if (batch_count < 200 || batch_count % 400 == 0) {
          log_printf("[Serial] Batch #%d: %s addr=0x%08x, %d bytes (merged) ", 
                 batch_count, start->is_rom ? "ROM" : "RAM", start->addr, count);
          //print data (first 16byte)
          for (int j = 0; j < (count < 16 ? count : 16); j++) {
            log_printf("%02x ", data[j]);
          }
          if (count > 16) printf("...");
          printf("\n");
        }
        batch_count++;
        
        // 根据类型选择写入函数
        if (start->is_rom) {
          uint32_t addr_word = start->addr >> 1; // 转为字地址
          gba_serial_write_rom(port, addr_word, data, count);
        } else {
          gba_serial_write_ram(port, start->addr, data, count);
        }
        
        free(data);
      }
    } else {
      // 单个写入
      if (batch_count < 200 || batch_count % 400 == 0) {
        log_printf("[Serial] Batch #%d: %s addr=0x%08x, 1 byte\n", 
               batch_count, start->is_rom ? "ROM" : "RAM", start->addr);
      }
      batch_count++;
      
      if (start->is_rom) {
        uint32_t addr_word = start->addr >> 1; // 转为字地址
        uint16_t data = start->data; // 单字节数据
        gba_serial_write_rom(port, addr_word, (uint8_t*)&data, 2);
      } else {
        gba_serial_write_ram(port, start->addr, &start->data, 1);
      }
    }
    
    i += count;
  }
  
  // 清空缓存
  scratch->serial_write_count = 0;
}

// 添加写入到缓存
static void gba_buffer_serial_write(gba_scratch_t *scratch, uint32_t addr, uint8_t data, bool is_rom) {
  if (!scratch) return;
  
  // 任何写入(无论ROM还是RAM)都会使RAM预读缓存失效
  if (scratch->ram_prefetch_valid) {
    scratch->ram_prefetch_valid = false;
      log_printf("[Serial] RAM prefetch cache invalidated due to %s write at 0x%08x\n",
            is_rom ? "ROM" : "RAM", addr);
  }
  // 疑似写命令,部分地址标记为不缓存
  if (data == 0x30 || data == 0x20 || data == 0x60) {
    for (int i = 0; i < 512; i++) {
        scratch->rom_cache_valid[addr + i] = 0xFF;
    }
  }
  scratch->rom_cache_valid[addr] = 0xFF;

  // 如果缓存满了，先刷新
  if (scratch->serial_write_count >= SB_CART_WRITE_BUFFER_SIZE) {
    gba_flush_serial_writes(scratch);
  }
  
  // 添加到缓存
  sb_cart_write_entry_t *entry = &scratch->serial_write_buffer[scratch->serial_write_count++];
  entry->addr = addr;
  entry->data = data;
  entry->is_rom = is_rom;
}

// 串口写入ROM(命令0xf5)
// Python: writeRom(addr_word, dat)
static bool gba_serial_write_rom(serial_port_t port, uint32_t addr_word, const uint8_t* data, uint16_t data_len) {
  if (port == INVALID_SERIAL_PORT || !data || data_len == 0) {
    return false;
  }
  
  // 构建命令包
  uint16_t cmd_len = 2 + 1 + 4 + data_len + 2;
  uint8_t* cmd = (uint8_t*)malloc(cmd_len);
  if (!cmd) return false;
  
  // Python: struct.pack("<H", 2 + 1 + 4 + len(dat) + 2)
  // 长度字段包含：长度字段自己(2) + 命令(1) + 地址(4) + 数据(data_len) + 填充(2)
  uint16_t body_len = 2 + 1 + 4 + data_len + 2;
  cmd[0] = body_len & 0xFF;
  cmd[1] = (body_len >> 8) & 0xFF;
  cmd[2] = 0xf5; // 写ROM命令
  
  // 地址(字地址，不需要<<1)
  cmd[3] = addr_word & 0xFF;
  cmd[4] = (addr_word >> 8) & 0xFF;
  cmd[5] = (addr_word >> 16) & 0xFF;
  cmd[6] = (addr_word >> 24) & 0xFF;
  
  // 数据
  memcpy(cmd + 7, data, data_len);
  
  // 填充
  cmd[7 + data_len] = 0;
  cmd[8 + data_len] = 0;
  
  // 发送命令
  size_t written = cs_serial_write(port, cmd, cmd_len);
  free(cmd);
  
  if (written != cmd_len) {
    log_printf("[Serial] ERROR: Failed to write ROM command\n");
    return false;
  }
  
  // 尝试读取1字节ACK(非阻塞，超时后继续)
  uint8_t ack;
  size_t ack_read = 0;
  int retry = 0;
  while (ack_read == 0 && retry < 5) {
    ack_read = cs_serial_read(port, &ack, 1);
    if (ack_read == 0) {
      retry++;
#ifdef _WIN32
      Sleep(1);
#else
      usleep(1000);
#endif
    }
  }
  
  // ACK读取失败只记录警告，不中断操作
  if (ack_read != 1) {
    static int warn_count = 0;
    if (warn_count < 3) {
      log_printf("[Serial] WARNING: No ACK for ROM write (retry %d)\n", retry);
      warn_count++;
    }
  }
  
  return true;
}

// 串口读取RAM(命令0xf8)
// Python: readRam(addr, length_byte)
static bool gba_serial_read_ram(serial_port_t port, uint32_t addr, uint8_t* buffer, uint16_t length_byte) {
  if (port == INVALID_SERIAL_PORT || !buffer || length_byte == 0) {
    return false;
  }
  
  // 构建命令包
  uint8_t cmd[11];
  // Python: struct.pack("<H", 2 + 1 + 4 + 2 + 2) = 11
  // 长度字段包含：长度字段自己(2) + 命令(1) + 地址(4) + 数据长度(2) + 填充(2)
  uint16_t cmd_body_len = 2 + 1 + 4 + 2 + 2;

  cmd[0] = cmd_body_len & 0xFF;
  cmd[1] = (cmd_body_len >> 8) & 0xFF;
  cmd[2] = 0xf8; // 读RAM命令
  
  // 地址(字节地址)
  cmd[3] = addr & 0xFF;
  cmd[4] = (addr >> 8) & 0xFF;
  cmd[5] = (addr >> 16) & 0xFF;
  cmd[6] = (addr >> 24) & 0xFF;
  
  // 长度
  cmd[7] = length_byte & 0xFF;
  cmd[8] = (length_byte >> 8) & 0xFF;
  
  // 填充
  cmd[9] = 0;
  cmd[10] = 0;
  
  // 发送命令
  size_t written = cs_serial_write(port, cmd, 11);
  if (written != 11) {
    log_printf("[Serial] ERROR: Failed to write RAM read command\n");
    return false;
  }
  
  // 读取响应 (length_byte + 2)
  size_t response_size = length_byte + 2;
  uint8_t* response = (uint8_t*)malloc(response_size);
  if (!response) {
    return false;
  }
  
  size_t total_read = 0;
  int retry_count = 0;
  const int max_retries = 20;
  
  while (total_read < response_size && retry_count < max_retries) {
    size_t read_count = cs_serial_read(port, response + total_read, response_size - total_read);
    if (read_count > 0) {
      total_read += read_count;
      if (total_read < response_size) {
#ifdef _WIN32
        Sleep(1);
#else
        usleep(1000);
#endif
      }
    } else {
      retry_count++;
#ifdef _WIN32
      Sleep(10);
#else
      usleep(10000);
#endif
    }
  }
  
  if (total_read < response_size) {
    log_printf("[Serial] ERROR: Failed to read RAM response (%zu/%zu bytes)\n", total_read, response_size);
    free(response);
    return false;
  }
  
  // 跳过前2字节，复制数据
  memcpy(buffer, response + 2, length_byte);
  free(response);
  
  return true;
}

// 串口写入RAM(命令0xf7)
// Python: writeRam(addr, dat)
static bool gba_serial_write_ram(serial_port_t port, uint32_t addr, const uint8_t* data, uint16_t data_len) {
  if (port == INVALID_SERIAL_PORT || !data || data_len == 0) {
    return false;
  }
  
  // 构建命令包
  uint16_t cmd_len = 2 + 1 + 4 + data_len + 2;
  uint8_t* cmd = (uint8_t*)malloc(cmd_len);
  if (!cmd) return false;
  
  // Python: struct.pack("<H", 2 + 1 + 4 + len(dat) + 2)
  // 长度字段包含：长度字段自己(2) + 命令(1) + 地址(4) + 数据(data_len) + 填充(2)
  uint16_t body_len = 2 + 1 + 4 + data_len + 2;
  cmd[0] = body_len & 0xFF;
  cmd[1] = (body_len >> 8) & 0xFF;
  cmd[2] = 0xf7; // 写RAM命令
  
  // 地址(字节地址)
  cmd[3] = addr & 0xFF;
  cmd[4] = (addr >> 8) & 0xFF;
  cmd[5] = (addr >> 16) & 0xFF;
  cmd[6] = (addr >> 24) & 0xFF;
  
  // 数据
  memcpy(cmd + 7, data, data_len);
  
  // 填充
  cmd[7 + data_len] = 0;
  cmd[8 + data_len] = 0;
  
  // 发送命令
  size_t written = cs_serial_write(port, cmd, cmd_len);
  free(cmd);
  
  if (written != cmd_len) {
    log_printf("[Serial] ERROR: Failed to write RAM command\n");
    return false;
  }
  
  // 尝试读取1字节ACK(非阻塞，超时后继续)
  uint8_t ack;
  size_t ack_read = 0;
  int retry = 0;
  while (ack_read == 0 && retry < 5) {
    ack_read = cs_serial_read(port, &ack, 1);
    if (ack_read == 0) {
      retry++;
#ifdef _WIN32
      Sleep(1);
#else
      usleep(1000);
#endif
    }
  }
  
  // ACK读取失败只记录警告，不中断操作
  if (ack_read != 1) {
    static int warn_count = 0;
    if (warn_count < 3) {
      log_printf("[Serial] WARNING: No ACK for RAM write (retry %d)\n", retry);
      warn_count++;
    }
  }
  
  return true;
}

// 串口Flash编程(命令0xf9)
// 用于将Flash内容写入到卡带的物理Flash
static bool gba_serial_flash_program(serial_port_t port, uint32_t addr, const uint8_t* data, uint16_t data_len) {
  if (port == INVALID_SERIAL_PORT || !data || data_len == 0) {
    return false;
  }
  
  // 构建命令包
  uint16_t cmd_len = 2 + 1 + 4 + data_len + 2;
  uint8_t* cmd = (uint8_t*)malloc(cmd_len);
  if (!cmd) return false;
  
  // 长度字段包含：长度字段自己(2) + 命令(1) + 地址(4) + 数据(data_len) + 填充(2)
  uint16_t body_len = 2 + 1 + 4 + data_len + 2;
  cmd[0] = body_len & 0xFF;
  cmd[1] = (body_len >> 8) & 0xFF;
  cmd[2] = 0xf9; // Flash编程命令
  
  // 地址(字节地址)
  cmd[3] = addr & 0xFF;
  cmd[4] = (addr >> 8) & 0xFF;
  cmd[5] = (addr >> 16) & 0xFF;
  cmd[6] = (addr >> 24) & 0xFF;
  
  // 数据
  memcpy(cmd + 7, data, data_len);
  
  // 填充
  cmd[7 + data_len] = 0;
  cmd[8 + data_len] = 0;
  
  // 发送命令
  size_t written = cs_serial_write(port, cmd, cmd_len);
  free(cmd);
  
  if (written != cmd_len) {
    log_printf("[Serial] ERROR: Failed to write Flash program command\n");
    return false;
  }
  
  // Flash编程需要更长的等待时间
#ifdef _WIN32
  Sleep(100);
#else
  usleep(100000);
#endif
  
  // 读取ACK
  uint8_t ack;
  size_t ack_read = 0;
  int retry = 0;
  while (ack_read == 0 && retry < 10) {
    ack_read = cs_serial_read(port, &ack, 1);
    if (ack_read == 0) {
      retry++;
#ifdef _WIN32
      Sleep(10);
#else
      usleep(10000);
#endif
    }
  }
  
  if (ack_read != 1 || ack != 0xAA) {
    log_printf("[Serial] WARNING: No ACK for Flash program (retry %d)\n", retry);
    return false;
  }
  
  return true;
}

// 读取backup字节(支持串口)
static uint8_t gba_read_backup_byte(gba_t* gba, uint32_t addr) {
  // 检查是否使用串口ROM
  if (gba->scratch && gba->scratch->use_realtime_rom && 
      gba->scratch->rom_protocol == SB_CART_PROTOCOL_SERIAL) {
    // 串口模式：读取前先刷新所有缓存的写入
    if (gba->scratch->serial_write_count > 0) {
      gba_flush_serial_writes(gba->scratch);
    }
    
    // 通过串口读取RAM
    serial_port_t port = *(serial_port_t*)gba->scratch->rom_source_serial;
    uint8_t data = 0xFF;
    if (gba_serial_read_ram(port, addr, &data, 1)) {
      return data;
    }
    return 0xFF;
  }
  // 普通模式：从内存读取
  return gba->mem.cart_backup[addr];
}

// 写入backup字节(支持串口)
static void gba_write_backup_byte(gba_t* gba, uint32_t addr, uint8_t data) {
  // 检查是否使用串口ROM
  if (gba->scratch && gba->scratch->use_realtime_rom && 
      gba->scratch->rom_protocol == SB_CART_PROTOCOL_SERIAL) {
    // 通过串口写入RAM
    serial_port_t port = *(serial_port_t*)gba->scratch->rom_source_serial;
    gba_serial_write_ram(port, addr, &data, 1);
    return;
  }
  // 普通模式：写入内存
  gba->mem.cart_backup[addr] = data;
}

// 字节级ROM读取 - 支持按需加载和缓存
// 当缓存未命中时，会读取一整块数据以优化性能
#define GBA_ROM_CACHE_CHUNK_SIZE 4096

static uint8_t gba_read_rom_byte(gba_scratch_t *scratch, size_t offset) {
  if (!scratch->use_realtime_rom) {
    return 0xFF; // 不应该调用到这里
  }
  
  // 串口模式：读取前先刷新所有缓存的写入
  if (scratch->rom_protocol == SB_CART_PROTOCOL_SERIAL && scratch->serial_write_count > 0) {
    gba_flush_serial_writes(scratch);
  }
  
  if (offset >= scratch->realtime_rom_size) {
    return 0xFF;
  }
  
  // 检查缓存是否已有这个字节
  if (scratch->rom_cache_valid[offset] == 1) {
    return scratch->rom_cache_data[offset];
  }

  // 缓存未命中，读取一整块数据以优化后续访问
  // 计算块的起始地址(对齐到块大小)
  size_t chunk_start = (offset / GBA_ROM_CACHE_CHUNK_SIZE) * GBA_ROM_CACHE_CHUNK_SIZE;
  size_t chunk_size = GBA_ROM_CACHE_CHUNK_SIZE;
  
  // 确保不超出ROM大小
  if (chunk_start + chunk_size > scratch->realtime_rom_size) {
    chunk_size = scratch->realtime_rom_size - chunk_start;
  }
  
  // 检查这个块是否已经部分或全部缓存
  bool need_read = false;
  for (size_t i = 0; i < chunk_size; i++) {
    if (scratch->rom_cache_valid[chunk_start + i] != 1) {
      need_read = true;
      break;
    }
  }

  if (scratch->rom_cache_valid[offset] == 0xFF) {
    chunk_size = 2; // 如果这个字节标记为不缓存，缩小块大小以减少无用读取
    chunk_start = offset - (offset % 2); // 对齐到2字节
    need_read = true;
  }
  
  if (need_read) {
    // 读取整个块
    bool success = false;
    uint8_t* temp_buffer = (uint8_t*)malloc(chunk_size);
    if (!temp_buffer) {
      return 0xFF;
    }
    
    switch (scratch->rom_protocol) {
      case SB_CART_PROTOCOL_FILE:
        if (scratch->rom_source_file) {
          success = cs_read_file_bytes(scratch->rom_source_file, chunk_start, temp_buffer, chunk_size);
        }
        break;
      
      case SB_CART_PROTOCOL_SERIAL:
        if (scratch->rom_source_serial) {
          serial_port_t port = *(serial_port_t*)scratch->rom_source_serial;
          uint32_t addr_word = chunk_start / 2;
          success = gba_serial_read_rom(port, addr_word, temp_buffer, chunk_size);
          // if (success) {
          //   printf("Loaded ROM chunk: offset=0x%zx, size=%zu bytes\n", chunk_start, chunk_size);
          // }
        }
        break;
      
      case SB_CART_PROTOCOL_NET:
        // TODO: 实现网络块读取
        break;
      
      default:
        break;
    }
    
    // 更新缓存
    if (success) {
      memcpy(scratch->rom_cache_data + chunk_start, temp_buffer, chunk_size);
      for (size_t i = 0; i < chunk_size; i++) {
        if (scratch->rom_cache_valid[chunk_start + i] == 0) {
          scratch->rom_cache_valid[chunk_start + i] = 1;
        }
      }
    }
    
    free(temp_buffer);
  }
  // if (scratch->rom_cache_valid[offset] == 0xFF) {
  //   log_printf("Byte at offset 0x%zx is marked as non-cacheable, value=0x%02x\n", 
  //          offset, scratch->rom_cache_data[offset]);
  // }
  return scratch->rom_cache_data[offset];
}



// 串口模式下切换Flash bank
static void gba_serial_flash_switch_bank(serial_port_t port, int bank) {
  bank = (bank == 0) ? 0 : 1;
  
  log_printf("[Serial] Switching Flash bank to %d\n", bank);
  
  // 发送bank切换命令序列
  uint8_t cmd_aa = 0xAA;
  uint8_t cmd_55 = 0x55;
  uint8_t cmd_b0 = 0xB0;
  uint8_t bank_byte = (uint8_t)bank;
  
  gba_serial_write_ram(port, 0x5555, &cmd_aa, 1);
  gba_serial_write_ram(port, 0x2AAA, &cmd_55, 1);
  gba_serial_write_ram(port, 0x5555, &cmd_b0, 1);
  gba_serial_write_ram(port, 0x0000, &bank_byte, 1);
}

// 串口模式下切换SRAM_128K bank (写入0x09000000切换)
static void gba_serial_sram_switch_bank(serial_port_t port, int bank) {
  bank = (bank == 0) ? 0 : 1;
  
  log_printf("[Serial] Switching SRAM bank to %d\n", bank);
  uint16_t bank_byte = bank;
  gba_serial_write_rom(port, 0x01000000>>1, (uint8_t*)&bank_byte, 2);
}

// 串口模式下擦除Flash芯片
static bool gba_serial_flash_erase_chip(serial_port_t port) {
  log_printf("[Serial] Erasing Flash chip...\n");
  
  // 发送Chip-Erase命令序列
  uint8_t cmd_aa = 0xAA;
  uint8_t cmd_55 = 0x55;
  uint8_t cmd_80 = 0x80;
  uint8_t cmd_10 = 0x10;
  
  gba_serial_write_ram(port, 0x5555, &cmd_aa, 1);
  gba_serial_write_ram(port, 0x2AAA, &cmd_55, 1);
  gba_serial_write_ram(port, 0x5555, &cmd_80, 1);
  gba_serial_write_ram(port, 0x5555, &cmd_aa, 1);
  gba_serial_write_ram(port, 0x2AAA, &cmd_55, 1);
  gba_serial_write_ram(port, 0x5555, &cmd_10, 1); // Chip-Erase
  
  // 等待擦除完成(轮询0x0000地址，直到返回0xFF)
  log_printf("[Serial] Waiting for erase to complete...\n");
  uint8_t status;
  int max_retries = 60; // 最多等待60秒
  int retry = 0;
  
  while (retry < max_retries) {
    // 等待1秒
#ifdef _WIN32
    Sleep(1000);
#else
    usleep(1000000);
#endif
    
    // 读取状态
    if (!gba_serial_read_ram(port, 0x0000, &status, 1)) {
      log_printf("[Serial] ERROR: Failed to read erase status\n");
      return false;
    }
    
    log_printf("[Serial] Erase status: 0x%02x\n", status);
    
    if (status == 0xFF) {
      log_printf("[Serial] Flash erase completed\n");
      return true;
    }
    
    retry++;
  }
  
  log_printf("[Serial] ERROR: Flash erase timeout after %d seconds\n", max_retries);
  return false;
}

// 串口模式下加载Flash存档
static bool gba_serial_load_flash_backup(gba_t* gba, uint32_t flash_size) {
  if (!gba || !gba->scratch || !gba->scratch->rom_source_serial) {
    return false;
  }
  
  serial_port_t port = *(serial_port_t*)gba->scratch->rom_source_serial;
  
  log_printf("[Serial] Loading Flash backup from cartridge (%u bytes)...\n", flash_size);
  
  // 如果是128KB Flash，需要分两个bank读取
  if (flash_size == 128 * 1024) {
    // 读取Bank 0 (前64KB)
    log_printf("[Serial] Reading Flash bank 0...\n");
    gba_serial_flash_switch_bank(port, 0);
    
    const uint32_t chunk_size = 4096;
    for (uint32_t offset = 0; offset < 64 * 1024; offset += chunk_size) {
      if (!gba_serial_read_ram(port, offset, gba->mem.cart_backup + offset, chunk_size)) {
        log_printf("[Serial] ERROR: Failed to read Flash bank 0 at offset 0x%05x\n", offset);
        return false;
      }
    }
    
    // 读取Bank 1 (后64KB)
    log_printf("[Serial] Reading Flash bank 1...\n");
    gba_serial_flash_switch_bank(port, 1);
    
    for (uint32_t offset = 0; offset < 64 * 1024; offset += chunk_size) {
      if (!gba_serial_read_ram(port, offset, gba->mem.cart_backup + 64 * 1024 + offset, chunk_size)) {
        log_printf("[Serial] ERROR: Failed to read Flash bank 1 at offset 0x%05x\n", offset);
        return false;
      }
    }
    
    // 切换回bank 0
    gba_serial_flash_switch_bank(port, 0);
    
  } else {
    // 64KB Flash，直接读取
    const uint32_t chunk_size = 4096;
    for (uint32_t offset = 0; offset < flash_size; offset += chunk_size) {
      if (!gba_serial_read_ram(port, offset, gba->mem.cart_backup + offset, chunk_size)) {
        log_printf("[Serial] ERROR: Failed to read Flash at offset 0x%05x\n", offset);
        return false;
      }
    }
  }
  
  log_printf("[Serial] Flash backup loaded successfully\n");
  return true;
}

// 串口模式下加载SRAM_128K存档
static bool gba_serial_load_sram_backup(gba_t* gba, int size) {
  if (!gba || !gba->scratch || !gba->scratch->rom_source_serial) {
    return false;
  }
  
  serial_port_t port = *(serial_port_t*)gba->scratch->rom_source_serial;
  
  log_printf("[Serial] Loading SRAM_128K backup from cartridge (128KB)...\n");
  
  const uint32_t chunk_size = 4096;
  
  // 读取Bank 0 (前64KB)
  log_printf("[Serial] Reading SRAM bank 0...\n");
  gba_serial_sram_switch_bank(port, 0);
  
  for (uint32_t offset = 0; offset < 64 * 1024; offset += chunk_size) {
    if (!gba_serial_read_ram(port, offset, gba->mem.cart_backup + offset, chunk_size)) {
      log_printf("[Serial] ERROR: Failed to read SRAM bank 0 at offset 0x%05x\n", offset);
      return false;
    }
  }
  if (size <= 64 * 1024) {
    log_printf("[Serial] SRAM_128K backup loaded successfully (only bank 0 used)\n");
    return true;
  }
  // 读取Bank 1 (后64KB)
  log_printf("[Serial] Reading SRAM bank 1...\n");
  gba_serial_sram_switch_bank(port, 1);
  
  for (uint32_t offset = 0; offset < 64 * 1024; offset += chunk_size) {
    if (!gba_serial_read_ram(port, offset, gba->mem.cart_backup + 64 * 1024 + offset, chunk_size)) {
      log_printf("[Serial] ERROR: Failed to read SRAM bank 1 at offset 0x%05x\n", offset);
      return false;
    }
  }
  
  // 切换回bank 0
  gba_serial_sram_switch_bank(port, 0);
  
  log_printf("[Serial] SRAM_128K backup loaded successfully\n");
  return true;
}

// 串口模式下同步Flash到卡带
// 当backup_is_dirty变为false时调用，表示发生了存档
static void gba_serial_sync_flash_backup(gba_t* gba, int size) {
  if (!gba || !gba->scratch) return;
  
  // 检查是否是串口模式
  if (!gba->scratch->use_realtime_rom || 
      gba->scratch->rom_protocol != SB_CART_PROTOCOL_SERIAL) {
    return;
  }
  
  // 检查是否是Flash类型
  if (gba->cart.backup_type != GBA_BACKUP_FLASH_64K && 
      gba->cart.backup_type != GBA_BACKUP_FLASH_128K) {
    return;
  }
  
  serial_port_t port = *(serial_port_t*)gba->scratch->rom_source_serial;
  
  log_printf("[Serial] Syncing Flash backup to 卡带 (%u bytes)...\n", size);
  
  const uint32_t chunk_size = 4096;
  bool success = true;
  
  // 如果是128KB Flash，需要分两个bank写入
  if (size == 128 * 1024) {
    // 切换到Bank 0并擦除
    log_printf("[Serial] Programming Flash bank 0...\n");
    gba_serial_flash_switch_bank(port, 0);
    
    // 擦除整个芯片
    if (!gba_serial_flash_erase_chip(port)) {
      log_printf("[Serial] ERROR: Failed to erase Flash chip\n");
      return;
    }
    
    for (uint32_t offset = 0; offset < 64 * 1024 && success; offset += chunk_size) {
      log_printf("[Serial] Programming Flash: bank=0, offset=0x%05x, size=%u bytes\n", offset, chunk_size);
      success = gba_serial_flash_program(port, offset, gba->mem.cart_backup + offset, chunk_size);
      if (!success) {
        log_printf("[Serial] ERROR: Flash program failed at bank 0, offset 0x%05x\n", offset);
        break;
      }
    }
    
    if (success) {
      // 写入Bank 1 (后64KB)
      log_printf("[Serial] Programming Flash bank 1...\n");
      gba_serial_flash_switch_bank(port, 1);
      
      for (uint32_t offset = 0; offset < 64 * 1024 && success; offset += chunk_size) {
        log_printf("[Serial] Programming Flash: bank=1, offset=0x%05x, size=%u bytes\n", offset, chunk_size);
        success = gba_serial_flash_program(port, offset, gba->mem.cart_backup + 64 * 1024 + offset, chunk_size);
        if (!success) {
          log_printf("[Serial] ERROR: Flash program failed at bank 1, offset 0x%05x\n", offset);
          break;
        }
      }
      
      // 切换回bank 0
      gba_serial_flash_switch_bank(port, 0);
    }
  } else {
    // 64KB Flash，先擦除再写入
    if (!gba_serial_flash_erase_chip(port)) {
      log_printf("[Serial] ERROR: Failed to erase Flash chip\n");
      return;
    }
    
    for (uint32_t offset = 0; offset < size && success; offset += chunk_size) {
      log_printf("[Serial] Programming Flash: offset=0x%05x, size=%u bytes\n", offset, chunk_size);
      success = gba_serial_flash_program(port, offset, gba->mem.cart_backup + offset, chunk_size);
      if (!success) {
        log_printf("[Serial] ERROR: Flash program failed at offset 0x%05x\n", offset);
        break;
      }
    }
  }
  
  if (success) {
    log_printf("[Serial] Flash sync completed successfully\n");
  } else {
    log_printf("[Serial] Flash sync failed\n");
  }
}

// 串口模式下同步SRAM_128K到卡带
static void gba_serial_sync_sram_backup(gba_t* gba, int size) {
  if (!gba || !gba->scratch) return;
  
  // 检查是否是串口模式
  if (!gba->scratch->use_realtime_rom || 
      gba->scratch->rom_protocol != SB_CART_PROTOCOL_SERIAL) {
    return;
  }
  
  // 检查是否是SRAM_128K类型
  if (gba->cart.backup_type != GBA_BACKUP_SRAM_128K) {
    return;
  }
  
  serial_port_t port = *(serial_port_t*)gba->scratch->rom_source_serial;
  
  log_printf("[Serial] Syncing SRAM_128K backup to cartridge (128KB)...\n");
  
  const uint32_t chunk_size = 4096;
  bool success = true;
  
  // 写入Bank 0 (前64KB)
  log_printf("[Serial] Writing SRAM bank 0...\n");
  gba_serial_sram_switch_bank(port, 0);
  
  for (uint32_t offset = 0; offset < 64 * 1024 && success; offset += chunk_size) {
    log_printf("[Serial] Writing SRAM: bank=0, offset=0x%05x, size=%u bytes\n", offset, chunk_size);
    success = gba_serial_write_ram(port, offset, gba->mem.cart_backup + offset, chunk_size);
    if (!success) {
      log_printf("[Serial] ERROR: SRAM write failed at bank 0, offset 0x%05x\n", offset);
      break;
    }
  }
  
  if (success && size > 64 * 1024) {
    // 写入Bank 1 (后64KB)
    log_printf("[Serial] Writing SRAM bank 1...\n");
    gba_serial_sram_switch_bank(port, 1);
    
    for (uint32_t offset = 0; offset < 64 * 1024 && success; offset += chunk_size) {
      log_printf("[Serial] Writing SRAM: bank=1, offset=0x%05x, size=%u bytes\n", offset, chunk_size);
      success = gba_serial_write_ram(port, offset, gba->mem.cart_backup + 64 * 1024 + offset, chunk_size);
      if (!success) {
        log_printf("[Serial] ERROR: SRAM write failed at bank 1, offset 0x%05x\n", offset);
        break;
      }
    }
    
    // 切换回bank 0
    gba_serial_sram_switch_bank(port, 0);
  }
  
  if (success) {
    log_printf("[Serial] SRAM_128K sync completed successfully\n");
  } else {
    log_printf("[Serial] SRAM_128K sync failed\n");
  }
}


