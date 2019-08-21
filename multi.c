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

// redis 的事务功能，使用 MULTI 开启事务，使用 EXEC 提交事务，所有提交的事务均是原子性的

/* ================================ MULTI/EXEC ============================== */

/* Client state initialization for MULTI/EXEC */
// 初始化客户端的事务状态
void initClientMultiState(client *c) {
    c->mstate.commands = NULL;
    c->mstate.count = 0;
    c->mstate.cmd_flags = 0;
}

/* Release all the resources associated with MULTI/EXEC state */
// 释放事务队列相关资源
void freeClientMultiState(client *c) {
    int j;
    // 遍历所有事务命令
    for (j = 0; j < c->mstate.count; j++) {
        int i;
        // 第 j 条事务命令
        multiCmd *mc = c->mstate.commands+j;
        // 释放所有命令参数
        for (i = 0; i < mc->argc; i++)
            decrRefCount(mc->argv[i]);
        // 释放参数数组本身（参数数组本身是二维数组）
        zfree(mc->argv);
    }
    // 释放事务队列，mstate 仅有 commands 创建在堆上
    zfree(c->mstate.commands);
}

/* Add a new command into the MULTI commands queue */
// 向事务队列中添加一个命令
void queueMultiCommand(client *c) {
    multiCmd *mc;
    int j;
    // 先分配空间
    c->mstate.commands = zrealloc(c->mstate.commands,
            sizeof(multiCmd)*(c->mstate.count+1));
    // 定位到命令插入位置
    mc = c->mstate.commands+c->mstate.count;
    // 填充新命令的数据
    mc->cmd = c->cmd;
    mc->argc = c->argc;
    mc->argv = zmalloc(sizeof(robj*)*c->argc);
    memcpy(mc->argv,c->argv,sizeof(robj*)*c->argc);
    for (j = 0; j < c->argc; j++)
        incrRefCount(mc->argv[j]);
    c->mstate.count++;
    // 统计一下 flags ？
    c->mstate.cmd_flags |= c->cmd->flags;
}

// 丢弃事务队列中的命令，并退出事务状态
void discardTransaction(client *c) {
    // 重置事务状态，先释放所有未执行命令，在初始化，一般在执行失败时进行事务列表清空
    freeClientMultiState(c);
    initClientMultiState(c);
    // 屏蔽事务状态
    c->flags &= ~(CLIENT_MULTI|CLIENT_DIRTY_CAS|CLIENT_DIRTY_EXEC);
    // 取消对所有键的监视
    unwatchAllKeys(c);
}

/* Flag the transacation as DIRTY_EXEC so that EXEC will fail.
 * Should be called every time there is an error while queueing a command. */
// 设置事务状态为 EXEC，让之后的 EXEC 命令失败
// 每次在入队命令出错时进行调用
void flagTransaction(client *c) {
    // 处于 MULTI 状态进行设置
    if (c->flags & CLIENT_MULTI)
        c->flags |= CLIENT_DIRTY_EXEC;
}

// 进入事务状态
void multiCommand(client *c) {
    // 判断是否已经处于 MULTI 状态
    if (c->flags & CLIENT_MULTI) {
        addReplyError(c,"MULTI calls can not be nested");
        return;
    }
    // 标记 MULTI
    c->flags |= CLIENT_MULTI;
    // 返回 OK
    addReply(c,shared.ok);
}

// 取消事务，丢弃所有命令
void discardCommand(client *c) {
    // 仅在处于事务状态进行使用
    if (!(c->flags & CLIENT_MULTI)) {
        addReplyError(c,"DISCARD without MULTI");
        return;
    }
    discardTransaction(c);
    addReply(c,shared.ok);
}

/* Send a MULTI command to all the slaves and AOF file. Check the execCommand
 * implementation for more information. */
