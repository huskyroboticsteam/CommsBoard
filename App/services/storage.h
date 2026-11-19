/**
  * storage.h - the log files on the card, on top of FatFs.
  *
  * One file per boot and part: /LOG/B00042_01.jsonl. Each file is preallocated in one
  * contiguous piece when it is opened, so writing log data never changes the FAT or
  * the directory - a power cut can only hurt the block being written.
  * Only the logger may call these.
  */
#ifndef STORAGE_H
#define STORAGE_H

#include <stdint.h>

/* Mount the card. Reports free space and the highest boot ID among the files in /LOG.
 * Returns 0 on success. */
int storage_mount(uint32_t *free_mb, uint32_t *max_boot_id);

/* Create /LOG/B<boot>_<part>.jsonl, preallocate it, write the 512-byte header block
 * and sync once. Fails if the file already exists. Returns 0 on success. */
int storage_open_part(uint32_t boot_id, uint32_t part, uint32_t prealloc_mb, const void *header_block);

/* Append whole 512-byte blocks to the open file. Returns 0 on success. */
int storage_write(const void *blocks, uint32_t len);

void storage_close(void);

#endif /* STORAGE_H */
