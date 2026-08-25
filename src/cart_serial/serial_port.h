// serial_port.h - DirectPlay 基础层: 平台串口 I/O
// 提取自 ChisBread/SkyEmu dev_readfromserial (DirectPlayV0.6), 更名 cs_serial_*。
// 适配 ChisFlash USB 读卡器: VID 0x0483 PID 0x0721, 115200 8N1,
// DTR 翻转复位固件命令缓冲。
#pragma once
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

// 串口自动查找所需头文件
// 注意：Windows 头文件必须在其他标准头文件之后包含
#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
  #include <setupapi.h>
  #include <devguid.h>
  #include <regstr.h>
  #pragma comment(lib, "setupapi.lib")
#elif defined(__APPLE__)
  #include <CoreFoundation/CoreFoundation.h>
  #include <IOKit/IOKitLib.h>
  #include <IOKit/serial/IOSerialKeys.h>
  #include <IOKit/usb/IOUSBLib.h>
#elif defined(__linux__)
  #include <dirent.h>


// 日志输出配置宏
// 日志输出配置宏
#ifndef CART_SERIAL_LOG_ENABLED
  #define CART_SERIAL_LOG_ENABLED 1  // 默认启用日志
#endif

#ifndef CART_SERIAL_LOG_TO_FILE
  #define CART_SERIAL_LOG_TO_FILE 0  // 默认输出到控制台，设置为1则输出到文件
#endif

#ifndef CART_SERIAL_LOG_FILE_PATH
  #define CART_SERIAL_LOG_FILE_PATH "log.txt"  // 默认日志文件路径
#endif

// 日志文件句柄(全局静态变量)
static FILE* log_file_handle = NULL;
static bool log_file_initialized = false;

// 日志输出函数
static void log_printf(const char* format, ...) {
#if CART_SERIAL_LOG_ENABLED
  va_list args;
  va_start(args, format);
  
#if CART_SERIAL_LOG_TO_FILE
  // 输出到文件
  if (!log_file_initialized) {
    log_file_handle = fopen(CART_SERIAL_LOG_FILE_PATH, "w");
    log_file_initialized = true;
    if (log_file_handle) {
      fprintf(log_file_handle, "=== SkyEmu Serial Log ===\n");
      fflush(log_file_handle);
    }
  }
  
  if (log_file_handle) {
    vfprintf(log_file_handle, format, args);
    fflush(log_file_handle);  // 立即刷新到文件
  }
#else
  // 输出到控制台
  vprintf(format, args);
  fflush(stdout);
#endif
  
  va_end(args);
#else
  (void)format;  // 避免未使用参数警告
#endif
}

// 关闭日志文件
static void log_close() {
#if CART_SERIAL_LOG_ENABLED && CART_SERIAL_LOG_TO_FILE
  if (log_file_handle) {
    fprintf(log_file_handle, "=== Log Closed ===\n");
    fclose(log_file_handle);
    log_file_handle = NULL;
    log_file_initialized = false;
  }
#endif
}

// 实时ROM读取函数的前向声明(需要在gba_dword_lookup之前)


// 串口端口类型定义
#ifdef _WIN32
  // windows.h 已经在文件开头包含了
  typedef HANDLE serial_port_t;
  #define INVALID_SERIAL_PORT INVALID_HANDLE_VALUE
#else
  #include <fcntl.h>
  #include <termios.h>
  #include <unistd.h>
  #include <errno.h>
  #include <sys/ioctl.h>
  typedef int serial_port_t;
  #define INVALID_SERIAL_PORT -1


// ---------- 平台串口原语 ----------
// DTR控制函数(模拟Python的ser.dtr操作)
static void cs_serial_set_dtr(serial_port_t port, bool state) {
#ifdef _WIN32
  EscapeCommFunction(port, state ? SETDTR : CLRDTR);
#else
  int flag = TIOCM_DTR;
  if (state) {
    ioctl(port, TIOCMBIS, &flag); // Set DTR
  } else {
    ioctl(port, TIOCMBIC, &flag); // Clear DTR
  }
#endif
}

