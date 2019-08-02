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
#include <math.h> /* isnan(), isinf() */

/*-----------------------------------------------------------------------------
 * String Commands
 *----------------------------------------------------------------------------*/
// 检查 string 的长度书否符合规范，小于 512*1024*1024 (512M)
static int checkStringLength(client *c, long long size) {
    if (size > 512*1024*1024) {
        addReplyError(c,"string exceeds maximum allowed size (512MB)");
        return C_ERR;
    }
    return C_OK;
}

/* The setGenericCommand() function implements the SET operation with different
 * options and variants. This function is called in order to implement the
 * following commands: SET, SETEX, PSETEX, SETNX.
 *
 * 'flags' changes the behavior of the command (NX or XX, see belove).
 *
 * 'expire' represents an expire to set in form of a Redis object as passed
 * by the user. It is interpreted according to the specified 'unit'.
 *
 * 'ok_reply' and 'abort_reply' is what the function will reply to the client
 * if the operation is performed, or when it is not because of NX or
 * XX flags.
 *
 * If ok_reply is NULL "+OK" is used.
 * If abort_reply is NULL, "$-1" is used. */
// set 指令时的 flag
#define OBJ_SET_NO_FLAGS 0    // no flag
#define OBJ_SET_NX (1<<0)     /* Set if key not exists. key 不存在才进行设置 */
#define OBJ_SET_XX (1<<1)     /* Set if key exists. key 存在时进行设置 */
#define OBJ_SET_EX (1<<2)     /* Set if time in seconds is given 到期时间（秒） */
#define OBJ_SET_PX (1<<3)     /* Set if time in ms in given 到期时间（毫秒） */

// 第一项：客户端结构体，第二项：使用了那些参数，第三项：key，第四项：值，第五项：？？？，第六项：是秒还是毫秒
void setGenericCommand(client *c, int flags, robj *key, robj *val, robj *expire, int unit, robj *ok_reply, robj *abort_reply) {
    long long milliseconds = 0; /* initialized to avoid any harmness warning */

    // 存在过期时间
    if (expire) {
        // 按照 longlong 取毫秒，可以转换成秒
        if (getLongLongFromObjectOrReply(c, expire, &milliseconds, NULL) != C_OK)
            return;
        if (milliseconds <= 0) {
            addReplyErrorFormat(c,"invalid expire time in %s",c->cmd->name);
            return;
        }
        if (unit == UNIT_SECONDS) milliseconds *= 1000;
    }
    // NX 和 EX 不会同时存在
    if ((flags & OBJ_SET_NX && lookupKeyWrite(c->db,key) != NULL) ||
        (flags & OBJ_SET_XX && lookupKeyWrite(c->db,key) == NULL))
    {
        // 对客户端进行回应
        addReply(c, abort_reply ? abort_reply : shared.null[c->resp]);
        return;
    }
    // 设置 key，value
    setKey(c->db,key,val);
    // 记录服务器 dirty 操作数，用来保存数据库等等操作
    server.dirty++;
    // 设置过期时间，记录的是时间戳
    if (expire) setExpire(c,c->db,key,mstime()+milliseconds);
    // 键空间通知，通知订阅的客户端等
    notifyKeyspaceEvent(NOTIFY_STRING,"set",key,c->db->id);
    // 键空间通知
    if (expire) notifyKeyspaceEvent(NOTIFY_GENERIC,
        "expire",key,c->db->id);
    // 使用默认的 ok replay（OK）
    addReply(c, ok_reply ? ok_reply : shared.ok);
}