// 向所有附属节点和 AOF 文件传播 MULTI 命令
void execCommandPropagateMulti(client *c) {
    robj *multistring = createStringObject("MULTI",5);

    propagate(server.multiCommand,c->db->id,&multistring,1,
              PROPAGATE_AOF|PROPAGATE_REPL);
    decrRefCount(multistring);
}

// 执行事务命令
void execCommand(client *c) {
    int j;
    robj **orig_argv;
    int orig_argc;
    struct redisCommand *orig_cmd;
    int must_propagate = 0; /* Need to propagate MULTI/EXEC to AOF / slaves? */
    int was_master = server.masterhost == NULL;
    // 不在事务状态
    if (!(c->flags & CLIENT_MULTI)) {
        addReplyError(c,"EXEC without MULTI");
        return;
    }

    /* Check if we need to abort the EXEC because:
     * 1) Some WATCHed key was touched.
     * 2) There was a previous error while queueing commands.
     * A failed EXEC in the first case returns a multi bulk nil object
     * (technically it is not an error but a special behavior), while
     * in the second an EXECABORT error is returned. */
    // 检查阻止事务运行的条件
    // 1）被监视的键发生了改变
    // 2）命令在入队时出错
    // 第一种情况返回多个批量回复的空对象
    // 第二种返回一个 EXECABOUT 对象
    if (c->flags & (CLIENT_DIRTY_CAS|CLIENT_DIRTY_EXEC)) {
        addReply(c, c->flags & CLIENT_DIRTY_EXEC ? shared.execaborterr :
                                                   shared.nullarray[c->resp]);
        // 取消事务
        discardTransaction(c);
        goto handle_monitor;
    }

    /* If there are write commands inside the transaction, and this is a read
     * only slave, we want to send an error. This happens when the transaction
     * was initiated when the instance was a master or a writable replica and
     * then the configuration changed (for example instance was turned into
     * a replica). */
    // 如果事务命令中存在写操作，但是执行的只读附属，报错。
    if (!server.loading && server.masterhost && server.repl_slave_ro &&
        !(c->flags & CLIENT_MASTER) && c->mstate.cmd_flags & CMD_WRITE)
    {
        addReplyError(c,
            "Transaction contains write commands but instance "
            "is now a read-only replica. EXEC aborted.");
        discardTransaction(c);
        goto handle_monitor;
    }

    /* Exec all the queued commands */
    // 开始执行命令
    // 取消客户端对所有键的监视，因为已经确定不会出错，节约资源
    unwatchAllKeys(c); /* Unwatch ASAP otherwise we'll waste CPU cycles */
    // 因为后面需要客户端的上下文进行执行事务命令，所以需要进行备份
    orig_argv = c->argv;
    orig_argc = c->argc;
    orig_cmd = c->cmd;
    addReplyArrayLen(c,c->mstate.count);
    // 执行事务中的命令
    for (j = 0; j < c->mstate.count; j++) {
        // 因为 Redis 的命令必须在客户端的上下文中执行
        // 所以要将事务队列中的命令、命令参数等设置给客户端
        c->argc = c->mstate.commands[j].argc;
        c->argv = c->mstate.commands[j].argv;
        c->cmd = c->mstate.commands[j].cmd;

        /* Propagate a MULTI request once we encounter the first command which
         * is not readonly nor an administrative one.
         * This way we'll deliver the MULTI/..../EXEC block as a whole and
         * both the AOF and the replication link will have the same consistency
         * and atomicity guarantees. */
        // 当遇上第一个写命令时，传播 MULTI 命令
        // 这可以确保服务器和 AOF 文件以及附属节点的数据一致性
        if (!must_propagate && !(c->cmd->flags & (CMD_READONLY|CMD_ADMIN))) {
            // 传播 MULTI 命令
            execCommandPropagateMulti(c);
            // 计数，仅第一次发送
            must_propagate = 1;
        }
        // 执行命令
        call(c,server.loading ? CMD_CALL_NONE : CMD_CALL_FULL);

        /* Commands may alter argc/argv, restore mstate. */
        // 因为执行后命令、命令参数可能会被改变
        // 比如 SPOP 会被改写为 SREM
        // 所以这里需要更新事务队列中的命令和参数
        // 确保附属节点和 AOF 的数据一致性
        c->mstate.commands[j].argc = c->argc;
        c->mstate.commands[j].argv = c->argv;
        c->mstate.commands[j].cmd = c->cmd;
    }
    // 还原
    c->argv = orig_argv;
    c->argc = orig_argc;
    c->cmd = orig_cmd;
    discardTransaction(c);

    /* Make sure the EXEC command will be propagated as well if MULTI
     * was already propagated. */
    // 将服务器设为脏，确保 EXEC 命令也会被传播
    if (must_propagate) {
        int is_master = server.masterhost == NULL;
        server.dirty++;
        /* If inside the MULTI/EXEC block this instance was suddenly
         * switched from master to slave (using the SLAVEOF command), the
         * initial MULTI was propagated into the replication backlog, but the
         * rest was not. We need to make sure to at least terminate the
         * backlog with the final EXEC. */
        if (server.repl_backlog && was_master && !is_master) {
            char *execcmd = "*1\r\n$4\r\nEXEC\r\n";
            feedReplicationBacklog(execcmd,strlen(execcmd));
        }
    }

handle_monitor:
    /* Send EXEC to clients waiting data from MONITOR. We do it here
     * since the natural order of commands execution is actually:
     * MUTLI, EXEC, ... commands inside transaction ...
     * Instead EXEC is flagged as CMD_SKIP_MONITOR in the command
     * table, and we do it here with correct ordering. */
    if (listLength(server.monitors) && !server.loading)
        replicationFeedMonitors(c,server.monitors,c->db->id,c->argv,c->argc);
}

