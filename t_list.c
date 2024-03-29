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

/*-----------------------------------------------------------------------------
 * List API    list 接口操作
 *----------------------------------------------------------------------------*/

/* The function pushes an element to the specified list object 'subject',
 * at head or tail position as specified by 'where'.
 *
 * There is no need for the caller to increment the refcount of 'value' as
 * the function takes care of it if needed. */
// 实现 Push 操作
void listTypePush(robj *subject, robj *value, int where) {
    if (subject->encoding == OBJ_ENCODING_QUICKLIST) {
        // Push_head or Push_tail
        int pos = (where == LIST_HEAD) ? QUICKLIST_HEAD : QUICKLIST_TAIL;
        value = getDecodedObject(value);
        size_t len = sdslen(value->ptr);
        // 调用 quicklist 的 Push 操作
        quicklistPush(subject->ptr, value->ptr, len, pos);
        // 减少 value 的引用计数，因为会复制一份进 quicklist，毕竟底层是 ziplist
        decrRefCount(value);
    } else {
        serverPanic("Unknown list encoding");
    }
}

// 保存数据方式，这里使用创建一个 sds 来进行保存，quicklist 中的版本是直接复制一块内存返回
void *listPopSaver(unsigned char *data, unsigned int sz) {
    return createStringObject((char*)data,sz);
}

// 删除节点，实现 Pop 操作，list 使用较多
robj *listTypePop(robj *subject, int where) {
    long long vlong;
    robj *value = NULL;
    // Pop_head or Pop_tail
    int ql_where = where == LIST_HEAD ? QUICKLIST_HEAD : QUICKLIST_TAIL;
    if (subject->encoding == OBJ_ENCODING_QUICKLIST) {
        // 使用 quicklist 的 Pop 操作
        if (quicklistPopCustom(subject->ptr, ql_where, (unsigned char **)&value,
                               NULL, &vlong, listPopSaver)) {
            // 判断是 string 类型，还是 longlong 类型
            if (!value)
                value = createStringObjectFromLongLong(vlong);
        }
    } else {
        serverPanic("Unknown list encoding");
    }
    return value;
}

// 返回长度
unsigned long listTypeLength(const robj *subject) {
    if (subject->encoding == OBJ_ENCODING_QUICKLIST) {
        // 返回 list 的长度，subject 就是一个 quicklist 实现的 list
        return quicklistCount(subject->ptr);
    } else {
        serverPanic("Unknown list encoding");
    }
}

/* Initialize an iterator at the specified index. */
// 初始化一个 list 迭代器
listTypeIterator *listTypeInitIterator(robj *subject, long index,
                                       unsigned char direction) {
    // 分配空间
    listTypeIterator *li = zmalloc(sizeof(listTypeIterator));
    // 指向的对象
    li->subject = subject;
    // 对象的编码方式
    li->encoding = subject->encoding;
    // 迭代方向
    li->direction = direction;
    li->iter = NULL;
    /* LIST_HEAD means start at TAIL and move *towards* head.
     * LIST_TAIL means start at HEAD and move *towards tail. */
    int iter_direction =
        direction == LIST_HEAD ? AL_START_TAIL : AL_START_HEAD;
    if (li->encoding == OBJ_ENCODING_QUICKLIST) {
        // 直接获取 quicklist 位于 idx 处的迭代器
        li->iter = quicklistGetIteratorAtIdx(li->subject->ptr,
                                             iter_direction, index);
    } else {
        // listTypeInitIterator 只支持 quicklist 一种编码方式
        serverPanic("Unknown list encoding");
    }
    return li;
}

/* Clean up the iterator. */
// 清空迭代器
void listTypeReleaseIterator(listTypeIterator *li) {
    // 释放其中包装的 quicklist 迭代器
    zfree(li->iter);
    // 释放自己本身
    zfree(li);
}

/* Stores pointer to current the entry in the provided entry structure
 * and advances the position of the iterator. Returns 1 when the current
 * entry is in fact an entry, 0 otherwise. */
// 迭代器前进，entry 中就是下一个节点
int listTypeNext(listTypeIterator *li, listTypeEntry *entry) {
    /* Protect from converting when iterating */
    // 使用该迭代器迭代时防止类型转换
    serverAssert(li->subject->encoding == li->encoding);

    entry->li = li;
    if (li->encoding == OBJ_ENCODING_QUICKLIST) {
        // 获取 li 的下一个节点，通过 entry 获取
        return quicklistNext(li->iter, &entry->entry);
    } else {
        serverPanic("Unknown list encoding");
    }
    return 0;
}