/* SET key value [NX] [XX] [EX <seconds>] [PX <milliseconds>] */
// 解析 set 命令
void setCommand(client *c) {
    int j;
    robj *expire = NULL;
    int unit = UNIT_SECONDS;
    // 用来标记命令的 flag
    int flags = OBJ_SET_NO_FLAGS;
    // 第三个 argc 开始遍历，保存在客户端的结构体中
    for (j = 3; j < c->argc; j++) {
        // 注意 argv 中存储的也是 robj 对象
        char *a = c->argv[j]->ptr;
        // 待解析的参数值
        robj *next = (j == c->argc-1) ? NULL : c->argv[j+1];
        if ((a[0] == 'n' || a[0] == 'N') &&
            (a[1] == 'x' || a[1] == 'X') && a[2] == '\0' &&
            !(flags & OBJ_SET_XX))
        {
            // nx?
            flags |= OBJ_SET_NX;
        } else if ((a[0] == 'x' || a[0] == 'X') &&
                   (a[1] == 'x' || a[1] == 'X') && a[2] == '\0' &&
                   !(flags & OBJ_SET_NX))
        {
            // xx?
            flags |= OBJ_SET_XX;
        } else if ((a[0] == 'e' || a[0] == 'E') &&
                   (a[1] == 'x' || a[1] == 'X') && a[2] == '\0' &&
                   !(flags & OBJ_SET_PX) && next)
        {
            // ex?
            flags |= OBJ_SET_EX;
            unit = UNIT_SECONDS;
            // 保存值，并且增加 j 指向下一个解析项
            expire = next;
            j++;
        } else if ((a[0] == 'p' || a[0] == 'P') &&
                   (a[1] == 'x' || a[1] == 'X') && a[2] == '\0' &&
                   !(flags & OBJ_SET_EX) && next)
        {
            // px?
            flags |= OBJ_SET_PX;
            unit = UNIT_MILLISECONDS;
            // 保存值，并且增加 j 指向下一个解析项
            expire = next;
            j++;
        } else {
            // 出错回应
            addReply(c,shared.syntaxerr);
            return;
        }
    }
    // 对需要存储的值尝试编码压缩内存
    c->argv[2] = tryObjectEncoding(c->argv[2]);
    // 解析完参数
    setGenericCommand(c,flags,c->argv[1],c->argv[2],expire,unit,NULL,NULL);
}

// 执行 SETNX KEY_NAME VALUE
void setnxCommand(client *c) {
    c->argv[2] = tryObjectEncoding(c->argv[2]);
    setGenericCommand(c,OBJ_SET_NX,c->argv[1],c->argv[2],NULL,0,shared.cone,shared.czero);
}

// 执行 SETEX KEY_NAME TIMEOUT VALUE
void setexCommand(client *c) {
    c->argv[3] = tryObjectEncoding(c->argv[3]);
    setGenericCommand(c,OBJ_SET_NO_FLAGS,c->argv[1],c->argv[3],c->argv[2],UNIT_SECONDS,NULL,NULL);
}

// 执行 PSETEX KEY_NAME TIMEOUT VALUE
void psetexCommand(client *c) {
    c->argv[3] = tryObjectEncoding(c->argv[3]);
    setGenericCommand(c,OBJ_SET_NO_FLAGS,c->argv[1],c->argv[3],c->argv[2],UNIT_MILLISECONDS,NULL,NULL);
}

// 执行 GET
int getGenericCommand(client *c) {
    robj *o;
    // 看是否有该数据
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.null[c->resp])) == NULL)
        return C_OK;

    if (o->type != OBJ_STRING) {
        addReply(c,shared.wrongtypeerr);
        return C_ERR;
    } else {
        // 返回 value
        addReplyBulk(c,o);
        return C_OK;
    }
}

// GET KEY_NAME 命令
void getCommand(client *c) {
    getGenericCommand(c);
}

// GETSET KEY_NAME VALUE 命令
void getsetCommand(client *c) {
    if (getGenericCommand(c) == C_ERR) return;
    // 尝试编码压缩空间
    c->argv[2] = tryObjectEncoding(c->argv[2]);
    // 设置新值
    setKey(c->db,c->argv[1],c->argv[2]);
    // 通知订阅
    notifyKeyspaceEvent(NOTIFY_STRING,"set",c->argv[1],c->db->id);
    // dirty operate
    server.dirty++;
}

