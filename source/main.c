
// Copyright 2024 David Guillen Fandos <david@davidgf.net>

// SuperCard firmware flashing tool.
//
// This NDS tool handles certain operations (like read/flash) on the Supercard
// firmware flash memory.

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdint.h>
#include <nds.h>
#include <fat.h>
#include <nds/arm9/dldi.h>
#include <nds/memory.h>
#include <sys/stat.h>

#define MAPPED_FIRMWARE      0
#define MAPPED_SDRAM         1

#define MAX(a, b)   ((a) < (b) ? (b) : (a))
#define MIN(a, b)   ((a) > (b) ? (b) : (a))

typedef struct {
  uint32_t size;         // Size in bytes
  uint32_t blkwrite;     // Buffer writing capabilities (zero means disabled)
  uint32_t regioncnt;    // Erase region count (ideally 1, or perhaps 0)
  struct {
    uint32_t blksize;    // Block size in bytes
    uint32_t blkcount;   // Number of blocks
  } regions[256];
} t_flash_info;
extern t_flash_info flashinfo;

enum {
  SupercardSD,
  SupercardLite,
  Superchis,
  MAXDEVICES
};
unsigned devtype = SupercardSD;
const char *devnames[] = { "Supercard SD", "Supercard Lite", "SuperChis" };
const unsigned flash_fwsizes[] = { 512*1024, 496*1024, 2*1024*1024 };
const unsigned flash_erase_timeout[] = { 60, 60, 300 * 8 };

void sha256sum(const uint8_t *inbuffer, unsigned length, void *output);

void sleep_1ms() {
  for (unsigned i = 0; i < (1<<14); i++)
    asm volatile ("nop");
}

// The flash device address bus is connected with some permutated wires.
// The permutation seems to only apply to the 9 LSB.
// In general we do not care unless we need to send a specifc address or play
// with sector/page erase.
// The supercard lite does not do any of this crazy mapping :)
static uint32_t addr_perm(uint32_t addr) {
  if (devtype != SupercardSD)
    return addr;
  else
    return (addr & 0xFFFFFE02) |
           ((addr & 0x001) << 7) |
           ((addr & 0x004) << 4) |
           ((addr & 0x008) << 2) |
           ((addr & 0x010) >> 4) |
           ((addr & 0x020) >> 3) |
           ((addr & 0x040) << 2) |
           ((addr & 0x080) >> 3) |
           ((addr & 0x100) >> 5);
}


#define SUPERCARD_LITE_FLASHWR        0x1510

void write_supercard_modereg(uint16_t value) {
  // Write magic value and then the mode value (twice) to trigger the mode change.
  // Using asm to ensure we place a proper memory barrier.
  const uint16_t MODESWITCH_MAGIC = 0xA55A;
  uint32_t REG_SC_MODE_REG_ADDR = 0x09FFFFFE;

  asm volatile (
    "strh %1, [%0]\n"
    "strh %1, [%0]\n"
    "strh %2, [%0]\n"
    "strh %2, [%0]\n"
    :: "l"(REG_SC_MODE_REG_ADDR),
       "l"(MODESWITCH_MAGIC),
       "l"(value)
    : "memory");
}

void reset_superchis_flashmap() {
  const uint16_t MODESWITCH_MAGIC = 0xA55A;
  uint32_t REG_SC_MODE_REG_ADDR = 0x09FFFFFE;

  for (unsigned i = 0; i < 8; i++) {
    asm volatile (
      "strh %1, [%0]\n"
      "strh %1, [%0]\n"
      "strh %2, [%0]\n"
      "nop; nop;"
      :: "l"(REG_SC_MODE_REG_ADDR),
         "l"(MODESWITCH_MAGIC),
         "l"(0x100)
      : "memory");
  }
}

