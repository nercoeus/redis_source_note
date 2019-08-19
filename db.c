/*
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
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

#include "server.h"
#include "cluster.h"
#include "atomicvar.h"

#include <signal.h>
#include <ctype.h>

/*-----------------------------------------------------------------------------
 * C-level DB API
 *----------------------------------------------------------------------------*/
// 判断键是否过期
int keyIsExpired(redisDb *db, robj *key);

/* Update LFU when an object is accessed.
 * Firstly, decrement the counter if the decrement time is reached.
 * Then logarithmically increment the counter, and update the access time. */
// 对于 LFU 策略，每次对键的访问均会更新值的 LFU
// 到达递减时间，会进行递减
// 增加计数器，更新访问时间
void updateLFU(robj *val) {
    unsigned long counter = LFUDecrAndReturn(val);
    counter = LFULogIncr(counter);
    val->lru = (LFUGetTimeInMinutes()<<8) | counter;
}

/* Low level key lookup API, not actually called directly from commands
 * implementations that should instead rely on lookupKeyRead(),
 * lookupKeyWrite() and lookupKeyReadWithFlags(). */
// 底层的 lookup 函数，被其余的 lookup 函数使用
// 传入一个 redisDb，以及需要查找的 key_name，返回 key_name 对应的 value
robj *lookupKey(redisDb *db, robj *key, int flags) {
    // 按照 key_name 从数据库的键值对里进行查找
    dictEntry *de = dictFind(db->dict,key->ptr);
    if (de) {
        // 取出 value
        robj *val = dictGetVal(de);

        /* Update the access time for the ageing algorithm.
         * Don't do it if we have a saving child, as this will trigger
         * a copy on write madness. */
        // TODO 对于更新访问时间信息（仅在不存在子进程时执行，防止破坏 copy-on-write 机制）
        if (server.rdb_child_pid == -1 &&
            server.aof_child_pid == -1 &&
            !(flags & LOOKUP_NOTOUCH))
        {
            if (server.maxmemory_policy & MAXMEMORY_FLAG_LFU) {
                updateLFU(val);
            } else {
                val->lru = LRU_CLOCK();
            }
        }
        // 返回 value
        return val;
    } else {
        // 没有找到
        return NULL;
    }
}

/* Lookup a key for read operations, or return NULL if the key is not found
 * in the specified DB.
 *
 * As a side effect of calling this function:
 * 1. A key gets expired if it reached it's TTL.
 * 2. The key last access time is updated.
 * 3. The global keys hits/misses stats are updated (reported in INFO).
 *
 * This API should not be used when we write to the key after obtaining
 * the object linked to the key, but only for read only operations.
 *
 * Flags change the behavior of this command:
 *
 *  LOOKUP_NONE (or zero): no special flags are passed.
 *  LOOKUP_NOTOUCH: don't alter the last access time of the key.
 *
 * Note: this function also returns NULL if the key is logically expired
 * but still existing, in case this is a slave, since this API is called only
 * for read operations. Even if the key expiry is master-driven, we can
 * correctly report a key is expired on slaves even if the master is lagging
 * expiring our key via DELs in the replication link. */
// 查找 read 操作的键，如果在指定的 DB 中找不到键，返回 NULL
// 影响：
// 1.如果 key 到达 TTL， key 会过期
// 2. key 上次访问时间已更新
// 3.全局 key 命中/未命中统计信息更新（在 INFO 中报告）
// 该 API 仅针对于读操作
// flag: LOOKUP_NONE: 无 flag ,LOOKUP_NOTOUCH: 不要改变 key 的访问时间
// 即使这个 key 还存在 DB 中，但是已经过期，会返回 NULL，防止这是一个子服务器，
robj *lookupKeyReadWithFlags(redisDb *db, robj *key, int flags) {
    robj *val;
    // 检查键是否过期
    if (expireIfNeeded(db,key) == 1) {
        /* Key expired. If we are in the context of a master, expireIfNeeded()
         * returns 0 only when the key does not exist at all, so it's safe
         * to return NULL ASAP. */
        // 键过期返回 NULL
        if (server.masterhost == NULL) {
            // 未命中计数 ++ 
            server.stat_keyspace_misses++;
            return NULL;
        }
        // TODO
        /* However if we are in the context of a slave, expireIfNeeded() will
         * not really try to expire the key, it only returns information
         * about the "logical" status of the key: key expiring is up to the
         * master in order to have a consistent view of master's data set.
         *
         * However, if the command caller is not the master, and as additional
         * safety measure, the command invoked is a read-only command, we can
         * safely return NULL here, and provide a more consistent behavior
         * to clients accessign expired values in a read-only fashion, that
         * will say the key as non existing.
         *
         * Notably this covers GETs when slaves are used to scale reads. */
        if (server.current_client &&
            server.current_client != server.master &&
            server.current_client->cmd &&
            server.current_client->cmd->flags & CMD_READONLY)
        {
            server.stat_keyspace_misses++;
            return NULL;
        }
    }
    // 查找键，找到与否更新命中/未命中计数
    val = lookupKey(db,key,flags);
    if (val == NULL)
        server.stat_keyspace_misses++;
    else
        server.stat_keyspace_hits++;
    return val;
}

/* Like lookupKeyReadWithFlags(), but does not use any flag, which is the
 * common case. */
// 没有使用任何 flag，平常情况
robj *lookupKeyRead(redisDb *db, robj *key) {
    return lookupKeyReadWithFlags(db,key,LOOKUP_NONE);
}

/* Lookup a key for write operations, and as a side effect, if needed, expires
 * the key if its TTL is reached.
 *
 * Returns the linked value object if the key exists or NULL if the key
 * does not exist in the specified DB. */
// 先更新一下这个键的时间，然后返回
robj *lookupKeyWrite(redisDb *db, robj *key) {
    // 检查键是否过期，删除过期键
    expireIfNeeded(db,key);
    // 查找并返回 key 的值对象
    return lookupKey(db,key,LOOKUP_NONE);
}

// 获取客户端 c 访问数据库的 key 进行读操作，如果找到则 replay 客户端
robj *lookupKeyReadOrReply(client *c, rob *key, robj *reply) {
    robj *o = lookupKeyRead(c->db, key);
    if (!o) addReply(c,reply);
    return o;
}

// 获取客户端 c 访问数据库的 key 进行写操作，如果找到则 replay 客户端
robj *lookupKeyWriteOrReply(client *c, robj *key, robj *reply) {
    robj *o = lookupKeyWrite(c->db, key);
    if (!o) addReply(c,reply);
    return o;
}

/* Add the key to the DB. It's up to the caller to increment the reference
 * counter of the value if needed.
 *
 * The program is aborted if the key already exists. */
// 添加 key-value
void dbAdd(redisDb *db, robj *key, robj *val) {
    // 包装成 sds 对象
    sds copy = sdsdup(key->ptr);
    // 先添加到数据库的 DB 中
    int retval = dictAdd(db->dict, copy, val);
    // 如果已经存在，则停止
    serverAssertWithInfo(NULL,key,retval == DICT_OK);
    if (val->type == OBJ_LIST ||
        val->type == OBJ_ZSET)
        signalKeyAsReady(db, key);
    // 开启了集群模式，将键保存到槽里面
    if (server.cluster_enabled) slotToKeyAdd(key);
}

/* Overwrite an existing key with a new value. Incrementing the reference
 * count of the new value is up to the caller.
 * This function does not modify the expire time of the existing key.
 *
 * The program is aborted if the key was not already present. */
