#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"

/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[];  // kernel.ld sets this to end of kernel code.

extern char trampoline[];  // trampoline.S

extern void _freewalk(pagetable_t pagetable);

/*
 * create a direct-map page table for the kernel.
 */
void kvminit() {
  kernel_pagetable = (pagetable_t)kalloc();
  memset(kernel_pagetable, 0, PGSIZE);

  // uart registers
  kvmmap(UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmap(VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

  // CLINT
  kvmmap(CLINT, CLINT, 0x10000, PTE_R | PTE_W);

  // PLIC
  kvmmap(PLIC, PLIC, 0x400000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(KERNBASE, KERNBASE, (uint64)etext - KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap((uint64)etext, (uint64)etext, PHYSTOP - (uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap(TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);
}

// Switch h/w page table register to the kernel's page table,
// and enable paging.
void kvminithart() {
  w_satp(MAKE_SATP(kernel_pagetable));
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
pte_t *walk(pagetable_t pagetable, uint64 va, int alloc) {
  if (va >= MAXVA) panic("walk");

  for (int level = 2; level > 0; level--) {
    pte_t *pte = &pagetable[PX(level, va)];
    if (*pte & PTE_V) {
      pagetable = (pagetable_t)PTE2PA(*pte);
    } else {
      if (!alloc || (pagetable = (pde_t *)kalloc()) == 0) return 0;
      memset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  return &pagetable[PX(0, va)];
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
uint64 walkaddr(pagetable_t pagetable, uint64 va) {
  pte_t *pte;
  uint64 pa;

  if (va >= MAXVA) return 0;

  pte = walk(pagetable, va, 0);
  if (pte == 0) return 0;
  if ((*pte & PTE_V) == 0) return 0;
  if ((*pte & PTE_U) == 0) return 0;
  pa = PTE2PA(*pte);
  return pa;
}

// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
void kvmmap(uint64 va, uint64 pa, uint64 sz, int perm) {
  if (mappages(kernel_pagetable, va, sz, pa, perm) != 0) panic("kvmmap");
}

// translate a kernel virtual address to
// a physical address. only needed for
// addresses on the stack.
// assumes va is page aligned.
uint64 kvmpa(uint64 va) {
  uint64 off = va % PGSIZE;
  pte_t *pte;
  uint64 pa;

  pte = walk(kernel_pagetable, va, 0);
  if (pte == 0) panic("kvmpa");
  if ((*pte & PTE_V) == 0) panic("kvmpa");
  pa = PTE2PA(*pte);
  return pa + off;
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not
// be page-aligned. Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
int mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm) {
  uint64 a, last;
  pte_t *pte;

  a = PGROUNDDOWN(va);
  last = PGROUNDDOWN(va + size - 1);
  for (;;) {
    if ((pte = walk(pagetable, a, 1)) == 0) return -1;
    if (*pte & PTE_V) panic("remap");
    *pte = PA2PTE(pa) | perm | PTE_V;
    if (a == last) break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// Remove npages of mappings starting from va. va must be
// page-aligned. The mappings must exist.
// Optionally free the physical memory.
void uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free) {
  uint64 a;
  pte_t *pte;

  if ((va % PGSIZE) != 0) panic("uvmunmap: not aligned");

  for (a = va; a < va + npages * PGSIZE; a += PGSIZE) {
    if ((pte = walk(pagetable, a, 0)) == 0) panic("uvmunmap: walk");
    if ((*pte & PTE_V) == 0) panic("uvmunmap: not mapped");
    if (PTE_FLAGS(*pte) == PTE_V) panic("uvmunmap: not a leaf");
    if (do_free) {
      uint64 pa = PTE2PA(*pte);
      kfree((void *)pa);
    }
    *pte = 0;
  }
}

// create an empty user page table.
// returns 0 if out of memory.
pagetable_t uvmcreate() {
  pagetable_t pagetable;
  pagetable = (pagetable_t)kalloc();
  if (pagetable == 0) return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// Load the user initcode into address 0 of pagetable,
// for the very first process.
// sz must be less than a page.
void uvminit(pagetable_t pagetable, uchar *src, uint sz) {
  char *mem;

  if (sz >= PGSIZE) panic("inituvm: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  mappages(pagetable, 0, PGSIZE, (uint64)mem, PTE_W | PTE_R | PTE_X | PTE_U);
  memmove(mem, src, sz);
}

// Allocate PTEs and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64 uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz) {
  char *mem;
  uint64 a;

  if (newsz < oldsz) return oldsz;

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

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
uint64 uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz) {
  if (newsz >= oldsz) return oldsz;

  if (PGROUNDUP(newsz) < PGROUNDUP(oldsz)) {
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
void freewalk(pagetable_t pagetable) {
  // there are 2^9 = 512 PTEs in a page table.
  for (int i = 0; i < 512; i++) {
    pte_t pte = pagetable[i];
    if ((pte & PTE_V) && (pte & (PTE_R | PTE_W | PTE_X)) == 0) {
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    } else if (pte & PTE_V) {
      panic("freewalk: leaf");
    }
  }
  kfree((void *)pagetable);
}

// Free user memory pages,
// then free page-table pages.
void uvmfree(pagetable_t pagetable, uint64 sz) {
  if (sz > 0) uvmunmap(pagetable, 0, PGROUNDUP(sz) / PGSIZE, 1);
  freewalk(pagetable);
}

// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
int uvmcopy(pagetable_t old, pagetable_t new, uint64 sz) {
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  char *mem;

  for (i = 0; i < sz; i += PGSIZE) {
    if ((pte = walk(old, i, 0)) == 0) panic("uvmcopy: pte should exist");
    if ((*pte & PTE_V) == 0) panic("uvmcopy: page not present");
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    if ((mem = kalloc()) == 0) goto err;
    memmove(mem, (char *)pa, PGSIZE);
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

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
void uvmclear(pagetable_t pagetable, uint64 va) {
  pte_t *pte;

  pte = walk(pagetable, va, 0);
  if (pte == 0) panic("uvmclear");
  *pte &= ~PTE_U;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len) {
  uint64 n, va0, pa0;

  while (len > 0) {
    va0 = PGROUNDDOWN(dstva);
    pa0 = walkaddr(pagetable, va0);
    if (pa0 == 0) return -1;
    n = PGSIZE - (dstva - va0);
    if (n > len) n = len;
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
int copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len) {
  //使用超级用户权限以确保访问用户空间地址
  w_sstatus(r_sstatus() | (1 << 18));
  int ret = copyin_new(pagetable, dst, srcva, len);
  //恢复原有权限设置
  w_sstatus(r_sstatus() & ~(1 << 18));
  return ret;
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max) {
  //使用超级用户权限以确保访问用户空间地址
  w_sstatus(r_sstatus() | (1 << 18));
  int ret = copyinstr_new(pagetable, dst, srcva, max);
  //恢复原有权限设置
  w_sstatus(r_sstatus() & ~(1 << 18));
  return ret;
}

// check if use global kpgtbl or not
int test_pagetable() {
  uint64 satp = r_satp();
  uint64 gsatp = MAKE_SATP(kernel_pagetable);
  printf("test_pagetable: %d\n", satp != gsatp);
  return satp != gsatp;
}

// 递归打印助手：level=2/1/0 对应 L2/L1/L0
static void
_vmprint_recurse(pagetable_t pt, int level, uint64 va_prefix)
{
  for (int idx = 0; idx < 512; idx++) {
    pte_t pte = pt[idx];
    if (!(pte & PTE_V)) continue;          // 跳过无效项

    uint64 pa   = PTE2PA(pte);
    uint64 flags = PTE_FLAGS(pte);
    char flagstr[5] = {0};
    flagstr[0] = (flags & PTE_R) ? 'r' : '-';
    flagstr[1] = (flags & PTE_W) ? 'w' : '-';
    flagstr[2] = (flags & PTE_X) ? 'x' : '-';
    flagstr[3] = (flags & PTE_U) ? 'u' : '-';
    flagstr[4] = 0;

    int groups = 3 - level;
    for (int g = 0; g < groups; g++) {
      if (g > 0) printf("   ");
      printf("||");
    }

    if (level > 0) {
      // 非叶节点：只打印 idx + 下一级页表物理地址
      printf("idx: %d: pa: %p, flags: %s\n", idx, (void *)pa, flagstr);
      _vmprint_recurse((pagetable_t)pa, level - 1, va_prefix | ((uint64)idx << PXSHIFT(level)));
    } else {
      // 叶节点：打印虚拟地址 -> 物理地址
      uint64 va = va_prefix | ((uint64)idx << PXSHIFT(0));
      printf("idx: %d: va: %p -> pa: %p, flags: %s\n", idx, (void *)va, (void *)pa, flagstr);
    }
  }
}

void vmprint(pagetable_t pagetable)
{
  printf("page table %p\n", (void *)pagetable);
  _vmprint_recurse(pagetable, 2, 0);
}

pagetable_t kvmcreate() 
{
  pagetable_t kpt = (pagetable_t)kalloc();
  if (kpt == 0)
    return 0;
  memset(kpt, 0, PGSIZE);//分配内核页表空间

  // 将与内核相关的必要映射复制（与 kvminit 相似，但不映射 CLINT）
  if (mappages(kpt, UART0, PGSIZE, UART0, PTE_R | PTE_W) != 0)
    goto bad;
  if (mappages(kpt, VIRTIO0, PGSIZE, VIRTIO0, PTE_R | PTE_W) != 0)
    goto bad;
  if (mappages(kpt, PLIC, 0x400000, PLIC, PTE_R | PTE_W) != 0)
    goto bad;
  // kernel text and ro
  if (mappages(kpt, KERNBASE, (uint64)etext - KERNBASE, KERNBASE, PTE_R | PTE_X) != 0)
    goto bad;
  // kernel data and physical RAM used
  if (mappages(kpt, (uint64)etext, (PHYSTOP - (uint64)etext), (uint64)etext, PTE_R | PTE_W) != 0)
    goto bad;
  // trampoline (与全局页表一致)
  if (mappages(kpt, TRAMPOLINE, PGSIZE, (uint64)trampoline, PTE_R | PTE_X) != 0)
    goto bad;

  return kpt;
bad:
  kfree((void *)kpt);
  return 0;
}

// 将用户页表的 L2 篇（覆盖用户地址空间）复制到进程的内核页表。
// 这里不复制 CLINT/设备映射，只把用户空间的根级目录项共享过来。
void tool_sync_pagetable(pagetable_t uptbl, pagetable_t kpt, uint64 va_prefix, int level) 
{
  for (int idx = 0; idx < 512; idx++, va_prefix++) 
  {
    if((va_prefix<<PXSHIFT(level))>=PLIC) return;// 超出用户地址空间范围，停止
    pte_t p = uptbl[idx];
    pte_t k = kpt[idx];
    if ((p & PTE_V) && !(k & PTE_V))//如果proc用户有效，内核k无效
    {
      if ((p & (PTE_R | PTE_W | PTE_X)) == 0)
      {
        // 非叶节点：为内核页表分配新的页表页
        pagetable_t pa = kalloc();
        if (pa == 0) panic("tool_sync_pagetable: kalloc failed");
        memset(pa, 0, PGSIZE);//新建pa，为pa提供空间
        kpt[idx] = PA2PTE(pa) | (p & (PTE_V | PTE_R | PTE_W | PTE_X | PTE_U));
      }
      else
      {
        kpt[idx] = p;//内核页表直接共享用户页表的叶子页表  
      }
    }
    if ((p & PTE_V) && (p & (PTE_R | PTE_W | PTE_X)) == 0)//这一层已经搞定了
    {
      // 非叶节点：递归同步下一级页表
      pagetable_t child_u = (pagetable_t)PTE2PA(p);
      pagetable_t child_k = (pagetable_t)PTE2PA(kpt[idx]);
      uint64 new_prefix = va_prefix  << 9;//更新虚拟地址前缀，下一级索引
      tool_sync_pagetable(child_u, child_k, new_prefix, level - 1);
    }
  }
}
// kpt: 进程专属内核页表根； uptbl: 进程的用户页表根
// 同步用户页表内容到新的内核页表
// sync_pagetable 的作用是将用户页表中的映射同步到新分配的内核页表，
// 这样可以确保进程在切换到新的内核页表后，用户空间的映射不会丢失，
// 保证进程能够正常访问用户空间的内存资源，防止因页表不同步导致的访问异常。
// 这是在重新分配内核页表时必须的步骤，确保新旧页表内容一致性和进程运行的正确性。
void sync_pagetable(pagetable_t uptbl, pagetable_t kpt)
{
  tool_sync_pagetable(uptbl, kpt, 0, 2);
}