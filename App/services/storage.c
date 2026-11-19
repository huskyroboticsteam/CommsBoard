/* storage.c - log files on the card through FatFs. See storage.h. */
#include "storage.h"
#include "app_fatfs.h"    /* USERFatFs, USERFile, USERPath from CubeMX */
#include <string.h>

#define LOG_DIR "LOG"

extern Disk_drvTypeDef disk;    /* ST's FatFs glue (ff_gen_drv.c) */

static int file_open;

/* "B00042_01.jsonl" -> 42. Returns 0 if the name is not one of ours. */
static uint32_t boot_id_of(const char *name)
{
  uint32_t id = 0;
  int i;

  if (name[0] != 'B' && name[0] != 'b') return 0;
  for (i = 1; i <= 5; i++) {
    if (name[i] < '0' || name[i] > '9') return 0;
    id = id * 10 + (uint32_t)(name[i] - '0');
  }
  return (name[6] == '_') ? id : 0;
}

/* Builds "LOG/B00042_01.jsonl" without printf. Only five digits of the boot ID fit, so
 * after 99,999 boots names repeat; storage_open_part() refuses to overwrite a file. */
static void part_path(char *out, uint32_t boot_id, uint32_t part)
{
  static const char prefix[] = LOG_DIR "/B";
  char *p = out;
  int i;

  memcpy(p, prefix, sizeof prefix - 1);
  p += sizeof prefix - 1;
  for (i = 4; i >= 0; i--) { p[i] = (char)('0' + boot_id % 10); boot_id /= 10; }
  p += 5;
  *p++ = '_';
  *p++ = (char)('0' + part / 10 % 10);
  *p++ = (char)('0' + part % 10);
  memcpy(p, ".jsonl", 7);
}

int storage_mount(uint32_t *free_mb, uint32_t *max_boot_id)
{
  FATFS *fs;
  DWORD free_clusters;
  DIR dir;
  FILINFO info;
  FRESULT res;

  file_open = 0;
  /* ST's glue only ever initialises the card once; clear its flag so that a card that
   * failed, or was swapped, gets a fresh start-up. */
  disk.is_initialized[0] = 0;
  if (f_mount(&USERFatFs, USERPath, 1) != FR_OK) return -1;
  if (f_getfree(USERPath, &free_clusters, &fs) != FR_OK) return -1;
  *free_mb = (uint32_t)(((uint64_t)free_clusters * fs->csize) / 2048);   /* 512-byte sectors */

  res = f_mkdir(LOG_DIR);
  if (res != FR_OK && res != FR_EXIST) return -1;

  *max_boot_id = 0;
  if (f_opendir(&dir, LOG_DIR) != FR_OK) return -1;
  while (f_readdir(&dir, &info) == FR_OK && info.fname[0]) {
    uint32_t id = boot_id_of(info.fname);
    if (id > *max_boot_id) *max_boot_id = id;
  }
  f_closedir(&dir);
  return 0;
}

int storage_open_part(uint32_t boot_id, uint32_t part, uint32_t prealloc_mb, const void *header_block)
{
  char path[24];
  UINT written;

  storage_close();
  part_path(path, boot_id, part);
  if (f_open(&USERFile, path, FA_CREATE_NEW | FA_WRITE) != FR_OK) return -1;    /* never overwrite a log */
  file_open = 1;
  /* Reserve the whole file now, in one contiguous run (fails if there is no such run).
   * After this, writing data never touches the FAT or the directory entry. */
  if (f_expand(&USERFile, (FSIZE_t)prealloc_mb * 1024 * 1024, 1) != FR_OK
      || f_write(&USERFile, header_block, 512, &written) != FR_OK || written != 512
      || f_sync(&USERFile) != FR_OK) {
    storage_close();
    return -1;
  }
  return 0;
}

int storage_write(const void *blocks, uint32_t len)
{
  UINT written;

  if (!file_open) return -1;
  if (f_write(&USERFile, blocks, len, &written) != FR_OK || written != len) return -1;
  return 0;
}

void storage_close(void)
{
  if (file_open) f_close(&USERFile);
  file_open = 0;
}
