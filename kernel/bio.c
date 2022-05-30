
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

#define BUCKET_NUM 13

struct
{
  struct buf buf[NBUF];
  struct
  {
    struct spinlock buckets_lock;
    struct spinlock lock[BUCKET_NUM + 1];
    struct buf* bucket[BUCKET_NUM + 1];
  } hash_buckets;
} bcache;

static void
print_buckets()
{
  for (int i = 0; i <= BUCKET_NUM; i++) {
    struct buf* b = bcache.hash_buckets.bucket[i];
    while (b != 0) {
      printf("bucket %d, buffer slot %d ref %d blockno %% BUCKET_NUM %d, blockno %d\n", i, b - bcache.buf, b->refcnt, b->blockno % BUCKET_NUM, b->blockno);
      b = b->next;
    }
  }
}

// NOTE: bkt.lock must be held
static void
bucket_insert(struct buf** bkt, struct buf* node)
{
  // debug
  struct buf *b = *bkt;
  while(b) {
    if (b->blockno != -1 && b->blockno == node->blockno) {
      panic("bucket_insert: duplicate blockno");
    }
    b = b->next;
  }

  node->next = *bkt;
  *bkt       = node;
}

// NOTE: bkt.lock must be held
static void
bucket_remove(struct buf** bkt, struct buf* node)
{
  struct buf* buf  = *bkt;
  struct buf* pbuf = 0;
  while (buf != 0 && buf != node) {
    pbuf = buf;
    buf  = buf->next;
  }

  if (buf == 0) {
    panic("bucket_remove: buffer slot does not exit in bucket");
  } else {
    if (pbuf == 0) {
      *bkt = buf->next;
    } else {
      // remove node
      pbuf->next = buf->next;
      // printf("remove buffer slot:%d from bucket %d\n", node - bcache.buf, bkt - bcache.hash_buckets.bucket);
    }
    buf->next = 0;
  }
}

// NOTE: bucket[obkt_id].lock nad bucket[nbkt_id].lock must be held
static void
bucket_move(uint obkt_id, uint nbkt_id, struct buf* node)
{
  bucket_remove(&bcache.hash_buckets.bucket[obkt_id], node);
  bucket_insert(&bcache.hash_buckets.bucket[nbkt_id], node);
}

void
binit(void)
{
  struct buf* b;

  initlock(&bcache.hash_buckets.buckets_lock, "bcache.bucket-glock");
  // init hash bucket locks and bucket pointer to 0
  for (int i = 0; i <= BUCKET_NUM; i++) {
    initlock(&bcache.hash_buckets.lock[i], "bcache.bucket");
    bcache.hash_buckets.bucket[i] = 0;
  }
  // init buf locks and buf
  for (b = bcache.buf; b < bcache.buf + NBUF; b++) {
    initsleeplock(&b->lock, "buffer");
  }
  // insert all buffers to hash bucket 0
  // NOTE: actually we don't need this lock
  acquire(&bcache.hash_buckets.lock[BUCKET_NUM]);
  for (b = bcache.buf; b < bcache.buf + NBUF; b++) {
    b->blockno = -1;
    bucket_insert(&bcache.hash_buckets.bucket[BUCKET_NUM], b);
  }
  release(&bcache.hash_buckets.lock[BUCKET_NUM]);
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf* b;

  int nbkt_id = blockno % BUCKET_NUM;
  acquire(&bcache.hash_buckets.lock[nbkt_id]);
  for (b = bcache.hash_buckets.bucket[nbkt_id]; b != 0; b = b->next) {
    if (b->dev == dev && b->blockno == blockno) {
      b->refcnt++;
      release(&bcache.hash_buckets.lock[nbkt_id]);
      acquiresleep(&b->lock);

      if(b->blockno != blockno){
        panic("blockno invariant volatile");
      }
      return b;
    }
  }
  release(&bcache.hash_buckets.lock[nbkt_id]);

  // Not cached.
  acquire(&bcache.hash_buckets.buckets_lock);
  int free_buffer_bkt_id = 0;
retry:
  // NOTE: 可以使用交替的两个lock来避免retry
  // find a free buffer
  for(int i = 0; i<= BUCKET_NUM; i++) {
    if(i == nbkt_id) continue;
    acquire(&bcache.hash_buckets.lock[i]);
    struct buf*tmp_b = bcache.hash_buckets.bucket[i];
    while(tmp_b) {
      if(tmp_b->refcnt == 0) {
        if(b == 0 || tmp_b->tick < b->tick) {
          b = tmp_b;
          free_buffer_bkt_id = i;
        }
      }
      tmp_b = tmp_b->next;
    }
    release(&bcache.hash_buckets.lock[i]); 
  }
  if (b == 0) {
    panic("no free buffer");
  } 
  acquire(&bcache.hash_buckets.lock[free_buffer_bkt_id]);
  // one more check
  if(b->refcnt != 0) {
    release(&bcache.hash_buckets.lock[free_buffer_bkt_id]);
    goto retry;
  }

  // lock current bucket
  if (free_buffer_bkt_id != nbkt_id) {
    acquire(&bcache.hash_buckets.lock[nbkt_id]);
  }
  // one more check
  for (struct buf *tmp_b = bcache.hash_buckets.bucket[nbkt_id]; tmp_b != 0; tmp_b = tmp_b->next) {
    if (tmp_b->dev == dev && tmp_b->blockno == blockno) {
      b = tmp_b;
      b->refcnt++;
      goto end;
    }
  }

  b->dev     = dev;
  b->blockno = blockno;
  b->valid   = 0;
  b->refcnt = 1;
  if(free_buffer_bkt_id != nbkt_id) {
     bucket_move(free_buffer_bkt_id, nbkt_id, b);
  }

end:
  release(&bcache.hash_buckets.lock[free_buffer_bkt_id]);
  if (free_buffer_bkt_id != nbkt_id) {
    release(&bcache.hash_buckets.lock[nbkt_id]);
  }
  release(&bcache.hash_buckets.buckets_lock);
  acquiresleep(&b->lock);
  return b;
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf* b;

  b = bget(dev, blockno);
  if (!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf* b)
{
  if (!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf* b)
{
  if (!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  int bkt_id = b->blockno % BUCKET_NUM;
  acquire(&bcache.hash_buckets.lock[bkt_id]);
  b->refcnt--;
  if (b->refcnt == -1) {
    print_buckets();
    panic("ref negative");
  }
  if (b->refcnt == 0) {
    // update tick
    acquire(&tickslock);
    b->tick = ticks;
    release(&tickslock);
  }
  release(&bcache.hash_buckets.lock[bkt_id]);
}

void
bpin(struct buf* b)
{
  int bkt_id = b->blockno % BUCKET_NUM;
  acquire(&bcache.hash_buckets.lock[bkt_id]);
  b->refcnt++;
  // printf("pin buffer slot %d ref %d blockno %d\n", b - bcache.buf, b->refcnt, b->blockno);
  release(&bcache.hash_buckets.lock[bkt_id]);
}

void
bunpin(struct buf* b)
{
  int bkt_id = b->blockno % BUCKET_NUM;
  acquire(&bcache.hash_buckets.lock[bkt_id]);
  b->refcnt--;
  // printf("unpin buffer slot %d ref %d blockno %d\n", b - bcache.buf, b->refcnt, b->blockno);
  release(&bcache.hash_buckets.lock[bkt_id]);
}