/* Return entry or NULL at the current position of the iterator. */
// 获取元素数据
robj *listTypeGet(listTypeEntry *entry) {
    robj *value = NULL;
    if (entry->li->encoding == OBJ_ENCODING_QUICKLIST) {
        if (entry->entry.value) {
            // 获取 string 类型值
            value = createStringObject((char *)entry->entry.value,
                                       entry->entry.sz);
        } else {
            // 获取 longlong 类型值
            value = createStringObjectFromLongLong(entry->entry.longval);
        }
    } else {
        serverPanic("Unknown list encoding");
    }
    return value;
}

// 在 where 位置插入 value，insert 操作
void listTypeInsert(listTypeEntry *entry, robj *value, int where) {
    if (entry->li->encoding == OBJ_ENCODING_QUICKLIST) {
        // 对 value 进行解码
        value = getDecodedObject(value);
        sds str = value->ptr;
        size_t len = sdslen(str);
        if (where == LIST_TAIL) {
            // 在 entry 所指的 quicklist 中进行插入
            quicklistInsertAfter((quicklist *)entry->entry.quicklist,
                                 &entry->entry, str, len);
        } else if (where == LIST_HEAD) {
            quicklistInsertBefore((quicklist *)entry->entry.quicklist,
                                  &entry->entry, str, len);
        }
        // 降低 value 引用值
        decrRefCount(value);
    } else {
        serverPanic("Unknown list encoding");
    }
}

/* Compare the given object with the entry at the current position. */
// 比较 list 元素和 o
int listTypeEqual(listTypeEntry *entry, robj *o) {
    if (entry->li->encoding == OBJ_ENCODING_QUICKLIST) {
        serverAssertWithInfo(NULL,o,sdsEncodedObject(o));
        // 比较两个 ziplist，quicklistCompare() 调用的还是 ziplist 的比较函数
        return quicklistCompare(entry->entry.zi,o->ptr,sdslen(o->ptr));
    } else {
        serverPanic("Unknown list encoding");
    }
}

/* Delete the element pointed to. */
// 删除元素
void listTypeDelete(listTypeIterator *iter, listTypeEntry *entry) {
    if (entry->li->encoding == OBJ_ENCODING_QUICKLIST) {
        // 直接调用 quicklist 删除函数，一个迭代器就够了，迭代器里啥都有。。。
        quicklistDelEntry(iter->iter, &entry->entry);
    } else {
        serverPanic("Unknown list encoding");
    }
}

/* Create a quicklist from a single ziplist */
// 根据 ziplist 创建一个 quicklist，编码方式转换
void listTypeConvert(robj *subject, int enc) {
    // ziplist 编码的 list 对象
    serverAssertWithInfo(NULL,subject,subject->type==OBJ_LIST);
    serverAssertWithInfo(NULL,subject,subject->encoding==OBJ_ENCODING_ZIPLIST);

    if (enc == OBJ_ENCODING_QUICKLIST) {
        // 服务器设置的最大 ziplist 编码 list 的长度
        size_t zlen = server.list_max_ziplist_size;
        // list 的压缩深度
        int depth = server.list_compress_depth;
        // 直接创建
        subject->ptr = quicklistCreateFromZiplist(zlen, depth, subject->ptr);
        // 设置编码方式
        subject->encoding = OBJ_ENCODING_QUICKLIST;
    } else {
        serverPanic("Unsupported list conversion");
    }
}

/*-----------------------------------------------------------------------------
 * List Commands  list 操作指令
 *----------------------------------------------------------------------------*/

