#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "threads/malloc.h"
#include "threads/synch.h"
#include "threads/palloc.h"
#include "lib/kernel/list.h"
#include "userprog/pagedir.h"
#include "vm/page.h"
#include "threads/pte.h"
#include "vm/swap.h"

#include "vm/frame.h"

static struct lock frametable_lock;
static struct lock eviction_lock;

void frame_init ()
{
  list_init (&frames);
  lock_init (&frametable_lock_lock);
  lock_init (&eviction_lock);
}


/* Remove the entry from frame table and free the memory space */
static void
remove_frame (void *frame)
{
  struct frame *vf;
  struct list_elem *e;
  
  lock_acquire (&vm_lock);
  e = list_head (&frames);
  while ((e = list_next (e)) != list_tail (&frames))
    {
      vf = list_entry (e, struct frame, elem);
      if (vf->frame == frame)
        {
          list_remove (e);
          free (vf);
          break;
        }
    }
  lock_release (&vm_lock);
}



/* Get the frame struct, whose frame contribute equals the given frame, from
 * the frame list frames. */
static struct frame *
get_frame (void *frame)
{
  struct frame *vf;
  struct list_elem *e;
  
  lock_acquire (&vm_lock);
  e = list_head (&frames);
  while ((e = list_next (e)) != list_tail (&frames))
    {
      vf = list_entry (e, struct frame, elem);
      if (vf->frame == frame)
        break;
      vf = NULL;
    }
  lock_release (&vm_lock);

  return vf;
}

/* Add an entry to frame table */
static bool
add_frame (void *frame)
{
  struct frame *vf;
  vf = calloc (1, sizeof *vf);
 
  if (vf == NULL)
    return false;

  vf->tid = thread_current ()->tid;
  vf->frame = frame;
  
  lock_acquire (&vm_lock);
  list_push_back (&frames, &vf->elem);
  lock_release (&vm_lock);

  return true;
  
}


void *
evict_frame ()
{
  bool result;
  struct frame *vf;
  struct thread *t = thread_current ();

  lock_acquire (&eviction_lock);

  vf = frame_to_evict ();
  if (vf == NULL)
    PANIC ("No frame to evict.");

  result = save_evicted_frame (vf);
  if (!result)
    PANIC ("can't save evicted frame");
  
  vf->tid = t->tid;
  vf->pte = NULL;
  vf->upage = NULL;

  lock_release (&eviction_lock);

  return vf->frame;
}


void *
allocate_frame (enum palloc_flags flags)
{
  void *frame = NULL;

  /* trying to allocate a page from user pool */
  if (flags & PAL_USER)
    {
      if (flags & PAL_ZERO)
        frame = palloc_get_page (PAL_USER | PAL_ZERO);
      else
        frame = palloc_get_page (PAL_USER);
    }

  /* if succeed, add to frame list
     otherwise, should evict one page to swap, but fail the allocator
     for now */
  if (frame != NULL)
    add_frame (frame);
  else
    if ((frame = evict_frame ()) == NULL)
      PANIC ("Evicting frame failed");

  return frame;
}

void
free_frame (void *frame)
{
  /* remove frame table entry */
  remove_frame (frame);
  /* free the frame */
  palloc_free_page (frame);
}

void
assign_frame (void *frame, uint32_t *pte, void *upage)
{
  struct frame *vf;
  vf = get_frame (frame);
  if (vf != NULL)
    {
      vf->pte = pte;
      vf->upage = upage;
    }
}




/* select a frame to evict */
static struct frame *
frame_to_evict ()
{
  struct frame *vf;
  struct thread *t;
  struct list_elem *e;

  struct frame *vf_class0 = NULL;

  int round_count = 1;
  bool found = false;
  /* iterate each entry in frame table */
  while (!found)
    {
      /* go through the vm frame list, try to locate the first encounter
         of each class of the four. The goal is to find a (0,0) class,
         if found, the break eviction selecting is ended,
         if not, set the reference/accessed bit to 0 of each page.
         The maxium round is 2, which is one scan after all the reference
         bit are set to 0, if we still cannot find (0,0), we have to live
         with the first encounter of the lowest nonempty class*/
      e = list_head (&frames);
      while ((e = list_next (e)) != list_tail (&frames))
        {
          vf = list_entry (e, struct frame, elem);
          t = thread_get_by_id (vf->tid);
          bool accessed  = pagedir_is_accessed (t->pagedir, vf->upage);
          if (!accessed)
            {
              vf_class0 = vf;
              list_remove (e);
              list_push_back (&frames, e);
              break;
            }
          else
            {
              pagedir_set_accessed (t->pagedir, vf->upage, false);
            }
        }

      if (vf_class0 != NULL)
        found = true;
      else if (round_count++ == 2)
        found = true;
    }

  return vf_class0;
}

static bool
save_evicted_frame (struct frame *vf)
{
  struct thread *t;
  struct suppl_pte *spte;

  /* Get corresponding thread frame->tid's suppl page table */
  t = thread_get_by_id (vf->tid);

  /* Get suppl page table entry corresponding to frame->upage */
  spte = get_suppl_pte (&t->suppl_page_table, vf->upage);

  /* if no suppl page table entry is found, create one and insert it
     into suppl page table */
  if (spte == NULL)
    {
      spte = calloc(1, sizeof *spte);
      spte->upageaddr = vf->upage;
      spte->in_sawp = true;
      if (!insert_suppl_pte (&t->suppl_page_table, spte))
        return false;
    }

  size_t swap_slot_idx;
  /* if the page is dirty, put into swap
   * if a page is not dirty and is not a file, then it is a stack,
   * it needs to put into swap*/

  if (pagedir_is_dirty (t->pagedir, spte->upageaddr)
           || (!spte->is_file))
    {
      swap_slot_idx = vm_swap_out (spte->upageaddr);
      if (swap_slot_idx == SWAP_ERROR)
        return false;

      spte->in_swap = spte->TRUE;
    }
  /* else if the page clean or read-only, do nothing */

  memset (vf->frame, 0, PGSIZE);
  /* update the swap attributes, including swap_slot_idx,
     and swap_writable */
  spte->swap_slot_idx = swap_slot_idx;
  spte->swap_writable = *(vf->pte) & PTE_W;

  spte->is_loaded = false;

  /* unmap it from user's pagedir, free vm page/frame */
  pagedir_clear_page (t->pagedir, spte->upageaddr);

  return true;
}




