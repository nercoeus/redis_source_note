/* Hash Tables Implementation.
 *
 * This file implements in memory hash tables with insert/del/replace/find/
 * get-random-element operations. Hash tables will auto resize if needed
 * tables of power of two in size are used, collisions are handled by
 * chaining. See the source code for more information... :)
 *
 * Copyright (c) 2006-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
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

#include "fmacros.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>
#include <sys/time.h>

#include "dict.h"
#include "zmalloc.h"
#ifndef DICT_BENCHMARK_MAIN
#include "redisassert.h"
#else
#include <assert.h>
#endif

/* Using dictEnableResize() / dictDisableResize() we make possible to
 * enable/disable resizing of the hash table as needed. This is very important
 * for Redis, as we use copy-on-write and don't want to move too much memory
 * around when there is a child performing saving operations.
 *
 * Note that even when dict_can_resize is set to 0, not all resizes are
 * prevented: a hash table is still allowed to grow if the ratio between
 * the number of elements and the buckets > dict_force_resize_ratio. */
//dict是否可以进行rehash（此时并没有进行AOF，BGSAVE备份操作），默认是达到 1 的时候可以进行rehash
static int dict_can_resize = 1;
//进行rehash的比例因子（正在进行AOF，BGSAVE操作，考虑性能比例因子较大）
static unsigned int dict_force_resize_ratio = 5;

/* -------------------------- private prototypes ---------------------------- */
//隐藏的原始函数

static int _dictExpandIfNeeded(dict *ht);
static unsigned long _dictNextPower(unsigned long size);
static long _dictKeyIndex(dict *ht, const void *key, uint64_t hash, dictEntry **existing);
//dict初始化
static int _dictInit(dict *ht, dictType *type, void *privDataPtr);

/* -------------------------- hash functions -------------------------------- */
//与hash相关的操作函数

static uint8_t dict_hash_function_seed[16];

void dictSetHashFunctionSeed(uint8_t *seed) {
    memcpy(dict_hash_function_seed,seed,sizeof(dict_hash_function_seed));
}

uint8_t *dictGetHashFunctionSeed(void) {
    return dict_hash_function_seed;
}

/* The default hashing function uses SipHash implementation    //默认的hash函数为Siphash implementation
 * in siphash.c. */

uint64_t siphash(const uint8_t *in, const size_t inlen, const uint8_t *k);
uint64_t siphash_nocase(const uint8_t *in, const size_t inlen, const uint8_t *k);

uint64_t dictGenHashFunction(const void *key, int len) {
    return siphash(key,len,dict_hash_function_seed);
}

uint64_t dictGenCaseHashFunction(const unsigned char *buf, int len) {
    return siphash_nocase(buf,len,dict_hash_function_seed);
}

/* ----------------------------- API implementation ------------------------- */

/* Reset a hash table already initialized with ht_init().
 * NOTE: This function should only be called by ht_destroy(). */
//该函数主要为了给一个hash表进行reset，在dict初始化的时候也进行了调用
//为一个dictht进行初始化，这里的内存在前一步就已经申请完毕了
static void _dictReset(dictht *ht)
{
    //全部初始化为空就好了
    ht->table = NULL;   //这里的表不急着初始化
    ht->size = 0;
    ht->sizemask = 0;
    ht->used = 0;
}

/* Create a new hash table */
//创建一个新的dict，这里传入一个type和一个void*表示隐藏数据指针
dict *dictCreate(dictType *type,
        void *privDataPtr)
{
    //直接分配头部节点的空间
    dict *d = zmalloc(sizeof(*d));
    //调用原始函数，多传入一个内存指针
    _dictInit(d,type,privDataPtr);
    return d;
}

/* Initialize the hash table */
//用来初始化hash表
int _dictInit(dict *d, dictType *type,
        void *privDataPtr)
{
    //初始化两个个hash桶，注意这里的空间早就分配成功了，传递一个地址来进行初始化而已
    _dictReset(&d->ht[0]);
    _dictReset(&d->ht[1]);
    //设置dict的type类型，这里就像前面看到的那样，里面为数据的处理函数
    d->type = type;
    d->privdata = privDataPtr;   //隐藏数据直接保存
    d->rehashidx = -1;           //dict默认的rehashidx为-1，这个参数在hash的时候会进行改变
    d->iterators = 0;            //默认的正在使用的指针
    return DICT_OK;              //返回成功标识，虽然并没有用
}

/* Resize the table to the minimal size that contains all the elements,
 * but with the invariant of a USED/BUCKETS ratio near to <= 1 */
 //该函数将表的大小变为所容纳元素的大小，但是USED/BUCJKETS <= 1，这是因为可能存在键冲突的情况发生
int dictResize(dict *d)
{
    int minimal;

    //这里两个判定条件缺一不可，一个是是否可以rehash，另一个是是否正在rehash，出错返回err
    if (!dict_can_resize || dictIsRehashing(d)) return DICT_ERR;
    minimal = d->ht[0].used;      //rehash前的大小
    //判断hash表的大小，如果hash表的size小于4，则将其设置为4
    if (minimal < DICT_HT_INITIAL_SIZE)
        minimal = DICT_HT_INITIAL_SIZE;
    //调用下面的函数来进行表的转换
    return dictExpand(d, minimal);
}

