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

#include <assert.h>
#include <hardware/address_mapped.h>
#include <hardware/dma.h>
#include <hardware/gpio.h>
#include <hardware/pio.h>
#include <hardware/regs/ssi.h>
#include <hardware/structs/ssi.h>
#include <hardware/sync.h>
#include <hardware/timer.h>
#include <hardware/vreg.h>
#include <pico/bootrom.h>
#include <pico/platform.h>
#include <pico/stdio.h>
#include <pico/stdio_uart.h>
#include <pico/stdlib.h>
#include <pico/time.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdio_ext.h>
#include <stdlib.h>
#include <string.h>
#include <sys/_intsup.h>
#include <sys/_types.h>
#include <time.h>

#include <lfs.h>
#include <lfs_pico_hal.h>

#include <git_commit.h>

#include "BuildVersion.h"
#include "gb-bootloader/gbbootloader.h"

#include "BuildVersion.h"
#include "GbDma.h"
#include "GlobalDefines.h"
#include "RomStorage.h"
#include "mbc.h"
#include "webusb.h"
#include "ws2812b_spi.h"

#include "gameboy_bus.pio.h"

#define SMEM_ADDR_START               ((uint16_t)(0xA000))
#define SMEM_ADDR_LED_CONTROL         ((uint16_t)(0xB010))
#define SMEM_ADDR_RP2040_BOOTLOADER   ((uint16_t)(0xB011))
#define SMEM_ADDR_GAME_MODE_SELECTOR  ((uint16_t)(0xB000))
#define SMEM_ADDR_GAME_SELECTOR       ((uint16_t)(0xB001))
#define SMEM_ADDR_GAME_CONTROL        ((uint16_t)(0xB002))

#define SMEM_ADDR_REALTIME_CONTROL    ((uint16_t)(0xB018))
#define SMEM_ADDR_REALTIME            ((uint16_t)(0xB020))

#define SMEM_GAME_START_MAGIC 42
#define SMEM_REALTIME_GET_MAGIC 69
#define SMEM_REALTIME_SET_MAGIC 13
#define SMEM_REALTIME_IDLE_MAGIC 37

const volatile uint8_t *volatile ram_base = NULL;
const volatile uint8_t *volatile rom_low_base = NULL;
volatile uint32_t rom_high_base_flash_direct = 0;

volatile uint8_t *_rtcLatchPtr = &g_rtcLatched.reg.seconds;
volatile uint8_t *_rtcRealPtr = &g_rtcReal.reg.seconds;


uint8_t memory[GB_ROM_BANK_SIZE * 3] __attribute__((aligned(GB_ROM_BANK_SIZE)));
uint8_t memory_vblank_hook_bank[0x200] __attribute__((aligned(0x200)));
uint8_t memory_vblank_hook_bank2[0x200] __attribute__((aligned(0x200)));
uint8_t __attribute__((section(".noinit_gb_ram.")))
ram_memory[(GB_MAX_RAM_BANKS + 1) * GB_RAM_BANK_SIZE]
    __attribute__((aligned(GB_RAM_BANK_SIZE)));

volatile union GbRtcUnion __attribute__((section(".noinit."))) g_rtcReal;
volatile union GbRtcUnion __attribute__((section(".noinit."))) g_rtcLatched;
volatile uint64_t __attribute__((section(".noinit."))) g_fakeTimestamp;
volatile uint64_t __attribute__((section(".noinit."))) g_lastFakeTimestampUpdate;
volatile int16_t __attribute__((section(".noinit."))) g_fakeTimestampUTCMinOffset;

uint8_t g_numRoms = 0;
const uint8_t *g_loadedRomBanks[MAX_BANKS_PER_ROM];
uint32_t g_loadedDirectAccessRomBanks[MAX_BANKS_PER_ROM];

struct ShortRomInfo __attribute__((section(".noinit."))) g_loadedShortRomInfo;
uint32_t __attribute__((section(".noinit."))) _noInitTest;
uint32_t __attribute__((section(".noinit."))) _lastRunningGame;

static lfs_t _lfs = {};
static uint8_t _lfsFileBuffer[LFS_CACHE_SIZE];

static uint _offset_main;
static uint _offset_write_data;
static uint16_t _mainStateMachineCopy
    [sizeof(gameboy_bus_double_speed_program_instructions) / sizeof(uint16_t)];

