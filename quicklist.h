/* quicklist.h - A generic doubly linked quicklist implementation
 *
 * Copyright (c) 2014, Matt Stancliff <matt@genges.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this quicklist of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this quicklist of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __QUICKLIST_H__
#define __QUICKLIST_H__

// quicklist 的结构体总的来说就是使用评了一个双向链表，但是每个链表的节点均是 ziplist 或者经过 LZF 压缩的数据结构
// 因为是双向链表，所以头尾的数据使用频率较为高一些（查找，修改，均是从头尾进行遍历）所以 quicklist 设置参数 compress 用来标记首尾各多少 quicklistNode 不需要使用 LZF 压缩算法，方便使用。
// 总体结构就是 ziplist + ... + ziplist + quicklistLZF + ... + quicklistLZF + ziplist + ... + ziplist

/* Node, quicklist, and Iterator are the only data structures used currently. */

/* quicklistNode is a 32 byte struct describing a ziplist for a quicklist.
 * We use bit fields keep the quicklistNode at 32 bytes.
 * count: 16 bits, max 65536 (max zl bytes is 65k, so max count actually < 32k).
 * encoding: 2 bits, RAW=1, LZF=2.
 * container: 2 bits, NONE=1, ZIPLIST=2.
 * recompress: 1 bit, bool, true if node is temporarry decompressed for usage.
 * attempted_compress: 1 bit, boolean, used for verifying during testing.
 * extra: 10 bits, free for future use; pads out the remainder of 32 bits */
// quicklist node
typedef struct quicklistNode {
    struct quicklistNode *prev;   // 前节点指针
    struct quicklistNode *next;   // 后节点指针
    unsigned char *zl;            // ziplist 指针
    unsigned int sz;              /* ziplist size in bytes 该 ziplist 的 byte 长度 */
    unsigned int count : 16;      /* count of items in ziplist ziplist 元素个数 */
    unsigned int encoding : 2;    /* RAW==1 or LZF==2 ziplist 或是 LZF 压缩存储 */
    unsigned int container : 2;   /* zl 是否被压缩 压缩为 2 NONE==1 or ZIPLIST==2  */
    unsigned int recompress : 1;  /* quicklist 是否已经压缩过 was this node previous compressed? */
    unsigned int attempted_compress : 1; /* 不可压缩标志(小于 MIN_COMPRESS_BYTES=48) node can't compress; too small */
    unsigned int extra : 10;      /* 预留位 10位 more bits to steal for future usage */
} quicklistNode;

/* quicklistLZF is a 4+N byte struct holding 'sz' followed by 'compressed'.
 * 'sz' is byte length of 'compressed' field.
 * 'compressed' is LZF data with total (compressed) length 'sz'
 * NOTE: uncompressed length is stored in quicklistNode->sz.
 * When quicklistNode->zl is compressed, node->zl points to a quicklistLZF */
// 
// 被压缩时节点 zl 指针指向 quicklistLZF
typedef struct quicklistLZF {
    unsigned int sz; /* LZF 压缩数据的字节数（解压时用来还还原原大小） LZF size in bytes*/
    char compressed[]; // 压缩后的数据
} quicklistLZF;

/* quicklist is a 40 byte struct (on 64-bit systems) describing a quicklist.
 * 'count' is the number of total entries.
 * 'len' is the number of quicklist nodes.
 * 'compress' is: -1 if compression disabled, otherwise it's the number
 *                of quicklistNodes to leave uncompressed at ends of quicklist.
 * 'fill' is the user-requested (or default) fill factor. */
// 快速链表头节点
typedef struct quicklist {
    quicklistNode *head;        // 头指针
    quicklistNode *tail;        // 尾指针
    unsigned long count;        /* 元素总个数 total count of all entries in all ziplists */
    unsigned long len;          /* ziplist 个数 number of quicklistNodes */
    // 正数时，标识 ziplist 包含的最大 entity 个数
    // -1 ziplist 不超过 4kb
    // -2 ziplist 不超过 8kb
    // -3 ziplist 不超过 16kb
    // -4 ziplist 不超过 32kb
    // -5 ziplist 不超过 64kb
    int fill : 16;              /* fill factor for individual nodes */
    // 首尾不被压缩的个数，首尾元素访问频繁，所以不进行压缩
    unsigned int compress : 16; /* depth of end nodes not to compress;0=off */
} quicklist;

