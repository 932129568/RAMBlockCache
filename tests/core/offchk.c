/* offchk.c —— rb_chunk 布局诊断镜像。
 * 同步义务：本结构体必须与 cache.c 的 struct rb_chunk 逐字段一致。
 * 2026-10-15：删 evicting（W18 死字段移除，随 cache.c 同步）。
 * 注：_Static_assert 只能断言本镜像自身，跨 TU 对齐靠本文件与 cache.c 的人工同步纪律。 */
#include "cache.h"
#include <stdio.h>
#include <stddef.h>
struct rb_chunk {
    rb_u64 start;
    unsigned char *data;
    uint16_t loaded;
    uint16_t dirty;
    rb_u64 last_access;
    uint32_t hits;                   /* [D2] 与 cache.c struct rb_chunk 同步（2026-10-15） */
    rb_u64 epoch;
    uint32_t dseq[16];
    struct rb_chunk *hash_next;
    struct rb_chunk *lru_prev, *lru_next;
    struct rb_chunk *d_prev, *d_next;
};
int main(void){
    printf("sizeof=%zu\n", sizeof(struct rb_chunk));
    printf("start=%zu data=%zu loaded=%zu dirty=%zu last_access=%zu hits=%zu epoch=%zu dseq=%zu hash_next=%zu lru_prev=%zu lru_next=%zu d_prev=%zu d_next=%zu\n",
      offsetof(struct rb_chunk,start), offsetof(struct rb_chunk,data), offsetof(struct rb_chunk,loaded),
      offsetof(struct rb_chunk,dirty), offsetof(struct rb_chunk,last_access), offsetof(struct rb_chunk,hits),
      offsetof(struct rb_chunk,epoch), offsetof(struct rb_chunk,dseq), offsetof(struct rb_chunk,hash_next),
      offsetof(struct rb_chunk,lru_prev), offsetof(struct rb_chunk,lru_next), offsetof(struct rb_chunk,d_prev),
      offsetof(struct rb_chunk,d_next));
    return 0;
}