void set_superchis_bankreg(unsigned bankn) {
  const uint16_t MODESWITCH_MAGIC = 0xA55A;
  uint32_t REG_SC_MODE_REG_ADDR = 0x09FFFFFE;

  asm volatile (
    "strh %1, [%0]\n"
    "strh %1, [%0]\n"
    "strh %2, [%0]\n"
    "nop; nop;"
    :: "l"(REG_SC_MODE_REG_ADDR),
       "l"(MODESWITCH_MAGIC),
       "l"(0x70 | ((bankn & 1) << 3))
    : "memory");
}

void set_supercard_mode(unsigned mapped_area, bool write_access, bool sdcard_interface) {
  // Bit0: Controls SDRAM vs internal Flash mapping
  // Bit1: Controls whether the SD card interface is mapped into the ROM addresspace.
  // Bit2: Controls read-only/write access. Doubles as SRAM bank selector!
  uint16_t value = mapped_area | (sdcard_interface ? 0x2 : 0x0) | (write_access ? 0x4 : 0x0);

  write_supercard_modereg(value);
}


void enable_sc_flash() {
  if (devtype == SupercardLite)
    write_supercard_modereg(SUPERCARD_LITE_FLASHWR);
  else
    set_supercard_mode(MAPPED_FIRMWARE, true, false);
}
void disable_sc_flash() {
  if (devtype == SupercardLite)
    write_supercard_modereg(0);
  else
    set_supercard_mode(MAPPED_FIRMWARE, false, false);
}
void sram_map_bank(unsigned bankn) {
  if (devtype == Superchis)
    set_superchis_bankreg(bankn);
  else
    set_supercard_mode(MAPPED_FIRMWARE, bankn != 0, false);
}

#define SLOT2_BASE_U16 ((volatile uint16_t*)(0x08000000))
#define SLOT2_SRAM_U8  ((volatile uint8_t*)( 0x0A000000))

static unsigned test_sram() {
  bool pmode = sysGetCartOwner();
  sysSetCartOwner(BUS_OWNER_ARM9);

  // Just write the SRAM with some well-known data, and read it back
  REG_EXMEMCNT |= 0x3;   // Use the slowest possible access time.

  for (unsigned bank = 0; bank < 2; bank++) {
    sram_map_bank(bank);
    for (unsigned i = 0; i < 64*1024; i++)
      SLOT2_SRAM_U8[i] = i ^ (i * i) ^ 0x5A ^ bank;
  }

  unsigned numerrs = 0;
  for (unsigned bank = 0; bank < 2; bank++) {
    sram_map_bank(bank);
    for (unsigned i = 0; i < 64*1024; i++)
      if (SLOT2_SRAM_U8[i] != ((i ^ (i * i) ^ 0x5A ^ bank) & 0xFF))
        numerrs++;
  }

  set_supercard_mode(MAPPED_FIRMWARE, false, false);
  sysSetCartOwner(pmode);
  return numerrs;
}

static uint32_t flash_ident() {
  // Map the GBA cart into the ARM9, enter flash mode with write enable.
  bool pmode = sysGetCartOwner();
  sysSetCartOwner(BUS_OWNER_ARM9);
  enable_sc_flash();

  REG_EXMEMCNT |= 0xF;  // use slow mode
  for (unsigned i = 0; i < 32; i++)
    SLOT2_BASE_U16[0] = 0x00F0;            // Reset for a few cycles

  SLOT2_BASE_U16[addr_perm(0x555)] = 0x00AA;
  SLOT2_BASE_U16[addr_perm(0x2AA)] = 0x0055;
  SLOT2_BASE_U16[addr_perm(0x555)] = 0x0090;

  uint32_t ret = SLOT2_BASE_U16[addr_perm(0x000)] << 16;
  ret |= SLOT2_BASE_U16[addr_perm(0x001)];

  for (unsigned i = 0; i < 32; i++)
    SLOT2_BASE_U16[0] = 0x00F0;            // Reset for a few cycles

  disable_sc_flash();
  sysSetCartOwner(pmode);

  return ret;
}

