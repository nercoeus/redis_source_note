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

int clientSubscriptionsCount(client *c);

/*-----------------------------------------------------------------------------
 * Pubsub client replies API     回复订阅客户端 API
 *----------------------------------------------------------------------------*/

/* Send a pubsub message of type "message" to the client. */
// 发布一个订阅消息给客户端
void addReplyPubsubMessage(client *c, robj *channel, robj *msg) {
    if (c->resp == 2)
        addReply(c,shared.mbulkhdr[3]);
    else
        addReplyPushLen(c,3);
    // messagebulk 字段
    addReply(c,shared.messagebulk);
    addReplyBulk(c,channel);
    addReplyBulk(c,msg);
}

/* Send a pubsub message of type "pmessage" to the client. The difference
 * with the "message" type delivered by addReplyPubsubMessage() is that
 * this message format also includes the pattern that matched the message. */
void addReplyPubsubPatMessage(client *c, robj *pat, robj *channel, robj *msg) {
    if (c->resp == 2)
        addReply(c,shared.mbulkhdr[4]);
    else
        addReplyPushLen(c,4);
    // pmessagebulk 字段
    addReply(c,shared.pmessagebulk);
    addReplyBulk(c,pat);
    addReplyBulk(c,channel);
    addReplyBulk(c,msg);
}

/* Send the pubsub subscription notification to the client. */
// 将订阅通知发送给客户端
void addReplyPubsubSubscribed(client *c, robj *channel) {
    // RESP 版本不同
    if (c->resp == 2)
        addReply(c,shared.mbulkhdr[3]);
    else
        addReplyPushLen(c,3);
    // subscribebulk 字符串
    addReply(c,shared.subscribebulk);
    // 有 channel 就返回 channel_name
    addReplyBulk(c,channel);
    // 客户端当前订阅的频道和模式总数
    addReplyLongLong(c,clientSubscriptionsCount(c));
}

/* Send the pubsub unsubscription notification to the client.
 * Channel can be NULL: this is useful when the client sends a mass
 * unsubscribe command but there are no channels to unsubscribe from: we
 * still send a notification. */
// 将退订通知发送给客户端
void addReplyPubsubUnsubscribed(client *c, robj *channel) {
    // RESP 版本不同
    if (c->resp == 2)
        addReply(c,shared.mbulkhdr[3]);
    else
        addReplyPushLen(c,3);
    // unsubscribebulk 字符串
    addReply(c,shared.unsubscribebulk);
    // 有 channel 就返回 channel_name
    if (channel)
        addReplyBulk(c,channel);
    else
        addReplyNull(c);
    // 客户端当前订阅的频道和模式总数
    addReplyLongLong(c,clientSubscriptionsCount(c));
}

/* Send the pubsub pattern subscription notification to the client. */
// 将订阅通知发送给客户端
void addReplyPubsubPatSubscribed(client *c, robj *pattern) {
    if (c->resp == 2)
        addReply(c,shared.mbulkhdr[3]);
    else
        addReplyPushLen(c,3);
    // psubscribebulk 字段
    addReply(c,shared.psubscribebulk);
    // 订阅的模式
    addReplyBulk(c,pattern);
    // 客户端当前订阅的频道和模式总数
    addReplyLongLong(c,clientSubscriptionsCount(c));
}

/* Send the pubsub pattern unsubscription notification to the client.
 * Pattern can be NULL: this is useful when the client sends a mass
 * punsubscribe command but there are no pattern to unsubscribe from: we
 * still send a notification. */
// 将退订通知发送给客户端
void addReplyPubsubPatUnsubscribed(client *c, robj *pattern) {
    if (c->resp == 2)
        addReply(c,shared.mbulkhdr[3]);
    else
        addReplyPushLen(c,3);
    // punsubscribebulk 字段
    addReply(c,shared.punsubscribebulk);
    // 订阅的模式
    if (pattern)
        addReplyBulk(c,pattern);
    else
        addReplyNull(c);
    // 客户端当前订阅的频道和模式总数
    addReplyLongLong(c,clientSubscriptionsCount(c));
}

/*-----------------------------------------------------------------------------
 * Pubsub low level API
 *----------------------------------------------------------------------------*/

// 释放订阅模式 P 的一个客户端节点
void freePubsubPattern(void *p) {
    pubsubPattern *pat = p;

    decrRefCount(pat->pattern);
    zfree(pat);
}