// 链表 push， push 操作实现
void pushGenericCommand(client *c, int where) {
    int j, pushed = 0;
    robj *lobj = lookupKeyWrite(c->db,c->argv[1]);
    // 类型检测
    if (lobj && lobj->type != OBJ_LIST) {
        addReply(c,shared.wrongtypeerr);
        return;
    }
    // 遍历所有插入数据
    for (j = 2; j < c->argc; j++) {
        if (!lobj) {
            // 空对象，初始化一个 quicklist，通过服务器值进行设置参数值
            lobj = createQuicklistObject();
            quicklistSetOptions(lobj->ptr, server.list_max_ziplist_size,
                                server.list_compress_depth);
            // 添加数据
            dbAdd(c->db,c->argv[1],lobj);
        }
        // 调用 list 的 Push
        listTypePush(lobj,c->argv[j],where);
        pushed++;
    }
    // 通知客户端新长度
    addReplyLongLong(c, (lobj ? listTypeLength(lobj) : 0));
    if (pushed) {
        // 确实 PUSH 了数据
        char *event = (where == LIST_HEAD) ? "lpush" : "rpush";
        // 向数据库发送值变更通知
        signalModifiedKey(c->db,c->argv[1]);
        // 发送事件通知
        notifyKeyspaceEvent(NOTIFY_LIST,event,c->argv[1],c->db->id);
    }
    server.dirty += pushed;
}

// LPUSH KEY_NAME VALUE1.. VALUEN，头部插入
void lpushCommand(client *c) {
    pushGenericCommand(c,LIST_HEAD);
}

// RPUSH KEY_NAME VALUE1.. VALUEN，尾部插入
void rpushCommand(client *c) {
    pushGenericCommand(c,LIST_TAIL);
}

// 非空链表 push， pushx 操作实现
// 这里和 push 几乎一样，判空一下即可
void pushxGenericCommand(client *c, int where) {
    int j, pushed = 0;
    robj *subject;

    if ((subject = lookupKeyWriteOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,subject,OBJ_LIST)) return;

    for (j = 2; j < c->argc; j++) {
        listTypePush(subject,c->argv[j],where);
        pushed++;
    }

    addReplyLongLong(c,listTypeLength(subject));

    if (pushed) {
        char *event = (where == LIST_HEAD) ? "lpush" : "rpush";
        signalModifiedKey(c->db,c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_LIST,event,c->argv[1],c->db->id);
    }
    server.dirty += pushed;
}

// LPUSHX KEY_NAME VALUE1.. VALUEN，非空链表头部插入
void lpushxCommand(client *c) {
    pushxGenericCommand(c,LIST_HEAD);
}

// RPUSHX KEY_NAME VALUE1.. VALUEN，非空链表尾部插入
void rpushxCommand(client *c) {
    pushxGenericCommand(c,LIST_TAIL);
}

// 链表插入，LINSERT key BEFORE|AFTER pivot value，insert 在 pivot 元素前后操作 value
void linsertCommand(client *c) {
    int where;
    robj *subject;
    listTypeIterator *iter;
    listTypeEntry entry;
    int inserted = 0;
    // 后方插入
    if (strcasecmp(c->argv[2]->ptr,"after") == 0) {
        where = LIST_TAIL;
    } else if (strcasecmp(c->argv[2]->ptr,"before") == 0) {
        // 前方插入
        where = LIST_HEAD;
    } else {
        // 语法错误
        addReply(c,shared.syntaxerr);
        return;
    }
    // 取数据
    if ((subject = lookupKeyWriteOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,subject,OBJ_LIST)) return;

    /* Seek pivot from head to tail */
    // 寻找插入位置
    iter = listTypeInitIterator(subject,0,LIST_TAIL);
    // 遍历寻找
    while (listTypeNext(iter,&entry)) {
        if (listTypeEqual(&entry,c->argv[3])) {
            // 找到就进行插入
            listTypeInsert(&entry,c->argv[4],where);
            inserted = 1;
            break;
        }
    }
    // 释放迭代器
    listTypeReleaseIterator(iter);
    if (inserted) {
        // 插入成功，进行通知操作
        signalModifiedKey(c->db,c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_LIST,"linsert",
                            c->argv[1],c->db->id);
        server.dirty++;
    } else {
        /* Notify client of a failed insert */
        // 插入失败，返回 -1
        addReplyLongLong(c,-1);
        return;
    }
    // 成功后告知客户端 key 的 length
    addReplyLongLong(c,listTypeLength(subject));
}

// LLEN KEY_NAME，链表长度
void llenCommand(client *c) {
    robj *o = lookupKeyReadOrReply(c,c->argv[1],shared.czero);
    if (o == NULL || checkType(c,o,OBJ_LIST)) return;
    // 回应客户端 list len
    addReplyLongLong(c,listTypeLength(o));
}

