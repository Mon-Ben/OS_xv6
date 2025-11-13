#include "param.h"
#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "spinlock.h"
#include "proc.h"
#include "memlayout.h"

//
// This file contains copyin_new() and copyinstr_new(), the
// replacements for copyin and coyinstr in vm.c.
//

static struct stats {
  int ncopyin;
  int ncopyinstr;
} stats;

int statscopyin(char *buf, int sz) {
  int n;
  n = snprintf(buf, sz, "copyin: %d\n", stats.ncopyin);
  n += snprintf(buf + n, sz, "copyinstr: %d\n", stats.ncopyinstr);
  return n;
}


// 将用户页表的 L2 篇（覆盖用户地址空间）复制到进程的内核页表。
// 这里不复制 CLINT/设备映射，只把用户空间的根级目录项共享过来。
void tool_sync_pagetable(pagetable_t kpt, pagetable_t uptbl, int level, uint64 va_prefix) {
  for (int idx = 0; idx < 512; idx++) {
    if((va_prefix<<PXSHIFT(level))>=PLIC) break; // 超出用户地址空间范围，停止
    pte_t k = kpt[idx];
    pte_t p = uptbl[idx];

    if ((p & PTE_V) && !(k & PTE_V))//如果proc用户有效，内核k无效
    {
      if ((p & (PTE_R | PTE_W | PTE_X)) == 0)
      {
        pagetable_t pa = kalloc();
        memset(pa, 0, PGSIZE);//新建pa，为pa提供空间
        kpt[idx]=PA2PTE(pa)|(p&(PTE_V|PTE_R|PTE_W|PTE_X|PTE_U));       
      }
      else
      {
        kpt[idx]= p;//内核页表直接共享用户页表的叶子页表   
      }
      if((p&PTE_V)&&(p&(PTE_R|PTE_W|PTE_X))==0)//这一层已经搞定了
      {
        tool_sync_pagetable((pagetable_t)PTE2PA(kpt[idx]), (pagetable_t)PTE2PA(uptbl[idx]), level - 1, va_prefix | ((uint64)idx << PXSHIFT(level)));
      }
    }
  }
}
// kpt: 进程专属内核页表根； uptbl: 进程的用户页表根
// 同步用户页表内容到新的内核页表
// sync_pagetable 的作用是将用户页表中的映射同步到新分配的内核页表，
// 这样可以确保进程在切换到新的内核页表后，用户空间的映射不会丢失，
// 保证进程能够正常访问用户空间的内存资源，防止因页表不同步导致的访问异常。
// 这是在重新分配内核页表时必须的步骤，确保新旧页表内容一致性和进程运行的正确性。
void sync_pagetable(pagetable_t kpt, pagetable_t uptbl) {
  tool_sync_pagetable(kpt, uptbl,2,0);
}


// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
int copyin_new(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len) {
  struct proc *p = myproc();

  if (srcva >= p->sz || srcva + len >= p->sz || srcva + len < srcva) return -1;
  memmove((void *)dst, (void *)srcva, len);
  stats.ncopyin++;  // XXX lock
  return 0;
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int copyinstr_new(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max) {
  struct proc *p = myproc();
  char *s = (char *)srcva;

  stats.ncopyinstr++;  // XXX lock
  for (int i = 0; i < max && srcva + i < p->sz; i++) {
    dst[i] = s[i];
    if (s[i] == '\0') return 0;
  }
  return -1;
}
