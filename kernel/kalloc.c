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

// 空闲页链表节点
struct run {
  struct run *next; // 指向下一个空闲页
};

// 全局内存管理结构
struct kmem{
  struct spinlock lock; // 保护空闲链表的自旋锁
  struct run *freelist; // 空闲物理页链表头
} ;

struct kmem kmem[8];

void
kinit()
{
  initlock(&kmem[0].lock, "kmem_0");
  initlock(&kmem[1].lock, "kmem_1");
  initlock(&kmem[2].lock, "kmem_2");
  initlock(&kmem[3].lock, "kmem_3");
  initlock(&kmem[4].lock, "kmem_4");
  initlock(&kmem[5].lock, "kmem_5");
  initlock(&kmem[6].lock, "kmem_6");
  initlock(&kmem[7].lock, "kmem_7");
  freerange(end, (void*)PHYSTOP);
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
void kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;
  
  push_off();
  int current_cpu = cpuid();
  pop_off();

  acquire(&kmem[current_cpu].lock);
  r->next = kmem[current_cpu].freelist;
  kmem[current_cpu].freelist = r;
  release(&kmem[current_cpu].lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;
  push_off();
  int current_cpu = cpuid();
  pop_off();
  //本地尝试
  acquire(&kmem[current_cpu].lock);
  r = kmem[current_cpu].freelist;
  if(r) {
        kmem[current_cpu].freelist = r->next;
  } else {
    for (int i = 0; i < 8; i++) {
      if (i == current_cpu) continue;

      acquire(&kmem[i].lock);
      r = kmem[i].freelist;
      if (r) {
        kmem[i].freelist = r->next;
        release(&kmem[i].lock);
        break;
      }
      release(&kmem[i].lock);                 // 窃取成功
    }
  }
  release(&kmem[current_cpu].lock);
  
  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;


}