// 重写新的值，调用者为新值增加引用计数，不会修改键的过期时间，不存在返回
void dbOverwrite(redisDb *db, robj *key, robj *val) {
    // 先查找键值对
    dictEntry *de = dictFind(db->dict,key->ptr);

    serverAssertWithInfo(NULL,key,de != NULL);
    dictEntry auxentry = *de;
    // 获取旧值
    robj *old = dictGetVal(de);
    // 如果使用 LFU 算法，则把新值的引用计数设置成旧值的引用计数
    if (server.maxmemory_policy & MAXMEMORY_FLAG_LFU) {
        val->lru = old->lru;
    }
    // 设置新值
    dictSetVal(db->dict, de, val);

    if (server.lazyfree_lazy_server_del) {
        freeObjAsync(old);
        dictSetVal(db->dict, &auxentry, NULL);
    }

    dictFreeVal(db->dict, &auxentry);
}

/* High level Set operation. This function can be used in order to set
 * a key, whatever it was existing or not, to a new object.
 *
 * 1) The ref count of the value object is incremented.
 * 2) clients WATCHing for the destination key notified.
 * 3) The expire time of the key is reset (the key is made persistent).
 *
 * All the new keys in the database should be created via this interface. */
// 高层次的 set 操作函数
// 不管 key 是否存在的情况下，将其和 value 关联起来
// 1，值对象的引用计数增加
// 2，监视键 key 的客户端会收到键修改通知
// 3，键的过期时间会被移除
void setKey(redisDb *db, robj *key, robj *val) {
    // 添加或者覆写数据库中的键值对
    if (lookupKeyWrite(db,key) == NULL) {
        // 增加
        dbAdd(db,key,val);
    } else {
        // 覆写
        dbOverwrite(db,key,val);
    }
    incrRefCount(val);
    // 移除键的过期时间
    removeExpire(db,key);
    // 发送键修改通知
    signalModifiedKey(db,key);
}

// 判断 key 是否存在于数据库之中，存在返回 1
int dbExists(redisDb *db, robj *key) {
    return dictFind(db->dict,key->ptr) != NULL;
}

/* Return a random key, in form of a Redis object.
 * If there are no keys, NULL is returned.
 *
 * The function makes sure to return keys not already expired. */
// 随机从数据库中取出一个键，并用字符串对象的方式返回这个键，如果数据库为空，返回 NULL
// 这个函数保证被返回的键都是未过期的
robj *dbRandomKey(redisDb *db) {
    dictEntry *de;
    int maxtries = 100;
    int allvolatile = dictSize(db->dict) == dictSize(db->expires);

    while(1) {
        sds key;
        robj *keyobj;
        // 从键空间中随机取出一个键节点
        de = dictGetRandomKey(db->dict);
        if (de == NULL) return NULL;
        // 取出键
        key = dictGetKey(de);
        // 为键创建一个字符串对象，对象的值为键的名字
        keyobj = createStringObject(key,sdslen(key));
        // 检查键是否有过期时间
        if (dictFind(db->expires,key)) {
            // 如果键已经过期，将其删除
            if (allvolatile && server.masterhost && --maxtries == 0) {
                /* If the DB is composed only of keys with an expire set,
                 * it could happen that all the keys are already logically
                 * expired in the slave, so the function cannot stop because
                 * expireIfNeeded() is false, nor it can stop because
                 * dictGetRandomKey() returns NULL (there are keys to return).
                 * To prevent the infinite loop we do some tries, but if there
                 * are the conditions for an infinite loop, eventually we
                 * return a key name that may be already expired. */
                // 防止无限循环，最多循环 100 次，可能返回一个已经过期的值
                return keyobj;
            }
            // 检测键是否过期，过期就删除键
            if (expireIfNeeded(db,keyobj)) {
                // 递减引用计数
                decrRefCount(keyobj);
                continue; /* 查找下一个键 search for another key. This expired. */
            }
        }
        // 返回随机到的键的 key_name
        return keyobj;
    }
}

/* Delete a key, value, and associated expiration entry if any, from the DB */
// 同步删除键值对
int dbSyncDelete(redisDb *db, robj *key) {
    /* Deleting an entry from the expires dict will not free the sds of
     * the key, because it is shared with the main dictionary. */
    // 先删除过期时间
    if (dictSize(db->expires) > 0) dictDelete(db->expires,key->ptr);
    // 删除键值对
    if (dictDelete(db->dict,key->ptr) == DICT_OK) {
        // 开启了集群模式，从槽中删除给定的键
        if (server.cluster_enabled) slotToKeyDel(key);
        return 1;
    } else {
        // 不存在
        return 0;
    }
}

/* This is a wrapper whose behavior depends on the Redis lazy free
 * configuration. Deletes the key synchronously or asynchronously. */
// 从数据库中删除给定的键值对以及过期时间，成功返回 1
// lazyfree_lazy_server_del：是否异步执行删除操作
// 根据删除策略，使用评估后交给后台线程删除或者直接删除
int dbDelete(redisDb *db, robj *key) {
    return server.lazyfree_lazy_server_del ? dbAsyncDelete(db,key) :
                                             dbSyncDelete(db,key);
}

/* Prepare the string object stored at 'key' to be modified destructively
 * to implement commands like SETBIT or APPEND.
 *
 * An object is usually ready to be modified unless one of the two conditions
 * are true:
 *
 * 1) The object 'o' is shared (refcount > 1), we don't want to affect
 *    other users.
 * 2) The object encoding is not "RAW".
 *
 * If the object is found in one of the above conditions (or both) by the
 * function, an unshared / not-encoded copy of the string object is stored
 * at 'key' in the specified 'db'. Otherwise the object 'o' itself is
 * returned.
 *
 * USAGE:
 *
 * The object 'o' is what the caller already obtained by looking up 'key'
 * in 'db', the usage pattern looks like this:
 *
 * o = lookupKeyWrite(db,key);
 * if (checkType(c,o,OBJ_STRING)) return;
 * o = dbUnshareStringValue(db,key,o);
 *
 * At this point the caller is ready to modify the object, for example
 * using an sdscat() call to append some data, or anything else.
 */
robj *dbUnshareStringValue(redisDb *db, robj *key, robj *o) {
    serverAssert(o->type == OBJ_STRING);
    if (o->refcount != 1 || o->encoding != OBJ_ENCODING_RAW) {
        robj *decoded = getDecodedObject(o);
        o = createRawStringObject(decoded->ptr, sdslen(decoded->ptr));
        decrRefCount(decoded);
        dbOverwrite(db,key,o);
    }
    return o;
}

/* Remove all keys from all the databases in a Redis server.
 * If callback is given the function is called from time to time to
 * signal that work is in progress.
 *
 * The dbnum can be -1 if all the DBs should be flushed, or the specified
 * DB number if we want to flush only a single Redis database number.
 *
 * Flags are be EMPTYDB_NO_FLAGS if no special flags are specified or
 * EMPTYDB_ASYNC if we want the memory to be freed in a different thread
 * and the function to return ASAP.
 *
 * On success the fuction returns the number of keys removed from the
 * database(s). Otherwise -1 is returned in the specific case the
 * DB number is out of range, and errno is set to EINVAL. */
// 清空服务器所有数据
long long emptyDb(int dbnum, int flags, void(callback)(void*)) {
    // 同步删除？
    int async = (flags & EMPTYDB_ASYNC);
    // remove 键计数
    long long removed = 0;
    // -1 清空所有数据库，或者是想要删除数据库的 number
    if (dbnum < -1 || dbnum >= server.dbnum) {
        errno = EINVAL;
        return -1;
    }

    int startdb, enddb;
    // 确定删除数据库范围，-1 删除所有数据库
    if (dbnum == -1) {
        startdb = 0;
        enddb = server.dbnum-1;
    } else {
        // 删除一个指定数据库
        startdb = enddb = dbnum;
    }
    // 开始清空
    for (int j = startdb; j <= enddb; j++) {
        // 删除键值对数量
        removed += dictSize(server.db[j].dict);
        if (async) {
            // 异步删除
            emptyDbAsync(&server.db[j]);
        } else {
            // 同步删除过期时间以及数据表
            dictEmpty(server.db[j].dict,callback);
            dictEmpty(server.db[j].expires,callback);
        }
    }
    // 集群情况，移除槽记录
    if (server.cluster_enabled) {
        if (async) {
            slotToKeyFlushAsync();
        } else {
            slotToKeyFlush();
        }
    }
    if (dbnum == -1) flushSlaveKeysWithExpireList();
    return removed;
}

