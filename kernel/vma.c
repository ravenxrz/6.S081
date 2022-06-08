#include "types.h"
#include "riscv.h"
#include "vma.h"
#include "spinlock.h"
#include "defs.h"

struct
{
  struct spinlock lock; /* protect vma_table  */
  struct vma_area vma_table[NVMA];
} kvma;

void
vmainit(void)
{
  initlock(&kvma.lock, "vma");
}

struct vma_area*
vmaalloc(void)
{
  struct vma_area* vma;

  acquire(&kvma.lock);
  for (vma = kvma.vma_table; vma < kvma.vma_table + NVMA; vma++) {
    if (vma->file == 0) {
      // zero vma
      memset(vma, 0, sizeof(struct vma_area));
      release(&kvma.lock);
      return vma;
    }
  }
  release(&kvma.lock);
  return 0;
}

void
vmafree(struct vma_area* vma)
{
  acquire(&kvma.lock);
  vma->file = 0;
  release(&kvma.lock);
}