// 执行 SETRANGE KEY_NAME OFFSET VALUE
void setrangeCommand(client *c) {
    robj *o;
    long offset;
    sds value = c->argv[3]->ptr;
    // 获取 offset 值
    if (getLongFromObjectOrReply(c,c->argv[2],&offset,NULL) != C_OK)
        return;
    // offset 要 >= 0
    if (offset < 0) {
        addReplyError(c,"offset is out of range");
        return;
    }
    // 写锁，获取值
    o = lookupKeyWrite(c->db,c->argv[1]);
    if (o == NULL) {
        // 没有该数据
        /* Return 0 when setting nothing on a non-existing string */
        if (sdslen(value) == 0) {
            // 返回 zero
            addReply(c,shared.czero);
            return;
        }

        /* Return when the resulting string exceeds allowed size */
        if (checkStringLength(c,offset+sdslen(value)) != C_OK)
            return;
        // 创建一个新 robj 用来保存 VALUE
        o = createObject(OBJ_STRING,sdsnewlen(NULL, offset+sdslen(value)));
        // 添加到数据库中
        dbAdd(c->db,c->argv[1],o);
    } else {
        size_t olen;

        /* Key exists, check type */
        // 检测类型
        if (checkType(c,o,OBJ_STRING))
            return;

        /* Return existing string length when setting nothing */
        olen = stringObjectLen(o);
        // 存在 value 为空，不需要设置
        if (sdslen(value) == 0) {
            addReplyLongLong(c,olen);
            return;
        }

        /* Return when the resulting string exceeds allowed size */
        // 设置后会超过 string 长度限制
        if (checkStringLength(c,offset+sdslen(value)) != C_OK)
            return;

        /* Create a copy when the object is shared or encoded. */
        // 该对象取消共享，如果已经共享，创建一个新数据进行操作
        o = dbUnshareStringValue(c->db,c->argv[1],o);
    }
    // 这里 value 的 len 肯定大于 0 
    if (sdslen(value) > 0) {
        // 扩展对象
        o->ptr = sdsgrowzero(o->ptr,offset+sdslen(value));
        // 使用库函数进行复制
        memcpy((char*)o->ptr+offset,value,sdslen(value));
        // 向数据库发送键被修改的信号
        signalModifiedKey(c->db,c->argv[1]);
        // 发送事件通知
        notifyKeyspaceEvent(NOTIFY_STRING,
            "setrange",c->argv[1],c->db->id);
        server.dirty++;
    }
    // 返回新字符串给客户端
    addReplyLongLong(c,sdslen(o->ptr));
}

// 执行 GETRANGE KEY_VALUE START END （闭区间）
void getrangeCommand(client *c) {
    robj *o;
    long long start, end;
    char *str, llbuf[32];
    size_t strlen;
    // 获取 start end
    if (getLongLongFromObjectOrReply(c,c->argv[2],&start,NULL) != C_OK)
        return;
    if (getLongLongFromObjectOrReply(c,c->argv[3],&end,NULL) != C_OK)
        return;
    // 获取 OBJ_STRING 值
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.emptybulk)) == NULL ||
        checkType(c,o,OBJ_STRING)) return;
    // 获取值以及对应长度
    if (o->encoding == OBJ_ENCODING_INT) {
        // INT 类型转化为 string 取长度
        str = llbuf;
        strlen = ll2string(llbuf,sizeof(llbuf),(long)o->ptr);
    } else {
        str = o->ptr;
        strlen = sdslen(str);
    }

    /* Convert negative indexes */
    // start end 必须合法
    if (start < 0 && end < 0 && start > end) {
        addReply(c,shared.emptybulk);
        return;
    }
    //可以取负数，一通转换
    if (start < 0) start = strlen+start;
    if (end < 0) end = strlen+end;
    if (start < 0) start = 0;
    if (end < 0) end = 0;
    if ((unsigned long long)end >= strlen) end = strlen-1;

    /* Precondition: end >= 0 && end < strlen, so the only condition where
     * nothing can be returned is: start > end. */
    if (start > end || strlen == 0) {
        // 不合理返回空值
        addReply(c,shared.emptybulk);
    } else {
        // 返回一段 buf
        addReplyBulkCBuffer(c,(char*)str+start,end-start+1);
    }
}