static bool flash_cfi(t_flash_info *info) {
  memset(info, 0, sizeof(*info));

  // Map the GBA cart into the ARM9, enter flash mode with write enable.
  bool pmode = sysGetCartOwner();
  sysSetCartOwner(BUS_OWNER_ARM9);
  enable_sc_flash();

  REG_EXMEMCNT |= 0xF;  // use slow mode
  for (unsigned i = 0; i < 32; i++)
    SLOT2_BASE_U16[0] = 0x00F0;            // Reset for a few cycles

  // Enter CFI mode and extract flash information
  SLOT2_BASE_U16[addr_perm(0x555)] = 0x0098;
  uint8_t qs[3] = {
    SLOT2_BASE_U16[addr_perm(0x010)],
    SLOT2_BASE_U16[addr_perm(0x011)],
    SLOT2_BASE_U16[addr_perm(0x012)],
  };
  bool ret = (qs[0] == 'Q' && qs[1] == 'R' && qs[2] == 'Y');

  if (ret) {
    info->size = 1 << SLOT2_BASE_U16[addr_perm(0x027)];
    info->blkwrite = (SLOT2_BASE_U16[addr_perm(0x02A)] & 0xFF);
    info->blkwrite = (info->blkwrite ? (1 << info->blkwrite) : 0);

    info->regioncnt = SLOT2_BASE_U16[addr_perm(0x02C)] & 0xFF;

    for (unsigned i = 0; i < info->regioncnt; i++) {
      unsigned baddr = 0x2D + i*4;
      info->regions[i].blkcount = ((SLOT2_BASE_U16[addr_perm(baddr)] & 0xFF) |
                                  ((SLOT2_BASE_U16[addr_perm(baddr + 1)] & 0xFF) << 8)) + 1;

      unsigned bs = ((SLOT2_BASE_U16[addr_perm(baddr + 2)] & 0xFF) |
                    ((SLOT2_BASE_U16[addr_perm(baddr + 3)] & 0xFF) << 8)) << 8;
      info->regions[i].blksize = (bs ?: 128);
    }
  }

  for (unsigned i = 0; i < 32; i++)
    SLOT2_BASE_U16[0] = 0x00F0;            // Reset for a few cycles

  disable_sc_flash();
  sysSetCartOwner(pmode);

  return ret;
}

static void flash_prot_dump(bool prot[128]) {
  bool pmode = sysGetCartOwner();
  sysSetCartOwner(BUS_OWNER_ARM9);
  enable_sc_flash();

  for (unsigned sa = 0; sa < 128; sa++) {

    REG_EXMEMCNT |= 0xF;  // use slow mode
    for (unsigned i = 0; i < 32; i++)
      SLOT2_BASE_U16[0] = 0x00F0;            // Reset for a few cycles

    SLOT2_BASE_U16[addr_perm(0x555)] = 0x00AA;
    SLOT2_BASE_U16[addr_perm(0x2AA)] = 0x0055;
    SLOT2_BASE_U16[addr_perm(0x555)] = 0x0090;

    unsigned protv = SLOT2_BASE_U16[addr_perm(0x002 | (sa << 11))];

    for (unsigned i = 0; i < 32; i++)
      SLOT2_BASE_U16[0] = 0x00F0;            // Reset for a few cycles

    prot[sa] = protv & 1;
  }

  disable_sc_flash();
  sysSetCartOwner(pmode);
}