// 自动查找串口设备(VID=0x0483, PID=0x0721)
static char* cs_serial_auto_find() {
#ifdef _WIN32
  // Windows: 使用SetupAPI枚举串口设备
  static char port_path[256] = {0};
  HDEVINFO deviceInfoSet;
  SP_DEVINFO_DATA deviceInfoData;
  DWORD deviceIndex = 0;
  
  log_printf("[Serial] Searching for USB serial devices (Windows)...\n");
  
  // 获取所有串口设备
  deviceInfoSet = SetupDiGetClassDevs(&GUID_DEVCLASS_PORTS, NULL, NULL, DIGCF_PRESENT);
  if (deviceInfoSet == INVALID_HANDLE_VALUE) {
    log_printf("[Serial] ERROR: Failed to get device information set\n");
    return NULL;
  }
  
  deviceInfoData.cbSize = sizeof(SP_DEVINFO_DATA);
  
  // 枚举所有串口设备
  while (SetupDiEnumDeviceInfo(deviceInfoSet, deviceIndex++, &deviceInfoData)) {
    HKEY hDeviceRegistryKey;
    char portName[256] = {0};
    DWORD size = sizeof(portName);
    
    // 获取端口名称 (COM1, COM2, 等)
    hDeviceRegistryKey = SetupDiOpenDevRegKey(deviceInfoSet, &deviceInfoData,
                                               DICS_FLAG_GLOBAL, 0,
                                               DIREG_DEV, KEY_READ);
    if (hDeviceRegistryKey != INVALID_HANDLE_VALUE) {
      DWORD type = REG_SZ;
      if (RegQueryValueExA(hDeviceRegistryKey, "PortName", NULL, &type,
                           (LPBYTE)portName, &size) == ERROR_SUCCESS) {
        
        // 获取设备的硬件ID以提取VID和PID
        char hardwareID[1024] = {0};
        if (SetupDiGetDeviceRegistryPropertyA(deviceInfoSet, &deviceInfoData,
                                               SPDRP_HARDWAREID, NULL,
                                               (PBYTE)hardwareID, sizeof(hardwareID), NULL)) {
          
          // 解析 VID 和 PID (格式: USB\VID_0483&PID_0721)
          int vid = 0, pid = 0;
          if (sscanf(hardwareID, "USB\\VID_%x&PID_%x", &vid, &pid) == 2) {
            log_printf("[Serial]   Found: %s (VID=0x%04x, PID=0x%04x)\n", portName, vid, pid);
            
            if (vid == 0x0483 && pid == 0x0721) {
              log_printf("[Serial]   *** Matched target device!\n");
              snprintf(port_path, sizeof(port_path), "\\\\.\\%s", portName);
              RegCloseKey(hDeviceRegistryKey);
              SetupDiDestroyDeviceInfoList(deviceInfoSet);
              return port_path;
            }
          }
        }
      }
      RegCloseKey(hDeviceRegistryKey);
    }
  }
  
  SetupDiDestroyDeviceInfoList(deviceInfoSet);
  log_printf("[Serial] No matching device found (VID=0x0483, PID=0x0721)\n");
  return NULL;
  
#elif defined(__APPLE__)
  // macOS: 使用IOKit查找USB设备
  static char port_path[256] = {0};
  io_iterator_t serialPortIterator = 0;
  io_object_t serialPort;
  
  // 创建匹配字典
  CFMutableDictionaryRef matchingDict = IOServiceMatching(kIOSerialBSDServiceValue);
  if (!matchingDict) return NULL;
  
  // 查找所有串口设备
  kern_return_t kr = IOServiceGetMatchingServices(kIOMasterPortDefault, matchingDict, &serialPortIterator);
  if (kr != KERN_SUCCESS) return NULL;
  
  log_printf("[Serial] Searching for USB serial devices...\n");
  
  while ((serialPort = IOIteratorNext(serialPortIterator))) {
    // 获取设备路径
    CFTypeRef devicePath = IORegistryEntryCreateCFProperty(serialPort,
                                                           CFSTR(kIOCalloutDeviceKey),
                                                           kCFAllocatorDefault, 0);
    if (devicePath) {
      if (CFStringGetCString(devicePath, port_path, sizeof(port_path), kCFStringEncodingUTF8)) {
        // 获取USB父设备以检查VID/PID
        io_registry_entry_t parent;
        kern_return_t kr = IORegistryEntryGetParentEntry(serialPort, kIOServicePlane, &parent);
        while (kr == KERN_SUCCESS) {
          CFTypeRef vid = IORegistryEntryCreateCFProperty(parent, CFSTR("idVendor"), kCFAllocatorDefault, 0);
          CFTypeRef pid = IORegistryEntryCreateCFProperty(parent, CFSTR("idProduct"), kCFAllocatorDefault, 0);
          
          if (vid && pid) {
            int vendor_id = 0, product_id = 0;
            CFNumberGetValue(vid, kCFNumberIntType, &vendor_id);
            CFNumberGetValue(pid, kCFNumberIntType, &product_id);
            
            log_printf("[Serial]   Found: %s (VID=0x%04x, PID=0x%04x)\n", port_path, vendor_id, product_id);
            
            if (vendor_id == 0x0483 && product_id == 0x0721) {
              log_printf("[Serial]   *** Matched target device!\n");
              if (vid) CFRelease(vid);
              if (pid) CFRelease(pid);
              IOObjectRelease(parent);
              CFRelease(devicePath);
              IOObjectRelease(serialPort);
              IOObjectRelease(serialPortIterator);
              return port_path;
            }
            if (vid) CFRelease(vid);
            if (pid) CFRelease(pid);
          }
          
          io_registry_entry_t next_parent;
          kr = IORegistryEntryGetParentEntry(parent, kIOServicePlane, &next_parent);
          IOObjectRelease(parent);
          parent = next_parent;
        }
      }
      CFRelease(devicePath);
    }
    IOObjectRelease(serialPort);
  }
  IOObjectRelease(serialPortIterator);
  
  log_printf("[Serial] No matching device found (VID=0x0483, PID=0x0721)\n");
  return NULL;
#elif defined(__linux__)
  // Linux: 读取/sys/bus/usb-serial/devices/
  static char port_path[256] = {0};
  DIR *dir = opendir("/sys/class/tty");
  if (!dir) return NULL;
  
  log_printf("[Serial] Searching for USB serial devices...\n");
  
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (strncmp(entry->d_name, "ttyUSB", 6) == 0 || strncmp(entry->d_name, "ttyACM", 6) == 0) {
      char path[512];
      snprintf(path, sizeof(path), "/sys/class/tty/%s/device/../idVendor", entry->d_name);
      
      FILE *f = fopen(path, "r");
      if (f) {
        int vid = 0;
        fscanf(f, "%x", &vid);
        fclose(f);
        
        snprintf(path, sizeof(path), "/sys/class/tty/%s/device/../idProduct", entry->d_name);
        f = fopen(path, "r");
        if (f) {
          int pid = 0;
          fscanf(f, "%x", &pid);
          fclose(f);
          
          snprintf(port_path, sizeof(port_path), "/dev/%s", entry->d_name);
          log_printf("[Serial]   Found: %s (VID=0x%04x, PID=0x%04x)\n", port_path, vid, pid);
          
          if (vid == 0x0483 && pid == 0x0721) {
            log_printf("[Serial]   *** Matched target device!\n");
            closedir(dir);
            return port_path;
          }
        }
      }
    }
  }
  closedir(dir);
  
  log_printf("[Serial] No matching device found (VID=0x0483, PID=0x0721)\n");
  return NULL;