// 对比两个节点是否是同一个，相同放回 1，不相同返回 0
int listMatchPubsubPattern(void *a, void *b) {
    pubsubPattern *pa = a, *pb = b;

    return (pa->client == pb->client) &&
           (equalStringObjects(pa->pattern,pb->pattern));
}

/* Return the number of channels + patterns a client is subscribed to. */
// 返回客户订阅的频道数 + 模式数
int clientSubscriptionsCount(client *c) {
    return dictSize(c->pubsub_channels)+      // 频道数
           listLength(c->pubsub_patterns);    // 模式数
}

/* Subscribe a client to a channel. Returns 1 if the operation succeeded, or
 * 0 if the client was already subscribed to that channel. */
// 设置客户端 c 订阅频道 channel
// 订阅成功返回 1，如果客户端已经订阅了该频道，那么返回 0 
int pubsubSubscribeChannel(client *c, robj *channel) {
    dictEntry *de;
    list *clients = NULL;
    int retval = 0;

    /* Add the channel to the client -> channels hash table */
    // 把该频道添加到客户端中，使用 dict 保存，key = channel，val = NULL
    if (dictAdd(c->pubsub_channels,channel,NULL) == DICT_OK) {
        // 添加成功
        retval = 1;
        // 增加对象的引用计数
        incrRefCount(channel);
        /* Add the client to the channel -> list of clients hash table */
        // 把客户端订阅信息添加到服务器订阅列表里
        de = dictFind(server.pubsub_channels,channel);
        if (de == NULL) {
            // 没有客户端订阅这个频道（使用双向链表进行保存）
            clients = listCreate();
            // 服务器频道订阅使用 dict，key = channel_name, val = adlist[client]
            dictAdd(server.pubsub_channels,channel,clients);
            incrRefCount(channel);
        } else {
            clients = dictGetVal(de);
        }
        // 在这里添加到服务器端的对应频道订阅链表
        listAddNodeTail(clients,c);
    }
    /* Notify the client */
    // 通知客户端
    addReplyPubsubSubscribed(c,channel);
    // 返回是否添加成功
    return retval;
}

/* Unsubscribe a client from a channel. Returns 1 if the operation succeeded, or
 * 0 if the client was not subscribed to the specified channel. */
// 客户端退订一个频道
int pubsubUnsubscribeChannel(client *c, robj *channel, int notify) {
    dictEntry *de;
    list *clients;
    listNode *ln;
    int retval = 0;

    /* Remove the channel from the client -> channels hash table */
    incrRefCount(channel); /* channel may be just a pointer to the same object
                            we have in the hash tables. Protect it... */
    // 在客户端频道列表中进行删除
    if (dictDelete(c->pubsub_channels,channel) == DICT_OK) {
        retval = 1;
        /* Remove the client from the channel -> clients list hash table */
        // 在服务器频道列表中进行查找
        de = dictFind(server.pubsub_channels,channel);
        serverAssertWithInfo(c,NULL,de != NULL);
        // 获取客户端列表
        clients = dictGetVal(de);
        ln = listSearchKey(clients,c);
        serverAssertWithInfo(c,NULL,ln != NULL);
        // 删除客户端
        listDelNode(clients,ln);
        if (listLength(clients) == 0) {
            /* Free the list and associated hash entry at all if this was
             * the latest client, so that it will be possible to abuse
             * Redis PUBSUB creating millions of channels. */
            // 频道没有客户端订阅，同样删除
            dictDelete(server.pubsub_channels,channel);
        }
    }
    /* Notify the client */
    // 通知客户端
    if (notify) addReplyPubsubUnsubscribed(c,channel);
    decrRefCount(channel); /* it is finally safe to release it */
    return retval;
}

/* Subscribe a client to a pattern. Returns 1 if the operation succeeded, or 0 if the client was already subscribed to that pattern. */
// 订阅模式
int pubsubSubscribePattern(client *c, robj *pattern) {
    int retval = 0;
    // 查找订阅的模式是否存在
    if (listSearchKey(c->pubsub_patterns,pattern) == NULL) {
        retval = 1;
        pubsubPattern *pat;
        listAddNodeTail(c->pubsub_patterns,pattern);
        incrRefCount(pattern);
        pat = zmalloc(sizeof(*pat));
        pat->pattern = getDecodedObject(pattern);
        pat->client = c;
        // 仅仅把订阅节点保存在链表里，并不像频道一样根据 channel 不同进行区分
        listAddNodeTail(server.pubsub_patterns,pat);
    }
    /* Notify the client */
    addReplyPubsubPatSubscribed(c,pattern);
    return retval;
}