// Performs a flash full-chip erase.
static bool flash_erase() {
  // Map the GBA cart into the ARM9, enter flash mode with write enable.
  bool pmode = sysGetCartOwner();
  sysSetCartOwner(BUS_OWNER_ARM9);
  enable_sc_flash();

  REG_EXMEMCNT |= 0xF;  // use slow mode
  for (unsigned i = 0; i < 32; i++)
    SLOT2_BASE_U16[0] = 0x00F0;            // Reset for a few cycles

  SLOT2_BASE_U16[addr_perm(0x555)] = 0x00AA;
  SLOT2_BASE_U16[addr_perm(0x2AA)] = 0x0055;
  SLOT2_BASE_U16[addr_perm(0x555)] = 0x0080; // Erase command
  SLOT2_BASE_U16[addr_perm(0x555)] = 0x00AA;
  SLOT2_BASE_U16[addr_perm(0x2AA)] = 0x0055;
  SLOT2_BASE_U16[addr_perm(0x555)] = 0x0010; // Full chip erase!

  // Wait for the erase operation to finish. We rely on Q6 toggling:
  for (unsigned i = 0; i < flash_erase_timeout[devtype]*1000; i++) {
    sleep_1ms();
    if (SLOT2_BASE_U16[0] == SLOT2_BASE_U16[0])
      break;
  }
  bool retok = (SLOT2_BASE_U16[0] == SLOT2_BASE_U16[0]);

  for (unsigned i = 0; i < 32; i++)
    SLOT2_BASE_U16[0] = 0x00F0;            // Reset for a few cycles

  disable_sc_flash();
  sysSetCartOwner(pmode);

  return retok;
}

// Checks that the erase operation actually erased the memory.
static bool flash_erase_check(unsigned size) {
  // Map the GBA cart into the ARM9, enter flash mode with write enable.
  bool pmode = sysGetCartOwner();
  sysSetCartOwner(BUS_OWNER_ARM9);
  enable_sc_flash();
  REG_EXMEMCNT |= 0xF;  // use slow mode

  bool errf = false;
  for (unsigned i = 0; i < size; i+= 2) {
    errf = (SLOT2_BASE_U16[i / 2] != 0xFFFF);
    if (errf)
      break;
  }

  disable_sc_flash();
  sysSetCartOwner(pmode);

  return errf;
}


static bool flash_write(const uint8_t *buf, unsigned size) {
  bool ok = true;
  // Map the GBA cart into the ARM9, enter flash mode with write enable.
  bool pmode = sysGetCartOwner();
  sysSetCartOwner(BUS_OWNER_ARM9);
  enable_sc_flash();
  REG_EXMEMCNT |= 0xF;  // use slow mode

  SLOT2_BASE_U16[0] = 0x00F0;   // Force IDLE

  for (unsigned i = 0; i < size; i+= 2) {
    uint16_t value = buf[i] | (buf[i+1] << 8);

    SLOT2_BASE_U16[addr_perm(0x555)] = 0x00AA;
    SLOT2_BASE_U16[addr_perm(0x2AA)] = 0x0055;
    SLOT2_BASE_U16[addr_perm(0x555)] = 0x00A0; // Program command

    // Perform the actual write operation
    SLOT2_BASE_U16[i / 2] = value;

    // It should take less than 1ms usually (in the order of us).
    for (unsigned j = 0; j < 32*1024; j++) {
      if (SLOT2_BASE_U16[0] == SLOT2_BASE_U16[0])
        break;
    }
    bool notfinished = (SLOT2_BASE_U16[0] != SLOT2_BASE_U16[0]);

    SLOT2_BASE_U16[0] = 0x00F0;   // Finish operation or abort.

    // Timed out or the write was incorrect
    if (notfinished || SLOT2_BASE_U16[i / 2] != value) {
      ok = false;
      break;
    }
  }

  disable_sc_flash();
  sysSetCartOwner(pmode);

  return ok;
}

static bool flash_validate(const uint8_t *fwimg, unsigned fwsize) {
  // Map the GBA cart into the ARM9, enter flash mode with write enable.
  bool pmode = sysGetCartOwner();
  sysSetCartOwner(BUS_OWNER_ARM9);
  enable_sc_flash();

  return (!memcmp(fwimg, (uint8_t*)0x08000000, fwsize));

  disable_sc_flash();
  sysSetCartOwner(pmode);
}