#else
  log_printf("[Serial] Auto-detection not implemented for this platform\n");
  return NULL;
#endif
}

static serial_port_t cs_serial_open(const char* port_name) {
  // 如果是 "AUTO"，自动查找设备
  if (strcmp(port_name, "AUTO") == 0) {
    log_printf("[Serial] AUTO mode: searching for device...\n");
    const char* found_port = cs_serial_auto_find();
    if (!found_port) {
      log_printf("[Serial] ERROR: No device found in AUTO mode\n");
      return INVALID_SERIAL_PORT;
    }
    port_name = found_port;
    log_printf("[Serial] Using auto-detected port: %s\n", port_name);
  }
  
#ifdef _WIN32
  HANDLE handle = CreateFileA(port_name, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                               OPEN_EXISTING, 0, NULL);
  if (handle == INVALID_HANDLE_VALUE) {
    log_printf("Failed to open serial port: %s\n", port_name);
    return INVALID_SERIAL_PORT;
  }
  
  DCB dcb = {0};
  dcb.DCBlength = sizeof(DCB);
  if (!GetCommState(handle, &dcb)) {
    CloseHandle(handle);
    return INVALID_SERIAL_PORT;
  }
  
  dcb.BaudRate = CBR_115200;
  dcb.ByteSize = 8;
  dcb.StopBits = ONESTOPBIT;
  dcb.Parity = NOPARITY;
  
  if (!SetCommState(handle, &dcb)) {
    CloseHandle(handle);
    return INVALID_SERIAL_PORT;
  }
  
  // 执行DTR重置序列
  cs_serial_set_dtr(handle, true);
  Sleep(100); // 等待100ms
  cs_serial_set_dtr(handle, false);
  Sleep(100); // 等待100ms
  
  log_printf("Serial port opened and reset: %s\n", port_name);
  
  return handle;
#else
  int fd = open(port_name, O_RDWR | O_NOCTTY | O_SYNC);
  if (fd < 0) {
    log_printf("Failed to open serial port: %s\n", port_name);
    return INVALID_SERIAL_PORT;
  }
  
  struct termios tty;
  if (tcgetattr(fd, &tty) != 0) {
    close(fd);
    return INVALID_SERIAL_PORT;
  }
  
  // 设置波特率 115200
  cfsetospeed(&tty, B115200);
  cfsetispeed(&tty, B115200);
  
  // 配置为8N1(8数据位，无校验，1停止位)
  tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;     // 8位数据
  tty.c_cflag &= ~PARENB;                          // 无校验
  tty.c_cflag &= ~PARODD;
  tty.c_cflag &= ~CSTOPB;                          // 1停止位
  tty.c_cflag &= ~CRTSCTS;                         // 禁用硬件流控
  tty.c_cflag |= (CLOCAL | CREAD);                 // 启用接收，忽略调制解调器状态
  
  // 禁用软件流控
  tty.c_iflag &= ~(IXON | IXOFF | IXANY);
  tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL);
  
  // 原始模式
  tty.c_lflag = 0;  // 非规范模式，无回显
  tty.c_oflag = 0;  // 禁用输出处理
  
  // 设置读取超时
  tty.c_cc[VMIN] = 0;   // 非阻塞读取
  tty.c_cc[VTIME] = 20; // 2秒超时(单位0.1秒)，匹配Python的timeout=2
  
  if (tcsetattr(fd, TCSANOW, &tty) != 0) {
    close(fd);
    return INVALID_SERIAL_PORT;
  }
  
  // 执行DTR重置序列(就像Python代码中的 ser.dtr = True; ser.dtr = False)
  //printf("[Serial] Performing DTR reset sequence...\n");
  cs_serial_set_dtr(fd, true);
  log_printf("[Serial] DTR = HIGH\n");
  usleep(10000); // 等待10ms
  cs_serial_set_dtr(fd, false);
  log_printf("[Serial] DTR = LOW\n");
  usleep(20000); // 等待20ms，给设备更多时间初始化
  
  // 清空可能残留的数据
  tcflush(fd, TCIOFLUSH);
  
  log_printf("[Serial] Port %s opened successfully\n", port_name);
  log_printf("[Serial] Configuration: 115200 8N1, no flow control\n");
  
  return fd;