void runGbBootloader(uint8_t *selectedGame, uint8_t *selectedGameMode);

int storeFakeTimestamp(void);
int loadFakeTimestamp(void);
void tickFakeTimestamp(void);

int main() {
  // bi_decl(bi_program_description("Sample binary"));
  // bi_decl(bi_1pin_with_name(LED_PIN, "on-board PIN"));
  g_lastFakeTimestampUpdate = 0;
  {
    vreg_set_voltage(VREG_VOLTAGE_1_15);
    sleep_ms(2);
    set_sys_clock_khz(266000, true);
    sleep_ms(2);
  }

#ifdef DEBUG 
  stdio_uart_init_full(uart0, 500000, 28, -1);
#endif

  printf("Hello RP2040 Croco Cartridge %d.%d.%d %s-%.7X(%s)\n",
         RP2040_GB_CARTRIDGE_VERSION_MAJOR, RP2040_GB_CARTRIDGE_VERSION_MINOR,
         RP2040_GB_CARTRIDGE_VERSION_PATCH, git_Branch(), git_CommitSHA1Short(),
         git_AnyUncommittedChanges() ? "dirty" : "");

  printf("SSI->BAUDR: %x\n", *((uint32_t *)(XIP_SSI_BASE + SSI_BAUDR_OFFSET)));

  gpio_init(PIN_GB_RESET);
  gpio_set_dir(PIN_GB_RESET, true);
  gpio_put(PIN_GB_RESET, 1);

  // pio_gpio_init(pio1, 28);
  // gpio_init(PIN_UART_TX);
  // gpio_set_dir(PIN_UART_TX, true);

  for (uint pin = PIN_AD_BASE - 1; pin < PIN_AD_BASE + 25; pin++) {
    // gpio_init(pin);
    // gpio_set_dir(pin, false);
    // gpio_set_function(pin, GPIO_FUNC_PIO1);

    /* Disable schmitt triggers on GB Bus. The bus transceivers
     * already have schmitt triggers. */
    gpio_set_input_hysteresis_enabled(pin, false);
    /* Use fast slew rate for GB Bus. */
    gpio_set_slew_rate(pin, GPIO_SLEW_RATE_FAST);
  }

  for (uint pin = PIN_DATA_BASE; pin < PIN_DATA_BASE + 8; pin++) {
    /* Initialise PIO0 pins. */
    pio_gpio_init(pio0, pin);
  }

  // initialize SPI for WS2812B RGB LED
  ws2812b_spi_init(spi1);
  gpio_set_function(WS2812_PIN, GPIO_FUNC_SPI);
  ws2812b_setRgb(0, 0, 0);

  memset((void *)&g_rtcLatched, 0, sizeof(g_rtcLatched));
  memset((void *)&g_rtcReal, 0, sizeof(g_rtcLatched));

  // Load the gameboy_bus programs into it's respective PIOs
  _offset_main = pio_add_program(pio1, &gameboy_bus_program);
  uint offset_detect_a14 =
      pio_add_program(pio1, &gameboy_bus_detect_a14_program);
  uint offset_ram = pio_add_program(pio1, &gameboy_bus_ram_program);

  _offset_write_data =
      pio_add_program(pio0, &gameboy_bus_write_to_data_program);
  uint offset_rom_low = pio_add_program(pio0, &gameboy_bus_rom_low_program);
  uint offset_rom_high = pio_add_program(pio0, &gameboy_bus_rom_high_program);
  uint offset_a14_irqs =
      pio_add_program(pio0, &gameboy_bus_detect_a15_low_a14_irqs_program);

  // Initialize all gameboy state machines
  gameboy_bus_program_init(pio1, SMC_GB_MAIN, _offset_main);
  gameboy_bus_detect_a14_program_init(pio1, SMC_GB_DETECT_A14,
                                      offset_detect_a14);
  gameboy_bus_ram_read_program_init(pio1, SMC_GB_RAM_READ, offset_ram);
  gameboy_bus_ram_write_program_init(pio1, SMC_GB_RAM_WRITE, offset_ram);

  gameboy_bus_detect_a15_low_a14_irqs_init(pio0, SMC_GB_A15LOW_A14IRQS,
                                           offset_a14_irqs);
  gameboy_bus_rom_low_program_init(pio0, SMC_GB_ROM_LOW, offset_rom_low);
  gameboy_bus_rom_high_program_init(pio0, SMC_GB_ROM_HIGH, offset_rom_high);
  gameboy_bus_write_to_data_program_init(pio0, SMC_GB_WRITE_DATA,
                                         _offset_write_data);

  // copy the gameboy main double speed statemachine to RAM
  for (size_t i = 0; i < (sizeof(_mainStateMachineCopy) / sizeof(uint16_t));
       i++) {
    _mainStateMachineCopy[i] = gameboy_bus_double_speed_program_instructions[i];
  }

  // initialze base pointers with some default values before initialzizing the
  // DMAs
  ram_base = &ram_memory[GB_MAX_RAM_BANKS * GB_RAM_BANK_SIZE];
  rom_low_base = memory;

  GbDma_Setup();
  GbDma_SetupHigherDmaDirectSsi();

  // enable all gameboy state machines
  pio_sm_set_enabled(pio1, SMC_GB_MAIN, true);
  pio_sm_set_enabled(pio1, SMC_GB_DETECT_A14, true);
  pio_sm_set_enabled(pio1, SMC_GB_RAM_READ, true);
  pio_sm_set_enabled(pio1, SMC_GB_RAM_WRITE, true);

  pio_sm_set_enabled(pio0, SMC_GB_ROM_LOW, true);
  pio_sm_set_enabled(pio0, SMC_GB_WRITE_DATA, true);
  pio_sm_set_enabled(pio0, SMC_GB_A15LOW_A14IRQS, true);

  if (_noInitTest != 0xcafeaffe) {
    _noInitTest = 0xcafeaffe;
    _lastRunningGame = 0xFF;
    printf("NoInit initialized\n");
  }

  int lfs_err = lfs_mount(&_lfs, &pico_cfg);
  if (lfs_err != LFS_ERR_OK) {
    printf("Error mounting FS %d\n", lfs_err);
    printf("Formatting...\n");

    lfs_format(&_lfs, &pico_cfg);
    lfs_err = lfs_mount(&_lfs, &pico_cfg);
  }

  if (lfs_err != LFS_ERR_OK) {
    printf("Final error mounting FS %d\n", lfs_err);
  }

  lfs_err = lfs_mkdir(&_lfs, SAVES_DIR_PATH);
  if ((lfs_err != LFS_ERR_OK) && (lfs_err != LFS_ERR_EXIST)) {
    printf("Error creating saves directory %d\n", lfs_err);
  }

  RomStorage_init(&_lfs);

  if (_lastRunningGame < g_numRoms) {
    printf("Game %d was running before reset\n", _lastRunningGame);

    if (g_loadedShortRomInfo.numRamBanks > 0) {
      storeSaveRamInFile(&g_loadedShortRomInfo);
      _lastRunningGame = 0xFF;
    }
    storeFakeTimestamp();
  }else{
    loadFakeTimestamp();
  }

  uint8_t game = 0xFF, mode = 0;

  runGbBootloader(&game, &mode);

  (void)save_and_disable_interrupts();

  _lastRunningGame = game;

  if (0 != RomStorage_LoadRom(game)) {
    printf("Error reading ROM\n");
  } else {
    loadGame(game, mode);
  }

  // should only be reached in case there was an error loading the game
  while (1) {
    tight_loop_contents();
  }
}

