// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.
                  
int  *pg_ref_arr;     // 实际上应该不用int就可以，毕竟一个页不会被引用这么多次
char *avail_mem_start;  // 可用物理内存开始地址

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

void
kinit()
{
  initlock(&kmem.lock, "kmem");

  // init pg_ref
  char *pa_start = (char *)PGROUNDUP((uint64)end);
  char *pa_end = (char *)PGROUNDDOWN(PHYSTOP);
  pg_ref_arr = (int *)pa_start;
  // if ((pa_end - pa_start) % 1025 != 0) {
  //   printf("end:%p\n", end);
  //   printf("pa start:%p\n", pa_start);
  //   printf("pa end:%p\n", pa_end);
  //   panic("memory is not aligned for page table reference initialization");
  // }
  avail_mem_start = (char*)PGROUNDUP((uint64)(pa_start + (pa_end - pa_start) / 1025));
  printf("pg_ref_arr start:%p, avail mem start:%p\n", pg_ref_arr, avail_mem_start);
  printf("max page:%d\n", (uint64)(pa_end - avail_mem_start) / 4);
  if((uint64)avail_mem_start % PGSIZE != 0) {
    panic("avail mem is not 4K-aligned");
  }
  memset(pg_ref_arr, 0, (uint64)avail_mem_start - (uint64)pg_ref_arr);

  // free avail mem
  freerange(avail_mem_start, pa_end);
}

void
pg_ref(void* pa)
{
  if((char *)pa < avail_mem_start) {
    panic("pg_ref: pa is not in avail mem");
  }
  acquire(&kmem.lock);
  int idx = ((uint64)pa - (uint64)avail_mem_start) / PGSIZE;
  // printf("ref -> pa:%p, page idx:%d, val=%d\n", pa, idx, pg_ref_arr[idx]);
  pg_ref_arr[idx]++;
  release(&kmem.lock);
}

void
pg_unref(void* pa)
{
  if((char *)pa < avail_mem_start) {
    panic("pg_ref: pa is not in avail mem");
  }
  acquire(&kmem.lock);
  int idx = ((uint64)pa - (uint64)avail_mem_start) / PGSIZE;
  // printf("unref -> pa:%p, page idx:%d, val=%d\n", pa, idx, pg_ref_arr[idx]);
  pg_ref_arr[idx]--;
  if(pg_ref_arr[idx] == 0) {
    release(&kmem.lock);
    kfree(pa);    // TOOD: kfree和ref cnt是否需要保证原子性？
  } else {
    release(&kmem.lock);
  }
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  acquire(&kmem.lock);
  // set ref cnt to 0
  int idx = ((uint64)pa - (uint64)avail_mem_start) / PGSIZE;
  if(pg_ref_arr[idx] != 0) {
    release(&kmem.lock);
    printf("kfree: pa:%p, idx:%d, ref cnt:%d\n", pa, idx, pg_ref_arr[idx]);
    panic("kfree a page with ref cnt being non-zero");
  }

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  if(r) {
    memset((char*)r, 5, PGSIZE); // fill with junk
    pg_ref((void*)r);
  }
  return (void*)r;
}