/* ===================== WATCH (CAS alike for MULTI/EXEC) ===================
 *
 * The implementation uses a per-DB hash table mapping keys to list of clients
 * WATCHing those keys, so that given a key that is going to be modified
 * we can mark all the associated clients as dirty.
 *
 * Also every client contains a list of WATCHed keys so that's possible to
 * un-watch such keys when the client is freed or when UNWATCH is called. */

/* In the client->watched_keys list we need to use watchedKey structures
 * as in order to identify a key in Redis we need both the key name and the
 * DB */
// 即需要保存被监视的键，还需要保存该建所在数据库
typedef struct watchedKey {
    robj *key;
    redisDb *db; // 监视键所在数据库
} watchedKey;

/* Watch for the specified key */
// 监视一个 key，客户端和服务器数据库均保存一份，但是使用的数据结构不同
// 服务器数据库中村树的结构是 dict，key = key_name,value = list[client]
void watchForKey(client *c, robj *key) {
    list *clients = NULL;
    listIter li;
    listNode *ln;
    watchedKey *wk;

    /* Check if we are already watching for this key */
    // 检查是否已经在监视
    listRewind(c->watched_keys,&li);
    while((ln = listNext(&li))) {
        wk = listNodeValue(ln);
        if (wk->db == c->db && equalStringObjects(key,wk->key))
            return; /* Key already watched */
    }
    /* This key is not already watched in this DB. Let's add it */
    // 数据库中不存在，add
    clients = dictFetchValue(c->db->watched_keys,key);
    if (!clients) {
        // 创建一个新的链表
        clients = listCreate();
        // 添加到对应的数据库中
        dictAdd(c->db->watched_keys,key,clients);
        incrRefCount(key);
    }
    // 添加在末尾
    listAddNodeTail(clients,c);
    /* Add the new key to the list of keys watched by this client */
    wk = zmalloc(sizeof(*wk));
    wk->key = key;
    wk->db = c->db;
    incrRefCount(key);
    // 添加到客户端监视链表的末尾
    listAddNodeTail(c->watched_keys,wk);
}

/* Unwatch all the keys watched by this client. To clean the EXEC dirty
 * flag is up to the caller. */
