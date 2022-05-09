#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "elf.h"

static int
loadseg(pde_t* pgdir, uint64 addr, struct inode* ip, uint offset, uint sz);

int
exec(char* path, char** argv)
{
  char *s, *last;
  int i, off;
  uint64 argc, sz = 0, sp, ustack[MAXARG + 1], stackbase;
  struct elfhdr elf;
  struct inode* ip;
  struct proghdr ph;
  pagetable_t pagetable = 0;
  // pagetable_t kern_pagetable = 0;
  pagetable_t oldpagetable;
  // pagetable_t old_ken_pagetable;
  struct proc* p = myproc();

  begin_op();

  if ((ip = namei(path)) == 0) { // 打开文件
    end_op();
    return -1;
  }
  ilock(ip);

  // Check ELF header
  if (readi(ip, 0, (uint64)&elf, 0, sizeof(elf)) != sizeof(elf))
    goto bad;
  if (elf.magic != ELF_MAGIC) // 检查是否有ELF header
    goto bad;

  if ((pagetable = proc_pagetable(p)) == 0) // 分配page table
    goto bad;

  // Load program into memory.
  for (i = 0, off = elf.phoff; i < elf.phnum; i++, off += sizeof(ph)) {
    if (readi(ip, 0, (uint64)&ph, off, sizeof(ph)) != sizeof(ph))
      goto bad;
    if (ph.type != ELF_PROG_LOAD)
      continue;
    if (ph.memsz < ph.filesz)
      goto bad;
    if (ph.vaddr + ph.memsz < ph.vaddr)
      goto bad;
    if (ph.vaddr + ph.memsz >= PLIC) {
      printf("ph.vaddr + ph.memsz >= PLIC\n");
      goto bad;
    }
    uint64 sz1;
    if ((sz1 = uvmalloc(pagetable, sz, ph.vaddr + ph.memsz)) == 0) // 为每个段分配内存
      goto bad;
    sz = sz1;
    if (ph.vaddr % PGSIZE != 0)
      goto bad;
    if (loadseg(pagetable, ph.vaddr, ip, ph.off, ph.filesz) < 0) // 从文件中加载每个segment到内存
      goto bad;
  }
  iunlockput(ip);
  end_op();
  ip = 0;

  p            = myproc();
  uint64 oldsz = p->sz;

  // Allocate two pages at the next page boundary.
  // Use the second as the user stack.
  sz = PGROUNDUP(sz);
  uint64 sz1;
  if ((sz1 = uvmalloc(pagetable, sz, sz + 2 * PGSIZE)) == 0) // 分配stack
    goto bad;
  sz = sz1;
  uvmclear(pagetable, sz - 2 * PGSIZE); // 标记guard page 为 user无法访问
  sp        = sz;
  stackbase = sp - PGSIZE;

  // Push argument strings, prepare rest of stack in ustack.
  for (argc = 0; argv[argc]; argc++) { // 拷贝参数到ustack
    if (argc >= MAXARG)
      goto bad;
    sp -= strlen(argv[argc]) + 1;
    sp -= sp % 16; // riscv sp must be 16-byte aligned
    if (sp < stackbase)
      goto bad;
    if (copyout(pagetable, sp, argv[argc], strlen(argv[argc]) + 1) < 0)
      goto bad;
    ustack[argc] = sp; // 记录参数的地址
  }
  ustack[argc] = 0;

  // TODO：书中有提到ustack下面有三个entires: fake return pc counter, argc 和 argv pointer, 但目前还没到设置，maybe in trapframe?

  // push the array of argv[] pointers.
  sp -= (argc + 1) * sizeof(uint64);
  sp -= sp % 16;
  if (sp < stackbase)
    goto bad;
  if (copyout(pagetable, sp, (char*)ustack, (argc + 1) * sizeof(uint64)) < 0)
    goto bad;

  // arguments to user main(argc, argv)
  // argc is returned via the system call return
  // value, which goes in a0.
  p->trapframe->a1 = sp;

  // Save program name for debugging.
  for (last = s = path; *s; s++)
    if (*s == '/')
      last = s + 1;
  safestrcpy(p->name, last, sizeof(p->name));

  // NOTE：不能再次使用重分配内核page table，因为之后的释放会出现问题
  // alloc kern page table and
  // if ((kern_pagetable = proc_kpagetable(p)) == 0) // 分配page table
  //   goto bad;

  // 解除kernel page table 中对user page table entries的映射
  uvmunmap(p->kpagetable, 0, PGROUNDUP(oldsz) / PGSIZE, 0);
  // copy content from user page table
  copy_u2k_ptbl(pagetable, p->kpagetable, sz);

  // Commit to the user image.
  oldpagetable = p->pagetable;
  // old_ken_pagetable = p->kpagetable;
  p->pagetable      = pagetable;
  p->sz             = sz;
  p->trapframe->epc = elf.entry;           // initial program counter = main
  p->trapframe->sp  = sp;                  // initial stack pointer
  proc_freepagetable(oldpagetable, oldsz); // 释放旧page table
  // proc_kfreepagetable(old_ken_pagetable, oldsz); // NOTE: 不能释放旧 kpage table, 因为此时系统(satp寄存器)采用的kernel page table就是该page table，释放过程中页表消失，指令就无法正常执行

  // lab: pgtbl, print the first process's page table
  if (p->pid == 1) {
    vmprint(p->pagetable);
  }

  return argc; // this ends up in a0, the first argument to main(argc, argv)

bad:
  if (pagetable)
    proc_freepagetable(pagetable, sz);
  if (ip) {
    iunlockput(ip);
    end_op();
  }
  return -1;
}

// Load a program segment into pagetable at virtual address va.
// va must be page-aligned
// and the pages from va to va+sz must already be mapped.
// Returns 0 on success, -1 on failure.
static int
loadseg(pagetable_t pagetable, uint64 va, struct inode* ip, uint offset, uint sz)
{
  uint i, n;
  uint64 pa;

  if ((va % PGSIZE) != 0)
    panic("loadseg: va must be page aligned");

  for (i = 0; i < sz; i += PGSIZE) {
    pa = walkaddr(pagetable, va + i);
    if (pa == 0)
      panic("loadseg: address should exist");
    if (sz - i < PGSIZE)
      n = sz - i;
    else
      n = PGSIZE;
    if (readi(ip, 0, (uint64)pa, offset + i, n) != n)
      return -1;
  }

  return 0;
}
