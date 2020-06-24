#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"
#include "filesys/buffer_cache.h"
#include "threads/thread.h"

/* Partition that contains the file system. */
struct block *fs_device;

static void do_format (void);

/* Initializes the file system module.
   If FORMAT is true, reformats the file system. */
void
filesys_init (bool format) 
{
  fs_device = block_get_role (BLOCK_FILESYS);
  if (fs_device == NULL)
    PANIC ("No file system device found, can't initialize file system.");

  inode_init ();
  bc_init ();
  free_map_init ();

  if (format) 
    do_format ();
  
  free_map_open ();
  /* When filesystem is start set current directory to root. */
  thread_current ()->dir = dir_open_root();
  
}

/* Shuts down the file system module, writing any unwritten data
   to disk. */
void
filesys_done (void) 
{
  bc_term ();
  free_map_close ();
}

/* Creates a file named NAME with the given INITIAL_SIZE.
   Returns true if successful, false otherwise.
   Fails if a file named NAME already exists,
   or if internal memory allocation fails. */
bool
filesys_create (const char *name, off_t initial_size) 
{

  char *file_name = malloc(sizeof(char) * (strlen(name) + 1));
  if(file_name == NULL)
    {
      return false;
    }
  block_sector_t inode_sector = 0;
  /* Open proper directory using parse_path(). */
  struct dir *dir = parse_path(name,file_name);

  /* Prevent to create something when directory is removed. */
  if(dir != NULL && inode_removed(dir_get_inode(dir)))
    {
      dir_close (dir);
      free(file_name);
      return false;
    }

  /* "inode_sector < 4095" is deal edge case, when
      block is full. To do safe operation we need
      to use all of block. */
  bool success = (dir != NULL
                  && free_map_allocate (1, &inode_sector)
                  && inode_create (inode_sector, initial_size,0)
                  && dir_add (dir, file_name, inode_sector)
                  && inode_sector < 4095);
  if (!success && inode_sector != 0) 
    free_map_release (inode_sector, 1);
  dir_close (dir);
  free(file_name);

  return success;
}

/* Opens the file with the given NAME.
   Returns the new file if successful or a null pointer
   otherwise.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
struct file *
filesys_open (const char *name)
{

  struct inode *inode = NULL;
  char *file_name = malloc(sizeof(char) * (strlen(name) + 1));
  struct dir *dir;
  if(file_name == NULL)
    {
      return NULL;
    }
  /* Open proper directory using parse_path(). */
  dir = parse_path(name,file_name);

  /* Prevent to open something when directory is removed. */
  if(dir != NULL && inode_removed(dir_get_inode(dir)))
    {
      dir_close (dir);
      free(file_name);
      return NULL;
    }

  if (dir != NULL)
    dir_lookup (dir, file_name, &inode);

  dir_close (dir);

  free(file_name);
  return file_open (inode);
}

/* Deletes the file named NAME.
   Returns true if successful, false on failure.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
bool
filesys_remove (const char *name) 
{
  char *file_name = malloc(sizeof(char) * (strlen(name) + 1));
  struct dir *dir;
  if(file_name == NULL)
    return false;

  /* Open proper directory using parse_path(). */
  dir = parse_path(name,file_name);
  
  bool success = dir != NULL && dir_remove (dir, file_name);
  dir_close (dir); 

  free(file_name);
  return success;
}

/* Formats the file system. */
static void
do_format (void)
{
  struct dir *root_dir = dir_open_root();
  struct inode *root_inode = dir_get_inode(root_dir);
  printf ("Formatting file system...");
  free_map_create ();
  if (!dir_create (ROOT_DIR_SECTOR, 16))
    PANIC ("root directory creation failed");
  
  /* Add "." and ".." to root. */
  dir_add(root_dir,".",inode_get_inumber(root_inode));
  dir_add(root_dir,"..",inode_get_inumber(root_inode));
  free_map_close ();
  printf ("done.\n");
}


struct dir*
parse_path (char *path_name, char *file_name)
{
  struct dir *dir;
  /* If start is '/' it is absolute directory so start
     from root, other case start from current directory. */
  if (path_name[0] == '/')
    dir = dir_open_root ();
  else
    dir = dir_reopen (thread_current ()->dir);
  if (path_name == NULL || file_name == NULL)
    return NULL;
  if (strlen (path_name) == 0)
    return NULL;

  struct inode *inode;
  char *token, *nextToken, *savePtr;
  char *path_name_copy;
  /* Tokenizing PATH_NAME. */
  path_name_copy = malloc (strlen(path_name) + 1);
  if(path_name_copy==NULL)
    {
      dir_close (dir);
      return NULL;
    }
  strlcpy (path_name_copy, path_name, strlen (path_name) + 1);
  token = strtok_r (path_name_copy, "/", &savePtr);
  nextToken = strtok_r (NULL, "/", &savePtr);

  while (token != NULL && nextToken != NULL)
    {
      inode = NULL;
      if (!dir_lookup (dir, token, &inode))
        {
          dir_close (dir);
          return NULL;
        }
      if (inode_is_dir (inode) == false)
        {
          dir_close (dir);
          return NULL;
        }
      dir_close (dir);
      dir = dir_open (inode);
      token = nextToken;
      nextToken = strtok_r (NULL, "/", &savePtr);
    }

  /* If TOKEN isn't null copy it to FILE_NAME.
     Else it means root case. So deal root it
     set FILE_NAME to '.' The dir is root and
     FILE_NAME is '.' mean point root. */
  if (token != NULL)
    strlcpy (file_name, token, strlen (token) + 1);
  else
    strlcpy (file_name, ".", 2);
  free (path_name_copy);
  return dir;
}

bool
filesys_create_dir (const char *name)
{
  if(name == NULL)
    return false;
  block_sector_t sector_idx;
  char *file_name = malloc(sizeof(char) * (strlen(name) + 1));
  if(file_name == NULL)
    return false;
  struct inode *inode;

  /* Open proper directory using parse_path(). */
  struct dir *dir = parse_path(name,file_name);
  if(dir == NULL)
    {
      return false;
    }
  /* If the file is already exist fail. */
  if(dir_lookup (dir, file_name, &inode))
    {
      dir_close(dir);
      return false;
    }

  if(!free_map_allocate(1, &sector_idx))
    {
      dir_close(dir);
      free(file_name);
      return false;
    }
  /* Prevent use all resource for safe operation. */
  if(sector_idx == 4095)
    {
      dir_close(dir);
      free_map_release(sector_idx,1);
      free(file_name);
      return false;
    }
  /* Make directory in allocated sector. */
  if(!dir_create(sector_idx,16))
    {
      dir_close(dir);
      free_map_release(sector_idx,1);
      free(file_name);
      return false;
    }

  /* Add FILE_NAME to directory. */
  if(!dir_add(dir,file_name,sector_idx))
    {
      free_map_release(sector_idx,1);
      dir_remove(dir,file_name);
      dir_close(dir);
      free(file_name);
      return false;
    }
  /* Add "." and ".." to directory. */
  struct dir *dir2 = dir_open(inode_open(sector_idx));
  dir_add(dir2,".",sector_idx);
  dir_add(dir2,"..", inode_get_inumber(dir_get_inode(dir)));

  free(file_name);
  return true;
}
