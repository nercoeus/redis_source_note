/*
 * Copyright (c) 2009-2012, Pieter Noordhuis <pcnoordhuis at gmail dot com>
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

#ifndef _ZIPLIST_H
#define _ZIPLIST_H

#define ZIPLIST_HEAD 0
#define ZIPLIST_TAIL 1

//-----------------------------------------API---------------------------------------------//
// 创建一个新的压缩列表
unsigned char *ziplistNew(void);
// 将列表2合并到列表1后面
unsigned char *ziplistMerge(unsigned char **first, unsigned char **second);
// 创建一个包含给定值的节点，并将其添加到表头或者表尾
unsigned char *ziplistPush(unsigned char *zl, unsigned char *s, unsigned int slen, int where);
// 返回列表上指定索引的节点
unsigned char *ziplistIndex(unsigned char *zl, int index);
// 返回下一个节点
unsigned char *ziplistNext(unsigned char *zl, unsigned char *p);
// 返回前一个节点
unsigned char *ziplistPrev(unsigned char *zl, unsigned char *p);
// 获取给定节点保存的值
unsigned int ziplistGet(unsigned char *p, unsigned char **sval, unsigned int *slen, long long *lval);
// 将包含给定值的新节点放到指定节点后
unsigned char *ziplistInsert(unsigned char *zl, unsigned char *p, unsigned char *s, unsigned int slen);
// 删除给定的节点
unsigned char *ziplistDelete(unsigned char *zl, unsigned char **p);
// 删除列表中指定范围的节点
unsigned char *ziplistDeleteRange(unsigned char *zl, int index, unsigned int num);
// 比较两个列表开始的特定范围
unsigned int ziplistCompare(unsigned char *p, unsigned char *s, unsigned int slen);
// 在列表中查找并返回包含给定值的节点
unsigned char *ziplistFind(unsigned char *p, unsigned char *vstr, unsigned int vlen, unsigned int skip);
// 返回目前包含的节点数
unsigned int ziplistLen(unsigned char *zl);
// 返回列表目前占用的字节数
size_t ziplistBlobLen(unsigned char *zl);
// 打印列表
void ziplistRepr(unsigned char *zl);

#ifdef REDIS_TEST
int ziplistTest(int argc, char *argv[]);
#endif

#endif /* _ZIPLIST_H */