struct __attribute__((packed)) SharedGameboyData {
  uint32_t git_sha1;
  uint8_t git_status;
  char buildType;
  uint8_t versionMajor;
  uint8_t versionMinor;
  uint8_t versionPatch;
  uint8_t number_of_roms;
  char rom_names[];
};

void __no_inline_not_in_flash_func(runGbBootloader)(uint8_t *selectedGame,
                                                    uint8_t *selectedGameMode) {
  // use spare RAM bank to not overwrite potential save
  bool cartridgeIsInGameboy = false;
  uint8_t *ram = &ram_memory[GB_MAX_RAM_BANKS * GB_RAM_BANK_SIZE];
  struct SharedGameboyData *shared_data = (void *)ram;

  *selectedGame = 0xFF;
  *selectedGameMode = 0xFF;

  memcpy(memory, GB_BOOTLOADER, GB_BOOTLOADER_SIZE);
  memset(ram, 0, GB_RAM_BANK_SIZE);

  shared_data->git_sha1 = git_CommitSHA1Short();
  shared_data->git_status = git_AnyUncommittedChanges();
  shared_data->buildType = RP2040_GB_CARTRIDGE_BUILD_VERSION_TYPE;
  shared_data->versionMajor = RP2040_GB_CARTRIDGE_VERSION_MAJOR;
  shared_data->versionMinor = RP2040_GB_CARTRIDGE_VERSION_MINOR;
  shared_data->versionPatch = RP2040_GB_CARTRIDGE_VERSION_PATCH;

  // initialize RAM with information about roms
  {
    char *pRomNames = shared_data->rom_names;
    for (size_t i = 0; i < g_numRoms; i++) {
      struct ShortRomInfo sRI = {};
      RomStorage_loadShortRomInfo(i, &sRI);
      strcpy(pRomNames, sRI.name);
      pRomNames += strlen(pRomNames)+1;
    }
  }
  shared_data->number_of_roms = g_numRoms;
  ram[SMEM_ADDR_GAME_SELECTOR-SMEM_ADDR_START] = 0xFF;

  printf("Found %d games\n", g_numRoms);

  usb_start();

  ram_base = ram;

  gpio_put(PIN_GB_RESET, 0); // let the gameboy start (deassert reset line)

  while (*selectedGame == 0xFF) {
    if (!pio_sm_is_rx_fifo_empty(pio1, SMC_GB_MAIN)) {
      cartridgeIsInGameboy = true;
      uint32_t rdAndAddr = *((uint32_t *)(&pio1->rxf[SMC_GB_MAIN]));
      bool write = rdAndAddr & 0x00000001;
      uint16_t addr = (rdAndAddr >> 1) & 0xFFFF;

      if (write) {
        uint8_t data = pio_sm_get_blocking(pio1, SMC_GB_MAIN) & 0xFF;

        if ((addr == SMEM_ADDR_GAME_CONTROL) && (data == SMEM_GAME_START_MAGIC)) {
          *selectedGame = ram[SMEM_ADDR_GAME_SELECTOR-SMEM_ADDR_START];
          *selectedGameMode = ram[SMEM_ADDR_GAME_MODE_SELECTOR-SMEM_ADDR_START];
          printf("Selected Game: %d\n", *selectedGame);

        }else if (addr == SMEM_ADDR_LED_CONTROL) {
          switch (data) {
          case 1:
            ws2812b_setRgb(0x15, 0, 0);
            break;
          case 2:
            ws2812b_setRgb(0, 0x15, 0);
            break;
          case 3:
            ws2812b_setRgb(0, 0, 0x15);
            break;
          default:
            ws2812b_setRgb(0, 0, 0);
            break;
          }

        } else if (addr == SMEM_ADDR_REALTIME_CONTROL){
          switch (data) {
            case SMEM_REALTIME_GET_MAGIC:
              {
                struct GB_CfgRTCReal *gbrtc = (struct GB_CfgRTCReal *)&ram[SMEM_ADDR_REALTIME-SMEM_ADDR_START];
                time_t rawtime = g_fakeTimestamp + g_fakeTimestampUTCMinOffset;
                struct tm *tm = localtime(&rawtime);
                gbrtc->s = 0;
                gbrtc->m = tm->tm_min;
                gbrtc->h = tm->tm_hour;
                gbrtc->d = tm->tm_mday;
                gbrtc->mon = tm->tm_mon;
                gbrtc->year = tm->tm_year-70;
                gbrtc->utcOffset = g_fakeTimestampUTCMinOffset / 15;
                ram[SMEM_ADDR_REALTIME_CONTROL-SMEM_ADDR_START] = SMEM_REALTIME_IDLE_MAGIC;
              }
              break;

            case SMEM_REALTIME_SET_MAGIC:
              {
                struct GB_CfgRTCReal *gbrtc = (struct GB_CfgRTCReal *)&ram[SMEM_ADDR_REALTIME-SMEM_ADDR_START];
                struct tm tm = {
                  .tm_sec = 0,
                  .tm_min = gbrtc->m,
                  .tm_hour = gbrtc->h,
                  .tm_mday = gbrtc->d,
                  .tm_mon = gbrtc->mon,
                  .tm_year = gbrtc->year+70,
                };
                time_t rawtime = mktime(&tm);
                g_fakeTimestampUTCMinOffset = gbrtc->utcOffset * 15;
                g_fakeTimestamp = rawtime - g_fakeTimestampUTCMinOffset;
                storeFakeTimestamp();
                ram[SMEM_ADDR_REALTIME_CONTROL-SMEM_ADDR_START] = SMEM_REALTIME_IDLE_MAGIC;
              }
              break;

            default:
              break;
          }
        }
      }
    }
    tickFakeTimestamp();

    if (!cartridgeIsInGameboy) usb_run();
  }

  usb_shutdown();

  // hold the gameboy in reset until we have loaded the game
  gpio_put(PIN_GB_RESET, 1);
}