// 取消所有键的监视
void unwatchAllKeys(client *c) {
    listIter li;
    listNode *ln;

    if (listLength(c->watched_keys) == 0) return;
    // 遍历本地监视队列
    listRewind(c->watched_keys,&li);
    while((ln = listNext(&li))) {
        list *clients;
        watchedKey *wk;

        /* Lookup the watched key -> clients list and remove the client
         * from the list */
        wk = listNodeValue(ln);
        // 监视对应键的 client 链表
        clients = dictFetchValue(wk->db->watched_keys, wk->key);
        serverAssertWithInfo(c,NULL,clients != NULL);
        listDelNode(clients,listSearchKey(clients,c));
        /* Kill the entry at all if this was the only client */
        if (listLength(clients) == 0)
            dictDelete(wk->db->watched_keys, wk->key);
        /* Remove this watched key from the client->watched list */
        listDelNode(c->watched_keys,ln);
        decrRefCount(wk->key);
        zfree(wk);
    }
}

/* "Touch" a key, so that if this key is being WATCHed by some client the
 * next EXEC will fail. */
// 传入被监视的键 key，进行标记，执行 EXEC 会失败
void touchWatchedKey(redisDb *db, robj *key) {
    list *clients;
    listIter li;
    listNode *ln;
    // 没有监视，直接返回
    if (dictSize(db->watched_keys) == 0) return;
    // 返回监视的客户端列表
    clients = dictFetchValue(db->watched_keys, key);
    if (!clients) return;

    /* Mark all the clients watching this key as CLIENT_DIRTY_CAS */
    /* Check if we are already watching for this key */
    listRewind(clients,&li);
    while((ln = listNext(&li))) {
        // 均设置 CLIENT_DIRTY_CAS flags 即可
        client *c = listNodeValue(ln);

        c->flags |= CLIENT_DIRTY_CAS;
    }
}

/* On FLUSHDB or FLUSHALL all the watched keys that are present before the
 * flush but will be deleted as effect of the flushing operation should
 * be touched. "dbid" is the DB that's getting the flush. -1 if it is
 * a FLUSHALL operation (all the DBs flushed). */
// 当一个数据库被 FLUSHDB 或者 FLUSHALL 清空时
// 它数据库内的所有 key 都应该被触碰
// dbid 参数指定要被 FLUSH 的数据库
// 如果 dbid 为 -1 ，那么表示执行的是 FLUSHALL
// 所有数据库都将被 FLUSH
void touchWatchedKeysOnFlush(int dbid) {
    listIter li1, li2;
    listNode *ln;

    /* For every client, check all the waited keys */
    // 大规模操作，遍历客户端即可
    listRewind(server.clients,&li1);
    while((ln = listNext(&li1))) {
        // 当前客户端
        client *c = listNodeValue(ln);
        // 客户端监视键
        listRewind(c->watched_keys,&li2);
        while((ln = listNext(&li2))) {
            watchedKey *wk = listNodeValue(ln);

            /* For every watched key matching the specified DB, if the
             * key exists, mark the client as dirty, as the key will be
             * removed. */
            if (dbid == -1 || wk->db->id == dbid) {
                // 判断键是否存在
                if (dictFind(wk->db->dict, wk->key->ptr) != NULL)
                    c->flags |= CLIENT_DIRTY_CAS;
            }
        }
    }
}

// WATCH key_name ...
void watchCommand(client *c) {
    int j;
    // 事务开始后不可以监视键
    if (c->flags & CLIENT_MULTI) {
        addReplyError(c,"WATCH inside MULTI is not allowed");
        return;
    }
    // 遍历输入的所有键
    for (j = 1; j < c->argc; j++)
        watchForKey(c,c->argv[j]);
    addReply(c,shared.ok);
}

// 取消监视
void unwatchCommand(client *c) {
    // 取消所有键的监视
    unwatchAllKeys(c);
    // 取消监视状态
    c->flags &= (~CLIENT_DIRTY_CAS);
    addReply(c,shared.ok);
}