// quicklist iterator
typedef struct quicklistIter {
    const quicklist *quicklist;     // 对应的 quicklist
    quicklistNode *current;         // 当前节点
    unsigned char *zi;              // ziplist 结构指针
    long offset; /* offset in current ziplist 当前数据是整数还是字符串 */
    int direction;                  // 迭代器前进方向
} quicklistIter;

typedef struct quicklistEntry {
    const quicklist *quicklist;     // 对应的 quicklist
    quicklistNode *node;            // 当前节点
    unsigned char *zi;              // 当前 ziplist 结构指针
    unsigned char *value;
    long long longval;
    unsigned int sz;                // 当前 zi 字节数
    int offset;                     // entry 的当前偏移量
} quicklistEntry;

#define QUICKLIST_HEAD 0
#define QUICKLIST_TAIL -1

/* quicklist node encodings */
#define QUICKLIST_NODE_ENCODING_RAW 1
#define QUICKLIST_NODE_ENCODING_LZF 2

/* quicklist compression disable */
#define QUICKLIST_NOCOMPRESS 0

/* quicklist container formats */
#define QUICKLIST_NODE_CONTAINER_NONE 1
#define QUICKLIST_NODE_CONTAINER_ZIPLIST 2

// 该节点是否是被压缩
#define quicklistNodeIsCompressed(node)                                        \
    ((node)->encoding == QUICKLIST_NODE_ENCODING_LZF)

/* Prototypes */
quicklist *quicklistCreate(void);
quicklist *quicklistNew(int fill, int compress);
void quicklistSetCompressDepth(quicklist *quicklist, int depth);
void quicklistSetFill(quicklist *quicklist, int fill);
void quicklistSetOptions(quicklist *quicklist, int fill, int depth);
void quicklistRelease(quicklist *quicklist);
int quicklistPushHead(quicklist *quicklist, void *value, const size_t sz);
int quicklistPushTail(quicklist *quicklist, void *value, const size_t sz);
void quicklistPush(quicklist *quicklist, void *value, const size_t sz,
                   int where);
void quicklistAppendZiplist(quicklist *quicklist, unsigned char *zl);
quicklist *quicklistAppendValuesFromZiplist(quicklist *quicklist,
                                            unsigned char *zl);
quicklist *quicklistCreateFromZiplist(int fill, int compress,
                                      unsigned char *zl);
void quicklistInsertAfter(quicklist *quicklist, quicklistEntry *node,
                          void *value, const size_t sz);
void quicklistInsertBefore(quicklist *quicklist, quicklistEntry *node,
                           void *value, const size_t sz);
void quicklistDelEntry(quicklistIter *iter, quicklistEntry *entry);
int quicklistReplaceAtIndex(quicklist *quicklist, long index, void *data,
                            int sz);
int quicklistDelRange(quicklist *quicklist, const long start, const long stop);
quicklistIter *quicklistGetIterator(const quicklist *quicklist, int direction);
quicklistIter *quicklistGetIteratorAtIdx(const quicklist *quicklist,
                                         int direction, const long long idx);
int quicklistNext(quicklistIter *iter, quicklistEntry *node);
void quicklistReleaseIterator(quicklistIter *iter);
quicklist *quicklistDup(quicklist *orig);
int quicklistIndex(const quicklist *quicklist, const long long index,
                   quicklistEntry *entry);
void quicklistRewind(quicklist *quicklist, quicklistIter *li);
void quicklistRewindTail(quicklist *quicklist, quicklistIter *li);
void quicklistRotate(quicklist *quicklist);
int quicklistPopCustom(quicklist *quicklist, int where, unsigned char **data,
                       unsigned int *sz, long long *sval,
                       void *(*saver)(unsigned char *data, unsigned int sz));
int quicklistPop(quicklist *quicklist, int where, unsigned char **data,
                 unsigned int *sz, long long *slong);
unsigned long quicklistCount(const quicklist *ql);
int quicklistCompare(unsigned char *p1, unsigned char *p2, int p2_len);
size_t quicklistGetLzf(const quicklistNode *node, void **data);

#ifdef REDIS_TEST
int quicklistTest(int argc, char *argv[]);
#endif

/* Directions for iterators */
#define AL_START_HEAD 0
#define AL_START_TAIL 1

#endif /* __QUICKLIST_H__ */