/* Expand or create the hash table */
//扩大或者初始化dict表，给一个dict指针，还有一个容量大小
int dictExpand(dict *d, unsigned long size)
{
    /* the size is invalid if it is smaller than the number of
     * elements already inside the hash table */
    //两种情况会出现错误，第一：不能正在rehash。第二：size的大小不能小于used
    if (dictIsRehashing(d) || d->ht[0].used > size)
        return DICT_ERR;

    //这里直接新建一个dictht是啥意思，也不用个指针吗？ ：不用，因为dictht是一个很小的struct，其次dict结构体中就是使用数组来进行存储的
    dictht n; /* the new hash table */
    //寻找一个合适的大小用来存放size个值，一般是最接近并大于size的 2 的整数幂
    unsigned long realsize = _dictNextPower(size);

    /* Rehashing to the same table size is not useful. */
    //新size完全等于原有dict used的话，redis认为不用rehash
    if (realsize == d->ht[0].size) return DICT_ERR;

    /* Allocate the new hash table and initialize all pointers to NULL */
    // 分配新的hash表需要的内存，并全部置为NULL
    // 这里其实就是初始化一下hash表的参数以及初始化hash表即可
    n.size = realsize;                                 //新size
    n.sizemask = realsize-1;                           //前面提到过，mask永远为size-1，因为hashkey % mask = [下标值]
    n.table = zcalloc(realsize*sizeof(dictEntry*));    //使用calloc来进行内存的分配
    n.used = 0;                                        //新表，used为 0

    /* Is this the first initialization? If so it's not really a rehashing
     * we just set the first hash table so that it can accept keys. */
    //函数开始时说过函数有两个用途，就是在这里进行判断
    if (d->ht[0].table == NULL) {
        d->ht[0] = n;       //前面说过了，n 只是hash表的头节点，直接复制即可
        return DICT_OK;
    }

    /* Prepare a second hash table for incremental rehashing */
    d->ht[1] = n;
    d->rehashidx = 0;      //这里并没有进行rehash，只是设置了rehash进度。。。从 0 开始
    return DICT_OK;        //这里没有进行rehash，仅仅设置了rehashidx是因为会有心跳函数来进行rehash函数的调用，并不需要显式的执行，主要是因为效率的考虑，rehash执行的话会卡住redis。。。可能太大了
}

/* Performs N steps of incremental rehashing. Returns 1 if there are still
 * keys to move from the old to the new hash table, otherwise 0 is returned.
 *
 * Note that a rehashing step consists in moving a bucket (that may have more
 * than one key as we use chaining) from the old to the new hash table, however
 * since part of the hash table may be composed of empty spaces, it is not
 * guaranteed that this function will rehash even a single bucket, since it
 * will visit at max N*10 empty buckets in total, otherwise the amount of
 * work it does would be unbound and the function may block for a long time. */
// 这里执行N步渐进型rehash，执行完函数后还在rehash返回 1，否则返回 0，每次rehash都会移动hash表数组中某个索引上的所有链表节点，这里因为会有碰撞，链地址法就会存在链表，这里把一个链表当作一个原子进行操作
// 这里传入需要rehash的dict，以及需要rehash的key组数，一般来说为100
int dictRehash(dict *d, int n) {
    int empty_visits = n*10; /* Max number of empty buckets to visit. */    //空桶的最多可遍历个数，太多了直接返回别过度使用cpu
    // rehash完毕
    if (!dictIsRehashing(d)) return 0;

    // 基本的判断条件，旧版本为while(n--){}
    while(n-- && d->ht[0].used != 0) {
        dictEntry *de, *nextde;

        /* Note that rehashidx can't overflow as we are sure there are more
         * elements because ht[0].used != 0 */
        //这里判断是否溢出，作者注释风格很舒服
        assert(d->ht[0].size > (unsigned long)d->rehashidx);
        // 根据dict的rehashidx来判断进行到了那个节点，过滤掉空的桶，但是dictRehashMilliseconds()已经加了100了。。。这里可能提前返回
        while(d->ht[0].table[d->rehashidx] == NULL) {
            // 前进一个空桶
            d->rehashidx++;
            if (--empty_visits == 0) return 1;
        }
        // 这个桶不为空
        de = d->ht[0].table[d->rehashidx];
        /* Move all the keys in this bucket from the old to the new hash HT */
        // 桶中可能是一个链表，用一个while循环来解决问题
        while(de) {
            uint64_t h;

            nextde = de->next;
            /* Get the index in the new hash table */
            //计算在新桶里的位置
            h = dictHashKey(d, de->key) & d->ht[1].sizemask;
            // 一系列操作，把它移动到新表中，搞个新桶放进去就行嘞
            de->next = d->ht[1].table[h];
            d->ht[1].table[h] = de;
            d->ht[0].used--;
            d->ht[1].used++;
            de = nextde;
        }
        // 将旧桶置为NULL
        d->ht[0].table[d->rehashidx] = NULL;
        // 前进rehash进度
        d->rehashidx++;
    }

    /* Check if we already rehashed the whole table... */
    // rehash完成，释放oldtable空间，ht[1]上位变成ht[0]
    if (d->ht[0].used == 0) {
        zfree(d->ht[0].table);
        d->ht[0] = d->ht[1];
        // 将ht[1]变为空
        _dictReset(&d->ht[1]);
        // 设置rehashidx为不在rehash状态
        d->rehashidx = -1;
        return 0;
    }

    /* More to rehash... */
    return 1;
}

// 获取系统当前时间， 这里用longlong数据来进行返回
long long timeInMilliseconds(void) {
    struct timeval tv;
    // 获取系统时间
    gettimeofday(&tv,NULL);
    // 第一个参数是秒，第二个是毫秒
    return (((long long)tv.tv_sec)*1000)+(tv.tv_usec/1000);
}

/* Rehash for an amount of time between ms milliseconds and ms+1 milliseconds */
// 每次rehash 100 组数据，在给定的时间里
int dictRehashMilliseconds(dict *d, int ms) {
    // 获取当前的时间，用毫秒来进行记录
    long long start = timeInMilliseconds();
    // 返回rehash的组数
    int rehashes = 0;

    while(dictRehash(d,100)) {
        // 每次rehash为100组数据，用rehashs记录并返回，时间到了，自动break
        rehashes += 100;
        if (timeInMilliseconds()-start > ms) break;
    }
    return rehashes;
}

/* This function performs just a step of rehashing, and only if there are
 * no safe iterators bound to our hash table. When we have iterators in the
 * middle of a rehashing we can't mess with the two hash tables otherwise
 * some element can be missed or duplicated.
 *
 * This function is called by common lookup or update operations in the
 * dictionary so that the hash table automatically migrates from H1 to H2
 * while it is actively used. */
// 这个函数仅执行rehash函数，且步数为1，而且需要没有正在使用的iterator时调用
// 该函数由常见的查找或更新函数进行调用，以便在rehash时将数据从h[0]到h[1]
// 该步骤属于lazy rehashing虽然每次仅仅rehash一个值，但是add，find操作的频繁也大大加快了rehash的速度
static void _dictRehashStep(dict *d) {
    if (d->iterators == 0) dictRehash(d,1);
}

