// 编译自检 TU: 验证 cart_serial 分层与核心头文件 (gba.h/gb.h) 可独立编译。
// 用法: gcc -fsyntax-only -I src tools/compile_check.c
#include "shared.h"   // SE_AUDIO_SAMPLE_RATE 等由上游约定在 gba.h/gb.h 之前提供
#include "gba.h"
#include "gb.h"
#include "cart_serial/serial_port.h"
#include "cart_serial/cart_serial_base.h"
#include "cart_serial/gba_cart_serial.h"
#include "cart_serial/gb_cart_serial.h"

int skyemu_cart_serial_compile_check(void) {
  gba_scratch_t gba_scratch = {0};
  gb_scratch_t gb_scratch = {0};
  sb_cart_serial_t* cs = &gb_scratch.serial;
  cs_sync_note_dirty(cs);
  if (cs_sync_should_write(cs, false)) cs_sync_done(cs);
  uint8_t b = gb_serial_read_rom_byte(cs, 0);
  (void)b;
  gb_serial_forward_bus_write(cs, 0x2000, 1);
  gba_read_rom_byte(&gba_scratch, 0);
  return 0;
}