static bool flash_dump(const char *filename, unsigned size) {
  // Map the GBA cart into the ARM9, enter flash mode with write enable.
  bool pmode = sysGetCartOwner();
  sysSetCartOwner(BUS_OWNER_ARM9);
  enable_sc_flash();

  char *data = (char*)malloc(size);
  memcpy(data, (void*)0x08000000, size);

  disable_sc_flash();
  sysSetCartOwner(pmode);

  FILE *fd = fopen(filename, "wb");
  if (!fd) {
    free(data);
    return false;
  }

  fwrite(data, 1, size, fd);

  fclose(fd);
  free(data);
  return true;
}

static bool rom_dump(const char *filename) {
  // Map the GBA cart into the ARM9, enter flash mode with write enable.
  FILE *fd = fopen(filename, "wb");
  if (!fd)
    return false;

  char *data = (char*)malloc(512*1024);
  bool pmode = sysGetCartOwner();
  sysSetCartOwner(BUS_OWNER_ARM9);
  set_supercard_mode(MAPPED_SDRAM, true, false);

  for (unsigned i = 0; i < 64; i++) {
    memcpy(data, (void*)(0x08000000 + i*512*1024), 512*1024);
    fwrite(data, 1, 512*1024, fd);
  }

  set_supercard_mode(MAPPED_FIRMWARE, false, false);
  sysSetCartOwner(pmode);

  fclose(fd);
  free(data);
  return true;
}

char superfw_str[128];

const char * firmware_ident() {
  // Identify a valid SuperFW firmware.
  if (!memcmp((uint8_t*)0x080000F0, "SUPERFW~DAVIDGF", 16)) {
    unsigned version = *(uint32_t*)0x080000C4;
    unsigned commtid = *(uint32_t*)0x080000C8;
    snprintf(superfw_str, sizeof(superfw_str), "SuperFW version %u.%u (%08x)",
             (version >> 16), (version & 0xFFFF), commtid);
    return superfw_str;
  }

  return NULL;
}

bool valid_header(const uint8_t *fw) {
  const uint8_t logo_hash[] = {0x08,0xa0,0x15,0x3c,0xfd,0x6b,0x0e,0xa5,0x4b,0x93,0x8f,0x7d,0x20,0x99,0x33,0xfa};

  // Check the logo
  uint8_t hash[32];
  sha256sum(&fw[0x4], 156, hash);
  bool logo_ok = !memcmp(hash, logo_hash, sizeof(logo_hash));

  // Check that the checksum is also valid
  uint8_t checksum = 0x19;
  for (unsigned i = 0xA0; i < 0xBD; i++)
    checksum += fw[i];
  checksum = -checksum;
  bool checksum_ok = checksum == fw[0xBD];

  return logo_ok && checksum_ok;
}

typedef struct {
  char fn[PATH_MAX];
  bool isdir;
} t_fs_entry;

int fncomp(const void* a, const void* b) {
  const t_fs_entry* ea = (t_fs_entry*)a;
  const t_fs_entry* eb = (t_fs_entry*)b;
  // Sort directories first
  if (ea->isdir && !eb->isdir)
    return -1;
  if (eb->isdir && !ea->isdir)
    return 1;
  return strcmp(ea->fn, eb->fn);
}

t_fs_entry *listdir(const char *path, int *nume) {
  unsigned cap = 8, nument = 0;
  t_fs_entry *ret = (t_fs_entry*)malloc(cap * sizeof(t_fs_entry));
  ret[0].fn[0] = 0;

  DIR *dirp = opendir(path);
  while (1) {
    struct dirent *cur = readdir(dirp);
    if (!cur || !cur->d_name[0])
      break;
    if (cur->d_name[0] == '.' && !cur->d_name[1])
      continue;

    strcpy(ret[nument].fn, cur->d_name);
    ret[nument].isdir = cur->d_type == DT_DIR;
    if (cur->d_type == DT_DIR)
      strcat(ret[nument].fn, "/");
    nument++;

    if (nument >= cap) {
      cap += 8;
      ret = (t_fs_entry*)realloc(ret, cap * sizeof(t_fs_entry));
    }
    ret[nument].fn[0] = 0;
  }

  qsort(ret, nument, sizeof(t_fs_entry), fncomp);

  if (nume) *nume = nument;
  return ret;
}

