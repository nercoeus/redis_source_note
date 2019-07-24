/* adlist.c - A generic doubly linked list implementation
 *
 * Copyright (c) 2006-2010, Salvatore Sanfilippo <antirez at gmail dot com>
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

//大家对于双端链表已经很熟悉了，这里主要看一下redis实现的链表有何高效之处以及特有技巧
#include <stdlib.h>
#include "adlist.h"
#include "zmalloc.h"

/* Create a new list. The created list can be freed with
 * AlFreeList(), but private value of every node need to be freed
 * by the user before to call AlFreeList().
 *
 * On error, NULL is returned. Otherwise the pointer to the new list. */
//创建一个新的链表，该函数返回一个list指针
list *listCreate(void)
{
    struct list *list;    //先拿一个链表指针

    if ((list = zmalloc(sizeof(*list))) == NULL)      //对list进行解引用，拿到list头节点的大小
        return NULL;
    list->head = list->tail = NULL;                   //以下一次对头部数据进行设置，这里包括头尾指针，长度，以及数据对应的处理函数，直接返回该list头部指针
    list->len = 0;
    list->dup = NULL;
    list->free = NULL;
    list->match = NULL;
    return list;
}

/* Remove all the elements from the list without destroying the list itself. */
//移除list中的全部节点，但是不删除该list，没有返回值
void listEmpty(list *list)
{
    unsigned long len;
    listNode *current, *next;
    //拿到删除开始节点，以及链表长度
    current = list->head;
    len = list->len;
    //开始遍历删除
    while(len--) {
        next = current->next;
        //这里注意：使用的是头部节点中存储的free函数
        if (list->free) list->free(current->value);
        //使用zfree来释放节点空间
        zfree(current);
        current = next;
    }
    //稍稍设置一下即可
    list->head = list->tail = NULL;
    list->len = 0;
}

/* Free the whole list.
 *
 * This function can't fail. */
//释放整个链表（该函数不可以fail）
void listRelease(list *list)
{
    listEmpty(list);
    zfree(list);
}

/* Add a new node to the list, to head, containing the specified 'value'
 * pointer as value.
 *
 * On error, NULL is returned and no operation is performed (i.e. the
 * list remains unaltered).
 * On success the 'list' pointer you pass to the function is returned. */
//添加新节点到list头部
list *listAddNodeHead(list *list, void *value)
{
    listNode *node;
    //新结点内存分配失败，这里直接返回NULL来表示失败
    if ((node = zmalloc(sizeof(*node))) == NULL)
        return NULL;
    node->value = value;
    //链表中有无节点的两种插入方式
    if (list->len == 0) {
        list->head = list->tail = node;
        node->prev = node->next = NULL;
    } else {
        node->prev = NULL;
        node->next = list->head;
        list->head->prev = node;
        list->head = node;
    }
    list->len++;
    return list;
}

/* Add a new node to the list, to tail, containing the specified 'value'
 * pointer as value.
 *
 * On error, NULL is returned and no operation is performed (i.e. the
 * list remains unaltered).
 * On success the 'list' pointer you pass to the function is returned. */
//插入节点到末尾，因为是双端链表，逻辑几乎上一个函数一模一样
list *listAddNodeTail(list *list, void *value)
{
    listNode *node;

    if ((node = zmalloc(sizeof(*node))) == NULL)
        return NULL;
    node->value = value;
    if (list->len == 0) {
        list->head = list->tail = node;
        node->prev = node->next = NULL;
    } else {
        node->prev = list->tail;
        node->next = NULL;
        list->tail->next = node;
        list->tail = node;
    }
    list->len++;
    return list;
}

//向链表某节点的前或后插入节点，after控制是前还是后，这个函数的逻辑还是很好理解的，画个图理解一下就差不多了^_^
list *listInsertNode(list *list, listNode *old_node, void *value, int after) {
    listNode *node;

    if ((node = zmalloc(sizeof(*node))) == NULL)
        return NULL;
    node->value = value;
    //这块控制新节点的两个指针
    if (after) {
        node->prev = old_node;
        node->next = old_node->next;
        if (list->tail == old_node) {
            list->tail = node;
        }
    } else {
        node->next = old_node;
        node->prev = old_node->prev;
        if (list->head == old_node) {
            list->head = node;
        }
    }
    //这块控制原链表中指向新节点
    if (node->prev != NULL) {
        node->prev->next = node;
    }
    if (node->next != NULL) {
        node->next->prev = node;
    }
    list->len++;
    return list;
}

/* Remove the specified node from the specified list.
 * It's up to the caller to free the private value of the node.
 *
 * This function can't fail. */
//删除一个节点，这里直接把要删除节点的地址当作参数传入进来
void listDelNode(list *list, listNode *node)
{
    //前节点不为空，为空的话需要更新链表头的头指针
    if (node->prev)
        node->prev->next = node->next;
    else
        list->head = node->next;
    //后节点不为空，为空的话需要更新链表头的尾指针
    if (node->next)
        node->next->prev = node->prev;
    else
        list->tail = node->prev;
    //调用zfree进行链表节点的释放
    if (list->free) list->free(node->value);
    zfree(node);
    list->len--;
}

/* Returns a list iterator 'iter'. After the initialization every
 * call to listNext() will return the next element of the list.
 *
 * This function can't fail. */
