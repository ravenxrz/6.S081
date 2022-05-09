#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "spinlock.h"
#include "proc.h"

/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[]; // kernel.ld sets this to end of kernel code.

extern char trampoline[]; // trampoline.S

extern int
copyin_new(pagetable_t pagetable, char* dst, uint64 srcva, uint64 len);

extern int
copyinstr_new(pagetable_t pagetable, char* dst, uint64 srcva, uint64 max);

/**
 *  risc-v use 3 level page table, root level is 2, middle 1, last level is 0
 *
 * page table 0x0000000087f6e000
 *  ..0: pte 0x0000000021fda801 pa 0x0000000087f6a000
 *  .. ..0: pte 0x0000000021fda401 pa 0x0000000087f69000
 *  .. .. ..0: pte 0x0000000021fdac1f pa 0x0000000087f6b000
 *  .. .. ..1: pte 0x0000000021fda00f pa 0x0000000087f68000
 *  .. .. ..2: pte 0x0000000021fd9c1f pa 0x0000000087f67000
 *  ..255: pte 0x0000000021fdb401 pa 0x0000000087f6d000
 *  .. ..511: pte 0x0000000021fdb001 pa 0x0000000087f6c000
 *  .. .. ..510: pte 0x0000000021fdd807 pa 0x0000000087f76000
 *  .. .. ..511: pte 0x0000000020001c0b pa 0x0000000080007000
 * lab pgtbl: Explain the output of vmprint in terms of Fig 3-4 from the text.
  What does page 0 contain? What is in page 2? When running in user mode, could
 the process read/write the memory mapped by page 1?
 * ANS: page 0是 stack， page 2是stack guard。 page 1如果运行在user
 mode是可以read/write的。
 */
static void
vmprint_level(pagetable_t pagetable, int level)
{
  if (level == -1) {
    return;
  }
  if (level < -1 || level > 2) {
    panic("vmprint_level: level out of range");
  }
  // there are 2^9 = 512 PTEs in a page table
  for (int i = 0; i < 512; i++) {
    pte_t pte = pagetable[i];
    if ((pte & PTE_V)) {
      // print tree level
      for (int j = 2; j >= level; j--) {
        if (j == level) {
          printf("..%d: ", i);
        } else {
          printf(".. ");
        }
      }
      printf("pte %p pa %p\n", pte, PTE2PA(pte));
      vmprint_level((pagetable_t)PTE2PA(pte), level - 1);
    }
  }
}

/**
 * for debugging
 */
void
vmprint(pagetable_t pagetable)
{
  printf("page table %p\n", pagetable);
  vmprint_level(pagetable, 2);
}

/**
 * create a direct-map page table for the kernel.
 */
void
kpgtbl_init(void)
{
  kernel_pagetable = (pagetable_t)kalloc(); // 分配top level page table entry
  memset(kernel_pagetable, 0, PGSIZE);      // 清空
  kvminit(kernel_pagetable);
  // CLINT
  if (mappages(kernel_pagetable, CLINT, 0x10000, CLINT, PTE_R | PTE_W) < 0) {
    panic("CLINT mapping failed");
  }
}

/*
 *
 * kernel page table init
 * return 0, means success, non-zero means failure
 */
void
kvminit(pagetable_t pagetable)
{
  // map io devices
  // uart registers
  if (mappages(pagetable, UART0, PGSIZE, UART0, PTE_R | PTE_W) < 0) {
    panic("uart0 mapping failed");
  }
  // virtio mmio disk interface
  if (mappages(pagetable, VIRTIO0, PGSIZE, VIRTIO0, PTE_R | PTE_W) < 0) {
    panic("virtio0 mapping failed");
  }
  // PLIC
  if (mappages(pagetable, PLIC, 0x400000, PLIC, PTE_R | PTE_W) < 0) {
    panic("PLC mapping failed");
  }

  // // CLINT
  // if (mappages(kernel_pagetable, CLINT, 0x10000, CLINT, PTE_R | PTE_W) < 0) {
  //   panic("CLINT mapping failed");
  // }

  // map kernel text executable and read-only.
  if (mappages(pagetable, KERNBASE, (uint64)etext - KERNBASE, KERNBASE, PTE_R | PTE_X) < 0) {
    panic("kernel text mapping failed");
  }

  // map kernel data and the physical RAM we'll make use of.
  if (mappages(pagetable, (uint64)etext, PHYSTOP - (uint64)etext, (uint64)etext, PTE_R | PTE_W)) {
    panic("kernel data and free memory mapping failed");
  }

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  if (mappages(pagetable, TRAMPOLINE, PGSIZE, (uint64)trampoline, PTE_R | PTE_X) < 0) {
    panic("kernel TRAMPOLINE mapping failed");
  }
}

// Switch h/w page table register to the kernel's page table,
// and enable paging.
void
kvminithart(void)
{
  load_kpgtbl(kernel_pagetable);
}