// 将客户端的目标数据库切换为 id 所指定的数据库
int selectDb(client *c, int id) {
    // id 正确
    if (id < 0 || id >= server.dbnum)
        return C_ERR;
        // 简单粗暴，直接设置指针引用即可
    c->db = &server.db[id];
    return C_OK;
}

/*-----------------------------------------------------------------------------
 * Hooks for key space changes.
 *
 * Every time a key in the database is modified the function
 * signalModifiedKey() is called.
 *
 * Every time a DB is flushed the function signalFlushDb() is called.
 *----------------------------------------------------------------------------*/
// 每次键空间的值进行改变，均会被调用

void signalModifiedKey(redisDb *db, robj *key) {
    touchWatchedKey(db,key);
}
// 每当一个数据库被清空，会被调用
void signalFlushedDb(int dbid) {
    touchWatchedKeysOnFlush(dbid);
}

/*-----------------------------------------------------------------------------
 * Type agnostic commands operating on the key space
 *----------------------------------------------------------------------------*/

/* Return the set of flags to use for the emptyDb() call for FLUSHALL
 * and FLUSHDB commands.
 *
 * Currently the command just attempts to parse the "ASYNC" option. It
 * also checks if the command arity is wrong.
 *
 * On success C_OK is returned and the flags are stored in *flags, otherwise
 * C_ERR is returned and the function sends an error to the client. */
// 返回 FLUSHALL，FLUSHDB 指令设置的 flag，失败会向客户端报错
int getFlushCommandFlags(client *c, int *flags) {
    /* Parse the optional ASYNC option. */
    if (c->argc > 1) {
        if (c->argc > 2 || strcasecmp(c->argv[1]->ptr,"async")) {
            addReply(c,shared.syntaxerr);
            return C_ERR;
        }
        // 同步清空数据库
        *flags = EMPTYDB_ASYNC;
    } else {
        *flags = EMPTYDB_NO_FLAGS;
    }
    return C_OK;
}

/* FLUSHDB [ASYNC]
 *
 * Flushes the currently SELECTed Redis DB. */
// 清空当前的 DB
void flushdbCommand(client *c) {
    int flags;

    if (getFlushCommandFlags(c,&flags) == C_ERR) return;
    // 通知消息
    signalFlushedDb(c->db->id);
    // 删除数据库
    server.dirty += emptyDb(c->db->id,flags,NULL);
    addReply(c,shared.ok);
}

/* FLUSHALL [ASYNC]
 *
 * Flushes the whole server data set. */
// 清空整个 server 的数据库
void flushallCommand(client *c) {
    int flags;

    if (getFlushCommandFlags(c,&flags) == C_ERR) return;
    signalFlushedDb(-1);
    // 清空所有数据库
    server.dirty += emptyDb(-1,flags,NULL);
    addReply(c,shared.ok);
    // 如果正在保存新的 RDB，取消保存操作
    if (server.rdb_child_pid != -1) {
        kill(server.rdb_child_pid,SIGUSR1);
        rdbRemoveTempFile(server.rdb_child_pid);
    }
    // 更新 RDB 文件
    if (server.saveparamslen > 0) {
        /* Normally rdbSave() will reset dirty, but we don't want this here
         * as otherwise FLUSHALL will not be replicated nor put into the AOF. */
        // rdbSave() 会清空服务器的 dirty 属性
        // 但为了确保 FLUSHALL 命令会被正常传播，
        // 程序需要保存并在 rdbSave() 调用之后还原服务器的 dirty 属性
        int saved_dirty = server.dirty;
        rdbSaveInfo rsi, *rsiptr;
        rsiptr = rdbPopulateSaveInfo(&rsi);
        rdbSave(server.rdb_filename,rsiptr);
        server.dirty = saved_dirty;
    }
    server.dirty++;
}

/* This command implements DEL and LAZYDEL. */
void delGenericCommand(client *c, int lazy) {
    int numdel = 0, j;

    for (j = 1; j < c->argc; j++) {
        expireIfNeeded(c->db,c->argv[j]);
        int deleted  = lazy ? dbAsyncDelete(c->db,c->argv[j]) :
                              dbSyncDelete(c->db,c->argv[j]);
        if (deleted) {
            // 删除成功，发送通知
            signalModifiedKey(c->db,c->argv[j]);
            notifyKeyspaceEvent(NOTIFY_GENERIC,
                "del",c->argv[j],c->db->id);
            server.dirty++;
            numdel++;
        }
    }
    // 返回删除键数量
    addReplyLongLong(c,numdel);
}

// 两种删除方式，一种同步删除，一种异步删除
void delCommand(client *c) {
    delGenericCommand(c,0);
}

void unlinkCommand(client *c) {
    delGenericCommand(c,1);
}

/* EXISTS key1 key2 ... key_N.
 * Return value is the number of keys existing. */
// 判断多个 key_name 存在的个数
void existsCommand(client *c) {
    long long count = 0;
    int j;
    // 开始遍历
    for (j = 1; j < c->argc; j++) {
        if (lookupKeyRead(c->db,c->argv[j])) count++;
    }
    addReplyLongLong(c,count);
}

void selectCommand(client *c) {
    long id;
    // 错误数据库 number
    if (getLongFromObjectOrReply(c, c->argv[1], &id,
        "invalid DB index") != C_OK)
        return;

    if (server.cluster_enabled && id != 0) {
        addReplyError(c,"SELECT is not allowed in cluster mode");
        return;
    }
    // 切换数据库
    if (selectDb(c,id) == C_ERR) {
        addReplyError(c,"DB index is out of range");
    } else {
        addReply(c,shared.ok);
    }
}

// 返回随机键
void randomkeyCommand(client *c) {
    robj *key;

    if ((key = dbRandomKey(c->db)) == NULL) {
        addReplyNull(c);
        return;
    }

    addReplyBulk(c,key);
    decrRefCount(key);
}

void keysCommand(client *c) {
    dictIterator *di;
    dictEntry *de;
    // 模式
    sds pattern = c->argv[1]->ptr;
    int plen = sdslen(pattern), allkeys;
    unsigned long numkeys = 0;
    void *replylen = addReplyDeferredLen(c);
    // 遍历整个数据库，返回（名字）和模式匹配的键
    di = dictGetSafeIterator(c->db->dict);
    allkeys = (pattern[0] == '*' && pattern[1] == '\0');
    while((de = dictNext(di)) != NULL) {
        sds key = dictGetKey(de);
        robj *keyobj;
        // 将 key_name 和模式进行比对
        if (allkeys || stringmatchlen(pattern,plen,key,sdslen(key),0)) {
            // 创建一个保存键名字的字符串对象
            keyobj = createStringObject(key,sdslen(key));
            // 删除已有过期键
            if (!keyIsExpired(c->db,keyobj)) {
                addReplyBulk(c,keyobj);
                numkeys++;
            }
            decrRefCount(keyobj);
        }
    }
    dictReleaseIterator(di);
    setDeferredArrayLen(c,replylen,numkeys);
}

/* This callback is used by scanGenericCommand in order to collect elements
 * returned by the dictionary iterator into a list. */
