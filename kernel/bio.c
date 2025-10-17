// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

#define NBUCKET 13

struct {
  struct spinlock lock[NBUCKET];
  struct buf      buf[NBUF];
  struct buf      hashbucket[NBUCKET]; // 桶链表头
} bcache;

static inline uint hash(uint blockno)
{
  return blockno % NBUCKET;
}

/* 原子读 ticks */
static uint64
get_ticks(void)
{
  uint64 t;
  push_off();
  t = ticks;
  pop_off();
  return t;
}

void binit(void)
{
  for (int i = 0; i < NBUCKET; i++) {
    initlock(&bcache.lock[i], "bcache.bucket");
    bcache.hashbucket[i].prev = &bcache.hashbucket[i];
    bcache.hashbucket[i].next = &bcache.hashbucket[i];
  }
  for (int i = 0; i < NBUF; i++) {
    initsleeplock(&bcache.buf[i].lock, "buffer");
    bcache.buf[i].refcnt    = 0;
    bcache.buf[i].timestamp = 0;
    /* 均匀预分布到桶 */
    int h = i % NBUCKET;
    bcache.buf[i].next = bcache.hashbucket[h].next;
    bcache.buf[i].prev = &bcache.hashbucket[h];
    bcache.hashbucket[h].next->prev = &bcache.buf[i];
    bcache.hashbucket[h].next       = &bcache.buf[i];
  }
}
/* 跨桶窃取：返回最小时间戳空闲块，调用者已放原桶锁 */
static struct buf * steal_min_ticks(int my_h)
{
  struct buf *min_b = 0;
  uint64 min_t = ~0ULL;

  for (int i = 0; i < NBUCKET; i++) {
    if (i == my_h) continue;
    acquire(&bcache.lock[i]);
    for (struct buf *b = bcache.hashbucket[i].next;
         b != &bcache.hashbucket[i]; b = b->next) {
      if (b->refcnt == 0 && b->timestamp < min_t) {
        min_t = b->timestamp;
        min_b = b;
      }
    }
    if (min_b) {
      min_b->refcnt = 1;
      /* 从原桶摘下 */
      min_b->next->prev = min_b->prev;
      min_b->prev->next = min_b->next;
      release(&bcache.lock[i]);
      return min_b;
    }
    release(&bcache.lock[i]);
  }
  return 0;
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf* bget(uint dev, uint blockno)
{
  uint h = hash(blockno);
  acquire(&bcache.lock[h]);

  //桶内命中
  for (struct buf *b = bcache.hashbucket[h].next;
       b != &bcache.hashbucket[h]; b = b->next) {
    if (b->dev == dev && b->blockno == blockno) {
      b->refcnt++;
      release(&bcache.lock[h]);
      acquiresleep(&b->lock);
      return b;
    }
  }
    /* 2. 桶内找最小时间戳空闲块 */
  struct buf *min_b = 0;
  uint64 min_t = ~0ULL;
  for (struct buf *b = bcache.hashbucket[h].next;
       b != &bcache.hashbucket[h]; b = b->next) {
    if (b->refcnt == 0 && b->timestamp < min_t) {
      min_t = b->timestamp;
      min_b = b;
    }
  }
  if (min_b) {
    min_b->dev = dev;
    min_b->blockno = blockno;
    min_b->valid = 0;
    min_b->refcnt = 1;
    min_b->timestamp = 0;   // brelse 将设置
    /* 移到桶头（MRU）*/
    min_b->next->prev = min_b->prev;
    min_b->prev->next = min_b->next;
    min_b->next = bcache.hashbucket[h].next;
    min_b->prev = &bcache.hashbucket[h];
    bcache.hashbucket[h].next->prev = min_b;
    bcache.hashbucket[h].next = min_b;
    release(&bcache.lock[h]);
    acquiresleep(&min_b->lock);
    return min_b;
  }

  /* 3. 本桶无空闲，跨桶窃取 */
  release(&bcache.lock[h]);
  struct buf *sb = steal_min_ticks(h);
  if (sb) {
    acquire(&bcache.lock[h]);
    sb->dev = dev;
    sb->blockno = blockno;
    sb->valid = 0;
    sb->timestamp = 0;
    /* 挂到桶头 */
    sb->next = bcache.hashbucket[h].next;
    sb->prev = &bcache.hashbucket[h];
    bcache.hashbucket[h].next->prev = sb;
    bcache.hashbucket[h].next = sb;
    release(&bcache.lock[h]);
    acquiresleep(&sb->lock);
    return sb;
  }

  panic("bget: no buffer");
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);
  
  if (__sync_sub_and_fetch(&b->refcnt, 1) == 0) {
    b->timestamp = get_ticks();
  }
}

void
bpin(struct buf *b) {
  __sync_fetch_and_add(&b->refcnt, 1);
}

void
bunpin(struct buf *b) {
  __sync_fetch_and_sub(&b->refcnt, 1);
}