/* Add an element to the target hash table */
// 向dict中添加一个元素
int dictAdd(dict *d, void *key, void *val)
{
    // 这里分为两步，首先先键的hash并进行元素的插入
    dictEntry *entry = dictAddRaw(d,key,NULL);
    // 添加失败
    if (!entry) return DICT_ERR;
    // 设置值，这里的宏定义可以看看
    dictSetVal(d, entry, val);
    return DICT_OK;
}

/* Low level add or find:
 * This function adds the entry but instead of setting a value returns the
 * dictEntry structure to the user, that will make sure to fill the value
 * field as he wishes.
 *
 * This function is also directly exposed to the user API to be called
 * mainly in order to store non-pointers inside the hash value, example:
 *
 * entry = dictAddRaw(dict,mykey,NULL);
 * if (entry != NULL) dictSetSignedIntegerVal(entry,1000);
 *
 * Return values:
 *
 * If key already exists NULL is returned, and "*existing" is populated
 * with the existing entry if existing is not NULL.
 *
 * If key was added, the hash entry is returned to be manipulated by the caller.
 */
// 底层添加、查找函数：第三项为NULL则添加，不为NULL则查找
dictEntry *dictAddRaw(dict *d, void *key, dictEntry **existing)
{
    long index;
    dictEntry *entry;
    dictht *ht;

    if (dictIsRehashing(d)) _dictRehashStep(d);

    /* Get the index of the new element, or -1 if
     * the element already exists. */
    // 获取key对应的hash值，已经存在返回-1，并通过existing拿到已经存在的元素的地址，否则返回桶的编号
    if ((index = _dictKeyIndex(d, key, dictHashKey(d,key), existing)) == -1)
        return NULL;

    /* Allocate the memory and store the new entry.
     * Insert the element in top, with the assumption that in a database
     * system it is more likely that recently added entries are accessed
     * more frequently. */
    // 分配空间并进行插入，要看应该往那个表插入，正在rehash就需要插入到新表中
    ht = dictIsRehashing(d) ? &d->ht[1] : &d->ht[0];
    entry = zmalloc(sizeof(*entry));
    // 这里采用头部插入
    entry->next = ht->table[index];
    ht->table[index] = entry;
    ht->used++;

    /* Set the hash entry fields. */
    dictSetKey(d, entry, key);
    return entry;
}

/* Add or Overwrite:
 * Add an element, discarding the old value if the key already exists.
 * Return 1 if the key was added from scratch, 0 if there was already an
 * element with such key and dictReplace() just performed a value update
 * operation. */
// 添加元素或者覆盖对应key的值
int dictReplace(dict *d, void *key, void *val)
{
    dictEntry *entry, *existing, auxentry;

    /* Try to add the element. If the key
     * does not exists dictAdd will succeed. */
    //尝试着添加新的元素，如果亦仅存在会返回NULL，并且通过existing拿到已经存在的key对应值的指针，否则返回对应的值的指针
    entry = dictAddRaw(d,key,&existing);
    if (entry) {
        // 如果不存在，直接赋值即可
        dictSetVal(d, entry, val);
        return 1;
    }

    /* Set the new value and free the old one. Note that it is important
     * to do that in this order, as the value may just be exactly the same
     * as the previous one. In this context, think to reference counting,
     * you want to increment (set), and then decrement (free), and not the
     * reverse. */
    // 这里代表已经存在相同的key
    auxentry = *existing;
    // 通过拿到的指针，直接赋值即可
    dictSetVal(d, existing, val);
    // 不要忘了free掉不用的数据
    dictFreeVal(d, &auxentry);
    return 0;
}

/* Add or Find:
 * dictAddOrFind() is simply a version of dictAddRaw() that always
 * returns the hash entry of the specified key, even if the key already
 * exists and can't be added (in that case the entry of the already
 * existing key is returned.)
 *
 * See dictAddRaw() for more information. */
// 直接插入或find， 调用dictAddFind
dictEntry *dictAddOrFind(dict *d, void *key) {
    dictEntry *entry, *existing;
    // 尝试插入，存在的话会直接返回元素的指针
    entry = dictAddRaw(d,key,&existing);
    // 是否存在，存在的话，直接返回查找的值，否则返回调用函数返回的值
    return entry ? entry : existing;
}

/* Search and remove an element. This is an helper function for
 * dictDelete() and dictUnlink(), please check the top comment
 * of those functions. */
// 根据key来删除元素，这里需要考虑rehash的影响， 第三项来控制是否释放这个元素
static dictEntry *dictGenericDelete(dict *d, const void *key, int nofree) {
    uint64_t h, idx;
    dictEntry *he, *prevHe;
    int table;
    // dict为空
    if (d->ht[0].used == 0 && d->ht[1].used == 0) return NULL;

    if (dictIsRehashing(d)) _dictRehashStep(d);
    // 对应的hash值
    h = dictHashKey(d, key);
    // 需要在两个hash表中进行查找 ht[0], ht[1]
    for (table = 0; table <= 1; table++) {
        // 对应的桶号
        idx = h & d->ht[table].sizemask;
        // 看看是否存在这个元素
        he = d->ht[table].table[idx];
        prevHe = NULL;
        // 元素存在，遍历桶中的链表
        while(he) {
            // 比较，这里先比较地址，地址相同不用比了
            //（这里涉及redis内存优化策略：redis初始化时会分配很多数字的sds，很多键值会指向这些共享内存
            // 所以通过地址比较又是可以快速的进行判断）
            if (key==he->key || dictCompareKeys(d, key, he->key)) {
                /* Unlink the element from the list */
                if (prevHe)
                    prevHe->next = he->next;
                else
                    d->ht[table].table[idx] = he->next;
                // 用这个项来控制是否进行元素的释放， 这里不管释放与否used均减一
                if (!nofree) {
                    dictFreeKey(d, he);
                    dictFreeVal(d, he);
                    zfree(he);
                }
                d->ht[table].used--;
                return he;
            }
            // 不是当前节点，遍历下去
            prevHe = he;
            he = he->next;
        }
        // 这里如果没有rehash就不需要遍历ht[1]
        if (!dictIsRehashing(d)) break;
    }
    return NULL; /* not found */
}

/* Remove an element, returning DICT_OK on success or DICT_ERR if the
 * element was not found. */
// 通过key来进行元素的删除，直接调用dictGenericDelete()，查找不到返回ERR
int dictDelete(dict *ht, const void *key) {
    return dictGenericDelete(ht,key,0) ? DICT_OK : DICT_ERR;
}