void scanCallback(void *privdata, const dictEntry *de) {
    void **pd = (void**) privdata;
    list *keys = pd[0];
    robj *o = pd[1];
    robj *key, *val = NULL;

    if (o == NULL) {
        sds sdskey = dictGetKey(de);
        key = createStringObject(sdskey, sdslen(sdskey));
    } else if (o->type == OBJ_SET) {
        sds keysds = dictGetKey(de);
        key = createStringObject(keysds,sdslen(keysds));
    } else if (o->type == OBJ_HASH) {
        sds sdskey = dictGetKey(de);
        sds sdsval = dictGetVal(de);
        key = createStringObject(sdskey,sdslen(sdskey));
        val = createStringObject(sdsval,sdslen(sdsval));
    } else if (o->type == OBJ_ZSET) {
        sds sdskey = dictGetKey(de);
        key = createStringObject(sdskey,sdslen(sdskey));
        val = createStringObjectFromLongDouble(*(double*)dictGetVal(de),0);
    } else {
        serverPanic("Type not handled in SCAN callback.");
    }

    listAddNodeTail(keys, key);
    if (val) listAddNodeTail(keys, val);
}

/* Try to parse a SCAN cursor stored at object 'o':
 * if the cursor is valid, store it as unsigned integer into *cursor and
 * returns C_OK. Otherwise return C_ERR and send an error to the
 * client. */
int parseScanCursorOrReply(client *c, robj *o, unsigned long *cursor) {
    char *eptr;

    /* Use strtoul() because we need an *unsigned* long, so
     * getLongLongFromObject() does not cover the whole cursor space. */
    errno = 0;
    *cursor = strtoul(o->ptr, &eptr, 10);
    if (isspace(((char*)o->ptr)[0]) || eptr[0] != '\0' || errno == ERANGE)
    {
        addReplyError(c, "invalid cursor");
        return C_ERR;
    }
    return C_OK;
}

/* This command implements SCAN, HSCAN and SSCAN commands.
 * If object 'o' is passed, then it must be a Hash or Set object, otherwise
 * if 'o' is NULL the command will operate on the dictionary associated with
 * the current database.
 *
 * When 'o' is not NULL the function assumes that the first argument in
 * the client arguments vector is a key so it skips it before iterating
 * in order to parse options.
 *
 * In the case of a Hash object the function returns both the field and value
 * of every element on the Hash. */
// SCAN，HSCAN，SSCAN 的实现函数，给定了对象 o，必须是一个 set 或者 dict 结构，如果为空，函数使用当前数据库作为迭代操作
// 如果 o 不为空，则它是一个键对象，函数会跳过这些键对象
// 被迭代的是 dict，返回键值对
void scanGenericCommand(client *c, robj *o, unsigned long cursor) {
    int i, j;
    list *keys = listCreate();
    listNode *node, *nextnode;
    long count = 10;
    sds pat = NULL;
    int patlen = 0, use_pattern = 0;
    dict *ht;

    /* Object must be NULL (to iterate keys names), or the type of the object
     * must be Set, Sorted Set, or Hash. */
    // 输入类型检查
    serverAssert(o == NULL || o->type == OBJ_SET || o->type == OBJ_HASH ||
                o->type == OBJ_ZSET);

    /* Set i to the first option argument. The previous one is the cursor. */
    i = (o == NULL) ? 2 : 3; /* Skip the key argument if needed. */

    /* Step 1: Parse options. */
    // 分析参数
    while (i < c->argc) {
        j = c->argc - i;
        if (!strcasecmp(c->argv[i]->ptr, "count") && j >= 2) {
            if (getLongFromObjectOrReply(c, c->argv[i+1], &count, NULL)
                != C_OK)
            {
                goto cleanup;
            }

            if (count < 1) {
                addReply(c,shared.syntaxerr);
                goto cleanup;
            }

            i += 2;
        } else if (!strcasecmp(c->argv[i]->ptr, "match") && j >= 2) {
            pat = c->argv[i+1]->ptr;
            patlen = sdslen(pat);

            /* The pattern always matches if it is exactly "*", so it is
             * equivalent to disabling it. */
            use_pattern = !(pat[0] == '*' && patlen == 1);

            i += 2;
        } else {
            addReply(c,shared.syntaxerr);
            goto cleanup;
        }
    }

    /* Step 2: Iterate the collection.
     *
     * Note that if the object is encoded with a ziplist, intset, or any other
     * representation that is not a hash table, we are sure that it is also
     * composed of a small number of elements. So to avoid taking state we
     * just return everything inside the object in a single call, setting the
     * cursor to zero to signal the end of the iteration. */

    /* Handle the case of a hash table. */
    ht = NULL;
    if (o == NULL) {
        ht = c->db->dict;
    } else if (o->type == OBJ_SET && o->encoding == OBJ_ENCODING_HT) {
        ht = o->ptr;
    } else if (o->type == OBJ_HASH && o->encoding == OBJ_ENCODING_HT) {
        ht = o->ptr;
        count *= 2; /* We return key / value for this type. */
    } else if (o->type == OBJ_ZSET && o->encoding == OBJ_ENCODING_SKIPLIST) {
        zset *zs = o->ptr;
        ht = zs->dict;
        count *= 2; /* We return key / value for this type. */
    }

    if (ht) {
        void *privdata[2];
        /* We set the max number of iterations to ten times the specified
         * COUNT, so if the hash table is in a pathological state (very
         * sparsely populated) we avoid to block too much time at the cost
         * of returning no or very few elements. */
        long maxiterations = count*10;

        /* We pass two pointers to the callback: the list to which it will
         * add new elements, and the object containing the dictionary so that
         * it is possible to fetch more data in a type-dependent way. */
        privdata[0] = keys;
        privdata[1] = o;
        do {
            cursor = dictScan(ht, cursor, scanCallback, NULL, privdata);
        } while (cursor &&
              maxiterations-- &&
              listLength(keys) < (unsigned long)count);
    } else if (o->type == OBJ_SET) {
        int pos = 0;
        int64_t ll;

        while(intsetGet(o->ptr,pos++,&ll))
            listAddNodeTail(keys,createStringObjectFromLongLong(ll));
        cursor = 0;
    } else if (o->type == OBJ_HASH || o->type == OBJ_ZSET) {
        unsigned char *p = ziplistIndex(o->ptr,0);
        unsigned char *vstr;
        unsigned int vlen;
        long long vll;

        while(p) {
            ziplistGet(p,&vstr,&vlen,&vll);
            listAddNodeTail(keys,
                (vstr != NULL) ? createStringObject((char*)vstr,vlen) :
                                 createStringObjectFromLongLong(vll));
            p = ziplistNext(o->ptr,p);
        }
        cursor = 0;
    } else {
        serverPanic("Not handled encoding in SCAN.");
    }

    /* Step 3: Filter elements. */
    node = listFirst(keys);
    while (node) {
        robj *kobj = listNodeValue(node);
        nextnode = listNextNode(node);
        int filter = 0;

        /* Filter element if it does not match the pattern. */
        if (!filter && use_pattern) {
            if (sdsEncodedObject(kobj)) {
                if (!stringmatchlen(pat, patlen, kobj->ptr, sdslen(kobj->ptr), 0))
                    filter = 1;
            } else {
                char buf[LONG_STR_SIZE];
                int len;

                serverAssert(kobj->encoding == OBJ_ENCODING_INT);
                len = ll2string(buf,sizeof(buf),(long)kobj->ptr);
                if (!stringmatchlen(pat, patlen, buf, len, 0)) filter = 1;
            }
        }

        /* Filter element if it is an expired key. */
        if (!filter && o == NULL && expireIfNeeded(c->db, kobj)) filter = 1;

        /* Remove the element and its associted value if needed. */
        if (filter) {
            decrRefCount(kobj);
            listDelNode(keys, node);
        }

        /* If this is a hash or a sorted set, we have a flat list of
         * key-value elements, so if this element was filtered, remove the
         * value, or skip it if it was not filtered: we only match keys. */
        if (o && (o->type == OBJ_ZSET || o->type == OBJ_HASH)) {
            node = nextnode;
            nextnode = listNextNode(node);
            if (filter) {
                kobj = listNodeValue(node);
                decrRefCount(kobj);
                listDelNode(keys, node);
            }
        }
        node = nextnode;
    }

    /* Step 4: Reply to the client. */
    addReplyArrayLen(c, 2);
    addReplyBulkLongLong(c,cursor);

    addReplyArrayLen(c, listLength(keys));
    while ((node = listFirst(keys)) != NULL) {
        robj *kobj = listNodeValue(node);
        addReplyBulk(c, kobj);
        decrRefCount(kobj);
        listDelNode(keys, node);
    }

cleanup:
    listSetFreeMethod(keys,decrRefCountVoid);
    listRelease(keys);
}