// 执行 MGET KEY1 KEY2 .. KEYN
void mgetCommand(client *c) {
    int j;

    addReplyArrayLen(c,c->argc-1);
    // 用一个 for 循环进行返回
    for (j = 1; j < c->argc; j++) {
        robj *o = lookupKeyRead(c->db,c->argv[j]);
        if (o == NULL) {
            addReplyNull(c);
        } else {
            if (o->type != OBJ_STRING) {
                addReplyNull(c);
            } else {
                // 这里返回 value
                addReplyBulk(c,o);
            }
        }
    }
}

void msetGenericCommand(client *c, int nx) {
    int j;
    // 一个 key 对应一个 value
    if ((c->argc % 2) == 0) {
        addReplyError(c,"wrong number of arguments for MSET");
        return;
    }

    /* Handle the NX flag. The MSETNX semantic is to return zero and don't
     * set anything if at least one key alerady exists. */
    // 如果是 nx 不可以存在
    if (nx) {
        for (j = 1; j < c->argc; j += 2) {
            if (lookupKeyWrite(c->db,c->argv[j]) != NULL) {
                addReply(c, shared.czero);
                return;
            }
        }
    }
    // 一个一个 set 即可
    for (j = 1; j < c->argc; j += 2) {
        c->argv[j+1] = tryObjectEncoding(c->argv[j+1]);
        setKey(c->db,c->argv[j],c->argv[j+1]);
        notifyKeyspaceEvent(NOTIFY_STRING,"set",c->argv[j],c->db->id);
    }
    server.dirty += (c->argc-1)/2;
    addReply(c, nx ? shared.cone : shared.ok);
}

// 执行 MGET KEY1 VALUE1 KEY2 VALUE2 .. KEYN VALUEN
void msetCommand(client *c) {
    msetGenericCommand(c,0);
}

// 执行 MSETNX key1 value1 key2 value2 .. keyN valueN
void msetnxCommand(client *c) {
    msetGenericCommand(c,1);
}

// 给对应的值添加 incr eg: incr = 1,value = 99, => value = 100
void incrDecrCommand(client *c, long long incr) {
    long long value, oldvalue;
    robj *o, *new;
    // 获取 string 编码的 int 对象
    o = lookupKeyWrite(c->db,c->argv[1]);
    if (o != NULL && checkType(c,o,OBJ_STRING)) return;
    // 取值
    if (getLongLongFromObjectOrReply(c,o,&value,NULL) != C_OK) return;

    oldvalue = value;
    // 各种判断
    if ((incr < 0 && oldvalue < 0 && incr < (LLONG_MIN-oldvalue)) ||
        (incr > 0 && oldvalue > 0 && incr > (LLONG_MAX-oldvalue))) {
        addReplyError(c,"increment or decrement would overflow");
        return;
    }
    // 直接 + 
    value += incr;
    
    if (o && o->refcount == 1 && o->encoding == OBJ_ENCODING_INT &&
        (value < 0 || value >= OBJ_SHARED_INTEGERS) &&
        value >= LONG_MIN && value <= LONG_MAX)
    {
        // value 不能使用系统的共享数据，可以用 long 存储
        new = o;
        o->ptr = (void*)((long)value);
    } else {
        // 创建 string 编码的 longlong 对象
        new = createStringObjectFromLongLongForValue(value);
        if (o) {
            // 已经存在直接覆盖
            dbOverwrite(c->db,c->argv[1],new);
        } else {
            // 不存在添加
            dbAdd(c->db,c->argv[1],new);
        }
    }
    signalModifiedKey(c->db,c->argv[1]);
    notifyKeyspaceEvent(NOTIFY_STRING,"incrby",c->argv[1],c->db->id);
    server.dirty++;
    addReply(c,shared.colon);
    addReply(c,new);
    addReply(c,shared.crlf);
}

// 执行 INCR KEY_NAME
void incrCommand(client *c) {
    incrDecrCommand(c,1);
}

// 执行 DECR KEY_NAME
void decrCommand(client *c) {
    incrDecrCommand(c,-1);
}

// 执行 INCRBY KEY_NAME INCR_AMOUNT
void incrbyCommand(client *c) {
    long long incr;

    if (getLongLongFromObjectOrReply(c, c->argv[2], &incr, NULL) != C_OK) return;
    incrDecrCommand(c,incr);
}

