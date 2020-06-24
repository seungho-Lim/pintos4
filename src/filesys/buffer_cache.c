#include <threads/malloc.h>
#include <stdio.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "filesys/buffer_cache.h"
#include "lib/kernel/bitmap.h"

#define BUFFER_CACHE_ENTRY_NB 64
#define SECTORSIZE 512

void *p_buffer_cache;

struct buffer_head head_buffer[BUFFER_CACHE_ENTRY_NB];

int clock;


struct buffer_head* bc_select_victim(void);
struct buffer_head* bc_lookup (block_sector_t sector);
void bc_flush_entry (struct buffer_head *p_flush_entry);
void bc_flush_all_entries (void);


void
bc_init(void)
{
  /* Init to manage buffer cache. We can reach buffer
     through HEAD_BUFFER. CLOCK is used to implement
     clock algorithm.  */
  p_buffer_cache = malloc(SECTORSIZE * BUFFER_CACHE_ENTRY_NB);
  if(p_buffer_cache == NULL)
    return;
  int i;
  clock=0;
  for(i=0;i<BUFFER_CACHE_ENTRY_NB;i++)
    {
      head_buffer[i].dirty = false;
      head_buffer[i].is_used = false;
      head_buffer[i].clock_bit = false;
      head_buffer[i].data = p_buffer_cache + SECTORSIZE * i;
      lock_init(&head_buffer[i].buffer_lock);
    }
}

void
bc_term(void)
{
  /* Terminate buffer cache. Free buffer cache and
     flush to disk using bc_flush_all_entries(). */
  bc_flush_all_entries();
  free(p_buffer_cache);
}

struct buffer_head*
bc_select_victim(void)
{
  /* If there is empty entry in buffer cache,
     select that. Other case select victim
     following clock algorithm. */
  int i;
  for(i=0; i<BUFFER_CACHE_ENTRY_NB;i++)
    {
      if(head_buffer[i].is_used == false)
        {
          return &head_buffer[i];
        }
    }
  while(true)
    {
      if(clock == BUFFER_CACHE_ENTRY_NB - 1)
        clock = 0;
      else
        clock++;
      if( head_buffer[clock].clock_bit == false)
        {
          if(head_buffer[clock].dirty == true)
            {
              bc_flush_entry(&head_buffer[clock]);
            }
          head_buffer[clock].sector = -1;
          head_buffer[clock].is_used = false;
          head_buffer[clock].clock_bit = false;
          return &head_buffer[clock];
        }
      else
        head_buffer[clock].clock_bit = false;
    }
}

struct buffer_head*
bc_lookup (block_sector_t sector)
{
  /* Search HEAD_BUFFER entries which has
     sector field is SECTOR, If exist return
     that, if not return NULL. */
  int i;
  for(i=0;i<BUFFER_CACHE_ENTRY_NB;i++)
    {
      if(head_buffer[i].is_used == true && 
		head_buffer[i].sector == sector)
        return &head_buffer[i];
    }
  return NULL;
}

void
bc_flush_entry (struct buffer_head *p_flush_entry)
{
  /* Flush buffer cache data to disk.  */
  block_write(fs_device,p_flush_entry->sector,p_flush_entry->data);
  p_flush_entry->dirty = false;
}

void
bc_flush_all_entries (void)
{
  /* Flush all entries which dirty in HEAD_BUFFER to disk. */
  int i;
  for(i=0;i<BUFFER_CACHE_ENTRY_NB;i++)
    {
      if(head_buffer[i].is_used == true &&
		head_buffer[i].dirty == true)
        bc_flush_entry(&head_buffer[i]);
    }
}

bool
bc_read (block_sector_t sector_idx, void *buffer,
		off_t bytes_read, int chunk_size, int sector_ofs)
{
  /* Read from buffer cache using bc_lookup(),
     and save that in BUFFER. If there isn't
     data in buffer cache read it from disk,
     usig bc_select_victim(). */
  struct buffer_head *sector_buffer = bc_lookup(sector_idx);
  if(sector_buffer == NULL)
    {
      sector_buffer = bc_select_victim();
      sector_buffer->sector = sector_idx;
      sector_buffer->is_used = true;
      block_read(fs_device,sector_idx,sector_buffer->data);
    }
  sector_buffer->clock_bit = true;
  memcpy(buffer + bytes_read, sector_buffer->data + sector_ofs,chunk_size);
  
  return true;
}

bool
bc_write (block_sector_t sector_idx, void *buffer,
		off_t bytes_written, int chunk_size, int sector_ofs)
{
  /* Save BUFFER's data in buffer cache. If there
     isn't empty entry in buffer cache, make space
     using bc_select_victim(). */
  struct buffer_head *sector_buffer = bc_lookup(sector_idx);
  if(sector_buffer==NULL)
    {
      sector_buffer = bc_select_victim();
      sector_buffer->sector = sector_idx;
      sector_buffer->is_used = true;
      block_write(fs_device,sector_idx,sector_buffer->data);
    }
  memcpy(sector_buffer->data + sector_ofs, buffer + bytes_written, chunk_size);
  lock_acquire(&sector_buffer->buffer_lock);
  sector_buffer->dirty = true;
  sector_buffer->clock_bit = true;
  lock_release(&sector_buffer->buffer_lock);
  return true;
}