/* Remove an element from the table, but without actually releasing
 * the key, value and dictionary entry. The dictionary entry is returned
 * if the element was found (and unlinked from the table), and the user
 * should later call `dictFreeUnlinkedEntry()` with it in order to release it.
 * Otherwise if the key is not found, NULL is returned.
 *
 * This function is useful when we want to remove something from the hash
 * table but want to use its value before actually deleting the entry.
 * Without this function the pattern would require two lookups:
 *
 *  entry = dictFind(...);
 *  // Do something with entry
 *  dictDelete(dictionary,entry);
 *
 * Thanks to this function it is possible to avoid this, and use
 * instead:
 *
 * entry = dictUnlink(dictionary,entry);
 * // Do something with entry
 * dictFreeUnlinkedEntry(entry); // <- This does not need to lookup again.
 */
// 以下两个函数的作用是为了在找到某个元素后还需要进行操作，然后在进行删除
// 使用普通的方式需要查找两次，用下面的方式只需要一次，例子见作者注释
// 该函数虽然调用dictgeneriDelete()但是并不会删除桶中的数据，会返回查找到的元素指针
dictEntry *dictUnlink(dict *ht, const void *key) {
    return dictGenericDelete(ht,key,1);
}

/* You need to call this function to really free the entry after a call
 * to dictUnlink(). It's safe to call this function with 'he' = NULL. */
// 在调用dictUnlink()函数后使用该函数来把获取到的元素真正的删除
void dictFreeUnlinkedEntry(dict *d, dictEntry *he) {
    if (he == NULL) return;
    dictFreeKey(d, he);
    dictFreeVal(d, he);
    zfree(he);
}

/* Destroy an entire dictionary */
// 使用该函数删除一张hash表，这里第三项暂时不知有什么用处，类似于对私有数据进行一些操作
int _dictClear(dict *d, dictht *ht, void(callback)(void *)) {
    unsigned long i;

    /* Free all the elements */
    // 删除表中所有数据
    for (i = 0; i < ht->size && ht->used > 0; i++) {
        dictEntry *he, *nextHe;
        // TODO ???
        if (callback && (i & 65535) == 0) callback(d->privdata);

        if ((he = ht->table[i]) == NULL) continue;
        // 删除一整个链表
        while(he) {
            nextHe = he->next;
            dictFreeKey(d, he);
            dictFreeVal(d, he);
            zfree(he);
            ht->used--;
            he = nextHe;
        }
    }
    // 释放桶空间
    /* Free the table and the allocated cache structure */
    zfree(ht->table);
    /* Re-initialize the table */
    // 重置该hash表
    _dictReset(ht);
    return DICT_OK; /* never fails */
}

/* Clear & Release the hash table */
// 清除以及释放dict
void dictRelease(dict *d)
{
    _dictClear(d,&d->ht[0],NULL);
    _dictClear(d,&d->ht[1],NULL);
    zfree(d);
}


// 查找一个key
dictEntry *dictFind(dict *d, const void *key)
{
    dictEntry *he;
    uint64_t h, idx, table;
    // dict为空
    if (d->ht[0].used + d->ht[1].used == 0) return NULL; /* dict is empty */
    // TODO ???
    if (dictIsRehashing(d)) _dictRehashStep(d);
    // 获得它的hash值
    h = dictHashKey(d, key);
    // 在两个hash表中进行查找
    for (table = 0; table <= 1; table++) {
        // 桶号
        idx = h & d->ht[table].sizemask;
        // 在链表中查找
        he = d->ht[table].table[idx];
        while(he) {
            // key比较
            if (key==he->key || dictCompareKeys(d, key, he->key))
                return he;
            he = he->next;
        }
        if (!dictIsRehashing(d)) return NULL;
    }
    // 找不到返回NULL
    return NULL;
}

// 获取对应key的value，这里直接调用dictFind()查找即可
void *dictFetchValue(dict *d, const void *key) {
    dictEntry *he;

    he = dictFind(d,key);
    return he ? dictGetVal(he) : NULL;
}

/* A fingerprint is a 64 bit number that represents the state of the dictionary
 * at a given time, it's just a few dict properties xored together.
 * When an unsafe iterator is initialized, we get the dict fingerprint, and check
 * the fingerprint again when the iterator is released.
 * If the two fingerprints are different it means that the user of the iterator
 * performed forbidden operations against the dictionary while iterating. */
// 作者的话说，给dict生成一个指纹，用来判断dict是否发生变化，一般用在迭代器操作之后进行判读
long long dictFingerprint(dict *d) {
    // 指纹是64位的数字
    long long integers[6], hash = 0;
    int j;
    // 找几个数据
    integers[0] = (long) d->ht[0].table;
    integers[1] = d->ht[0].size;
    integers[2] = d->ht[0].used;
    integers[3] = (long) d->ht[1].table;
    integers[4] = d->ht[1].size;
    integers[5] = d->ht[1].used;

    /* We hash N integers by summing every successive integer with the integer
     * hashing of the previous sum. Basically:
     *
     * Result = hash(hash(hash(int1)+int2)+int3) ...
     *
     * This way the same set of integers in a different order will (likely) hash
     * to a different number. */
    // 具体算法暂且不做研究
    for (j = 0; j < 6; j++) {
        hash += integers[j];
        /* For the hashing step we use Tomas Wang's 64 bit integer hash. */
        hash = (~hash) + (hash << 21); // hash = (hash << 21) - hash - 1;
        hash = hash ^ (hash >> 24);
        hash = (hash + (hash << 3)) + (hash << 8); // hash * 265
        hash = hash ^ (hash >> 14);
        hash = (hash + (hash << 2)) + (hash << 4); // hash * 21
        hash = hash ^ (hash >> 28);
        hash = hash + (hash << 31);
    }
    return hash;
}

// 获得一个迭代器
dictIterator *dictGetIterator(dict *d)
{
    // 分配内存，也不判断是否为NULL？
    dictIterator *iter = zmalloc(sizeof(*iter));

    iter->d = d;             // 指向的dict
    iter->table = 0;         // 表号
    iter->index = -1;        // 桶号
    iter->safe = 0;          // 初始Iterator是不安全的
    iter->entry = NULL;      // 指向的元素
    iter->nextEntry = NULL;  // 下一个元素
    return iter;
}