// 执行 DECRBY KEY_NAME DECR_AMOUNT
void decrbyCommand(client *c) {
    long long incr;

    if (getLongLongFromObjectOrReply(c, c->argv[2], &incr, NULL) != C_OK) return;
    incrDecrCommand(c,-incr);
}

// 执行 INCRBYFLOAT KEY_NAME INCR_AMOUNT
void incrbyfloatCommand(client *c) {
    long double incr, value;
    robj *o, *new, *aux;
    
    // 获取 string 编码的对象
    o = lookupKeyWrite(c->db,c->argv[1]);
    if (o != NULL && checkType(c,o,OBJ_STRING)) return;
    // 取值
    if (getLongDoubleFromObjectOrReply(c,o,&value,NULL) != C_OK ||
        getLongDoubleFromObjectOrReply(c,c->argv[2],&incr,NULL) != C_OK)
        return;

    value += incr;
    // 是否溢出
    if (isnan(value) || isinf(value)) {
        addReplyError(c,"increment would produce NaN or Infinity");
        return;
    }
    // 创建新 robj
    new = createStringObjectFromLongDouble(value,1);
    // 存入数据库
    if (o)
        dbOverwrite(c->db,c->argv[1],new);
    else
        dbAdd(c->db,c->argv[1],new);
    // 向数据库发送值变更的消息
    signalModifiedKey(c->db,c->argv[1]);
    // 发送事件通知
    notifyKeyspaceEvent(NOTIFY_STRING,"incrbyfloat",c->argv[1],c->db->id);
    server.dirty++;
    // 回复
    addReplyBulk(c,new);

    /* Always replicate INCRBYFLOAT as a SET command with the final value
     * in order to make sure that differences in float precision or formatting
     * will not create differences in replicas or after an AOF restart. */
    // 在传播 INCRBYFLOAT 命令时，总是用 SET 命令来替换 INCRBYFLOAT 命令
    // 从而防止因为不同的浮点精度和格式化造成 AOF 重启时的数据不一致 *****
    aux = createStringObject("SET",3);
    rewriteClientCommandArgument(c,0,aux);
    decrRefCount(aux);
    rewriteClientCommandArgument(c,2,new);
}

// 执行 APPEND KEY_NAME NEW_VALUE
void appendCommand(client *c) {
    size_t totlen;
    robj *o, *append;
    // 取出操作对象
    o = lookupKeyWrite(c->db,c->argv[1]);
    if (o == NULL) {
        /* Create the key */
        // 键值对不存在，创建新对象添加到数据库即可
        c->argv[2] = tryObjectEncoding(c->argv[2]);
        dbAdd(c->db,c->argv[1],c->argv[2]);
        incrRefCount(c->argv[2]);
        totlen = stringObjectLen(c->argv[2]);
    } else {
        /* Key exists, check type */
        // 只操作 string 编码对象
        if (checkType(c,o,OBJ_STRING))
            return;

        /* "append" is an argument, so always an sds */
        append = c->argv[2];
        // 新长度
        totlen = stringObjectLen(o)+sdslen(append->ptr);
        // 确保新 string 不会太长
        if (checkStringLength(c,totlen) != C_OK)
            return;

        /* Append the value */
        // 执行 append 操作
        // 该对象取消共享，如果已经共享，创建一个新数据进行操作
        o = dbUnshareStringValue(c->db,c->argv[1],o);
        // 使用 sds 进行追加
        o->ptr = sdscatlen(o->ptr,append->ptr,sdslen(append->ptr));
        totlen = sdslen(o->ptr);
    }
    // 向数据库发送修改键信号
    signalModifiedKey(c->db,c->argv[1]);
    // 发送事件通知
    notifyKeyspaceEvent(NOTIFY_STRING,"append",c->argv[1],c->db->id);
    server.dirty++;
    // 回复
    addReplyLongLong(c,totlen);
}

// 执行 STRLEN KEY_NAME
void strlenCommand(client *c) {
    robj *o;
    // 不为空且是 OBJ_STRING 类型编码
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,o,OBJ_STRING)) return;
    addReplyLongLong(c,stringObjectLen(o));
}
