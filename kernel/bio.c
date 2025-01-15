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

struct {
  struct spinlock lock;
  struct buf buf[NBUF];

  // Linked list of all buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.
  // struct buf head; // dummy head node
} bcache;

struct {
  struct buf head;// dummy head node
  struct spinlock bucketlock; // lock of per bucket
} hashtable[NBUCKT];

void
binit(void)
{
  struct buf *b;

  initlock(&bcache.lock, "bcache");

  // Create linked list of buffers
  // bcache.head.prev = &bcache.head;
  // bcache.head.next = &bcache.head;
  // for(b = bcache.buf; b < bcache.buf+NBUF; b++){
  //   b->next = bcache.head.next;
  //   b->prev = &bcache.head;
  //   initsleeplock(&b->lock, "buffer");
  //   bcache.head.next->prev = b;
  //   bcache.head.next = b;
  // }

  for(int i = 0; i < NBUCKT; i++){
    hashtable[i].head.prev = &(hashtable[i].head);
    hashtable[i].head.next = &(hashtable[i].head);
    initlock(&(hashtable[i].bucketlock), "bcache.bucket");
  }

  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    b->next = hashtable[0].head.next;
    b->prev = &(hashtable[0].head);
    initsleeplock(&b->lock, "buffer");
    hashtable[0].head.next->prev = b;
    hashtable[0].head.next = b;
  }
}

static int
hashcompute(uint dev, uint blockno){
  return blockno % NBUCKT;
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  // struct buf *b;

  // acquire(&bcache.lock);

  // Is the block already cached?
  // for(b = bcache.head.next; b != &bcache.head; b = b->next){
  //   if(b->dev == dev && b->blockno == blockno){
  //     b->refcnt++;
  //     release(&bcache.lock);
  //     acquiresleep(&b->lock);
  //     return b;
  //   }
  // }

  // // Not cached.
  // // Recycle the least recently used (LRU) unused buffer.
  // for(b = bcache.head.prev; b != &bcache.head; b = b->prev){
  //   if(b->refcnt == 0) {
  //     b->dev = dev;
  //     b->blockno = blockno;
  //     b->valid = 0;
  //     b->refcnt = 1;
  //     release(&bcache.lock);
  //     acquiresleep(&b->lock);
  //     return b;
  //   }
  // }
  // panic("bget: no buffers");

  struct buf *b;
  
  int bucketIndex = hashcompute(dev, blockno);
  // printf("bucketIndex:%d\n",bucketIndex);
  acquire(&(hashtable[bucketIndex].bucketlock));
  // Is the block already cached?
  for(b = hashtable[bucketIndex].head.next; b != &hashtable[bucketIndex].head; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&(hashtable[bucketIndex].bucketlock));
      acquiresleep(&b->lock);
      return b;
    }
  }

  // Not cached
  struct buf *tmp = 0;
  uint minticks = 0x8fffffff;
  // first select the curr bucket
  for(b = hashtable[bucketIndex].head.next; b != &(hashtable[bucketIndex].head); b = b->next){
    if(b->refcnt == 0 && b->ticks < minticks){
      tmp = b;
      minticks = b->ticks;
    }
  }
  if(tmp){
    goto find;
  }
  // release(&hashtable[bucketIndex].bucketlock);

  // select the whole array
  acquire(&bcache.lock);
refind:
  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    if(b->refcnt == 0 && b->ticks < minticks){
      tmp = b;
      minticks = b->ticks;
    }
  }

  if(tmp){
    // int index = hashcompute(tmp->dev, tmp->blockno);
    // acquire(&(hashtable[index].bucketlock));
    if(tmp->refcnt != 0)
      goto refind;
    tmp->next->prev = tmp->prev;
    tmp->prev->next = tmp->next;
    tmp->next = hashtable[bucketIndex].head.next;
    tmp->prev = &(hashtable[bucketIndex].head);
    hashtable[bucketIndex].head.next->prev = tmp;
    hashtable[bucketIndex].head.next = tmp;
    // release(&(hashtable[index].bucketlock));
    release(&bcache.lock);
    goto find;
  } else {
    goto refind;
  }

find:
  tmp->dev = dev;
  tmp->blockno = blockno;
  tmp->valid = 0;
  tmp->refcnt = 1;
  release(&(hashtable[bucketIndex].bucketlock));
  acquiresleep(&tmp->lock);
  return tmp;

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

  int bucketIndex = hashcompute(b->dev, b->blockno);
  acquire(&(hashtable[bucketIndex].bucketlock));
  b->refcnt--;
  if (b->refcnt == 0) {
    // no one is waiting for it.
    // acquire(&bcache.lock);
    b->next->prev = b->prev;
    b->prev->next = b->next;
    b->next = hashtable[bucketIndex].head.next;
    b->prev = &(hashtable[bucketIndex].head);
    hashtable[bucketIndex].head.next->prev = b;
    hashtable[bucketIndex].head.next = b;
    // release(&bcache.lock);
  }
  b->ticks = ticks;
  // printf("ticks:%d\n", b->ticks);
  release(&(hashtable[bucketIndex].bucketlock));
}

void
bpin(struct buf *b) {
  int bucketIndex = hashcompute(b->dev, b->blockno);
  acquire(&(hashtable[bucketIndex].bucketlock));
  // acquire(&bcache.lock);
  b->refcnt++;
  // release(&bcache.lock);
  release(&(hashtable[bucketIndex].bucketlock));
}

void
bunpin(struct buf *b) {
  int bucketIndex = hashcompute(b->dev, b->blockno);
  acquire(&(hashtable[bucketIndex].bucketlock));
  // acquire(&bcache.lock);
  b->refcnt--;
  // release(&bcache.lock);
  release(&(hashtable[bucketIndex].bucketlock));
}