// LINDEX KEY_NAME INDEX_POSITION，通过索引获取元素
void lindexCommand(client *c) {
    robj *o = lookupKeyReadOrReply(c,c->argv[1],shared.null[c->resp]);
    if (o == NULL || checkType(c,o,OBJ_LIST)) return;
    long index;
    robj *value = NULL;
    // 获取索引
    if ((getLongFromObjectOrReply(c, c->argv[2], &index, NULL) != C_OK))
        return;

    if (o->encoding == OBJ_ENCODING_QUICKLIST) {
        quicklistEntry entry;
        // 取值
        if (quicklistIndex(o->ptr, index, &entry)) {
            if (entry.value) {
                // string 类型值
                value = createStringObject((char*)entry.value,entry.sz);
            } else {
                // longlong 类型值
                value = createStringObjectFromLongLong(entry.longval);
            }
            // 回应客户端获取的值
            addReplyBulk(c,value);
            // 已经通知客户端，减少 value 的引用计数
            decrRefCount(value);
        } else {
            // 未找到
            addReplyNull(c);
        }
    } else {
        serverPanic("Unknown list encoding");
    }
}

// LSET KEY_NAME INDEX VALUE，通过索引设置值
void lsetCommand(client *c) {
    robj *o = lookupKeyWriteOrReply(c,c->argv[1],shared.nokeyerr);
    if (o == NULL || checkType(c,o,OBJ_LIST)) return;
    long index;
    robj *value = c->argv[3];
    // 获取索引
    if ((getLongFromObjectOrReply(c, c->argv[2], &index, NULL) != C_OK))
        return;

    if (o->encoding == OBJ_ENCODING_QUICKLIST) {
        quicklist *ql = o->ptr;
        // 调用 quicklistReplaceAtIndex() 内部实现原理是先删除再增加
        int replaced = quicklistReplaceAtIndex(ql, index,
                                               value->ptr, sdslen(value->ptr));
        if (!replaced) {
            // out of range
            addReply(c,shared.outofrangeerr);
        } else {
            // 通知客户端
            addReply(c,shared.ok);
            // 向数据库发送值变更通知
            signalModifiedKey(c->db,c->argv[1]);
            // 发送事件通知
            notifyKeyspaceEvent(NOTIFY_LIST,"lset",c->argv[1],c->db->id);
            // 增加修改数据操作数
            server.dirty++;
        }
    } else {
        serverPanic("Unknown list encoding");
    }
}

// 链表 Pop，Pop 操作实现
void popGenericCommand(client *c, int where) {
    robj *o = lookupKeyWriteOrReply(c,c->argv[1],shared.null[c->resp]);
    if (o == NULL || checkType(c,o,OBJ_LIST)) return;

    robj *value = listTypePop(o,where);
    if (value == NULL) {
        // 没有删除节点
        addReplyNull(c);
    } else {
        // 删除完成，通知客户端
        char *event = (where == LIST_HEAD) ? "lpop" : "rpop";

        addReplyBulk(c,value);
        decrRefCount(value);
        notifyKeyspaceEvent(NOTIFY_LIST,event,c->argv[1],c->db->id);
        if (listTypeLength(o) == 0) {
            // 删除后是空 list 删除该数据即可
            notifyKeyspaceEvent(NOTIFY_GENERIC,"del",
                                c->argv[1],c->db->id);
            dbDelete(c->db,c->argv[1]);
        }
        signalModifiedKey(c->db,c->argv[1]);
        server.dirty++;
    }
}

// LPOP KEY_NAME
void lpopCommand(client *c) {
    popGenericCommand(c,LIST_HEAD);
}

// RPOP KEY_NAME
void rpopCommand(client *c) {
    popGenericCommand(c,LIST_TAIL);
}