//获取链表前行或反向的迭代器（这个迭代器和STL的相差甚远）
listIter *listGetIterator(list *list, int direction)
{
    listIter *iter;

    if ((iter = zmalloc(sizeof(*iter))) == NULL) return NULL;
    if (direction == AL_START_HEAD)
        iter->next = list->head;
    else
        iter->next = list->tail;
    iter->direction = direction;
    return iter;
}

/* Release the iterator memory */
//释放一个迭代器
void listReleaseIterator(listIter *iter) {
    zfree(iter);
}

/* Create an iterator in the list private iterator structure */
//设置一个前向迭代器
void listRewind(list *list, listIter *li) {
    li->next = list->head;
    li->direction = AL_START_HEAD;
}
//设置一个反向迭代器
void listRewindTail(list *list, listIter *li) {
    li->next = list->tail;
    li->direction = AL_START_TAIL;
}

/* Return the next element of an iterator.
 * It's valid to remove the currently returned element using
 * listDelNode(), but not to remove other elements.
 *
 * The function returns a pointer to the next element of the list,
 * or NULL if there are no more elements, so the classical usage patter
 * is:
 *
 * iter = listGetIterator(list,<direction>);
 * while ((node = listNext(iter)) != NULL) {
 *     doSomethingWith(listNodeValue(node));
 * }
 *
 * */
//返回迭代器的下一个节点，考虑迭代器的方向以及链表边界即可，这里链表两端的指针均为NULL，很好判断
listNode *listNext(listIter *iter)
{
    listNode *current = iter->next;

    if (current != NULL) {
        if (iter->direction == AL_START_HEAD)
            iter->next = current->next;
        else
            iter->next = current->prev;
    }
    return current;
}

/* Duplicate the whole list. On out of memory NULL is returned.
 * On success a copy of the original list is returned.
 *
 * The 'Dup' method set with listSetDupMethod() function is used
 * to copy the node value. Otherwise the same pointer value of
 * the original node is used as value of the copied node.
 *
 * The original list both on success or error is never modified. */
//复制一个链表，返回新链表的指针
list *listDup(list *orig)
{
    list *copy;
    listIter iter;
    listNode *node;

    if ((copy = listCreate()) == NULL)
        return NULL;
    //先复制原链表的三个处理函数
    copy->dup = orig->dup;
    copy->free = orig->free;
    copy->match = orig->match;
    listRewind(orig, &iter);
    while((node = listNext(&iter)) != NULL) {
        void *value;

        if (copy->dup) {
            value = copy->dup(node->value);
            //复制节点失败，直接free整个list放回NULL
            if (value == NULL) {
                listRelease(copy);
                return NULL;
            }
        } else
            value = node->value;
        //将节点添加到新链表的末尾
        if (listAddNodeTail(copy, value) == NULL) {
            listRelease(copy);
            return NULL;
        }
    }
    return copy;
}

/* Search the list for a node matching a given key.
 * The match is performed using the 'match' method
 * set with listSetMatchMethod(). If no 'match' method
 * is set, the 'value' pointer of every node is directly
 * compared with the 'key' pointer.
 *
 * On success the first matching node pointer is returned
 * (search starts from head). If no matching node exists
 * NULL is returned. */
//在链表中查找key，并返回查找到的节点
listNode *listSearchKey(list *list, void *key)
{
    listIter iter;
    listNode *node;

    listRewind(list, &iter);  //首先，设置一个向前的迭代器
    while((node = listNext(&iter)) != NULL) {
        //使用match函数来进行value的比较
        if (list->match) {
            if (list->match(node->value, key)) {
                return node;
            }
        } else {
            //没有match则通过比较指针地址来进行判断
            if (key == node->value) {
                return node;
            }
        }
    }
    return NULL;
}

/* Return the element at the specified zero-based index
 * where 0 is the head, 1 is the element next to head
 * and so on. Negative integers are used in order to count
 * from the tail, -1 is the last element, -2 the penultimate
 * and so on. If the index is out of range NULL is returned. */
//返回第index节点的指针，这里可以使用负数来进行反向取值，-1为尾节点，代码很简洁
listNode *listIndex(list *list, long index) {
    listNode *n;

    if (index < 0) {
        index = (-index)-1;
        n = list->tail;
        while(index-- && n) n = n->prev;  //这里通过循环来取节点，判断条件包括两部分，小细节
    } else {
        n = list->head;
        while(index-- && n) n = n->next;
    }
    return n;
}

/* Rotate the list removing the tail node and inserting it to the head. */
//把链表尾节点移动到头部
void listRotate(list *list) {
    listNode *tail = list->tail;

    if (listLength(list) <= 1) return;

    /* Detach current tail */
    //总共四步指针操作即可
    list->tail = tail->prev;
    list->tail->next = NULL;
    /* Move it as head */
    list->head->prev = tail;
    tail->prev = NULL;
    tail->next = list->head;
    list->head = tail;
}

/* Add all the elements of the list 'o' at the end of the
 * list 'l'. The list 'other' remains empty but otherwise valid. */
//该函数把链表 o 中的节点放到链表 l 的末尾，并将 o 置为空链表
void listJoin(list *l, list *o) {
    //一系列的指针操作来移动节点
    if (o->head)
        o->head->prev = l->tail;

    if (l->tail)
        l->tail->next = o->head;
    else
        l->head = o->head;

    if (o->tail) l->tail = o->tail;
    l->len += o->len;

    /* Setup other as an empty list. */
    //设置链表 o 为空
    o->head = o->tail = NULL;
    o->len = 0;
}