void select_image(const char *path, PrintConsole *tops, PrintConsole *bots) {
  consoleSelect(bots);

  printf("Assuming that the flash is %d KiBs in size\n", flash_fwsizes[devtype] >> 10);

  struct stat st;
  if (stat(path, &st)) {
    printf("Could not stat() the selected file (%s)\n", path);
    return;
  }
  if (st.st_size > flash_fwsizes[devtype]) {
    printf("The file is bigger than the assumed flash size!\n");
    return;
  }

  FILE *fd = fopen(path, "rb");
  if (!fd) {
    printf("Could not open the selected file!\n");
    return;
  }

  printf("Reading file ...\n");
  uint8_t *fwimg = (uint8_t*)malloc(st.st_size);
  size_t ret = fread(fwimg, 1, st.st_size, fd);
  fclose(fd);
  if (ret != st.st_size) {
    free(fwimg);
    printf("Could not read the file correctly!\n");
    return;
  }

  uint8_t hash[32];
  sha256sum(fwimg, st.st_size, hash);
  printf("File loaded (size %d) with hash: %02x%02x%02x%02x%02x%02x%02x%02x!\n", (int)st.st_size,
         hash[0], hash[1], hash[2], hash[3], hash[4], hash[5], hash[6], hash[7]);

  if (!valid_header(fwimg)) {
    free(fwimg);
    printf("Invalid firmware file detected (invalid header)\n");
    return;
  } else {
    printf("Looks like a valid GBA rom/firmware\n");
  }

  // TODO: Parse SuperFW firmware images for more info.

  consoleSelect(tops);
  consoleClear();
  printf("\x1b[1;5HSuperFW flashing tool");

  printf("\x1b[4;2HFile: %s", path);
  printf("\x1b[5;2HSize: %ld bytes", st.st_size);

  printf("\x1b[9;9HReady to flash");
  printf("\x1b[12;2HPress L + R + A to begin");

  printf("\x1b[14;2HPress B to cancel");

  while (1) {
    swiWaitForVBlank();
    scanKeys();

    if (keysDown() & KEY_B)
      break;

    if ((keysHeld() & (KEY_L|KEY_R|KEY_A)) == (KEY_L|KEY_R|KEY_A)) {
      consoleSelect(bots);
      printf("Erasing flash chip ...\n");
      if (!flash_erase()) {
        printf("\x1b[31;1mErase failed!\x1b[37;1m\n");
        break;
      }
      printf("\x1b[32;1mErase operation complete\x1b[37;1m\n");

      printf("Verifying erase operation (%d KiB) ...\n", flash_fwsizes[devtype] >> 10);

      if (flash_erase_check(flash_fwsizes[devtype])) {
        printf("\x1b[31;1mErase validation failed!\x1b[37;1m\n");
        break;
      }
      printf("Writing flash chip ...\n");

      if (flash_write(fwimg, st.st_size))
        printf("\x1b[32;1mFirmware flashed successfully!\x1b[37;1m\n");
      else
        printf("\x1b[31;1mFlashing operation failed!\x1b[37;1m\n");

      printf("Verifying written data ...\n");
      if (flash_validate(fwimg, st.st_size))
        printf("\x1b[32;1mValidation passed!\x1b[37;1m\n");
      else
        printf("\x1b[31;1mValidation error!\x1b[37;1m\n");

      break;
    }
  }

  free(fwimg);
}

void key_handler() {
  lcdSwap();
}