// 获得一个安全的迭代器，直接拿一个不安全的修改一下safe字段即可
dictIterator *dictGetSafeIterator(dict *d) {
    dictIterator *i = dictGetIterator(d);

    i->safe = 1;
    return i;
}

// 字面意思Next，注意：按照iterator的说明，不安全的迭代器只能进行Next()操作
// 所以另一方面说明Next()可以被安全和不安全的iterator使用
dictEntry *dictNext(dictIterator *iter)
{
    while (1) {
        // iterator可能已经删除或者是一个新节点
        if (iter->entry == NULL) {
            // 先获取表指针
            dictht *ht = &iter->d->ht[iter->table];
            // 是一个新iterator
            if (iter->index == -1 && iter->table == 0) {
                // 安全的话在这里增加dict的iterator计数，因为这里也算是初始化这个iterator
                if (iter->safe)
                    iter->d->iterators++;
                else
                // 计算这个迭代器此时指向的dict的"指纹值"
                    iter->fingerprint = dictFingerprint(iter->d);
            }
            // 增加桶的值
            iter->index++;
            // 如果增加后的桶值大于桶的size
            if (iter->index >= (long) ht->size) {
                // 可能是当前桶太小了，此时正在rehash的话，换一个表
                if (dictIsRehashing(iter->d) && iter->table == 0) {
                    iter->table++;
                    iter->index = 0;
                    ht = &iter->d->ht[1];
                } else {
                    // 遍历完了，没有元素
                    break;
                }
            }

            iter->entry = ht->table[iter->index];
        } else {
            // entry不为空，代表这个iterator使用过，直接等于next
            iter->entry = iter->nextEntry;
        }
        // 已经取得next元素
        if (iter->entry) {
            /* We need to save the 'next' here, the iterator user
             * may delete the entry we are returning. */
            // 这里要把下一个元素地址保存在iterator里，因为当我们删除元素时这个iterator可能需要执行删除操作
            iter->nextEntry = iter->entry->next;
            return iter->entry;
        }
    }
    return NULL;
}

// 释放iterator
void dictReleaseIterator(dictIterator *iter)
{
    // 使用过的iterator
    if (!(iter->index == -1 && iter->table == 0)) {
        // 安全的就修改dict的iterators
        if (iter->safe)
            iter->d->iterators--;
        else
            // 不安全iterator可能修改了dict，判断是否报错
            assert(iter->fingerprint == dictFingerprint(iter->d));
    }
    // 释放内存空间
    zfree(iter);
}

/* Return a random entry from the hash table. Useful to
 * implement randomized algorithms */
// 获得一个随机的key
dictEntry *dictGetRandomKey(dict *d)
{
    dictEntry *he, *orighe;
    unsigned long h;
    int listlen, listele;

    if (dictSize(d) == 0) return NULL;
    if (dictIsRehashing(d)) _dictRehashStep(d);
    if (dictIsRehashing(d)) {
        do {
            /* We are sure there are no elements in indexes from 0
             * to rehashidx-1 */
            // 。。。
            h = d->rehashidx + (random() % (d->ht[0].size +
                                            d->ht[1].size -
                                            d->rehashidx));
            he = (h >= d->ht[0].size) ? d->ht[1].table[h - d->ht[0].size] :
                                      d->ht[0].table[h];
        } while(he == NULL);
    } else {
        // 一直随机直到第一个不为空的
        do {
            h = random() & d->ht[0].sizemask;
            he = d->ht[0].table[h];
        } while(he == NULL);
    }

    /* Now we found a non empty bucket, but it is a linked
     * list and we need to get a random element from the list.
     * The only sane way to do so is counting the elements and
     * select a random index. */
    // 随即完了桶，还需要把桶中的链表随机一下
    listlen = 0;
    orighe = he;
    while(he) {
        he = he->next;
        listlen++;
    }
    listele = random() % listlen;
    he = orighe;
    while(listele--) he = he->next;
    return he;
}

/* This function samples the dictionary to return a few keys from random
 * locations.
 *
 * It does not guarantee to return all the keys specified in 'count', nor
 * it does guarantee to return non-duplicated elements, however it will make
 * some effort to do both things.
 *
 * Returned pointers to hash table entries are stored into 'des' that
 * points to an array of dictEntry pointers. The array must have room for
 * at least 'count' elements, that is the argument we pass to the function
 * to tell how many random elements we need.
 *
 * The function returns the number of items stored into 'des', that may
 * be less than 'count' if the hash table has less than 'count' elements
 * inside, or if not enough elements were found in a reasonable amount of
 * steps.
 *
 * Note that this function is not suitable when you need a good distribution
 * of the returned items, but only when you need to "sample" a given number
 * of continuous elements to run some kind of algorithm or to produce
 * statistics. However the function is much faster than dictGetRandomKey()
 * at producing N elements. */
// 随机抽样，抽取count个元素返回
unsigned int dictGetSomeKeys(dict *d, dictEntry **des, unsigned int count) {
    unsigned long j; /* internal hash table id, 0 or 1. */
    unsigned long tables; /* 1 or 2 tables? */
    unsigned long stored = 0, maxsizemask;
    unsigned long maxsteps;

    if (dictSize(d) < count) count = dictSize(d);
    maxsteps = count*10;

    /* Try to do a rehashing work proportional to 'count'. */
    for (j = 0; j < count; j++) {
        if (dictIsRehashing(d))
            _dictRehashStep(d);
        else
            break;
    }

    tables = dictIsRehashing(d) ? 2 : 1;
    maxsizemask = d->ht[0].sizemask;
    if (tables > 1 && maxsizemask < d->ht[1].sizemask)
        maxsizemask = d->ht[1].sizemask;

    /* Pick a random point inside the larger table. */
    unsigned long i = random() & maxsizemask;
    unsigned long emptylen = 0; /* Continuous empty entries so far. */
    while(stored < count && maxsteps--) {
        for (j = 0; j < tables; j++) {
            /* Invariant of the dict.c rehashing: up to the indexes already
             * visited in ht[0] during the rehashing, there are no populated
             * buckets, so we can skip ht[0] for indexes between 0 and idx-1. */
            if (tables == 2 && j == 0 && i < (unsigned long) d->rehashidx) {
                /* Moreover, if we are currently out of range in the second
                 * table, there will be no elements in both tables up to
                 * the current rehashing index, so we jump if possible.
                 * (this happens when going from big to small table). */
                if (i >= d->ht[1].size)
                    i = d->rehashidx;
                else
                    continue;
            }
            if (i >= d->ht[j].size) continue; /* Out of range for this table. */
            dictEntry *he = d->ht[j].table[i];

            /* Count contiguous empty buckets, and jump to other
             * locations if they reach 'count' (with a minimum of 5). */
            if (he == NULL) {
                emptylen++;
                if (emptylen >= 5 && emptylen > count) {
                    i = random() & maxsizemask;
                    emptylen = 0;
                }
            } else {
                emptylen = 0;
                while (he) {
                    /* Collect all the elements of the buckets found non
                     * empty while iterating. */
                    *des = he;
                    des++;
                    he = he->next;
                    stored++;
                    if (stored == count) return stored;
                }
            }
        }
        i = (i+1) & maxsizemask;
    }
    return stored;
}

