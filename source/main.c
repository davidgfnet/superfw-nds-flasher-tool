
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

void sha256sum(const uint8_t *inbuffer, unsigned length, void *output);

void sleep_1ms() {
  for (unsigned i = 0; i < (1<<14); i++)
    asm volatile ("nop");
}

// The flash device address bus is connected with some permutated wires.
// The permutation seems to only apply to the 9 LSB.
// In general we do not care unless we need to send a specifc address or play
// with sector/page erase.
static uint32_t addr_perm(uint32_t addr) {
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

static inline bool sysGetCartOwner() {
  return !(REG_EXMEMCNT & ARM7_OWNS_ROM);
}

void set_supercard_mode(unsigned mapped_area, bool write_access, bool sdcard_interface) {
  // Bit0: Controls SDRAM vs internal Flash mapping
  // Bit1: Controls whether the SD card interface is mapped into the ROM addresspace.
  // Bit2: Controls read-only/write access.
  uint16_t value = mapped_area | (sdcard_interface ? 0x2 : 0x0) | (write_access ? 0x4 : 0x0);
  const uint16_t MODESWITCH_MAGIC = 0xA55A;
  volatile uint16_t *REG_SD_MODE = (volatile uint16_t*)(0x09FFFFFE);

  // Write magic value and then the mode value (twice) to trigger the mode change.
  *REG_SD_MODE = MODESWITCH_MAGIC;
  *REG_SD_MODE = MODESWITCH_MAGIC;
  *REG_SD_MODE = value;
  *REG_SD_MODE = value;
}

#define SLOT2_BASE_U16 ((volatile uint16_t*)(0x08000000))
#define SLOT2_SRAM_U8  ((volatile uint8_t*)( 0x0A000000))

static unsigned test_sram() {
  bool pmode = sysGetCartOwner();
  sysSetCartOwner(BUS_OWNER_ARM9);

  // Just write the SRAM with some well-known data, and read it back
  REG_EXMEMCNT |= 0x3;   // Use the slowest possible access time.
  for (unsigned i = 0; i < 64*1024; i++)
    SLOT2_SRAM_U8[i] = 0x00;
  for (unsigned i = 0; i < 64*1024; i++)
    SLOT2_SRAM_U8[i] = i ^ (i * i) ^ 0x5A;
  unsigned numerrs = 0;
  for (unsigned i = 0; i < 64*1024; i++)
    if (SLOT2_SRAM_U8[i] != ((i ^ (i * i) ^ 0x5A) & 0xFF))
      numerrs++;

  sysSetCartOwner(pmode);
  return numerrs;
}

static uint32_t flash_ident() {
  // Map the GBA cart into the ARM9, enter flash mode with write enable.
  bool pmode = sysGetCartOwner();
  sysSetCartOwner(BUS_OWNER_ARM9);
  set_supercard_mode(MAPPED_FIRMWARE, true, false);

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

  set_supercard_mode(MAPPED_FIRMWARE, false, false);
  sysSetCartOwner(pmode);

  return ret;
}

// Performs a flash full-chip erase.
static bool flash_erase() {
  // Map the GBA cart into the ARM9, enter flash mode with write enable.
  bool pmode = sysGetCartOwner();
  sysSetCartOwner(BUS_OWNER_ARM9);
  set_supercard_mode(MAPPED_FIRMWARE, true, false);

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
  for (unsigned i = 0; i < 60*1000; i++) {
    sleep_1ms();
    if (SLOT2_BASE_U16[0] == SLOT2_BASE_U16[0])
      break;
  }
  bool retok = (SLOT2_BASE_U16[0] == SLOT2_BASE_U16[0]);

  for (unsigned i = 0; i < 32; i++)
    SLOT2_BASE_U16[0] = 0x00F0;            // Reset for a few cycles

  set_supercard_mode(MAPPED_FIRMWARE, false, false);
  sysSetCartOwner(pmode);

  return retok;
}

// Checks that the erase operation actually erased the memory.
static bool flash_erase_check() {
  // Map the GBA cart into the ARM9, enter flash mode with write enable.
  bool pmode = sysGetCartOwner();
  sysSetCartOwner(BUS_OWNER_ARM9);
  set_supercard_mode(MAPPED_FIRMWARE, true, false);
  REG_EXMEMCNT |= 0xF;  // use slow mode

  bool errf = false;
  for (unsigned i = 0; i < 512*1024; i+= 2) {
    errf = (SLOT2_BASE_U16[i / 2] != 0xFFFF);
    if (errf)
      break;
  }

  set_supercard_mode(MAPPED_FIRMWARE, false, false);
  sysSetCartOwner(pmode);

  return errf;
}


static bool flash_write(const uint8_t *buf, unsigned size) {
  bool ok = true;
  // Map the GBA cart into the ARM9, enter flash mode with write enable.
  bool pmode = sysGetCartOwner();
  sysSetCartOwner(BUS_OWNER_ARM9);
  set_supercard_mode(MAPPED_FIRMWARE, true, false);
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

  set_supercard_mode(MAPPED_FIRMWARE, false, false);
  sysSetCartOwner(pmode);

  return ok;
}

static bool flash_validate(const uint8_t *fwimg, unsigned fwsize) {
  // Map the GBA cart into the ARM9, enter flash mode with write enable.
  bool pmode = sysGetCartOwner();
  sysSetCartOwner(BUS_OWNER_ARM9);
  set_supercard_mode(MAPPED_FIRMWARE, true, false);

  return (!memcmp(fwimg, (uint8_t*)0x08000000, fwsize));

  set_supercard_mode(MAPPED_FIRMWARE, false, false);
  sysSetCartOwner(pmode);
}

static bool flash_dump(const char *filename) {
  // Map the GBA cart into the ARM9, enter flash mode with write enable.
  bool pmode = sysGetCartOwner();
  sysSetCartOwner(BUS_OWNER_ARM9);
  set_supercard_mode(MAPPED_FIRMWARE, true, false);

  char *data = (char*)malloc(512*1024);
  memcpy(data, (void*)0x08000000, 512*1024);

  set_supercard_mode(MAPPED_FIRMWARE, false, false);
  sysSetCartOwner(pmode);

  FILE *fd = fopen(filename, "wb");
  if (!fd) {
    free(data);
    return false;
  }

  fwrite(data, 1, 512*1024, fd);

  fclose(fd);
  free(data);
  return true;
}

const struct {
  const char *fw_name;
  uint8_t sha256[16];
} known_images[] = {
  {
    "Empty/Zeroed",   // All 0x00
    {0x07,0x85,0x4d,0x2f,0xef,0x29,0x7a,0x06,0xba,0x81,0x68,0x5e,0x66,0x0c,0x33,0x2d}
  },
  {
    "Empty/Cleared",   // All 0xFF
    {0x04,0x3e,0x23,0x8a,0x76,0x5f,0x7c,0xfb,0xc6,0x25,0x96,0xa5,0x0e,0x53,0xc8,0xff}
  },
  {
    "Official firmware v1.85 (EN)",
    {0xc1,0x1d,0x86,0x4d,0x39,0xa4,0x58,0x60,0xa7,0xc5,0xc3,0x4c,0xa6,0x65,0xa9,0xc1}
  },
};

const char * firmware_ident() {
  // Calculate the Firmware hash, attempt to identify it as a well-known firmware.
  uint8_t hash[32];
  sha256sum((uint8_t*)0x08000000, 512*1024, hash);

  for (unsigned i = 0; i < sizeof(known_images)/sizeof(known_images[0]); i++) {
    if (!memcmp(hash, known_images[i].sha256, sizeof(known_images[i].sha256)))
      return known_images[i].fw_name;
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
  return strcmp(ea->fn, eb->fn);
}

t_fs_entry *listdir(const char *path, unsigned *nume) {
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

  struct stat st;
  if (stat(path, &st)) {
    printf("Could not stat() the selected file\n");
    return;
  }
  if (st.st_size > 512*1024) {
    printf("The file is bigger than 512KiB!\n");
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
  printf("File loaded with hash: %02x%02x%02x%02x%02x%02x%02x%02x!\n",
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

      if (flash_erase_check()) {
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

int main(int argc, char **argv) {
  PrintConsole tops, bots;

  videoSetMode(MODE_0_2D);
  videoSetModeSub(MODE_0_2D);
  vramSetBankA(VRAM_A_MAIN_BG);
  vramSetBankC(VRAM_C_SUB_BG);

  consoleInit(&tops, 3, BgType_Text4bpp, BgSize_T_256x256, 31, 0, true, true);
  consoleInit(&bots, 3, BgType_Text4bpp, BgSize_T_256x256, 31, 0, false, true);

  // Init FAT filesystem and slot2 ...
  consoleSelect(&bots);
  consoleClear();
  printf("Debug console:\n\n");
  if (!fatInitDefault()) {
    perror("fatInitDefault()");
  }
  printf("DLDI name:\n%s\n\n", io_dldi_data->friendlyName);
  printf("DSi mode: %d\n\n", isDSiMode());

  unsigned menu_sel = 0;
  while (1) {
    // Render menu
    consoleSelect(&tops);
    consoleClear();
    printf("\x1b[36;1m");
    printf("\x1b[1;5HSuperFW flashing tool");
    printf("\x1b[37;1m");

    printf("\x1b[5;1H %s Identify cart", menu_sel == 0 ? ">" : " ");
    printf("\x1b[7;1H %s Dump flash",    menu_sel == 1 ? ">" : " ");
    printf("\x1b[9;1H %s Write flash",   menu_sel == 2 ? ">" : " ");
    printf("\x1b[11;1H %s Test SRAM",    menu_sel == 3 ? ">" : " ");

    printf("\x1b[20;8H Version 0.1");

    swiWaitForVBlank();
    scanKeys();

    if (keysDown() & KEY_A) {
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
        }
        break;
      case 3:
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
        printf("Starting dump ...\n");
        if (!flash_dump("fat:/sc_flash_dump.bin"))
          printf("Failed!\n");
        else
          printf("Dump complete!\n");
        break;
      case 2:
        // Present a small file browser or something.
        char curpath[PATH_MAX] = "fat:/";
        unsigned cur_entry = 0, top_entry = 0;
        unsigned num_entries;
        t_fs_entry * l = listdir(curpath, &num_entries);

        while (1) {
          swiWaitForVBlank();
          scanKeys();

          if (keysDown() & KEY_B)
            break;
          if (keysDown() & KEY_A) {
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

          if (keysDown() & KEY_DOWN)
            cur_entry = cur_entry + 1 < num_entries ? cur_entry + 1 : cur_entry;
          if (keysDown() & KEY_UP)
            cur_entry = cur_entry ? cur_entry - 1 : 0;

          if ((signed)cur_entry - (signed)top_entry >= 8)
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

    if (keysDown() & KEY_START)
      break;
    if (keysDown() & KEY_DOWN)
      menu_sel = (menu_sel + 1) & 3;
    if (keysDown() & KEY_UP)
      menu_sel = (menu_sel - 1) & 3;
  }

  return 0;
}