void
load_kpgtbl(pagetable_t pagetable)
{
  w_satp(MAKE_SATP(pagetable));
  // TODO: 弄清楚为什么是flush all TLB entries,
  // 修改是先修改TLB中的映射项，然后flush到内存吗？那为什么xv6 (34页)提到的是 invalidate corresponding TLB entries?
  sfence_vma();
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..29 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..11 -- 12 bits of byte offset within the page.
pte_t*
walk(pagetable_t pagetable, uint64 va, int alloc)
{
  if (va >= MAXVA)
    panic("walk");

  for (int level = 2; level > 0; level--) {
    pte_t* pte = &pagetable[PX(level, va)];
    if (*pte & PTE_V) { // *pte代表的address，这里既是下一层page
                        // table的物理地址，也是用来做判定的虚拟地址，这里依赖了xv6的direct-mapping方式
      pagetable = (pagetable_t)PTE2PA(*pte);
    } else {
      if (!alloc || (pagetable = (pde_t*)kalloc()) == 0)
        return 0;
      memset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V; // 初始化到第0个 PTE, 同时设置 perm为 PTE_V
    }
  }
  return &pagetable[PX(0, va)];
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  pte_t* pte;
  uint64 pa;

  if (va >= MAXVA)
    return 0;

  pte = walk(pagetable, va, 0);
  if (pte == 0)
    return 0;
  if ((*pte & PTE_V) == 0)
    return 0;
  if ((*pte & PTE_U) == 0)
    return 0;
  pa = PTE2PA(*pte);
  return pa;
}

// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
void
kvmmap(uint64 va, uint64 pa, uint64 sz, int perm)
{
  if (mappages(kernel_pagetable, va, sz, pa, perm) != 0)
    panic("kvmmap");
}

// translate a kernel virtual address to
// a physical address. only needed for
// addresses on the stack.
// assumes va is page aligned.
uint64
kvmpa(uint64 va)
{
  uint64 off = va % PGSIZE;
  pte_t* pte;
  uint64 pa;
  struct proc* p = myproc();

  pte = walk(p->kpagetable, va, 0);
  if (pte == 0)
    panic("kvmpa");
  if ((*pte & PTE_V) == 0)
    panic("kvmpa");
  pa = PTE2PA(*pte);
  return pa + off;
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not
// be page-aligned. Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
int
mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
  uint64 a, last;
  pte_t* pte;

  a    = PGROUNDDOWN(va); // 对齐处理
  last = PGROUNDDOWN(va + size - 1);
  for (;;) {
    if ((pte = walk(pagetable, a, 1)) == 0)
      return -1;
    if (*pte & PTE_V)
      panic("remap");
    *pte = PA2PTE(pa) | perm | PTE_V;
    if (a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// Remove npages of mappings starting from va. va must be
// page-aligned. The mappings must exist.
// Optionally free the physical memory.
void
uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  uint64 a;
  pte_t* pte;

  if ((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

  for (a = va; a < va + npages * PGSIZE; a += PGSIZE) {
    if ((pte = walk(pagetable, a, 0)) == 0)
      panic("uvmunmap: walk");
    if ((*pte & PTE_V) == 0)
      panic("uvmunmap: not mapped");
    if (PTE_FLAGS(*pte) == PTE_V)
      panic("uvmunmap: not a leaf");
    if (do_free) {
      uint64 pa = PTE2PA(*pte);
      kfree((void*)pa);
    }
    *pte = 0;
  }
}

// create an empty page table.
// returns 0 if out of memory.
pagetable_t
uvmcreate()
{
  pagetable_t pagetable;
  pagetable = (pagetable_t)kalloc();
  if (pagetable == 0)
    return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// Load the user initcode into address 0 of pagetable,
// for the very first process.
// sz must be less than a page.
void
ukvminit(struct proc* proc, uchar* src, uint sz)
{
  char* mem;

  if (sz >= PGSIZE)
    panic("inituvm: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  mappages(proc->pagetable, 0, PGSIZE, (uint64)mem, PTE_W | PTE_R | PTE_X | PTE_U);
  mappages(proc->kpagetable, 0, PGSIZE, (uint64)mem, PTE_W | PTE_R | PTE_X);
  memmove(mem, src, sz);
}

// Allocate PTEs and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  char* mem;
  uint64 a;

  if (newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for (a = oldsz; a < newsz; a += PGSIZE) {
    mem = kalloc();
    if (mem == 0) {
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if (mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_W | PTE_X | PTE_R | PTE_U) != 0) {
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
  }
  return newsz;
}

// Allocate PTEs and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
// NOTE: the difference between uvmalloc and this is that
// In addition to map user page table, we do kernel page table mapping too
uint64
ukvmalloc(pagetable_t uptbl, pagetable_t kptbl, uint64 oldsz, uint64 newsz)
{
  char* mem;
  uint64 a;

  if (newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for (a = oldsz; a < newsz; a += PGSIZE) {
    mem = kalloc();
    if (mem == 0) {
      uvmdealloc(uptbl, a, oldsz);
      kvmdealloc(kptbl, a, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if (mappages(uptbl, a, PGSIZE, (uint64)mem, PTE_W | PTE_X | PTE_R | PTE_U) != 0) {
      kfree(mem);
      uvmdealloc(uptbl, a, oldsz);
      kvmdealloc(kptbl, a, oldsz);
      return 0;
    }
    if (mappages(kptbl, a, PGSIZE, (uint64)mem, PTE_W | PTE_X | PTE_R) != 0) {
      panic("ukvmalloc: mapping kernel page table entry failed");
      // kfree(mem);
      // uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if (newsz >= oldsz)
    return oldsz;

  if (PGROUNDUP(newsz) < PGROUNDUP(oldsz)) {
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

uint64
kvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if (newsz >= oldsz)
    return oldsz;

  if (PGROUNDUP(newsz) < PGROUNDUP(oldsz)) {
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 0);
  }

  return newsz;
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
void
freewalk(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for (int i = 0; i < 512; i++) {
    pte_t pte = pagetable[i];
    if ((pte & PTE_V) && (pte & (PTE_R | PTE_W | PTE_X)) == 0) {
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    } else if (pte & PTE_V) {
      vmprint(pagetable);
      printf("%p\n", pte);
      panic("freewalk: leaf");
    }
  }
  kfree((void*)pagetable);
}

// Free user memory pages,
// then free page-table pages.
void
uvmfree(pagetable_t pagetable, uint64 sz)
{
  if (sz > 0)
    uvmunmap(pagetable, 0, PGROUNDUP(sz) / PGSIZE, 1); // 释放page table叶子节点中的物理页
  freewalk(pagetable);                                 // 释放page table本身
}

// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  pte_t* pte;
  uint64 pa, i;
  uint flags;
  char* mem;

  for (i = 0; i < sz; i += PGSIZE) {
    if ((pte = walk(old, i, 0)) == 0)
      panic("uvmcopy: pte should exist");
    if ((*pte & PTE_V) == 0)
      panic("uvmcopy: page not present");
    pa    = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    if ((mem = kalloc()) == 0)
      goto err;
    memmove(mem, (char*)pa, PGSIZE);
    if (mappages(new, i, PGSIZE, (uint64)mem, flags) != 0) {
      kfree(mem);
      goto err;
    }
  }
  return 0;

err:
  uvmunmap(new, 0, i / PGSIZE, 1);
  return -1;
}

// copy `user` page table mapping to `ken`
// this copying does not allocate any physical mem
// while do kern page table mapping, the PTE_U will be unset
// 0 means success, -1 failed
// NOTE: used by fork and exec
int
copy_u2k_ptbl(pagetable_t user, pagetable_t ken, uint64 sz)
{
  pte_t* pte;
  uint64 pa, i;
  uint flags;

  for (i = 0; i < sz; i += PGSIZE) {
    if ((pte = walk(user, i, 0)) == 0) {
      panic("copy_u2k_ptbl: pte should b exist in user page table");
    }
    if ((*pte & PTE_V) == 0) {
      panic("copy_u2k_ptbl: page not present");
    }
    pa    = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    UNSET_FLAG(flags, PTE_U);
    if (mappages(ken, i, PGSIZE, pa, flags) != 0) {
      panic("copy_u2k_ptbl: mappages failed");
    }
  }

  return 0;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
void
uvmclear(pagetable_t pagetable, uint64 va)
{
  pte_t* pte;

  pte = walk(pagetable, va, 0);
  if (pte == 0)
    panic("uvmclear");
  *pte &= ~PTE_U;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int
copyout(pagetable_t pagetable, uint64 dstva, char* src, uint64 len)
{
  uint64 n, va0, pa0;

  while (len > 0) {
    va0 = PGROUNDDOWN(dstva);
    pa0 = walkaddr(pagetable, va0);
    if (pa0 == 0) // 如果目标地址无法访问, 返回-1
      return -1;
    n = PGSIZE - (dstva - va0);
    if (n > len)
      n = len;
    memmove((void*)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
int
copyin(pagetable_t pagetable, char* dst, uint64 srcva, uint64 len)
{
  return copyin_new(pagetable, dst, srcva, len);
  /*
  uint64 n, va0, pa0;

  while (len > 0) {
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if (pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);
    if (n > len)
      n = len;
    memmove(dst, (void*)(pa0 + (srcva - va0)), n);

    len -= n;
    dst += n;
    srcva = va0 + PGSIZE;
  }
  return 0;
  */
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int
copyinstr(pagetable_t pagetable, char* dst, uint64 srcva, uint64 max)
{
  return copyinstr_new(pagetable, dst, srcva, max);
  /*
  uint64 n, va0, pa0;
  int got_null = 0;

  while (got_null == 0 && max > 0) {
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if (pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);
    if (n > max)
      n = max;

    char* p = (char*)(pa0 + (srcva - va0));
    while (n > 0) {
      if (*p == '\0') {
        *dst     = '\0';
        got_null = 1;
        break;
      } else {
        *dst = *p;
      }
      --n;
      --max;
      p++;
      dst++;
    }

    srcva = va0 + PGSIZE;
  }
  if (got_null) {
    return 0;
  } else {
    return -1;
  }*/
}