/* Function to reverse bits. Algorithm from:
 * http://graphics.stanford.edu/~seander/bithacks.html#ReverseParallel */
 // 该函数用来反转位操作，具体算法不做注释
static unsigned long rev(unsigned long v) {
    unsigned long s = 8 * sizeof(v); // bit size; must be power of 2
    unsigned long mask = ~0;
    while ((s >>= 1) > 0) {
        mask ^= (mask << s);
        v = ((v >> s) & mask) | ((v << s) & ~mask);
    }
    return v;
}

/* dictScan() is used to iterate over the elements of a dictionary.
 *
 * Iterating works the following way:
 *
 * 1) Initially you call the function using a cursor (v) value of 0.
 * 2) The function performs one step of the iteration, and returns the
 *    new cursor value you must use in the next call.
 * 3) When the returned cursor is 0, the iteration is complete.
 *
 * The function guarantees all elements present in the
 * dictionary get returned between the start and end of the iteration.
 * However it is possible some elements get returned multiple times.
 *
 * For every element returned, the callback argument 'fn' is
 * called with 'privdata' as first argument and the dictionary entry
 * 'de' as second argument.
 *
 * HOW IT WORKS.
 *
 * The iteration algorithm was designed by Pieter Noordhuis.
 * The main idea is to increment a cursor starting from the higher order
 * bits. That is, instead of incrementing the cursor normally, the bits
 * of the cursor are reversed, then the cursor is incremented, and finally
 * the bits are reversed again.
 *
 * This strategy is needed because the hash table may be resized between
 * iteration calls.
 *
 * dict.c hash tables are always power of two in size, and they
 * use chaining, so the position of an element in a given table is given
 * by computing the bitwise AND between Hash(key) and SIZE-1
 * (where SIZE-1 is always the mask that is equivalent to taking the rest
 *  of the division between the Hash of the key and SIZE).
 *
 * For example if the current hash table size is 16, the mask is
 * (in binary) 1111. The position of a key in the hash table will always be
 * the last four bits of the hash output, and so forth.
 *
 * WHAT HAPPENS IF THE TABLE CHANGES IN SIZE?
 *
 * If the hash table grows, elements can go anywhere in one multiple of
 * the old bucket: for example let's say we already iterated with
 * a 4 bit cursor 1100 (the mask is 1111 because hash table size = 16).
 *
 * If the hash table will be resized to 64 elements, then the new mask will
 * be 111111. The new buckets you obtain by substituting in ??1100
 * with either 0 or 1 can be targeted only by keys we already visited
 * when scanning the bucket 1100 in the smaller hash table.
 *
 * By iterating the higher bits first, because of the inverted counter, the
 * cursor does not need to restart if the table size gets bigger. It will
 * continue iterating using cursors without '1100' at the end, and also
 * without any other combination of the final 4 bits already explored.
 *
 * Similarly when the table size shrinks over time, for example going from
 * 16 to 8, if a combination of the lower three bits (the mask for size 8
 * is 111) were already completely explored, it would not be visited again
 * because we are sure we tried, for example, both 0111 and 1111 (all the
 * variations of the higher bit) so we don't need to test it again.
 *
 * WAIT... YOU HAVE *TWO* TABLES DURING REHASHING!
 *
 * Yes, this is true, but we always iterate the smaller table first, then
 * we test all the expansions of the current cursor into the larger
 * table. For example if the current cursor is 101 and we also have a
 * larger table of size 16, we also test (0)101 and (1)101 inside the larger
 * table. This reduces the problem back to having only one table, where
 * the larger one, if it exists, is just an expansion of the smaller one.
 *
 * LIMITATIONS
 *
 * This iterator is completely stateless, and this is a huge advantage,
 * including no additional memory used.
 *
 * The disadvantages resulting from this design are:
 *
 * 1) It is possible we return elements more than once. However this is usually
 *    easy to deal with in the application level.
 * 2) The iterator must return multiple elements per call, as it needs to always
 *    return all the keys chained in a given bucket, and all the expansions, so
 *    we are sure we don't miss keys moving during rehashing.
 * 3) The reverse cursor is somewhat hard to understand at first, but this
 *    comment is supposed to help.
 */