/* Unsubscribe a client from a channel. Returns 1 if the operation succeeded, or
 * 0 if the client was not subscribed to the specified channel. */
// 退订模式
int pubsubUnsubscribePattern(client *c, robj *pattern, int notify) {
    listNode *ln;
    pubsubPattern pat;
    int retval = 0;

    incrRefCount(pattern); /* Protect the object. May be the same we remove */
    // 查找订阅的模式是否存在
    if ((ln = listSearchKey(c->pubsub_patterns,pattern)) != NULL) {
        retval = 1;
        // 存在就清除即可
        listDelNode(c->pubsub_patterns,ln);
        pat.client = c;
        pat.pattern = pattern;
        ln = listSearchKey(server.pubsub_patterns,&pat);
        listDelNode(server.pubsub_patterns,ln);
    }
    /* Notify the client */
    if (notify) addReplyPubsubPatUnsubscribed(c,pattern);
    decrRefCount(pattern);
    return retval;
}

/* Unsubscribe from all the channels. Return the number of channels the
 * client was subscribed to. */
// 退订所有频道
int pubsubUnsubscribeAllChannels(client *c, int notify) {
    dictIterator *di = dictGetSafeIterator(c->pubsub_channels);
    dictEntry *de;
    int count = 0;

    while((de = dictNext(di)) != NULL) {
        robj *channel = dictGetKey(de);

        count += pubsubUnsubscribeChannel(c,channel,notify);
    }
    /* We were subscribed to nothing? Still reply to the client. */
    // 根据 notify 来判断是否进行回复，并且仅在本来就是空的才会进行回复
    if (notify && count == 0) addReplyPubsubUnsubscribed(c,NULL);
    dictReleaseIterator(di);
    return count;
}

/* Unsubscribe from all the patterns. Return the number of patterns the
 * client was subscribed from. */
// 退订所有模式
int pubsubUnsubscribeAllPatterns(client *c, int notify) {
    listNode *ln;
    listIter li;
    int count = 0;

    listRewind(c->pubsub_patterns,&li);
    while ((ln = listNext(&li)) != NULL) {
        robj *pattern = ln->value;

        count += pubsubUnsubscribePattern(c,pattern,notify);
    }
    // 根据 notify 来判断是否进行回复，并且仅在本来就是空的才会进行回复
    if (notify && count == 0) addReplyPubsubPatUnsubscribed(c,NULL);
    return count;
}

/* Publish a message */
// 将 message 发送到所有订阅频道 channel 的客户端
// 以及所有订阅了 channel 频道匹配的模式的客户端
int pubsubPublishMessage(robj *channel, robj *message) {
    int receivers = 0;
    dictEntry *de;
    listNode *ln;
    listIter li;

    /* Send to clients listening for that channel */
    // 寻找相同频道
    de = dictFind(server.pubsub_channels,channel);
    if (de) {
        // 获取频道里的所有客户端
        list *list = dictGetVal(de);
        listNode *ln;
        listIter li;
        // 前向迭代发送
        listRewind(list,&li);
        while ((ln = listNext(&li)) != NULL) {
            client *c = ln->value;
            // 给这个客户端送消息
            addReplyPubsubMessage(c,channel,message);
            receivers++;
        }
    }
    /* Send to clients listening to matching channels */
    // 模式不为空
    if (listLength(server.pubsub_patterns)) {
        // 前向便利所有模式即客户端
        listRewind(server.pubsub_patterns,&li);
        channel = getDecodedObject(channel);
        while ((ln = listNext(&li)) != NULL) {
            // 获取一个模式订阅节点
            pubsubPattern *pat = ln->value;
            // 订阅模式和通知频道相匹配
            if (stringmatchlen((char*)pat->pattern->ptr,
                                sdslen(pat->pattern->ptr),
                                (char*)channel->ptr,
                                sdslen(channel->ptr),0))
            {
                // 通知该客户端
                addReplyPubsubPatMessage(pat->client,
                    pat->pattern,channel,message);
                receivers++;
            }
        }
        decrRefCount(channel);
    }
    // 返回通知客户端个数
    return receivers;
}