// LRANGE KEY_NAME START END，返回指定空间中的元素，不修改数据
void lrangeCommand(client *c) {
    robj *o;
    long start, end, llen, rangelen;
    // 获取区间值
    if ((getLongFromObjectOrReply(c, c->argv[2], &start, NULL) != C_OK) ||
        (getLongFromObjectOrReply(c, c->argv[3], &end, NULL) != C_OK)) return;

    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.null[c->resp])) == NULL
         || checkType(c,o,OBJ_LIST)) return;
         // 原长
    llen = listTypeLength(o);

    /* convert negative indexes */
    if (start < 0) start = llen+start;
    if (end < 0) end = llen+end;
    if (start < 0) start = 0;

    /* Invariant: start >= 0, so this test will be true when end < 0.
     * The range is empty when start > end or start >= length. */
    // 范围出错
    if (start > end || start >= llen) {
        addReplyNull(c);
        return;
    }
    if (end >= llen) end = llen-1;
    rangelen = (end-start)+1;

    /* Return the result in form of a multi-bulk reply */
    // 批量返回给客户端
    addReplyArrayLen(c,rangelen);
    if (o->encoding == OBJ_ENCODING_QUICKLIST) {
        // 获取一个正向迭代器
        listTypeIterator *iter = listTypeInitIterator(o, start, LIST_TAIL);
        // 开始遍历
        while(rangelen--) {
            listTypeEntry entry;
            // 下一个元素
            listTypeNext(iter, &entry);
            quicklistEntry *qe = &entry.entry;
            // 返回值
            if (qe->value) {
                addReplyBulkCBuffer(c,qe->value,qe->sz);
            } else {
                addReplyBulkLongLong(c,qe->longval);
            }
        }
        // 释放迭代器
        listTypeReleaseIterator(iter);
    } else {
        serverPanic("List encoding is not QUICKLIST!");
    }
}

// LTRIM KEY_NAME START STOP，对一个 list 进行修剪，保存区间中的数据，这里是右侧开区间
void ltrimCommand(client *c) {
    robj *o;
    long start, end, llen, ltrim, rtrim;
    // 一系列的区间处理
    if ((getLongFromObjectOrReply(c, c->argv[2], &start, NULL) != C_OK) ||
        (getLongFromObjectOrReply(c, c->argv[3], &end, NULL) != C_OK)) return;

    if ((o = lookupKeyWriteOrReply(c,c->argv[1],shared.ok)) == NULL ||
        checkType(c,o,OBJ_LIST)) return;
    llen = listTypeLength(o);

    /* convert negative indexes */
    if (start < 0) start = llen+start;
    if (end < 0) end = llen+end;
    if (start < 0) start = 0;

    /* Invariant: start >= 0, so this test will be true when end < 0.
     * The range is empty when start > end or start >= length. */
    if (start > end || start >= llen) {
        /* Out of range start or start > end result in empty list */
        ltrim = llen;
        rtrim = 0;
    } else {
        if (end >= llen) end = llen-1;
        ltrim = start;
        rtrim = llen-end-1;
    }

    /* Remove list elements to perform the trim */
    if (o->encoding == OBJ_ENCODING_QUICKLIST) {
        // 删除范围外的数据，两端分别删除
        quicklistDelRange(o->ptr,0,ltrim);
        quicklistDelRange(o->ptr,-rtrim,rtrim);
    } else {
        serverPanic("Unknown list encoding");
    }
    // 事件通知
    notifyKeyspaceEvent(NOTIFY_LIST,"ltrim",c->argv[1],c->db->id);
    if (listTypeLength(o) == 0) {
        // 删除后数据为空，删除该数据
        dbDelete(c->db,c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_GENERIC,"del",c->argv[1],c->db->id);
    }
    // 向数据库发送值变更通知
    signalModifiedKey(c->db,c->argv[1]);
    server.dirty++;
    // 通知客户端
    addReply(c,shared.ok);
}

// LREM KEY_NAME COUNT VALUE，移除 list 中的值 value
// count > 0 从头开始移除 count 个 value
// count < 0 从尾开始移除 -count 个 value
// count == 0 移除所有 value
void lremCommand(client *c) {
    robj *subject, *obj;
    obj = c->argv[3];
    long toremove;
    long removed = 0;

    // 获取 count 值
    if ((getLongFromObjectOrReply(c, c->argv[2], &toremove, NULL) != C_OK))
        return;

    subject = lookupKeyWriteOrReply(c,c->argv[1],shared.czero);
    if (subject == NULL || checkType(c,subject,OBJ_LIST)) return;

    listTypeIterator *li;
    // 从头或尾开始操作
    if (toremove < 0) {
        toremove = -toremove;
        li = listTypeInitIterator(subject,-1,LIST_HEAD);
    } else {
        li = listTypeInitIterator(subject,0,LIST_TAIL);
    }

    listTypeEntry entry;
    // 存在下一个节点
    while (listTypeNext(li,&entry)) {
        // 如果是该节点
        if (listTypeEqual(&entry,obj)) {
            // 删除
            listTypeDelete(li, &entry);
            server.dirty++;
            removed++;
            // 删除完成
            if (toremove && removed == toremove) break;
        }
    }
    // 释放 iter
    listTypeReleaseIterator(li);

    if (removed) {
        // 向数据库发送值更改消息
        signalModifiedKey(c->db,c->argv[1]);
        // 事件通知
        notifyKeyspaceEvent(NOTIFY_GENERIC,"lrem",c->argv[1],c->db->id);
    }

    if (listTypeLength(subject) == 0) {
        // 删除空白数据
        dbDelete(c->db,c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_GENERIC,"del",c->argv[1],c->db->id);
    }
    // 告知客户端删除数据量
    addReplyLongLong(c,removed);
}