// 对dict进行遍历，访问所有元素，该函数考虑到了rehash过程对元素位置的影响，所以会存在遍历重复的情况，但已经很好的减少了这种情况的发生
// 这个函数十分精巧，可以在查找相关资料进行学习
unsigned long dictScan(dict *d,
                       unsigned long v,
                       dictScanFunction *fn,
                       dictScanBucketFunction* bucketfn,
                       void *privdata)
{
    dictht *t0, *t1;
    const dictEntry *de, *next;
    unsigned long m0, m1;
    // 空表
    if (dictSize(d) == 0) return 0;

    if (!dictIsRehashing(d)) {
        t0 = &(d->ht[0]);
        m0 = t0->sizemask;

        /* Emit entries at cursor */
        if (bucketfn) bucketfn(privdata, &t0->table[v & m0]);
        de = t0->table[v & m0];
        while (de) {
            next = de->next;
            fn(privdata, de);
            de = next;
        }

        /* Set unmasked bits so incrementing the reversed cursor
         * operates on the masked bits */
        v |= ~m0;

        /* Increment the reverse cursor */
        v = rev(v);
        v++;
        v = rev(v);

    } else {
        t0 = &d->ht[0];
        t1 = &d->ht[1];

        /* Make sure t0 is the smaller and t1 is the bigger table */
        if (t0->size > t1->size) {
            t0 = &d->ht[1];
            t1 = &d->ht[0];
        }

        m0 = t0->sizemask;
        m1 = t1->sizemask;

        /* Emit entries at cursor */
        if (bucketfn) bucketfn(privdata, &t0->table[v & m0]);
        de = t0->table[v & m0];
        while (de) {
            next = de->next;
            fn(privdata, de);
            de = next;
        }

        /* Iterate over indices in larger table that are the expansion
         * of the index pointed to by the cursor in the smaller table */
        do {
            /* Emit entries at cursor */
            if (bucketfn) bucketfn(privdata, &t1->table[v & m1]);
            de = t1->table[v & m1];
            while (de) {
                next = de->next;
                fn(privdata, de);
                de = next;
            }

            /* Increment the reverse cursor not covered by the smaller mask.*/
            v |= ~m1;
            v = rev(v);
            v++;
            v = rev(v);

            /* Continue while bits covered by mask difference is non-zero */
        } while (v & (m0 ^ m1));
    }

    return v;
}

/* ------------------------- private functions ------------------------------ */

/* Expand the hash table if needed */
// 调用此函数来判断dict是否需要hash，如果需要则rehash
static int _dictExpandIfNeeded(dict *d)
{
    /* Incremental rehashing already in progress. Return. */
    // 正在rehash直接返回
    if (dictIsRehashing(d)) return DICT_OK;

    /* If the hash table is empty expand it to the initial size. */
    // 表为空，收缩变成初始大小
    if (d->ht[0].size == 0) return dictExpand(d, DICT_HT_INITIAL_SIZE);

    /* If we reached the 1:1 ratio, and we are allowed to resize the hash
     * table (global setting) or we should avoid it but the ratio between
     * elements/buckets is over the "safe" threshold, we resize doubling
     * the number of buckets. */
    // 两种情况
    // 第一种：rehash因子为 1 ，此时没有进行AOF或BGSAVE操作
    // 第二种：rehash因子为 5 ，此时正在进行AOF或BGSAVE操作
    if (d->ht[0].used >= d->ht[0].size &&
        (dict_can_resize ||
         d->ht[0].used/d->ht[0].size > dict_force_resize_ratio))
    {
        // 进行扩展，桶大默认扩展为 2 被，虽然可能超过最大大小，但是不在这里进行判断
        return dictExpand(d, d->ht[0].used*2);
    }
    return DICT_OK;
}

/* Our hash table capability is a power of two */
static unsigned long _dictNextPower(unsigned long size)
{
    //默认表的大小为4
    unsigned long i = DICT_HT_INITIAL_SIZE;
    //如果表的大小太大的话，直接申请最大的表结构加1，这里类似于内存分配策略
    if (size >= LONG_MAX) return LONG_MAX + 1LU;
    //否则的话，不断*2，找到最接近并大于size的值返回
    while(1) {
        if (i >= size)
            return i;
        i *= 2;
    }
}

/* Returns the index of a free slot that can be populated with
 * a hash entry for the given 'key'.
 * If the key already exists, -1 is returned
 * and the optional output parameter may be filled.
 *
 * Note that if we are in the process of rehashing the hash table, the
 * index is always returned in the context of the second (new) hash table. */
// 获得对应key的index，已经存在返回-1
// 第一个参数是对应的dict，第二个是key，第三项是对应的hash值，第四项是用来记录元素是否存在并进行返回
static long _dictKeyIndex(dict *d, const void *key, uint64_t hash, dictEntry **existing)
{
    unsigned long idx, table;
    dictEntry *he;
    if (existing) *existing = NULL;

    /* Expand the hash table if needed */
    if (_dictExpandIfNeeded(d) == DICT_ERR)
        return -1;
    for (table = 0; table <= 1; table++) {
        idx = hash & d->ht[table].sizemask;
        /* Search if this slot does not already contain the given key */
        he = d->ht[table].table[idx];
        while(he) {
            if (key==he->key || dictCompareKeys(d, key, he->key)) {
                // 这里返回指针的引用没有问题，因为这是通过传入的existing来记录元素地址
                if (existing) *existing = he;
                // 存在返回-1
                return -1;
            }
            he = he->next;
        }
        if (!dictIsRehashing(d)) break;
    }
    // 不存在返回对应的桶号
    return idx;
}

// 清空一个dict但是并不释放这个表，这里传入的函数暂时不知有啥用
void dictEmpty(dict *d, void(callback)(void*)) {
    _dictClear(d,&d->ht[0],callback);
    _dictClear(d,&d->ht[1],callback);
    d->rehashidx = -1;
    d->iterators = 0;
}

// dict可以进行rehash
void dictEnableResize(void) {
    dict_can_resize = 1;
}

// dict不能进行rehash
void dictDisableResize(void) {
    dict_can_resize = 0;
}

// 获取key对应的hash值
uint64_t dictGetHash(dict *d, const void *key) {
    return dictHashKey(d, key);
}

/* Finds the dictEntry reference by using pointer and pre-calculated hash.
 * oldkey is a dead pointer and should not be accessed.
 * the hash value should be provided using dictGetHash.
 * no string / key comparison is performed.
 * return value is the reference to the dictEntry if found, or NULL if not found. */
// 类似于寻找函数，只是这个函数接收一个key的地址以及一个hash值进行查找，返回值找到的key指针的引用
dictEntry **dictFindEntryRefByPtrAndHash(dict *d, const void *oldptr, uint64_t hash) {
    dictEntry *he, **heref;
    unsigned long idx, table;

    if (d->ht[0].used + d->ht[1].used == 0) return NULL; /* dict is empty */
    // 遍历两个表
    for (table = 0; table <= 1; table++) {
        // 获取对应桶元素地址的引用
        idx = hash & d->ht[table].sizemask;
        heref = &d->ht[table].table[idx];
        he = *heref;
        // 遍历查找，比较key的地址
        while(he) {
            if (oldptr==he->key)
                return heref;
            heref = &he->next;
            he = *heref;
        }
        if (!dictIsRehashing(d)) return NULL;
    }
    return NULL;
}