/*-----------------------------------------------------------------------------
 * Pubsub commands implementation
 *----------------------------------------------------------------------------*/

// 订阅频道
void subscribeCommand(client *c) {
    int j;

    for (j = 1; j < c->argc; j++)
        pubsubSubscribeChannel(c,c->argv[j]);
    c->flags |= CLIENT_PUBSUB;
}

// 退订频道
void unsubscribeCommand(client *c) {
    if (c->argc == 1) {
        pubsubUnsubscribeAllChannels(c,1);
    } else {
        int j;

        for (j = 1; j < c->argc; j++)
            pubsubUnsubscribeChannel(c,c->argv[j],1);
    }
    if (clientSubscriptionsCount(c) == 0) c->flags &= ~CLIENT_PUBSUB;
}

// 订阅模式
void psubscribeCommand(client *c) {
    int j;

    for (j = 1; j < c->argc; j++)
        pubsubSubscribePattern(c,c->argv[j]);
    c->flags |= CLIENT_PUBSUB;
}

// 退订模式
void punsubscribeCommand(client *c) {
    if (c->argc == 1) {
        pubsubUnsubscribeAllPatterns(c,1);
    } else {
        int j;

        for (j = 1; j < c->argc; j++)
            pubsubUnsubscribePattern(c,c->argv[j],1);
    }
    if (clientSubscriptionsCount(c) == 0) c->flags &= ~CLIENT_PUBSUB;
}

// 发送一个消息
void publishCommand(client *c) {
    int receivers = pubsubPublishMessage(c->argv[1],c->argv[2]);
    // 集群开启
    if (server.cluster_enabled)
        clusterPropagatePublish(c->argv[1],c->argv[2]);
    else
        forceCommandPropagation(c,PROPAGATE_REPL);
    addReplyLongLong(c,receivers);
}

/* PUBSUB command for Pub/Sub introspection. */
void pubsubCommand(client *c) {
    if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr,"help")) {
        const char *help[] = {
"CHANNELS [<pattern>] -- Return the currently active channels matching a pattern (default: all).",
"NUMPAT -- Return number of subscriptions to patterns.",
"NUMSUB [channel-1 .. channel-N] -- Returns the number of subscribers for the specified channels (excluding patterns, default: none).",
NULL
        };
        addReplyHelp(c, help);
    } else if (!strcasecmp(c->argv[1]->ptr,"channels") &&
        (c->argc == 2 || c->argc == 3))
    {
        /* PUBSUB CHANNELS [<pattern>] */
        // 返回活跃频道，至少有一个客户端进行订阅
        sds pat = (c->argc == 2) ? NULL : c->argv[2]->ptr;
        dictIterator *di = dictGetIterator(server.pubsub_channels);
        dictEntry *de;
        long mblen = 0;
        void *replylen;

        replylen = addReplyDeferredLen(c);
        while((de = dictNext(di)) != NULL) {
            robj *cobj = dictGetKey(de);
            sds channel = cobj->ptr;

            if (!pat || stringmatchlen(pat, sdslen(pat),
                                       channel, sdslen(channel),0))
            {
                addReplyBulk(c,cobj);
                mblen++;
            }
        }
        dictReleaseIterator(di);
        setDeferredArrayLen(c,replylen,mblen);
    } else if (!strcasecmp(c->argv[1]->ptr,"numsub") && c->argc >= 2) {
        // 返回给定频道的订阅者数量
        /* PUBSUB NUMSUB [Channel_1 ... Channel_N] */
        int j;

        addReplyArrayLen(c,(c->argc-2)*2);
        for (j = 2; j < c->argc; j++) {
            list *l = dictFetchValue(server.pubsub_channels,c->argv[j]);

            addReplyBulk(c,c->argv[j]);
            addReplyLongLong(c,l ? listLength(l) : 0);
        }
    } else if (!strcasecmp(c->argv[1]->ptr,"numpat") && c->argc == 2) {
        /* PUBSUB NUMPAT */
        // 返回服务器被订阅模式个数
        addReplyLongLong(c,listLength(server.pubsub_patterns));
    } else {
        addReplySubcommandSyntaxError(c);
    }
}