/* This is the semantic of this command:
 *  RPOPLPUSH srclist dstlist:
 *    IF LLEN(srclist) > 0
 *      element = RPOP srclist
 *      LPUSH dstlist element
 *      RETURN element
 *    ELSE
 *      RETURN nil
 *    END
 *  END
 *
 * The idea is to be able to get an element from a list in a reliable way
 * since the element is not just returned but pushed against another list
 * as well. This command was originally proposed by Ezra Zygmuntowicz.
 */
// RPOPLPUSH 操作的插入部分实现
void rpoplpushHandlePush(client *c, robj *dstkey, robj *dstobj, robj *value) {
    /* Create the list if the key does not exist */
    // list 不存在，创建
    if (!dstobj) {
        dstobj = createQuicklistObject();
        quicklistSetOptions(dstobj->ptr, server.list_max_ziplist_size,
                            server.list_compress_depth);
        // 添加到 DB
        dbAdd(c->db,dstkey,dstobj);
    }
    signalModifiedKey(c->db,dstkey);
    // 把 value 添加到 list 中
    listTypePush(dstobj,value,LIST_HEAD);
    // 事件通知
    notifyKeyspaceEvent(NOTIFY_LIST,"lpush",dstkey,c->db->id);
    /* Always send the pushed value to the client. */
    addReplyBulk(c,value);
}

// RPOPLPUSH source destination，把 source 的尾元素，删除并添加到 destination 头部，并返回给客户端
void rpoplpushCommand(client *c) {
    robj *sobj, *value;
    if ((sobj = lookupKeyWriteOrReply(c,c->argv[1],shared.null[c->resp]))
        == NULL || checkType(c,sobj,OBJ_LIST)) return;

    if (listTypeLength(sobj) == 0) {
        /* This may only happen after loading very old RDB files. Recent
         * versions of Redis delete keys of empty lists. */
        // source list 为空，返回 NULL
        addReplyNull(c);
    } else {
        robj *dobj = lookupKeyWrite(c->db,c->argv[2]);
        robj *touchedkey = c->argv[1];

        if (dobj && checkType(c,dobj,OBJ_LIST)) return;
        // Pop 尾节点
        value = listTypePop(sobj,LIST_TAIL);
        /* We saved touched key, and protect it, since rpoplpushHandlePush
         * may change the client command argument vector (it does not
         * currently). */
        incrRefCount(touchedkey);
        // 添加到 destination 头部
        rpoplpushHandlePush(c,c->argv[2],dobj,value);

        /* listTypePop returns an object with its refcount incremented */
        decrRefCount(value);

        /* Delete the source list when it is empty */
        // 事件通知
        notifyKeyspaceEvent(NOTIFY_LIST,"rpop",touchedkey,c->db->id);
        if (listTypeLength(sobj) == 0) {
            // source 为空，进行删除
            dbDelete(c->db,touchedkey);
            notifyKeyspaceEvent(NOTIFY_GENERIC,"del",
                                touchedkey,c->db->id);
        }
        signalModifiedKey(c->db,touchedkey);
        decrRefCount(touchedkey);
        server.dirty++;
        if (c->cmd->proc == brpoplpushCommand) {
            rewriteClientCommandVector(c,3,shared.rpoplpush,c->argv[1],c->argv[2]);
        }
    }
}

/*-----------------------------------------------------------------------------
 * Blocking POP operations
 *----------------------------------------------------------------------------*/