int main(int argc, char **argv) {
  PrintConsole tops, bots;

  videoSetMode(MODE_0_2D);
  videoSetModeSub(MODE_0_2D);
  vramSetBankA(VRAM_A_MAIN_BG);
  vramSetBankC(VRAM_C_SUB_BG);

  consoleInit(&tops, 3, BgType_Text4bpp, BgSize_T_256x256, 31, 0, true, true);
  consoleInit(&bots, 3, BgType_Text4bpp, BgSize_T_256x256, 31, 0, false, true);

  irqSet(IRQ_KEYS, key_handler);
  irqEnable(IRQ_KEYS);
  REG_KEYCNT = KEY_SELECT | (1 << 14);

  // Init FAT filesystem and slot2 ...
  consoleSelect(&bots);
  consoleClear();
  printf("Debug console:\n\n");
  if (!fatInitDefault()) {
    perror("fatInitDefault()");
  }
  printf("DLDI name:\n%s\n\n", io_dldi_data->friendlyName);
  printf("DSi mode: %d\n\n", isDSiMode());

  while (1) {

    while (1) {
      // Render simple dev sel menu
      consoleSelect(&tops);
      consoleClear();
      printf("\x1b[36;1m");
      printf("\x1b[1;5HSuperFW flashing tool");
      printf("\x1b[37;1m");
      printf("\x1b[5;1H Select device type");
      printf("\x1b[9;3H < %s >", devnames[devtype]);
      printf("\x1b[13;0H Select the right device type");

      unsigned keys = keysDown();

      if (keys & KEY_LEFT)
        devtype = (devtype + MAXDEVICES - 1) % MAXDEVICES;
      if (keys & KEY_RIGHT)
        devtype = (devtype + 1) % MAXDEVICES;

      if (keys & KEY_A)
        break;
      if (keys & KEY_START)
        return 0;

      swiWaitForVBlank();
      scanKeys();
    }

    if (devtype == Superchis)
      reset_superchis_flashmap();

    unsigned menu_sel = 0;
    while (1) {
      // Render menu
      consoleSelect(&tops);
      consoleClear();
      printf("\x1b[36;1m");
      printf("\x1b[1;5HSuperFW flashing tool");
      printf("\x1b[37;1m");

      printf("\x1b[5;1H %s Identify cart",  menu_sel == 0 ? ">" : " ");
      printf("\x1b[7;1H %s Dump flash",     menu_sel == 1 ? ">" : " ");
      printf("\x1b[9;1H %s Write flash",    menu_sel == 2 ? ">" : " ");
      printf("\x1b[11;1H %s Dump ROM",      menu_sel == 3 ? ">" : " ");
      printf("\x1b[13;1H %s Test SRAM",     menu_sel == 4 ? ">" : " ");
      if (devtype == SupercardLite)
        printf("\x1b[15;1H %s Protbits dump", menu_sel == 5 ? ">" : " ");

      printf("\x1b[20;8H Version 0.7");
      printf("\x1b[22;6H %s", devnames[devtype]);

      swiWaitForVBlank();
      scanKeys();
      unsigned keys = keysDown();

      if (keys & KEY_A) {
        switch (menu_sel) {
        case 0:
          consoleSelect(&bots);
          printf("Identified flash device ID as %08lx\n", flash_ident());
          {
            const char *fwname = firmware_ident();
            if (fwname)
              printf("Identified the firmware as %s\n", fwname);
            else {
              if (!valid_header((uint8_t*)0x08000000))
                printf("Invalid firmware header detected!\n");
              else
                printf("Unknown firmware detected!\n");
            }

            t_flash_info info;
            if (!flash_cfi(&info))
              printf("Flash does not support CFI\n");
            else {
              printf("Size: %lu bytes\n", info.size);
              printf("Write buffer size: %lu bytes\n", info.blkwrite);
              printf("Has %lu erase sections\n", info.regioncnt);
              for (unsigned i = 0; i < info.regioncnt; i++)
                printf("S%u cnt: %lu size: %lu\n", i, info.regions[i].blkcount, info.regions[i].blksize);
            }
          }
          break;
        case 5:
          {
            bool prot[128];
            consoleSelect(&bots);
            flash_prot_dump(prot);

            for (unsigned i = 0; i < 128; i++)
              printf("%d ", prot[i] ? 1 : 0);
            printf("\n");
          }
          break;
        case 4:
          {
            unsigned numerrs = test_sram();
            consoleSelect(&bots);
            if (numerrs)
              printf("\x1b[31;1mSRAM check failed with %d diffs!\x1b[37;1m\n", numerrs);
            else
              printf("\x1b[32;1mSRAM integrity check passed!\x1b[37;1m\n");
          }
          break;
        case 1:
          consoleSelect(&bots);
          printf("Starting dump (%d KiB)...\n", flash_fwsizes[devtype] >> 10);
          if (!flash_dump("fat:/sc_flash_dump.bin", flash_fwsizes[devtype]))
            printf("Failed!\n");
          else
            printf("Dump complete! File written: sc_flash_dump.bin\n");
          break;
        case 3:
          consoleSelect(&bots);
          printf("Starting dump ...\n");
          if (!rom_dump("fat:/sc_rom_dump.bin"))
            printf("Failed!\n");
          else
            printf("Dump complete! File written: sc_rom_dump.bin\n");
          break;
        case 2:
          // Present a small file browser or something.
          char curpath[PATH_MAX] = "fat:/";
          int cur_entry = 0, top_entry = 0;
          int num_entries;
          t_fs_entry * l = listdir(curpath, &num_entries);

          while (1) {
            swiWaitForVBlank();
            scanKeys();
            unsigned keys = keysDown();

            if (keys & KEY_B)
              break;
            if (keys & KEY_A) {
              if (l[cur_entry].fn[0]) {
                char tmp[PATH_MAX];
                strcpy(tmp, curpath);
                strcat(tmp, "/");
                strcat(tmp, l[cur_entry].fn);

                if (l[cur_entry].fn[strlen(l[cur_entry].fn)-1] == '/') {
                  // Is a directory, go down the rabbit hole
                  realpath(tmp, curpath);  // Simplify the path (like "//" or "/../")

                  top_entry = cur_entry = 0;
                  free(l);
                  l = listdir(curpath, &num_entries);
                }
                else {
                  select_image(tmp, &tops, &bots);
                  break; //  Go back
                }
              }
            }

            if (keys & KEY_DOWN)
              cur_entry = MIN(num_entries - 1, cur_entry + 1);
            if (keys & KEY_UP)
              cur_entry = MAX(0, cur_entry - 1);
            if (keys & KEY_RIGHT)
              cur_entry = MIN(cur_entry + 8, num_entries - 1);
            if (keys & KEY_LEFT)
              cur_entry = MAX(0, cur_entry - 8);

            if (cur_entry - top_entry >= 8)
              top_entry = cur_entry - 7;
            if (cur_entry < top_entry)
              top_entry = cur_entry;

            // Render path list
            consoleSelect(&tops);
            consoleClear();
            printf("\x1b[1;5HSuperFW flashing tool");

            for (unsigned i = 0; i < 8; i++) {
              if (!l[top_entry + i].fn[0])
                break;
              printf("\x1b[%d;1H %s %.28s", 5 + i*2, i + top_entry == cur_entry ? ">" : " ", l[top_entry + i].fn);
            }
          }
          free(l);
          break;
        };
      }

      if (keys & KEY_B)
        break;

      const uint8_t maxopt[3] = { 5, 6, 5 };

      if (keys & KEY_DOWN)
        menu_sel = (menu_sel + 1) % maxopt[devtype];
      if (keys & KEY_UP)
        menu_sel = (menu_sel + maxopt[devtype] - 1) % maxopt[devtype];
    }
  }

  return 0;
}

