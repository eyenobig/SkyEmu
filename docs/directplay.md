# DirectPlay — 真实卡带串口直读 (GBA / GB / GBC)

把真实游戏卡带插在 ChisFlash USB 读卡器上（VID `0x0483` / PID `0x0721`，115200 8N1），
SkyEmu 无需先完整 dump ROM，运行时按需从卡带读取字节；存档改动会延迟写回真实卡带。

原始实现来自 [ChisBread/SkyEmu](https://github.com/ChisBread/SkyEmu) `dev_readfromserial`
分支（GBA，DirectPlayV0.6）；本仓库将其重构为分层结构并新增 GB/GBC 支持。

## 代码分层 (`src/cart_serial/`)

| 文件 | 层 | 职责 |
|---|---|---|
| `serial_port.h` | base | 平台串口 I/O（Win/macOS/Linux）、VID/PID 自动发现、DTR 复位、日志 (`cs_serial_*`) |
| `cart_serial_base.h` | base | 共用状态 `sb_cart_serial_t`、ROM 来源协议枚举、ROM 字节缓存、存档延迟同步、统一关闭 (`cs_*`) |
| `gba_cart_serial.h` | GBA | 固件 GBA 命令族 `0xF5~0xF9`（读写 ROM/SRAM、Flash 编程）、备份类型自动检测、bank 切换、存档回写 |
| `gb_cart_serial.h` | GB/GBC | 固件 GB 总线命令族 `0xFA`（写透传）/`0xFB`（读透传）、MBC bank 切换、SRAM 镜像与回写 |

接入点：`src/gba.h` / `src/gb.h` 在总线读写路径调用平台层函数，`src/main.c`
在存档落盘时做延迟同步与回写。

## 使用方法

制作一个小文本文件（UTF-8 / ASCII，扩展名决定平台）作为"ROM"打开即可。

### GBA（扩展名 `.gba`）

```
READREALTIME
<ROM大小字节数>
SERIAL
<串口地址 或 AUTO>
<存档类型 或 AUTO>
```

存档类型：`NONE` `EEPROM` `EEPROM_512B` `EEPROM_8KB` `SRAM` `FLASH_64K` `FLASH_128K`
`SRAM_128K` `AUTO`（自动检测：先试 SRAM 写回读，再用 JEDEC 序列读 Flash ID）。

### GB / GBC（扩展名 `.gb` 或 `.gbc`）

```
READREALTIME
<ROM大小字节数, 0=从卡带头自动识别>
SERIAL
<串口地址 或 AUTO>
[MBC5_BANK_SHIFT1]
```

- ROM 大小填 `0` 时从真实卡带头（0x148）自动识别。
- `MBC5_BANK_SHIFT1`：Chis 复制卡（ChisFlash 系列）的 MBC5 特性——线性 bank N 需要
  写寄存器 N+1。原装卡不要加。
- MBC 类型从卡带头（0x147）自动识别，支持 MBC1 / MBC2 / MBC3 / MBC5 / 无 MBC。
- 启动时真实卡带 SRAM 整块镜像进模拟器（真实卡带为存档权威来源），之后游戏存档
  先写本地 `.sav`，再整块回写卡带。

### 存档同步策略（两平台共用）

游戏写存档后并不立即落盘：连续 10 帧（约 170ms）无新写入才写 `.sav` 并回写真实
卡带，避免存档高频写入打爆串口。ROM 缓存按字节有效性位图管理，退出时落盘为
`<save_base>_rom_cache.gba/.gb`，下次启动直接复用。

## 固件命令参考（与 chis-burner-cmd / ChisFlashBurner 对齐）

帧格式（小端）：`[u16 帧总长][u8 命令码][payload][2B 占位]`；
读应答 = `2B 忽略前缀 + N 字节数据`，写应答 = 单字节 ACK `0xAA`。

| 命令码 | 总线 | 含义 |
|---|---|---|
| 0xF5 | GBA | 写 ROM（字地址，帧内 `<<1` 转字节地址） |
| 0xF6 | GBA | 读 ROM |
| 0xF7 / 0xF8 | GBA | 写 / 读 SRAM |
| 0xF9 | GBA | Flash 编程（固件内做 JEDEC 序列） |
| 0xFA | GB | GB 总线写透传（地址 = GB 总线字节地址，含 MBC 寄存器） |
| 0xFB | GB | GB 总线读透传 |

GB 的 MBC bank 切换通过 0xFA 写寄存器完成（0x0000 RAM 使能、0x2000/0x3000 ROM bank、
0x4000 RAM bank、0x6000 MBC1 模式），与烧录器上位机一致；模拟器自身的 MBC 寄存器写
会原样转发到真实卡带，保持两边状态一致。

## 已知限制

- MBC2 的 512B 内置 SRAM 沿用 SkyEmu 上游行为（按头 0x149 解析，多数 MBC2 卡带读
  到 ram_size=0），直读模式同样如此。
- MBC3 的 RTC 走模拟器内部状态，不同步真实卡带 RTC。
- GB/GBC 直读在原装卡上按标准 MBC 时序切换 bank；Chis 复制卡需加
  `MBC5_BANK_SHIFT1` 标志。
- 串口读卡首次进入游戏时会有数秒的按需缓存填充（115200 波特率所限）。