void __no_inline_not_in_flash_func(loadDoubleSpeedPio)() {
  pio_sm_set_enabled(pio1, SMC_GB_MAIN, false);

  // manually replace statemachine instruction in order to not use flash
  for (size_t i = 0; i < (sizeof(_mainStateMachineCopy) / sizeof(uint16_t));
       i++) {
    uint16_t instr = _mainStateMachineCopy[i];
    pio1->instr_mem[_offset_main + i] =
        pio_instr_bits_jmp != _pio_major_instr_bits(instr)
            ? instr
            : instr + _offset_main;
  }

  pio_sm_set_enabled(pio1, SMC_GB_MAIN, true);

  /*
   * The sleep cycle of the Gameboy during the clock switch causes some
   * missfetching of RAM reads in some games. Clearing the TX FIFO fixes that.
   * There is enough time to do that as the Gameboy needs around 15 ms to change
   * the clock speed.
   */

  pio_sm_exec(pio0, SMC_GB_WRITE_DATA, pio_encode_jmp(_offset_write_data));

  // wait for 2 ms until all DMA and SMs are stable
  uint64_t now = time_us_64();
  while ((time_us_64() - now) < 2000) {
    tight_loop_contents();
  }

  // flush the tx buffers and reset the write data SM back to waiting for data
  pio_sm_clear_fifos(pio0, SMC_GB_WRITE_DATA);
  while (pio0->sm[SMC_GB_WRITE_DATA].instr != 0x80a0) {
    pio_sm_exec(pio0, SMC_GB_WRITE_DATA, pio_encode_jmp(_offset_write_data));
    while (pio_sm_is_exec_stalled(pio0, SMC_GB_WRITE_DATA)) {
      tight_loop_contents();
    }
  }
}