/* The SCAN command completely relies on scanGenericCommand. */
// 执行 SCAN 类型命令
void scanCommand(client *c) {
    unsigned long cursor;
    if (parseScanCursorOrReply(c,c->argv[1],&cursor) == C_ERR) return;
    scanGenericCommand(c,NULL,cursor);
}

// 获取 DB 的 size
void dbsizeCommand(client *c) {
    addReplyLongLong(c,dictSize(c->db->dict));
}

// 最后添加键值的时间
void lastsaveCommand(client *c) {
    addReplyLongLong(c,server.lastsave);
}

// 获取命令对应键的类型，通知客户端
void typeCommand(client *c) {
    robj *o;
    char *type;

    o = lookupKeyReadWithFlags(c->db,c->argv[1],LOOKUP_NOTOUCH);
    if (o == NULL) {
        type = "none";
    } else {
        switch(o->type) {
        case OBJ_STRING: type = "string"; break;
        case OBJ_LIST: type = "list"; break;
        case OBJ_SET: type = "set"; break;
        case OBJ_ZSET: type = "zset"; break;
        case OBJ_HASH: type = "hash"; break;
        case OBJ_STREAM: type = "stream"; break;
        case OBJ_MODULE: {
            moduleValue *mv = o->ptr;
            type = mv->type->name;
        }; break;
        default: type = "unknown"; break;
        }
    }
    addReplyStatus(c,type);
}

// 关闭 redis-server
void shutdownCommand(client *c) {
    int flags = 0;
    if (c->argc > 2) {
        // 命令不正确
        addReply(c,shared.syntaxerr);
        return;
    } else if (c->argc == 2) {
        // 关闭时是否保存数据
        if (!strcasecmp(c->argv[1]->ptr,"nosave")) {
            flags |= SHUTDOWN_NOSAVE;
        } else if (!strcasecmp(c->argv[1]->ptr,"save")) {
            flags |= SHUTDOWN_SAVE;
        } else {
            addReply(c,shared.syntaxerr);
            return;
        }
    }
    /* When SHUTDOWN is called while the server is loading a dataset in
     * memory we need to make sure no attempt is performed to save
     * the dataset on shutdown (otherwise it could overwrite the current DB
     * with half-read data).
     *
     * Also when in Sentinel mode clear the SAVE flag and force NOSAVE. */
    // 当服务器在内存中加载数据集时调用了 SHUTDOWN,我们需要确保不会在关闭时保存数据集（会使用半读数据覆盖当前数据库）
    // 在 sentinel 模式下，清楚 SAVE 标志，强行不保存
    if (server.loading || server.sentinel_mode)
        flags = (flags & ~SHUTDOWN_SAVE) | SHUTDOWN_NOSAVE;
    // 关闭后调用 exit(0)
    if (prepareForShutdown(flags) == C_OK) exit(0);
    addReplyError(c,"Errors trying to SHUTDOWN. Check logs.");
}

void renameGenericCommand(client *c, int nx) {
    robj *o;
    long long expire;
    int samekey = 0;

    /* When source and dest key is the same, no operation is performed,
     * if the key exists, however we still return an error on unexisting key. */
    // 相同名字
    if (sdscmp(c->argv[1]->ptr,c->argv[2]->ptr) == 0) samekey = 1;
    // 查找键，不存在直接返回
    if ((o = lookupKeyWriteOrReply(c,c->argv[1],shared.nokeyerr)) == NULL)
        return;
    // 存在并且不改名，返回 0 or ok
    if (samekey) {
        addReply(c,nx ? shared.czero : shared.ok);
        return;
    }
    // 增加引用计数
    incrRefCount(o);
    // 取出来源键的过期时间
    expire = getExpire(c->db,c->argv[1]);
    // 检查目标键是否存在
    if (lookupKeyWrite(c->db,c->argv[2]) != NULL) {
        // 如果目标键存在，并且执行的是 RENAMENX ，那么直接返回
        if (nx) {
            decrRefCount(o);
            addReply(c,shared.czero);
            return;
        }
        /* Overwrite: delete the old key before creating the new one
         * with the same name. */
        // 执行的时 RENAME 删除目标键之后进行改名
        dbDelete(c->db,c->argv[2]);
    }
    // 增加目标键
    dbAdd(c->db,c->argv[2],o);
    // 存在过期时间，添加过期时间
    if (expire != -1) setExpire(c,c->db,c->argv[2],expire);
    // 删除来源键
    dbDelete(c->db,c->argv[1]);
    // 发送通知
    signalModifiedKey(c->db,c->argv[1]);
    signalModifiedKey(c->db,c->argv[2]);
    notifyKeyspaceEvent(NOTIFY_GENERIC,"rename_from",
        c->argv[1],c->db->id);
    notifyKeyspaceEvent(NOTIFY_GENERIC,"rename_to",
        c->argv[2],c->db->id);
    server.dirty++;
    addReply(c,nx ? shared.cone : shared.ok);
}

// 更改键名
void renameCommand(client *c) {
    renameGenericCommand(c,0);
}

// renamenx
void renamenxCommand(client *c) {
    renameGenericCommand(c,1);
}

void moveCommand(client *c) {
    robj *o;
    redisDb *src, *dst;
    int srcid;
    long long dbid, expire;
    // 集群不支持该操作
    if (server.cluster_enabled) {
        addReplyError(c,"MOVE is not allowed in cluster mode");
        return;
    }

    /* Obtain source and target DB pointers */
    // 源数据库
    src = c->db;
    // 源数据库 id
    srcid = c->db->id;
    // 获取目标 DB id
    if (getLongLongFromObject(c->argv[2],&dbid) == C_ERR ||
        dbid < INT_MIN || dbid > INT_MAX ||
        selectDb(c,dbid) == C_ERR)
    {
        addReply(c,shared.outofrangeerr);
        return;
    }
    // 目标数据库
    dst = c->db;
    // 回到源 DB
    selectDb(c,srcid); /* Back to the source DB */

    /* If the user is moving using as target the same
     * DB as the source DB it is probably an error. */
    // 相同数据库，比较的是数据库指针
    if (src == dst) {
        addReply(c,shared.sameobjecterr);
        return;
    }

    /* Check if the element exists and get a reference */
    // 获取要移动的元素
    o = lookupKeyWrite(c->db,c->argv[1]);
    if (!o) {
        addReply(c,shared.czero);
        return;
    }
    // 判断是否有过期时间
    expire = getExpire(c->db,c->argv[1]);

    /* Return zero if the key already exists in the target DB */
    // 如果已经在目标数据库中存在，直接返回 0
    if (lookupKeyWrite(dst,c->argv[1]) != NULL) {
        addReply(c,shared.czero);
        return;
    }
    // 添加
    dbAdd(dst,c->argv[1],o);
    // 设置过期时间
    if (expire != -1) setExpire(c,dst,c->argv[1],expire);
    incrRefCount(o);

    /* OK! key moved, free the entry in the source DB */
    // 最后再进行删除操作
    dbDelete(src,c->argv[1]);
    server.dirty++;
    addReply(c,shared.cone);
}

