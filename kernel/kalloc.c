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

struct run {
  struct run *next;
};

typedef struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

kmem cpuskmem[NCPU];

void
kinit()
{
  // initlock(&kmem.lock, "kmem");
  for(int i = 0; i < NCPU; i++){
    cpuskmem[i].freelist = 0;
    initlock(&cpuskmem[i].lock, "kmem");
  }
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  int counter = 0;
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE){
    memset(p, 1, PGSIZE);
    struct run* r = (struct run*)p;
    int index = counter % NCPU;
    r->next = cpuskmem[index].freelist;
    cpuskmem[index].freelist = r;
  }
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

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  push_off();
  int hart = cpuid();
  pop_off();

  acquire(&cpuskmem[hart].lock);
  r->next = cpuskmem[hart].freelist;
  cpuskmem[hart].freelist = r;
  release(&cpuskmem[hart].lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  // struct run *r;

  // acquire(&kmem.lock);
  // r = kmem.freelist;
  // if(r)
  //   kmem.freelist = r->next;
  // release(&kmem.lock);

  // if(r)
  //   memset((char*)r, 5, PGSIZE); // fill with junk
  // return (void*)r;

  struct run *r;

  push_off();
  int hart = cpuid();
  pop_off();

  acquire(&cpuskmem[hart].lock);
  r = cpuskmem[hart].freelist;
  if(r)
    cpuskmem[hart].freelist = r->next;
  release(&cpuskmem[hart].lock);

  if(r){
    memset((char*)r, 5, PGSIZE);
    return (void*)r;
  } else {
    for(int i = 0; i < NCPU; i++){
      acquire(&cpuskmem[i].lock);
      r = cpuskmem[i].freelist;
      if(r)
        cpuskmem[i].freelist = r->next;
      release(&cpuskmem[i].lock);
      if(r){
        memset((char*)r, 5, PGSIZE);
        return (void*)r;
      }
    }
  }
  return (void*)r;
}
