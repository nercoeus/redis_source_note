/* A simple event-driven programming library. Originally I wrote this code
 * for the Jim's event-loop (Jim is a Tcl interpreter) but later translated
 * it in form of a library for easy reuse.
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

// redis 事件处理采用 Reactor 模式，并添加了定时事件的处理，redis 是单线程的
// AE 事件库接口

#ifndef __AE_H__
#define __AE_H__

#include <time.h>

// 事件执行状态
#define AE_OK 0
#define AE_ERR -1

// 文件事件状态
#define AE_NONE 0       /* 未设置 No events registered. */
#define AE_READABLE 1   /* 可读 Fire when descriptor is readable. */
#define AE_WRITABLE 2   /* 可写 Fire when descriptor is writable. */
// 可写状态下，如果读事件已在同一事件循环中触发，则永远不会触发事件。当你想要在发送回复前将内容保存在内存中并希望以组进行操作时
#define AE_BARRIER 4    /* 屏障 With WRITABLE, never fire the event if the
                           READABLE event already fired in the same event
                           loop iteration. Useful when you want to persist
                           things to disk before sending replies, and want
                           to do that in a group fashion. */

// 事件事务和定时事务
#define AE_FILE_EVENTS 1
#define AE_TIME_EVENTS 2
// 所有事务类型
#define AE_ALL_EVENTS (AE_FILE_EVENTS|AE_TIME_EVENTS)
// 不阻塞，也不等待
#define AE_DONT_WAIT 4
#define AE_CALL_AFTER_SLEEP 8

// 代表定时事件，执行完删除
#define AE_NOMORE -1
// 时间事件删除后的标志
#define AE_DELETED_EVENT_ID -1

/* Macros */
#define AE_NOTUSED(V) ((void) V)

struct aeEventLoop;

/* Types and data structures */
// 处理器函数的函数类型
typedef void aeFileProc(struct aeEventLoop *eventLoop, int fd, void *clientData, int mask);
typedef int aeTimeProc(struct aeEventLoop *eventLoop, long long id, void *clientData);
typedef void aeEventFinalizerProc(struct aeEventLoop *eventLoop, void *clientData);
typedef void aeBeforeSleepProc(struct aeEventLoop *eventLoop);

/* File event structure */
// 文件事件
typedef struct aeFileEvent {
    // 事件类型
    int mask; /* one of AE_(READABLE|WRITABLE|BARRIER) */
    // 读事件处理器
    aeFileProc *rfileProc;
    // 写事件处理器
    aeFileProc *wfileProc;
    // 多路复用库的私有数据
    void *clientData;
} aeFileEvent;

/* Time event structure */
// 定时事件，每次都会全部遍历事件链表
typedef struct aeTimeEvent {
    // 全局 ID
    long long id; /* time event identifier. */
    // 秒精确时间戳，记录时间事件到达时间
    long when_sec; /* seconds */
    // 毫秒精确时间戳，记录时间事件到达时间
    long when_ms; /* milliseconds */
    // 时间处理器
    aeTimeProc *timeProc;
    // 时间事件结束回调函数，析构一些资源
    aeEventFinalizerProc *finalizerProc;
    // 私有数据
    void *clientData;
    // 前驱节点
    struct aeTimeEvent *prev;
    // 后继节点
    struct aeTimeEvent *next;
} aeTimeEvent;

/* A fired event */
// 就绪事件类型
typedef struct aeFiredEvent {
    int fd;
    int mask;
} aeFiredEvent;

/* State of an event based program */
// 事件管理器
typedef struct aeEventLoop {
    // 当前事件槽中的最大 fd
    int maxfd;   /* highest file descriptor currently registered */
    int setsize; /* max number of file descriptors tracked */
    long long timeEventNextId;
    time_t lastTime;     /* Used to detect system clock skew */
    // 注册文件事件
    aeFileEvent *events; /* Registered events */
    // 待执行文件事件
    aeFiredEvent *fired; /* Fired events */
    // 时间事件列表
    aeTimeEvent *timeEventHead;
    int stop;
    void *apidata; /* This is used for polling API specific data */
    aeBeforeSleepProc *beforesleep;
    aeBeforeSleepProc *aftersleep;
} aeEventLoop;

/* Prototypes */
// 创建事件循环
aeEventLoop *aeCreateEventLoop(int setsize);
void aeDeleteEventLoop(aeEventLoop *eventLoop);
void aeStop(aeEventLoop *eventLoop);
int aeCreateFileEvent(aeEventLoop *eventLoop, int fd, int mask,
        aeFileProc *proc, void *clientData);
void aeDeleteFileEvent(aeEventLoop *eventLoop, int fd, int mask);
int aeGetFileEvents(aeEventLoop *eventLoop, int fd);
long long aeCreateTimeEvent(aeEventLoop *eventLoop, long long milliseconds,
        aeTimeProc *proc, void *clientData,
        aeEventFinalizerProc *finalizerProc);
int aeDeleteTimeEvent(aeEventLoop *eventLoop, long long id);
int aeProcessEvents(aeEventLoop *eventLoop, int flags);
int aeWait(int fd, int mask, long long milliseconds);
void aeMain(aeEventLoop *eventLoop);
char *aeGetApiName(void);
void aeSetBeforeSleepProc(aeEventLoop *eventLoop, aeBeforeSleepProc *beforesleep);
void aeSetAfterSleepProc(aeEventLoop *eventLoop, aeBeforeSleepProc *aftersleep);
int aeGetSetSize(aeEventLoop *eventLoop);
int aeResizeSetSize(aeEventLoop *eventLoop, int setsize);

#endif