/* Helper function for dbSwapDatabases(): scans the list of keys that have
 * one or more blocked clients for B[LR]POP or other blocking commands
 * and signal the keys as ready if they are of the right type. See the comment
 * where the function is used for more info. */
void scanDatabaseForReadyLists(redisDb *db) {
    dictEntry *de;
    // 获取一个新的迭代器（阻塞中的键）
    dictIterator *di = dictGetSafeIterator(db->blocking_keys);
    // 遍历
    while((de = dictNext(di)) != NULL) {
        // 获取键值对
        robj *key = dictGetKey(de);
        robj *value = lookupKey(db,key,LOOKUP_NOTOUCH);
        // 这三种类型则标记为就绪
        if (value && (value->type == OBJ_LIST ||
                      value->type == OBJ_STREAM ||
                      value->type == OBJ_ZSET))
            signalKeyAsReady(db, key);
    }
    dictReleaseIterator(di);
}

/* Swap two databases at runtime so that all clients will magically see
 * the new database even if already connected. Note that the client
 * structure c->db points to a given DB, so we need to be smarter and
 * swap the underlying referenced structures, otherwise we would need
 * to fix all the references to the Redis DB structure.
 *
 * Returns C_ERR if at least one of the DB ids are out of range, otherwise
 * C_OK is returned. */
int dbSwapDatabases(int id1, int id2) {
    if (id1 < 0 || id1 >= server.dbnum ||
        id2 < 0 || id2 >= server.dbnum) return C_ERR;
    if (id1 == id2) return C_OK;
    // 先复制一个数据库，并没有复制数据，仅复制指针
    redisDb aux = server.db[id1];
    // 两个数据库
    redisDb *db1 = &server.db[id1], *db2 = &server.db[id2];

    /* Swap hash tables. Note that we don't swap blocking_keys,
     * ready_keys and watched_keys, since we want clients to
     * remain in the same DB they were. */
    // 直接交换两个数据库的各种字段
    db1->dict = db2->dict;
    db1->expires = db2->expires;
    db1->avg_ttl = db2->avg_ttl;

    db2->dict = aux.dict;
    db2->expires = aux.expires;
    db2->avg_ttl = aux.avg_ttl;

    /* Now we need to handle clients blocked on lists: as an effect
     * of swapping the two DBs, a client that was waiting for list
     * X in a given DB, may now actually be unblocked if X happens
     * to exist in the new version of the DB, after the swap.
     *
     * However normally we only do this check for efficiency reasons
     * in dbAdd() when a list is created. So here we need to rescan
     * the list of clients blocked on lists and signal lists as ready
     * if needed. */
    scanDatabaseForReadyLists(db1);
    scanDatabaseForReadyLists(db2);
    return C_OK;
}

/* SWAPDB db1 db2 */
// 交换两个数据库
void swapdbCommand(client *c) {
    long id1, id2;

    /* Not allowed in cluster mode: we have just DB 0 there. */
    if (server.cluster_enabled) {
        addReplyError(c,"SWAPDB is not allowed in cluster mode");
        return;
    }

    /* Get the two DBs indexes. */
    if (getLongFromObjectOrReply(c, c->argv[1], &id1,
        "invalid first DB index") != C_OK)
        return;

    if (getLongFromObjectOrReply(c, c->argv[2], &id2,
        "invalid second DB index") != C_OK)
        return;

    /* Swap... */
    // 交换两个数据库
    if (dbSwapDatabases(id1,id2) == C_ERR) {
        addReplyError(c,"DB index is out of range");
        return;
    } else {
        server.dirty++;
        addReply(c,shared.ok);
    }
}

/*-----------------------------------------------------------------------------
 * Expires API
 *----------------------------------------------------------------------------*/
// 删除键的过期时间，一般在 set 键之后才会删除旧键的过期时间
int removeExpire(redisDb *db, robj *key) {
    /* An expire may only be removed if there is a corresponding entry in the
     * main dict. Otherwise, the key will never be freed. */
    // 只有字典中有相应键才会删除过期时间
    serverAssertWithInfo(NULL,key,dictFind(db->dict,key->ptr) != NULL);
    return dictDelete(db->expires,key->ptr) == DICT_OK;
}

/* Set an expire to the specified key. If the expire is set in the context
 * of an user calling a command 'c' is the client, otherwise 'c' is set
 * to NULL. The 'when' parameter is the absolute unix time in milliseconds
 * after which the key will no longer be considered valid. */
// 设置过期时间
void setExpire(client *c, redisDb *db, robj *key, long long when) {
    dictEntry *kde, *de;

    /* Reuse the sds from the main dict in the expire dict */
    kde = dictFind(db->dict,key->ptr);
    serverAssertWithInfo(NULL,key,kde != NULL);
    de = dictAddOrFind(db->expires,dictGetKey(kde));
    // 设置键的过期时间
    // 用整数值来保存过期的时间
    dictSetSignedIntegerVal(de,when);

    int writable_slave = server.masterhost && server.repl_slave_ro == 0;
    if (c && writable_slave && !(c->flags & CLIENT_MASTER))
        rememberSlaveKeyWithExpire(db,key);
}

/* Return the expire time of the specified key, or -1 if no expire
 * is associated with this key (i.e. the key is non volatile) */
// 获得一个键的过期时间
long long getExpire(redisDb *db, robj *key) {
    dictEntry *de;

    /* No expire? return ASAP */
    // 没有过期
    if (dictSize(db->expires) == 0 ||
       (de = dictFind(db->expires,key->ptr)) == NULL) return -1;

    /* The entry was found in the expire dict, this means it should also
     * be present in the main dict (safety check). */
    serverAssertWithInfo(NULL,key,dictFind(db->dict,key->ptr) != NULL);
    // 获取时间戳
    return dictGetSignedIntegerVal(de);
}

/* Propagate expires into slaves and the AOF file.
 * When a key expires in the master, a DEL operation for this key is sent
 * to all the slaves and the AOF file if enabled.
 *
 * This way the key expiry is centralized in one place, and since both
 * AOF and the master->slave link guarantee operation ordering, everything
 * will be consistent even if we allow write operations against expiring
 * keys. */
// 通知过期键给附属节点和 AOF
// 当主服务器的键过期，会通知从服务器以及 AOF，传播一个 DEL 命令
// 这样可以对键的过期处理在同一位置集中执行，可以保证操作的执行顺序
// 即使是写过期键，所有数据也均是一致的
void propagateExpire(redisDb *db, robj *key, int lazy) {
    robj *argv[2];
    // 构造一个 DEL 命令
    argv[0] = lazy ? shared.unlink : shared.del;
    argv[1] = key;
    incrRefCount(argv[0]);
    incrRefCount(argv[1]);
    // 传播给 AOF
    if (server.aof_state != AOF_OFF)
        feedAppendOnlyFile(server.delCommand,db->id,argv,2);
        // 通知所有附属节点
    replicationFeedSlaves(server.slaves,db->id,argv,2);

    decrRefCount(argv[0]);
    decrRefCount(argv[1]);
}

/* Check if the key is expired. */
// 判断 key 是否过期，过期也不会进行删除
int keyIsExpired(redisDb *db, robj *key) {
    // 获取过期时间戳
    mstime_t when = getExpire(db,key);
    // 没有过期时间
    if (when < 0) return 0; /* No expire for this key */

    /* Don't expire anything while loading. It will be done later. */
    // 正在从磁盘加载数据，不会进行删除
    if (server.loading) return 0;

    /* If we are in the context of a Lua script, we pretend that time is
     * blocked to when the Lua script started. This way a key can expire
     * only the first time it is accessed and not in the middle of the
     * script execution, making propagation to slaves / AOF consistent.
     * See issue #1525 on Github for more information. */
    // 现在的时间，对 lua 脚本使用脚本中的时间
    mstime_t now = server.lua_caller ? server.lua_time_start : mstime();
    // 比较一下进行返回结果
    return now > when;
}

