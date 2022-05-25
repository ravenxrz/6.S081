// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void
freerange(void* pa_start, void* pa_end);

static void kfreebycpuid(void* pa, int cpuid);

static int steal_mem(int cur_cpuid);

static int free_memory_pages(int cpuid);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run
{
  struct run* next;
};

struct
{
  struct spinlock lock;
  struct run* freelist;
} kmem[NCPU]; // keme free list per CPU

char kmem_lock_names[NCPU][8];

/**
 *  CPU 0 will call this function to initialize the memory allocator.
 */
void
kinit()
{
  // init locks
  for (int i = 0; i < NCPU; i++) {
    snprintf(kmem_lock_names[i], sizeof(kmem_lock_names[i]), "kmem%d", i);
    initlock(&kmem[i].lock, kmem_lock_names[i]);
  }
  // free memory for all cpus free list
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void* pa_start, void* pa_end)
{
  char* p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  int cpuid = 0;
  for (; p + PGSIZE <= (char*)pa_end; p += PGSIZE) {
    kfreebycpuid(p, cpuid);
    cpuid = (cpuid + 1) % NCPU;
  }
}

/**
 * @brief  only used by freerange
 * 
 * @param p start physical address of that physical page
 * @param cpuid which cpu this physical page belongs to
 */
void
kfreebycpuid(void* pa, int cpuid)
{
  struct run* r;

  if(cpuid < 0 || cpuid >= NCPU)
    panic("kfreebycpuid: wrong cpuid");

  if (((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfreebycpuid: wrong physical address");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run *)pa;

  acquire(&kmem[cpuid].lock);
  r->next = kmem[cpuid].freelist;
  kmem[cpuid].freelist = r;
  release(&kmem[cpuid].lock);
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void* pa)
{
  struct run* r;

  if (((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  push_off();
  int cid = cpuid();
  r = (struct run*)pa;
  acquire(&kmem[cid].lock);
  r->next       = kmem[cid].freelist;
  kmem[cid].freelist = r;
  release(&kmem[cid].lock);
  pop_off();
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void*
kalloc(void)
{
  struct run* r;

  push_off();
  int cid = cpuid();
  acquire(&kmem[cid].lock);
alloc:
  r = kmem[cid].freelist;
  if(!r) {
    // current cpu has no free memory
    // steal memory from other cpus
    if(steal_mem(cid) == -1) {
      release(&kmem[cid].lock);
      pop_off();
      return 0;
    }
    // steal success
    // printf("steal memory sucecss\n");
    goto alloc;
  } else {
    kmem[cid].freelist = r->next;
  }

  release(&kmem[cid].lock);
  pop_off();

  if (r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}


/**
 * @brief steal memory from other cpus
 * 
 * @param cur_cpuid  current cpu which is trying to allocate memory but no free memory found
 * @return 0 success, -1 failed
 * @note keme[cid].lock must be held
 */
int steal_mem(int cur_cpuid) 
{
  // search free page from other cpu
  release(&kmem[cur_cpuid].lock); // release current cpu lock to avoid dead lock
  int next_cpuid = (cur_cpuid + 1) % NCPU;
  for(int i = 0; i< NCPU - 1;i++) {
    acquire(&kmem[next_cpuid].lock);
    if (kmem[next_cpuid].freelist)
      break;
    release(&kmem[next_cpuid].lock);
    next_cpuid = (next_cpuid + 1) % NCPU;
  }
  acquire(&kmem[cur_cpuid].lock);

  if(cur_cpuid == next_cpuid) {
    // no cpu has free memory
    return -1;
  } else {
    // now we hold cur cpu and next cpu lock at the same time
    // steal pages from next cpu
    int free_page_num = free_memory_pages(next_cpuid);
    int steal_page_num = free_page_num/2 + 1;
    // printf("cpu %d steal %d page from cpu %d\n", cur_cpuid, steal_page_num, next_cpuid);

    struct run* r = kmem[next_cpuid].freelist;
    struct run* rprev = r;
    while(steal_page_num) {
      steal_page_num--;
      rprev = r;
      r = r->next;
    }
    rprev->next = kmem[cur_cpuid].freelist;
    kmem[cur_cpuid].freelist = kmem[next_cpuid].freelist;
    kmem[next_cpuid].freelist = r;

    release(&kmem[next_cpuid].lock);
  }
  return 0;
}

/**
 * @brief get kmem[cpuid].free_list length
 * 
 * @param cpuid 
 * @return the length of freelist of keme[cpuid]
 * @note kmem[cpuid].lock must be held
 */
int free_memory_pages(int cpuid)
{
  int ret = 0;
  struct run *r = kmem[cpuid].freelist;
  while(r) {
    ret++;
    r = r->next;
  }
  return ret;
}