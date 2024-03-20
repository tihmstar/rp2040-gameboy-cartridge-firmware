/* RP2040 GameBoy cartridge
 * Copyright (C) 2023 Sebastian Quilitz
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef A6E4EABE_18C1_4BCB_A021_7C59DEE53104
#define A6E4EABE_18C1_4BCB_A021_7C59DEE53104

#include <stdint.h>
#define USEC_PER_SEC             1000000L


#define WS2812_PIN 27

#define PIN_GB_RESET 26

#define SMC_GB_MAIN 0
#define SMC_GB_DETECT_A14 1
#define SMC_GB_RAM_READ 2
#define SMC_GB_RAM_WRITE 3

#define SMC_GB_ROM_LOW 0
#define SMC_GB_ROM_HIGH 1
#define SMC_GB_WRITE_DATA 2
#define SMC_GB_A15LOW_A14IRQS 3

#define GB_RAM_BANK_SIZE 0x2000U
#define GB_ROM_BANK_SIZE 0x4000U
/* 16 banks = 128K of RAM enough for MBC3 (32K) and MBC5*/
#define GB_MAX_RAM_BANKS 16

#define ROM_STORAGE_FLASH_START_ADDR 0x00020000
#define MAX_BANKS 888
#define MAX_BANKS_PER_ROM 0x200

#define FAKE_TIMESTAMP_PATH "faketimestamp.bin"
#define SAVES_DIR_PATH "/saves/"
#define ROMS_DIR_PATH "/roms/"

extern const volatile uint8_t *volatile ram_base;
extern const volatile uint8_t *volatile rom_low_base;
extern volatile uint32_t rom_high_base_flash_direct;

extern volatile uint8_t *_rtcLatchPtr;
extern volatile uint8_t *_rtcRealPtr;

extern uint8_t memory[];
extern uint8_t ram_memory[];
extern uint8_t memory_vblank_hook_bank[];
extern uint8_t memory_vblank_hook_bank2[];

struct ShortRomInfo {
  const uint8_t *firstBank;
  uint16_t speedSwitchBank;
  uint8_t numRamBanks;
  char name[17];
};

extern uint8_t g_numRoms;

extern const uint8_t *g_loadedRomBanks[MAX_BANKS_PER_ROM];
extern uint32_t g_loadedDirectAccessRomBanks[MAX_BANKS_PER_ROM];
extern struct ShortRomInfo g_loadedShortRomInfo;


void setSsi8bit();
void setSsi32bit();
void loadDoubleSpeedPio();
int storeSaveRamInFile(const struct ShortRomInfo *shortRomInfo);
int restoreSaveRamFromFile(const struct ShortRomInfo *shortRomInfo, uint64_t *savedTimestamp);


/*
    libgeneral: https://github.com/tihmstar/libgeneral
*/

#define cassure(a) do{ if ((a) == 0){err=-__LINE__; goto error;} }while(0)
#define cretassure(cond, errstr ...) do{ if ((cond) == 0){err=-__LINE__;printf(errstr); goto error;} }while(0)
#define creterror(errstr ...) do{printf(errstr);err=-__LINE__; goto error; }while(0)

struct GB_CfgRTCReal{
  uint8_t s;   //seconds 0-59
  uint8_t m;   //minute 0-59
  uint8_t h;   //hour   0-23
  uint8_t d;   //day    0-30
  uint8_t mon; //month  0-11
  uint8_t year;//year   0-255 (+1970) -> 1970-2225
  int8_t utcOffset;
};

struct __attribute__((packed)) GbRtc {
  uint8_t seconds;
  uint8_t minutes;
  uint8_t hours;
  uint8_t days;
  union {
    struct {
      uint8_t days_high : 1;
      uint8_t reserved : 5;
      uint8_t halt : 1;
      uint8_t days_carry : 1;
    };
    uint8_t asByte;
  } status;
};
union GbRtcUnion {
  struct GbRtc reg;
  uint8_t asArray[5];
};

extern volatile union GbRtcUnion g_rtcReal;
extern volatile union GbRtcUnion g_rtcLatched;

#endif /* A6E4EABE_18C1_4BCB_A021_7C59DEE53104 */