#endif
}

static void cs_serial_close(serial_port_t port) {
  if (port == INVALID_SERIAL_PORT) return;
#ifdef _WIN32
  CloseHandle(port);
#else
  close(port);
#endif
}

static size_t cs_serial_write(serial_port_t port, const uint8_t* data, size_t size) {
#ifdef _WIN32
  DWORD written = 0;
  if (!WriteFile(port, data, size, &written, NULL)) {
    log_printf("[Serial] WriteFile error: %lu\n", GetLastError());
    return 0;
  }
  FlushFileBuffers(port); // 确保数据发送
  return written;
#else
  ssize_t written = write(port, data, size);
  if (written < 0) {
    log_printf("[Serial] Write error: %s (errno=%d)\n", strerror(errno), errno);
    return 0;
  }
  // 确保数据发送完成
  tcdrain(port);
  return written;
#endif
}

static size_t cs_serial_read(serial_port_t port, uint8_t* data, size_t size) {
#ifdef _WIN32
  DWORD read_count = 0;
  if (!ReadFile(port, data, size, &read_count, NULL)) {
    return 0;
  }
  return read_count;
#else
  size_t total_read = 0;
  while (total_read < size) {
    ssize_t n = read(port, data + total_read, size - total_read);
    if (n < 0) {
      if (errno == EAGAIN || errno == EINTR) {
        continue; // 重试
      }
      return total_read; // 错误
    } else if (n == 0) {
      break; // 超时或连接关闭
    }
    total_read += n;
  }
  return total_read;
#endif
}