/* This function is called when we are going to perform some operation
 * in a given key, but such key may be already logically expired even if
 * it still exists in the database. The main way this function is called
 * is via lookupKey*() family of functions.
 *
 * The behavior of the function depends on the replication role of the
 * instance, because slave instances do not expire keys, they wait
 * for DELs from the master for consistency matters. However even
 * slaves will try to have a coherent return value for the function,
 * so that read commands executed in the slave side will be able to
 * behave like if the key is expired even if still present (because the
 * master has yet to propagate the DEL).
 *
 * In masters as a side effect of finding a key which is expired, such
 * key will be evicted from the database. Also this may trigger the
 * propagation of a DEL/UNLINK command in AOF / replication stream.
 *
 * The return value of the function is 0 if the key is still valid,
 * otherwise the function returns 1 if the key is expired. */
// 这个函数用来防止一些指令操作已经在逻辑上过期的键
// 附属节点并不考虑过期节点，他们仅接受 DEL 指令用来保持一致性
// 键过期了会进行删除并通知附属节点以及 AOF
int expireIfNeeded(redisDb *db, robj *key) {
    // 没过期返回 0
    if (!keyIsExpired(db,key)) return 0;

    /* If we are running in the context of a slave, instead of
     * evicting the expired key from the database, we return ASAP:
     * the slave key expiration is controlled by the master that will
     * send us synthesized DEL operations for expired keys.
     *
     * Still we try to return the right information to the caller,
     * that is, 0 if we think the key should be still valid, 1 if
     * we think the key is expired at this time. */
    if (server.masterhost != NULL) return 1;

    /* Delete the key */
    // 递增过期键数量
    server.stat_expiredkeys++;
    // 通知附属节点以及 AOF
    propagateExpire(db,key,server.lazyfree_lazy_expire);
    notifyKeyspaceEvent(NOTIFY_EXPIRED,
        "expired",key,db->id);
    // 删除策略
    return server.lazyfree_lazy_expire ? dbAsyncDelete(db,key) :
                                         dbSyncDelete(db,key);
}

/* -----------------------------------------------------------------------------
 * API to get key arguments from commands 从各种命令中获取 key 的参数
 * ---------------------------------------------------------------------------*/

/* The base case is to use the keys position as given in the command table
 * (firstkey, lastkey, step). */
// 获取命令中所有 key
int *getKeysUsingCommandTable(struct redisCommand *cmd,robj **argv, int argc, int *numkeys) {
    int j, i = 0, last, *keys;
    UNUSED(argv);

    if (cmd->firstkey == 0) {
        *numkeys = 0;
        return NULL;
    }

    last = cmd->lastkey;
    if (last < 0) last = argc+last;
    keys = zmalloc(sizeof(int)*((last - cmd->firstkey)+1));
    for (j = cmd->firstkey; j <= last; j += cmd->keystep) {
        if (j >= argc) {
            /* Modules commands, and standard commands with a not fixed number
             * of arguments (negative arity parameter) do not have dispatch
             * time arity checks, so we need to handle the case where the user
             * passed an invalid number of arguments here. In this case we
             * return no keys and expect the command implementation to report
             * an arity or syntax error. */
            if (cmd->flags & CMD_MODULE || cmd->arity < 0) {
                zfree(keys);
                *numkeys = 0;
                return NULL;
            } else {
                serverPanic("Redis built-in command declared keys positions not matching the arity requirements.");
            }
        }
        keys[i++] = j;
    }
    *numkeys = i;
    return keys;
}

/* Return all the arguments that are keys in the command passed via argc / argv.
 *
 * The command returns the positions of all the key arguments inside the array,
 * so the actual return value is an heap allocated array of integers. The
 * length of the array is returned by reference into *numkeys.
 *
 * 'cmd' must be point to the corresponding entry into the redisCommand
 * table, according to the command name in argv[0].
 *
 * This function uses the command table if a command-specific helper function
 * is not required, otherwise it calls the command-specific function. */
// 从命令中获取 key 添加了集群转向的支持
int *getKeysFromCommand(struct redisCommand *cmd, robj **argv, int argc, int *numkeys) {
    if (cmd->flags & CMD_MODULE_GETKEYS) {
        return moduleGetCommandKeysViaAPI(cmd,argv,argc,numkeys);
    } else if (!(cmd->flags & CMD_MODULE) && cmd->getkeys_proc) {
        return cmd->getkeys_proc(cmd,argv,argc,numkeys);
    } else {
        return getKeysUsingCommandTable(cmd,argv,argc,numkeys);
    }
}

/* Free the result of getKeysFromCommand. */
// 释放 getKeysFromCommand() 的返回值
void getKeysFreeResult(int *result) {
    zfree(result);
}

/* Helper function to extract keys from following commands:
 * ZUNIONSTORE <destkey> <num-keys> <key> <key> ... <key> <options>
 * ZINTERSTORE <destkey> <num-keys> <key> <key> ... <key> <options> */
int *zunionInterGetKeys(struct redisCommand *cmd, robj **argv, int argc, int *numkeys) {
    int i, num, *keys;
    UNUSED(cmd);

    num = atoi(argv[2]->ptr);
    /* Sanity check. Don't return any key if the command is going to
     * reply with syntax error. */
    if (num < 1 || num > (argc-3)) {
        *numkeys = 0;
        return NULL;
    }

    /* Keys in z{union,inter}store come from two places:
     * argv[1] = storage key,
     * argv[3...n] = keys to intersect */
    keys = zmalloc(sizeof(int)*(num+1));

    /* Add all key positions for argv[3...n] to keys[] */
    for (i = 0; i < num; i++) keys[i] = 3+i;

    /* Finally add the argv[1] key position (the storage key target). */
    keys[num] = 1;
    *numkeys = num+1;  /* Total keys = {union,inter} keys + storage key */
    return keys;
}

/* Helper function to extract keys from the following commands:
 * EVAL <script> <num-keys> <key> <key> ... <key> [more stuff]
 * EVALSHA <script> <num-keys> <key> <key> ... <key> [more stuff] */
int *evalGetKeys(struct redisCommand *cmd, robj **argv, int argc, int *numkeys) {
    int i, num, *keys;
    UNUSED(cmd);

    num = atoi(argv[2]->ptr);
    /* Sanity check. Don't return any key if the command is going to
     * reply with syntax error. */
    if (num <= 0 || num > (argc-3)) {
        *numkeys = 0;
        return NULL;
    }

    keys = zmalloc(sizeof(int)*num);
    *numkeys = num;

    /* Add all key positions for argv[3...n] to keys[] */
    for (i = 0; i < num; i++) keys[i] = 3+i;

    return keys;
}

/* Helper function to extract keys from the SORT command.
 *
 * SORT <sort-key> ... STORE <store-key> ...
 *
 * The first argument of SORT is always a key, however a list of options
 * follow in SQL-alike style. Here we parse just the minimum in order to
 * correctly identify keys in the "STORE" option. */
int *sortGetKeys(struct redisCommand *cmd, robj **argv, int argc, int *numkeys) {
    int i, j, num, *keys, found_store = 0;
    UNUSED(cmd);

    num = 0;
    keys = zmalloc(sizeof(int)*2); /* Alloc 2 places for the worst case. */

    keys[num++] = 1; /* <sort-key> is always present. */

    /* Search for STORE option. By default we consider options to don't
     * have arguments, so if we find an unknown option name we scan the
     * next. However there are options with 1 or 2 arguments, so we
     * provide a list here in order to skip the right number of args. */
    struct {
        char *name;
        int skip;
    } skiplist[] = {
        {"limit", 2},
        {"get", 1},
        {"by", 1},
        {NULL, 0} /* End of elements. */
    };

    for (i = 2; i < argc; i++) {
        for (j = 0; skiplist[j].name != NULL; j++) {
            if (!strcasecmp(argv[i]->ptr,skiplist[j].name)) {
                i += skiplist[j].skip;
                break;
            } else if (!strcasecmp(argv[i]->ptr,"store") && i+1 < argc) {
                /* Note: we don't increment "num" here and continue the loop
                 * to be sure to process the *last* "STORE" option if multiple
                 * ones are provided. This is same behavior as SORT. */
                found_store = 1;
                keys[num] = i+1; /* <store-key> */
                break;
            }
        }
    }
    *numkeys = num + found_store;
    return keys;
}

