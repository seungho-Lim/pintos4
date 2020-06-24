#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"
#include "filesys/buffer_cache.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44



#define INDIRECT_BLOCK_ENTRIES 128
#define DIRECT_BLOCK_ENTRIES 123

enum direct_t
{
  NORMAL_DIRECT,
  INDIRECT,
  DOUBLE_INDIRECT,
  OUT_LIMIT,
};

struct sector_location
{
  int directness;
  off_t index1;
  off_t index2;
};

struct inode_indirect_block
{
  block_sector_t map_table[INDIRECT_BLOCK_ENTRIES];
};

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk
  {
    block_sector_t direct_map_table[DIRECT_BLOCK_ENTRIES];
    block_sector_t indirect_block_sec;
    block_sector_t double_indirect_block_sec;

    int is_dir;
    off_t length;                       /* File size in bytes. */
    unsigned magic;                     /* Magic number. */
  };

/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t
bytes_to_sectors (off_t size)
{
  return DIV_ROUND_UP (size, BLOCK_SECTOR_SIZE);
}



/* In-memory inode. */
struct inode 
  {
    struct list_elem elem;              /* Element in inode list. */
    block_sector_t sector;              /* Sector number of disk location. */
    int open_cnt;                       /* Number of openers. */
    bool removed;                       /* True if deleted, false otherwise. */
    int deny_write_cnt;                 /* 0: writes ok, >0: deny writes. */
    struct lock extend_lock;		/* Semaphore lock. */    
  };

/* Returns the block device sector that contains byte offset POS
   within INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */

static block_sector_t byte_to_sector (const struct inode_disk *inode_disk, off_t pos);
bool inode_update_file_length (struct inode_disk *inode_disk, off_t start_pos, off_t end_pos);
static bool get_disk_inode (const struct inode *inode, struct inode_disk *inode_disk);
static void free_inode_sectors (struct inode_disk *inode_disk);

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/* Initializes the inode module. */
void
inode_init (void) 
{
  list_init (&open_inodes);
}

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool
inode_create (block_sector_t sector, off_t length, uint32_t is_dir)
{
  struct inode_disk *disk_inode = NULL;
  bool success = false;

  ASSERT (length >= 0);

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT (sizeof *disk_inode == BLOCK_SECTOR_SIZE);

  disk_inode = calloc (1, sizeof *disk_inode);
  if (disk_inode != NULL)
    {
      disk_inode->length = length;
      disk_inode->magic = INODE_MAGIC;
      disk_inode->is_dir = is_dir;
      /* Allocate disk block which size is LENGTH
         using inode_update_file_length(). */
      if (length > 0)
	if(inode_update_file_length (disk_inode, 0, length) == false)
          {
	    free (disk_inode);
            return false;
          }
      /* Record on-disk inode to buffer cache using bc_write(). */
      bc_write(sector, disk_inode, 0, BLOCK_SECTOR_SIZE, 0);
      free (disk_inode);
      success = true;
    }
  return success;
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *
inode_open (block_sector_t sector)
{
  struct list_elem *e;
  struct inode *inode;

  /* Check whether this inode is already open. */
  for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
       e = list_next (e)) 
    {
      inode = list_entry (e, struct inode, elem);
      if (inode->sector == sector) 
        {
          inode_reopen (inode);
          return inode; 
        }
    }

  /* Allocate memory. */
  inode = malloc (sizeof *inode);
  if (inode == NULL)
    return NULL;

  /* Initialize. */
  list_push_front (&open_inodes, &inode->elem);
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  lock_init (&inode->extend_lock);
  return inode;
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen (struct inode *inode)
{
  if (inode != NULL)
    inode->open_cnt++;
  return inode;
}

/* Returns INODE's inode number. */
block_sector_t
inode_get_inumber (const struct inode *inode)
{
  return inode->sector;
}

/* Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void
inode_close (struct inode *inode) 
{
  /* Ignore null pointer. */
  if (inode == NULL)
    return;

  /* Release resources if this was the last opener. */
  if (--inode->open_cnt == 0)
    {
      /* Remove from inode list and release lock. */
      list_remove (&inode->elem);
 
      /* Deallocate blocks if removed. */
      if (inode->removed) 
        {
          /* Get INODE_DISK using get_disk_inode() and free
             correspond block using free_inode_sectors() and
             free_map_release(). Also free INODE_DISK. */
	  struct inode_disk *inode_disk;
          inode_disk = malloc (BLOCK_SECTOR_SIZE);
          get_disk_inode(inode,inode_disk);
	  free_inode_sectors (inode_disk);
          free_map_release (inode->sector, 1);
          free (inode_disk);
        }

      free (inode); 
    }
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void
inode_remove (struct inode *inode) 
{
  ASSERT (inode != NULL);
  inode->removed = true;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset) 
{
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;

  /* Allocate for INODE_DISK which represent on-disk inode,
     and get information which correspond to INODE
     from buffer cache using get_disk_inode(). */
  struct inode_disk *inode_disk;
  inode_disk = (struct inode_disk *)malloc(BLOCK_SECTOR_SIZE);
  if(inode_disk==NULL)
    return -1;
  get_disk_inode(inode,inode_disk);
  while (size > 0) 
    {
      /* Disk sector to read, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode_disk, offset);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually copy out of this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      /* Read full sector directly into caller's buffer. */
      bc_read(sector_idx,buffer,bytes_read,chunk_size,sector_ofs);
      
      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_read += chunk_size;
    }
  free(inode_disk);
  return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size,
                off_t offset) 
{
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;

  if (inode->deny_write_cnt)
    return 0;

  /* Allocate for INODE_DISK which represent on-disk inode,
     and get information which correspond to INODE
     from buffer cache using get_disk_inode(). */
  struct inode_disk *inode_disk;
  inode_disk = malloc (BLOCK_SECTOR_SIZE);
  if(inode_disk == NULL)
    return -1;
  get_disk_inode(inode,inode_disk);

  lock_acquire (&inode->extend_lock);
  int old_length = inode_disk->length;
  int write_end = offset + size - 1;
  /* If file length is increased, call inode_update_file_length(). */
  if (write_end > old_length - 1)
    {
      inode_update_file_length (inode_disk, old_length, write_end);
      inode_disk->length += write_end - old_length + 1;
    }
  lock_release (&inode->extend_lock);

  while (size > 0) 
    {
      /* Sector to write, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode_disk, offset);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_disk->length - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually write into this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      /* Write full sector directly to disk. */
      bc_write (sector_idx, (void *)buffer, bytes_written, chunk_size, sector_ofs);

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_written += chunk_size;
    }
  /* Record INODE_DISK to buffer cache using bc_write(). */
  bc_write (inode->sector, (void *)inode_disk, 0, BLOCK_SECTOR_SIZE, 0);
  free(inode_disk);
  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void
inode_deny_write (struct inode *inode) 
{
  inode->deny_write_cnt++;
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode) 
{
  ASSERT (inode->deny_write_cnt > 0);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
}

/* Returns the length, in bytes, of INODE's data. */
off_t
inode_length (const struct inode *inode)
{
  struct inode_disk disk_inode;
  get_disk_inode(inode,&disk_inode);
  return disk_inode.length;
}

static bool
get_disk_inode (const struct inode *inode, struct inode_disk *inode_disk)
{
  /* Read on-disk inode which correspond to INODE->SECTOR
     from buffer cache using bc_read(). */
  bc_read (inode->sector, (void *)inode_disk, 0, BLOCK_SECTOR_SIZE, 0);
  return true;
}

static void
locate_byte (off_t pos, struct sector_location *sec_loc)
{
  /* Calculate offset folloing disk block access method. */
  off_t pos_sector = pos / BLOCK_SECTOR_SIZE;
  
  /* In direct,indirect method only use index1. */
  if (pos_sector < DIRECT_BLOCK_ENTRIES)
    {
      sec_loc->directness = NORMAL_DIRECT;
      sec_loc->index1 = pos_sector;
      sec_loc->index2 = 0;
    }
  else if (pos_sector < (off_t)(DIRECT_BLOCK_ENTRIES + INDIRECT_BLOCK_ENTRIES))
    {
      sec_loc->directness = INDIRECT;
      sec_loc->index1 = pos_sector - (off_t)DIRECT_BLOCK_ENTRIES;
      sec_loc->index2 = 0;
    }
  else if (pos_sector < (off_t)(DIRECT_BLOCK_ENTRIES + 
		INDIRECT_BLOCK_ENTRIES * (INDIRECT_BLOCK_ENTRIES + 1)))
    {
      sec_loc->directness = DOUBLE_INDIRECT;
      sec_loc->index1 = (pos_sector - (off_t)DIRECT_BLOCK_ENTRIES -
		(off_t)INDIRECT_BLOCK_ENTRIES)/ INDIRECT_BLOCK_ENTRIES;
      sec_loc->index2 = (pos_sector - (off_t)DIRECT_BLOCK_ENTRIES - 
		(off_t)INDIRECT_BLOCK_ENTRIES)% INDIRECT_BLOCK_ENTRIES;
    }
  else
    sec_loc->directness = OUT_LIMIT;
}

static inline off_t
map_table_offset (int index)
{
  /* Change offset to byte. */
  return (off_t)index * 4;
}

static bool
register_sector (struct inode_disk *inode_disk, block_sector_t new_sector,
			struct sector_location sec_loc)
{
  block_sector_t *new_block;
  block_sector_t *new_block2;
  /* Update INODE_DISK's disk block number to NEW_SECTOR.  */
  switch(sec_loc.directness)
    {
      /* In direct method, just update SEC_LOC.INDEX1's index
         to NEW_SECTOR. */
      case NORMAL_DIRECT:
        inode_disk->direct_map_table[sec_loc.index1] = new_sector;
        break;
      /* In indirect method, put NEW_SECTOR to NEW_BLOCK and
         record that block to buffer cache. */
      case INDIRECT:
        if(sec_loc.index1 ==0)
          {
            block_sector_t sector_idx;
            if(free_map_allocate(1,&sector_idx))
              inode_disk->indirect_block_sec = sector_idx;
          }
        new_block = malloc(BLOCK_SECTOR_SIZE);
        if(new_block == NULL)
          return false;
        new_block[sec_loc.index1] = new_sector;
        bc_write(inode_disk->indirect_block_sec,(void *)new_block,map_table_offset(sec_loc.index1),4,map_table_offset(sec_loc.index1));
        free(new_block);
        break;
      /* In double indirect method put NEW_SECTOR to NEW_BLOCK2
         and also put NEW_BLOCK2 to NEW_BLOCK1 and record
         that block to buffer cache. */

      case DOUBLE_INDIRECT:
        if(sec_loc.index1 == 0 && sec_loc.index2 == 0)
          {
            block_sector_t sector_idx;
            if(free_map_allocate(1,&sector_idx))
              inode_disk->double_indirect_block_sec = sector_idx;
          }
        if(sec_loc.index2 == 0)
          {
            block_sector_t sector_idx;
            if(!free_map_allocate(1,&sector_idx))
              return false;
            new_block = malloc(BLOCK_SECTOR_SIZE);
            if(new_block == NULL)
              return false;
            new_block[sec_loc.index1] = sector_idx;
            bc_write(inode_disk->double_indirect_block_sec, (void*)new_block,
		map_table_offset(sec_loc.index1), 4, map_table_offset(sec_loc.index1));
            free(new_block);
          }
        new_block = malloc(BLOCK_SECTOR_SIZE);
        if(new_block == NULL)
          return false;
        new_block2 = malloc(BLOCK_SECTOR_SIZE);
        if(new_block2 == NULL)
          {
            free(new_block);
            return false;
          }
        bc_read(inode_disk->double_indirect_block_sec,
		(void*)new_block, 0, BLOCK_SECTOR_SIZE, 0);
        new_block2[sec_loc.index2] = new_sector;
        bc_write(new_block[sec_loc.index1], (void *)new_block2,
		map_table_offset(sec_loc.index2), 4, map_table_offset(sec_loc.index2));
        free(new_block);
        free(new_block2);
        break;

      default:
        return false;
    }
  
  return true;
}

static block_sector_t
byte_to_sector (const struct inode_disk *inode_disk, off_t pos)
{
  /* Using file offset, search on-disk inode and return
     that's sector number. */
  block_sector_t result_sec;
  if (pos < inode_disk->length)
    {
      struct inode_indirect_block *ind_block;
      struct inode_indirect_block *ind_block2;
      struct sector_location sec_loc;
      locate_byte (pos, &sec_loc);

      switch (sec_loc.directness)
        {
          /* In direct method get disk block number from direct_map_table. */
          case NORMAL_DIRECT:
	    result_sec = inode_disk->direct_map_table [sec_loc.index1];
	    break;
          /* In indirect method get disk block from buffer cache.
             And get block number in block's map_table. */
          case INDIRECT:
	    ind_block = (struct inode_indirect_block *) malloc (BLOCK_SECTOR_SIZE);
	    if (ind_block)
	      {
	        bc_read (inode_disk->indirect_block_sec,
			(void *)ind_block, 0, BLOCK_SECTOR_SIZE, 0);
	        result_sec = ind_block->map_table[sec_loc.index1];
                free (ind_block);
	      }
	    else
	      result_sec = 0;
	
	    break;
          /* In double indirect method, first, read DOUBLE_INDIRECT_BLOCK_SEC
             and get IND_BLOCK then get map_table from IND_BLOCK and save it
             IND_BLOCK2. Finally get sector number from IND_BLOCK2. */
          case DOUBLE_INDIRECT:
      	    ind_block = (struct inode_indirect_block *) malloc (BLOCK_SECTOR_SIZE);
	    ind_block2 = (struct inode_indirect_block *) malloc (BLOCK_SECTOR_SIZE);
	    if (ind_block && ind_block2)
	      {
	        bc_read (inode_disk->double_indirect_block_sec,
			(void *)ind_block, 0, BLOCK_SECTOR_SIZE, 0);
 	        bc_read (ind_block->map_table[sec_loc.index1],
			(void *)ind_block2, 0, BLOCK_SECTOR_SIZE, 0);
	        result_sec = ind_block2->map_table[sec_loc.index2];
                free (ind_block2);
	        free (ind_block);
	      }
	    else
	      result_sec = 0;

	    break;
          default:
	    result_sec = 0;
	    break;
        }
        return result_sec;
    }
  return 0;
}

bool
inode_update_file_length (struct inode_disk *inode_disk, off_t start_pos, off_t end_pos)
{
  /* When file offset is larger than original file size,
     allocate new blocks and update inode. */
  off_t size = end_pos - start_pos;
  off_t offset = start_pos;
  int chunk_size = BLOCK_SECTOR_SIZE;
  void *zeros = calloc(1, BLOCK_SECTOR_SIZE);

  /* In each loop step, allocate new disk block which size is same block. */
  while (size > 0)
    {
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;
      /* If offset SECTOR_OFS is not 0, it means the block is already
         allocated. So change offset to be 0. */
      if (sector_ofs > 0)
        {
          offset -= sector_ofs;
          size += sector_ofs; 
        }
      else
        {
          struct sector_location sec_loc;
          block_sector_t sector_idx;
	  /* Allocate new block and update INODE_DISK's
             block number to SECTOR_IDX. */
          if (free_map_allocate (1, &sector_idx))
            {
       	      locate_byte (offset, &sec_loc);
	      register_sector (inode_disk, sector_idx, sec_loc);
            }
          else
            {
	      free(zeros);
	      return false;
            }
          /* Init new disk block to 0. */
          bc_write (sector_idx, zeros, 0, BLOCK_SECTOR_SIZE, 0);
        }
      size -= chunk_size;
      offset += chunk_size;
    }
  free(zeros);
  return true;
}

static void
free_inode_sectors (struct inode_disk *inode_disk)
{
  struct inode_indirect_block *ind_block_1;
  struct inode_indirect_block *ind_block_2;
  int i, j;
  /* Free all disk blocks. */
  
  /* Free which allocated by double indirect method. */
  if (inode_disk->double_indirect_block_sec > 0)
    {
      i = 0;
      ind_block_1 = malloc (BLOCK_SECTOR_SIZE);
      if (ind_block_1 == NULL)
        return;
      /* Read 1st index block from buffer cache using bc_read(). */
      bc_read (inode_disk->double_indirect_block_sec,
		(void *)ind_block_1, 0, BLOCK_SECTOR_SIZE, 0);
      /* Using MAP_TABLE, access to all 2nd index block and
         free that using free_map_release(). */
      while (ind_block_1->map_table[i] > 0)
        {
          j = 0;
          ind_block_2 = malloc (BLOCK_SECTOR_SIZE);
          if (ind_block_2 == NULL)
            {
	      free (ind_block_1);
	      return;
            }
          bc_read (ind_block_1->map_table[i],
		(void *)ind_block_2, 0, BLOCK_SECTOR_SIZE, 0);
          while (ind_block_2->map_table[j] > 0)
            {
	      free_map_release (ind_block_2->map_table[j], 1);
	      j++;
            }
          free (ind_block_2);
          free_map_release (ind_block_1->map_table[i], 1);
          i++;
        }
      free (ind_block_1);
      free_map_release (inode_disk->double_indirect_block_sec, 1);
    }
  /* Free which allocated by indirect method. */
  if (inode_disk->indirect_block_sec > 0)
    {
      i = 0;
      ind_block_1 = malloc (BLOCK_SECTOR_SIZE);
      if (ind_block_1 == NULL)
        return;
      /* Read index block from buffer cache using bc_read(). */
      bc_read (inode_disk->indirect_block_sec,
		(void *)ind_block_1, 0, BLOCK_SECTOR_SIZE, 0);
      /* Using MAP_TABLE, access to all blocks and
         free that using free_map_release(). */
      while (ind_block_1->map_table[i] > 0)
        {
          free_map_release (ind_block_1->map_table[i], 1);
          i++;
        }
      free (ind_block_1);
      free_map_release (inode_disk->indirect_block_sec, 1);
    }

  i = 0;
  /* Free which allocated by direct method. */
  while (inode_disk->direct_map_table[i] > 0)
    {
      free_map_release (inode_disk->direct_map_table[i], 1);
      i++;
    }
}

bool
inode_is_dir (const struct inode *inode)
{
  /* Get on-disk inode information using get_disk_inode()
     and return that is directory or not. */
  bool result;
  struct inode_disk *inode_disk;
  inode_disk = malloc (BLOCK_SECTOR_SIZE);
  if (inode_disk == NULL)
    return false;
  get_disk_inode (inode, inode_disk);
  if (inode_disk->is_dir == 1)
    result = true;
  else
    result = false;

  free (inode_disk);
  return result;
}

bool
inode_removed(struct inode *inode)
{
  return inode->removed;
}