// 阻塞操作，就是当需要操作的 list 为空时，先将其加入列表中，并阻塞对应客户端，遍历阻塞列表，入到非空的 list 就开始执行操作
/* This is a helper function for handleClientsBlockedOnLists(). It's work
 * is to serve a specific client (receiver) that is blocked on 'key'
 * in the context of the specified 'db', doing the following:
 *
 * 1) Provide the client with the 'value' element.
 * 2) If the dstkey is not NULL (we are serving a BRPOPLPUSH) also push the
 *    'value' element on the destination list (the LPUSH side of the command).
 * 3) Propagate the resulting BRPOP, BLPOP and additional LPUSH if any into
 *    the AOF and replication channel.
 *
 * The argument 'where' is LIST_TAIL or LIST_HEAD, and indicates if the
 * 'value' element was popped fron the head (BLPOP) or tail (BRPOP) so that
 * we can propagate the command properly.
 *
 * The function returns C_OK if we are able to serve the client, otherwise
 * C_ERR is returned to signal the caller that the list POP operation
 * should be undone as the client was not served: This only happens for
 * BRPOPLPUSH that fails to push the value to the destination key as it is
 * of the wrong type. */
// 函数对被阻塞的客户端 receiver 、造成阻塞的 key 、 key 所在的数据库 db 以及一个值 value 和一个位置值 where 执行以下动作：
// 1，将 value 提供给 receiver
// 2，如果 dstkey 不为空（代表 list 数据进行了修改，对空表的修改就是添加）,将 value 推入到 dstkey 指定的列表中
// 3，将 BRPOP，BLPOP 和可能有的 LPUSH 传播到 AOF 和同步节点
// where 可能是 REDIS_TAIL 或者 REDIS_HEAD ，用于识别该 value 是从那个地方 POP 出来，依靠这个参数，可以同样传播 BLPOP 或者 BRPOP 。
// 如果一切成功，返回 REDIS_OK 。
// 如果执行失败，那么返回 REDIS_ERR ，让 Redis 撤销对目标节点的 POP 操作。
// 失败的情况只会出现在 BRPOPLPUSH 命令中，
// 比如 POP 列表成功，却发现想 PUSH 的目标不是列表时。
int serveClientBlockedOnList(client *receiver, robj *key, robj *dstkey, redisDb *db, robj *value, int where)
{
    robj *argv[3];
    // 不是 BLPOPRPUSH ？
    if (dstkey == NULL) {
        /* Propagate the [LR]POP operation. */
        // 传播到 Pop 操作
        argv[0] = (where == LIST_HEAD) ? shared.lpop :
                                          shared.rpop;
        argv[1] = key;
        propagate((where == LIST_HEAD) ?
            server.lpopCommand : server.rpopCommand,
            db->id,argv,2,PROPAGATE_AOF|PROPAGATE_REPL);

        /* BRPOP/BLPOP */
        // 回复客户端
        addReplyArrayLen(receiver,2);
        addReplyBulk(receiver,key);
        addReplyBulk(receiver,value);

        /* Notify event. */
        char *event = (where == LIST_HEAD) ? "lpop" : "rpop";
        notifyKeyspaceEvent(NOTIFY_LIST,event,key,receiver->db->id);
    } else {
        /* BRPOPLPUSH */
        // 获取 dstkey 对象
        robj *dstobj =
            lookupKeyWrite(receiver->db,dstkey);
            // 可以进行操作
        if (!(dstobj &&
             checkType(receiver,dstobj,OBJ_LIST)))
        {
            /* Propagate the RPOP operation. */
            // 传播 RPOP 操作
            argv[0] = shared.rpop;
            argv[1] = key;
            propagate(server.rpopCommand,
                db->id,argv,2,
                PROPAGATE_AOF|
                PROPAGATE_REPL);
            // 将 value 添加到 dstkey 列表里
            // 如果 dstkey 不存在，那么创建一个新列表，
            // 然后进行添加操作
            rpoplpushHandlePush(receiver,dstkey,dstobj,
                value);
            /* Propagate the LPUSH operation. */
            // 传播 LPUSH 操作
            argv[0] = shared.lpush;
            argv[1] = dstkey;
            argv[2] = value;
            propagate(server.lpushCommand,
                db->id,argv,3,
                PROPAGATE_AOF|
                PROPAGATE_REPL);

            /* Notify event ("lpush" was notified by rpoplpushHandlePush). */
            notifyKeyspaceEvent(NOTIFY_LIST,"rpop",key,receiver->db->id);
        } else {
            /* BRPOPLPUSH failed because of wrong
             * destination type. */
            return C_ERR;
        }
    }
    return C_OK;
}