int *migrateGetKeys(struct redisCommand *cmd, robj **argv, int argc, int *numkeys) {
    int i, num, first, *keys;
    UNUSED(cmd);

    /* Assume the obvious form. */
    first = 3;
    num = 1;

    /* But check for the extended one with the KEYS option. */
    if (argc > 6) {
        for (i = 6; i < argc; i++) {
            if (!strcasecmp(argv[i]->ptr,"keys") &&
                sdslen(argv[3]->ptr) == 0)
            {
                first = i+1;
                num = argc-first;
                break;
            }
        }
    }

    keys = zmalloc(sizeof(int)*num);
    for (i = 0; i < num; i++) keys[i] = first+i;
    *numkeys = num;
    return keys;
}

/* Helper function to extract keys from following commands:
 * GEORADIUS key x y radius unit [WITHDIST] [WITHHASH] [WITHCOORD] [ASC|DESC]
 *                             [COUNT count] [STORE key] [STOREDIST key]
 * GEORADIUSBYMEMBER key member radius unit ... options ... */
int *georadiusGetKeys(struct redisCommand *cmd, robj **argv, int argc, int *numkeys) {
    int i, num, *keys;
    UNUSED(cmd);

    /* Check for the presence of the stored key in the command */
    int stored_key = -1;
    for (i = 5; i < argc; i++) {
        char *arg = argv[i]->ptr;
        /* For the case when user specifies both "store" and "storedist" options, the
         * second key specified would override the first key. This behavior is kept
         * the same as in georadiusCommand method.
         */
        if ((!strcasecmp(arg, "store") || !strcasecmp(arg, "storedist")) && ((i+1) < argc)) {
            stored_key = i+1;
            i++;
        }
    }
    num = 1 + (stored_key == -1 ? 0 : 1);

    /* Keys in the command come from two places:
     * argv[1] = key,
     * argv[5...n] = stored key if present
     */
    keys = zmalloc(sizeof(int) * num);

    /* Add all key positions to keys[] */
    keys[0] = 1;
    if(num > 1) {
         keys[1] = stored_key;
    }
    *numkeys = num;
    return keys;
}

/* XREAD [BLOCK <milliseconds>] [COUNT <count>] [GROUP <groupname> <ttl>]
 *       STREAMS key_1 key_2 ... key_N ID_1 ID_2 ... ID_N */
int *xreadGetKeys(struct redisCommand *cmd, robj **argv, int argc, int *numkeys) {
    int i, num = 0, *keys;
    UNUSED(cmd);

    /* We need to parse the options of the command in order to seek the first
     * "STREAMS" string which is actually the option. This is needed because
     * "STREAMS" could also be the name of the consumer group and even the
     * name of the stream key. */
    int streams_pos = -1;
    for (i = 1; i < argc; i++) {
        char *arg = argv[i]->ptr;
        if (!strcasecmp(arg, "block")) {
            i++; /* Skip option argument. */
        } else if (!strcasecmp(arg, "count")) {
            i++; /* Skip option argument. */
        } else if (!strcasecmp(arg, "group")) {
            i += 2; /* Skip option argument. */
        } else if (!strcasecmp(arg, "noack")) {
            /* Nothing to do. */
        } else if (!strcasecmp(arg, "streams")) {
            streams_pos = i;
            break;
        } else {
            break; /* Syntax error. */
        }
    }
    if (streams_pos != -1) num = argc - streams_pos - 1;

    /* Syntax error. */
    if (streams_pos == -1 || num == 0 || num % 2 != 0) {
        *numkeys = 0;
        return NULL;
    }
    num /= 2; /* We have half the keys as there are arguments because
                 there are also the IDs, one per key. */

    keys = zmalloc(sizeof(int) * num);
    for (i = streams_pos+1; i < argc-num; i++) keys[i-streams_pos-1] = i;
    *numkeys = num;
    return keys;
}

/* Slot to Key API. This is used by Redis Cluster in order to obtain in
 * a fast way a key that belongs to a specified hash slot. This is useful
 * while rehashing the cluster and in other conditions when we need to
 * understand if we have keys for a given hash slot. */
void slotToKeyUpdateKey(robj *key, int add) {
    unsigned int hashslot = keyHashSlot(key->ptr,sdslen(key->ptr));
    unsigned char buf[64];
    unsigned char *indexed = buf;
    size_t keylen = sdslen(key->ptr);

    server.cluster->slots_keys_count[hashslot] += add ? 1 : -1;
    if (keylen+2 > 64) indexed = zmalloc(keylen+2);
    indexed[0] = (hashslot >> 8) & 0xff;
    indexed[1] = hashslot & 0xff;
    memcpy(indexed+2,key->ptr,keylen);
    if (add) {
        raxInsert(server.cluster->slots_to_keys,indexed,keylen+2,NULL,NULL);
    } else {
        raxRemove(server.cluster->slots_to_keys,indexed,keylen+2,NULL);
    }
    if (indexed != buf) zfree(indexed);
}

// 将给定键添加到槽里面
void slotToKeyAdd(robj *key) {
    slotToKeyUpdateKey(key,1);
}

// 将给定键从槽里面删除
void slotToKeyDel(robj *key) {
    slotToKeyUpdateKey(key,0);
}

// 清空槽中的所有键
void slotToKeyFlush(void) {
    raxFree(server.cluster->slots_to_keys);
    server.cluster->slots_to_keys = raxNew();
    memset(server.cluster->slots_keys_count,0,
           sizeof(server.cluster->slots_keys_count));
}

/* Pupulate the specified array of objects with keys in the specified slot.
 * New objects are returned to represent keys, it's up to the caller to
 * decrement the reference count to release the keys names. */
// 从 hashslot 槽中获取 count 个 key
unsigned int getKeysInSlot(unsigned int hashslot, robj **keys, unsigned int count) {
    raxIterator iter;
    int j = 0;
    unsigned char indexed[2];

    indexed[0] = (hashslot >> 8) & 0xff;
    indexed[1] = hashslot & 0xff;
    raxStart(&iter,server.cluster->slots_to_keys);
    raxSeek(&iter,">=",indexed,2);
    while(count-- && raxNext(&iter)) {
        if (iter.key[0] != indexed[0] || iter.key[1] != indexed[1]) break;
        keys[j++] = createStringObject((char*)iter.key+2,iter.key_len-2);
    }
    raxStop(&iter);
    return j;
}

/* Remove all the keys in the specified hash slot.
 * The number of removed items is returned. */
// 删除 hashslot 槽中所有的 key
unsigned int delKeysInSlot(unsigned int hashslot) {
    raxIterator iter;
    int j = 0;
    unsigned char indexed[2];

    indexed[0] = (hashslot >> 8) & 0xff;
    indexed[1] = hashslot & 0xff;
    raxStart(&iter,server.cluster->slots_to_keys);
    while(server.cluster->slots_keys_count[hashslot]) {
        raxSeek(&iter,">=",indexed,2);
        raxNext(&iter);

        robj *key = createStringObject((char*)iter.key+2,iter.key_len-2);
        dbDelete(&server.db[0],key);
        decrRefCount(key);
        j++;
    }
    raxStop(&iter);
    return j;
}

// 返回指定 slot 包含的键数量
unsigned int countKeysInSlot(unsigned int hashslot) {
    return server.cluster->slots_keys_count[hashslot];
}