/* ------------------------------- Debugging ---------------------------------*/

#define DICT_STATS_VECTLEN 50
size_t _dictGetStatsHt(char *buf, size_t bufsize, dictht *ht, int tableid) {
    unsigned long i, slots = 0, chainlen, maxchainlen = 0;
    unsigned long totchainlen = 0;
    unsigned long clvector[DICT_STATS_VECTLEN];
    size_t l = 0;

    if (ht->used == 0) {
        return snprintf(buf,bufsize,
            "No stats available for empty dictionaries\n");
    }

    /* Compute stats. */
    for (i = 0; i < DICT_STATS_VECTLEN; i++) clvector[i] = 0;
    for (i = 0; i < ht->size; i++) {
        dictEntry *he;

        if (ht->table[i] == NULL) {
            clvector[0]++;
            continue;
        }
        slots++;
        /* For each hash entry on this slot... */
        chainlen = 0;
        he = ht->table[i];
        while(he) {
            chainlen++;
            he = he->next;
        }
        clvector[(chainlen < DICT_STATS_VECTLEN) ? chainlen : (DICT_STATS_VECTLEN-1)]++;
        if (chainlen > maxchainlen) maxchainlen = chainlen;
        totchainlen += chainlen;
    }

    /* Generate human readable stats. */
    l += snprintf(buf+l,bufsize-l,
        "Hash table %d stats (%s):\n"
        " table size: %ld\n"
        " number of elements: %ld\n"
        " different slots: %ld\n"
        " max chain length: %ld\n"
        " avg chain length (counted): %.02f\n"
        " avg chain length (computed): %.02f\n"
        " Chain length distribution:\n",
        tableid, (tableid == 0) ? "main hash table" : "rehashing target",
        ht->size, ht->used, slots, maxchainlen,
        (float)totchainlen/slots, (float)ht->used/slots);

    for (i = 0; i < DICT_STATS_VECTLEN-1; i++) {
        if (clvector[i] == 0) continue;
        if (l >= bufsize) break;
        l += snprintf(buf+l,bufsize-l,
            "   %s%ld: %ld (%.02f%%)\n",
            (i == DICT_STATS_VECTLEN-1)?">= ":"",
            i, clvector[i], ((float)clvector[i]/ht->size)*100);
    }

    /* Unlike snprintf(), teturn the number of characters actually written. */
    if (bufsize) buf[bufsize-1] = '\0';
    return strlen(buf);
}

void dictGetStats(char *buf, size_t bufsize, dict *d) {
    size_t l;
    char *orig_buf = buf;
    size_t orig_bufsize = bufsize;

    l = _dictGetStatsHt(buf,bufsize,&d->ht[0],0);
    buf += l;
    bufsize -= l;
    if (dictIsRehashing(d) && bufsize > 0) {
        _dictGetStatsHt(buf,bufsize,&d->ht[1],1);
    }
    /* Make sure there is a NULL term at the end. */
    if (orig_bufsize) orig_buf[orig_bufsize-1] = '\0';
}

/* ------------------------------- Benchmark ---------------------------------*/

#ifdef DICT_BENCHMARK_MAIN

#include "sds.h"

uint64_t hashCallback(const void *key) {
    return dictGenHashFunction((unsigned char*)key, sdslen((char*)key));
}

int compareCallback(void *privdata, const void *key1, const void *key2) {
    int l1,l2;
    DICT_NOTUSED(privdata);

    l1 = sdslen((sds)key1);
    l2 = sdslen((sds)key2);
    if (l1 != l2) return 0;
    return memcmp(key1, key2, l1) == 0;
}

void freeCallback(void *privdata, void *val) {
    DICT_NOTUSED(privdata);

    sdsfree(val);
}

dictType BenchmarkDictType = {
    hashCallback,
    NULL,
    NULL,
    compareCallback,
    freeCallback,
    NULL
};

#define start_benchmark() start = timeInMilliseconds()
#define end_benchmark(msg) do { \
    elapsed = timeInMilliseconds()-start; \
    printf(msg ": %ld items in %lld ms\n", count, elapsed); \
} while(0);

/* dict-benchmark [count] */
int main(int argc, char **argv) {
    long j;
    long long start, elapsed;
    dict *dict = dictCreate(&BenchmarkDictType,NULL);
    long count = 0;

    if (argc == 2) {
        count = strtol(argv[1],NULL,10);
    } else {
        count = 5000000;
    }

    start_benchmark();
    for (j = 0; j < count; j++) {
        int retval = dictAdd(dict,sdsfromlonglong(j),(void*)j);
        assert(retval == DICT_OK);
    }
    end_benchmark("Inserting");
    assert((long)dictSize(dict) == count);

    /* Wait for rehashing. */
    while (dictIsRehashing(dict)) {
        dictRehashMilliseconds(dict,100);
    }

    start_benchmark();
    for (j = 0; j < count; j++) {
        sds key = sdsfromlonglong(j);
        dictEntry *de = dictFind(dict,key);
        assert(de != NULL);
        sdsfree(key);
    }
    end_benchmark("Linear access of existing elements");

    start_benchmark();
    for (j = 0; j < count; j++) {
        sds key = sdsfromlonglong(j);
        dictEntry *de = dictFind(dict,key);
        assert(de != NULL);
        sdsfree(key);
    }
    end_benchmark("Linear access of existing elements (2nd round)");

    start_benchmark();
    for (j = 0; j < count; j++) {
        sds key = sdsfromlonglong(rand() % count);
        dictEntry *de = dictFind(dict,key);
        assert(de != NULL);
        sdsfree(key);
    }
    end_benchmark("Random access of existing elements");

    start_benchmark();
    for (j = 0; j < count; j++) {
        sds key = sdsfromlonglong(rand() % count);
        key[0] = 'X';
        dictEntry *de = dictFind(dict,key);
        assert(de == NULL);
        sdsfree(key);
    }
    end_benchmark("Accessing missing");

    start_benchmark();
    for (j = 0; j < count; j++) {
        sds key = sdsfromlonglong(j);
        int retval = dictDelete(dict,key);
        assert(retval == DICT_OK);
        key[0] += 17; /* Change first number to letter. */
        retval = dictAdd(dict,key,(void*)j);
        assert(retval == DICT_OK);
    }
    end_benchmark("Removing and adding");
}
#endif