/* Blocking RPOP/LPOP */
// 带限时阻塞的 Pop 操作
void blockingPopGenericCommand(client *c, int where) {
    robj *o;
    mstime_t timeout;
    int j;
    // 获取 Timeout 参数
    if (getTimeoutFromObjectOrReply(c,c->argv[c->argc-1],&timeout,UNIT_SECONDS)
        != C_OK) return;
    // 遍历所有 key 
    // 如果找到第一个不为空的列表对象，那么对它进行 POP ，然后返回
    for (j = 1; j < c->argc-1; j++) {
        o = lookupKeyWrite(c->db,c->argv[j]);
        if (o != NULL) {
            if (o->type != OBJ_LIST) {
                addReply(c,shared.wrongtypeerr);
                return;
            } else {
                if (listTypeLength(o) != 0) {
                    /* Non empty list, this is like a non normal [LR]POP. */
                    char *event = (where == LIST_HEAD) ? "lpop" : "rpop";
                    // 非空，执行 Pop
                    robj *value = listTypePop(o,where);
                    serverAssert(value != NULL);

                    addReplyArrayLen(c,2);
                    addReplyBulk(c,c->argv[j]);
                    addReplyBulk(c,value);
                    decrRefCount(value);
                    notifyKeyspaceEvent(NOTIFY_LIST,event,
                                        c->argv[j],c->db->id);
                    // 删除空 list
                    if (listTypeLength(o) == 0) {
                        dbDelete(c->db,c->argv[j]);
                        notifyKeyspaceEvent(NOTIFY_GENERIC,"del",
                                            c->argv[j],c->db->id);
                    }
                    signalModifiedKey(c->db,c->argv[j]);
                    server.dirty++;

                    /* Replicate it as an [LR]POP instead of B[LR]POP. */
                    rewriteClientCommandVector(c,2,
                        (where == LIST_HEAD) ? shared.lpop : shared.rpop,
                        c->argv[j]);
                    return;
                }
            }
        }
    }

    /* If we are inside a MULTI/EXEC and the list is empty the only thing
     * we can do is treating it as a timeout (even with timeout 0). */
    if (c->flags & CLIENT_MULTI) {
        addReplyNullArray(c);
        return;
    }

    /* If the list is empty or the key does not exists we must block */
    blockForKeys(c,BLOCKED_LIST,c->argv + 1,c->argc - 2,timeout,NULL,NULL);
}

// BLPOP LIST1 LIST2 .. LISTN TIMEOUT，移除并获取 list 的第一个元素，带限时
void blpopCommand(client *c) {
    blockingPopGenericCommand(c,LIST_HEAD);
}

// BRPOP LIST1 LIST2 .. LISTN TIMEOUT，移除并获取 list 的最后一个元素，带限时
void brpopCommand(client *c) {
    blockingPopGenericCommand(c,LIST_TAIL);
}

// BRPOPLPUSH LIST1 ANOTHER_LIST TIMEOUT，从 LIST1 弹出一个元素并插入到 ANOTHER_LIST 中
void brpoplpushCommand(client *c) {
    mstime_t timeout;
    // 获取 timeout 参数
    if (getTimeoutFromObjectOrReply(c,c->argv[3],&timeout,UNIT_SECONDS)
        != C_OK) return;
    // key 对象
    robj *key = lookupKeyWrite(c->db, c->argv[1]);

    if (key == NULL) {
        if (c->flags & CLIENT_MULTI) {
            /* Blocking against an empty list in a multi state
             * returns immediately. */
            addReplyNull(c);
        } else {
            /* The list is empty and the client blocks. */
            // 等待元素 push 到 key
            blockForKeys(c,BLOCKED_LIST,c->argv + 1,1,timeout,c->argv[2],NULL);
        }
    } else {
        if (key->type != OBJ_LIST) {
            addReply(c, shared.wrongtypeerr);
        } else {
            /* The list exists and has elements, so
             * the regular rpoplpushCommand is executed. */
            serverAssertWithInfo(c,key,listTypeLength(key) > 0);
            rpoplpushCommand(c);
        }
    }
}