void __no_inline_not_in_flash_func(setSsi8bit)() {
  __compiler_memory_barrier();

  ssi_hw->ssienr = 0; // disable SSI so it can be configured
  ssi_hw->ctrlr0 =
      (SSI_CTRLR0_SPI_FRF_VALUE_QUAD /* Quad I/O mode */
       << SSI_CTRLR0_SPI_FRF_LSB) |
      (7 << SSI_CTRLR0_DFS_32_LSB) |     /* 8 data bits */
      (SSI_CTRLR0_TMOD_VALUE_EEPROM_READ /* Send INST/ADDR, Receive Data */
       << SSI_CTRLR0_TMOD_LSB);

  ssi_hw->dmacr = SSI_DMACR_TDMAE_BITS | SSI_DMACR_RDMAE_BITS;
  ssi_hw->ssienr = 1; // enable SSI again
}

void __no_inline_not_in_flash_func(setSsi32bit)() {
  ssi_hw->ssienr = 0; // disable SSI so it can be configured
  ssi_hw->ctrlr0 =
      (SSI_CTRLR0_SPI_FRF_VALUE_QUAD /* Quad I/O mode */
       << SSI_CTRLR0_SPI_FRF_LSB) |
      (31 << SSI_CTRLR0_DFS_32_LSB) |    /* 32 data bits */
      (SSI_CTRLR0_TMOD_VALUE_EEPROM_READ /* Send INST/ADDR, Receive Data */
       << SSI_CTRLR0_TMOD_LSB);

  ssi_hw->dmacr = 0;
  ssi_hw->ssienr = 1; // enable SSI again

  __compiler_memory_barrier();
}

void __attribute__((__noreturn__))
__assert_fail(const char *expr, const char *file, unsigned int line,
              const char *function) {
  printf("assert");
  while (1)
    ;
}

void restoreSaveRamFromFile(const struct ShortRomInfo *shortRomInfo) {
  lfs_file_t file;
  struct lfs_file_config fileconfig = {.buffer = _lfsFileBuffer};
  char filenamebuffer[40] = "saves/";

  strcpy(&filenamebuffer[strlen(filenamebuffer)], shortRomInfo->name);

  int lfs_err =
      lfs_file_opencfg(&_lfs, &file, filenamebuffer, LFS_O_RDONLY, &fileconfig);

  if (lfs_err == LFS_ERR_OK) {
    printf("found save at %s\n", filenamebuffer);

    lfs_err =
        lfs_file_read(&_lfs, &file, ram_memory,
                      shortRomInfo->numRamBanks * GB_RAM_BANK_SIZE);
    printf("read %d bytes\n", lfs_err);

    if (lfs_err >= 0) {
      lfs_file_close(&_lfs, &file);
    }
  }
}

void storeSaveRamInFile(const struct ShortRomInfo *shortRomInfo) {
  int err = 0;
  lfs_file_t file;
  int lfs_err;
  struct lfs_file_config fileconfig = {.buffer = _lfsFileBuffer};
  char filenamebuffer[40];
  snprintf(filenamebuffer,sizeof(filenamebuffer)-1,SAVES_DIR_PATH "%s",(const char *)&(shortRomInfo->name));

  cassure((lfs_err = lfs_file_opencfg(&_lfs, &file, filenamebuffer,LFS_O_WRONLY | LFS_O_CREAT, &fileconfig)) == LFS_ERR_OK);
  cassure((lfs_err = lfs_file_write(&_lfs, &file, ram_memory, shortRomInfo->numRamBanks * GB_RAM_BANK_SIZE)) == shortRomInfo->numRamBanks * GB_RAM_BANK_SIZE);

error:
  lfs_file_close(&_lfs, &file);
  if (!err){
    ws2812b_setRgb(0, 0x10, 0); // light up LED in green
  }
}

int storeFakeTimestamp(void){
  int err = 0;
  bool fileIsOpen = false;
  lfs_file_t file = {};
  int lfs_err = 0;
  struct lfs_file_config fileconfig = {.buffer = _lfsFileBuffer};
  cassure((lfs_err = lfs_file_opencfg(&_lfs, &file, FAKE_TIMESTAMP_PATH, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC, &fileconfig)) == LFS_ERR_OK);
  fileIsOpen = true;
  cassure((lfs_err = lfs_file_write(&_lfs, &file, (void*)&g_fakeTimestamp, sizeof(g_fakeTimestamp))) == sizeof(g_fakeTimestamp));
  cassure((lfs_err = lfs_file_write(&_lfs, &file, (void*)&g_fakeTimestampUTCMinOffset, sizeof(g_fakeTimestampUTCMinOffset))) == sizeof(g_fakeTimestampUTCMinOffset));

error:
  if (fileIsOpen) lfs_file_close(&_lfs, &file);
  return err;
}

int loadFakeTimestamp(void){
  int err = 0;
  lfs_file_t file = {};
  int lfs_err = 0;
  bool fileIsOpen = false;
  struct lfs_file_config fileconfig = {.buffer = _lfsFileBuffer};

  lfs_err = lfs_file_opencfg(&_lfs, &file, FAKE_TIMESTAMP_PATH, LFS_O_RDONLY, &fileconfig);
  if (lfs_err == LFS_ERR_OK){
    fileIsOpen = true;
    lfs_err = lfs_file_read(&_lfs, &file, (void*)&g_fakeTimestamp, sizeof(g_fakeTimestamp));
  }

  if (lfs_err != sizeof(g_fakeTimestamp)){
    /*
      Wed Mar 20 2024 18:44:46 GMT+0100
    */
    g_fakeTimestamp = 1710956686;
    /*
      UTC+01:00 (Germany)
    */
    g_fakeTimestampUTCMinOffset = 60*1;
  }else{
    lfs_err = lfs_file_read(&_lfs, &file, (void*)&g_fakeTimestampUTCMinOffset, sizeof(g_fakeTimestampUTCMinOffset));
    if (lfs_err != sizeof(g_fakeTimestampUTCMinOffset)){
      /*
        UTC+01:00 (Germany)
      */
      g_fakeTimestampUTCMinOffset = 60*1;
    }
  }
error:
  if (fileIsOpen) lfs_file_close(&_lfs, &file);
  return err;
}

void tickFakeTimestamp(void){  
  uint64_t curTime = time_us_64();
  uint64_t diff = (curTime - g_lastFakeTimestampUpdate) / USEC_PER_SEC;
  if (diff > 0){
    g_fakeTimestamp += diff;
    g_lastFakeTimestampUpdate += diff*USEC_PER_SEC;
  }
}