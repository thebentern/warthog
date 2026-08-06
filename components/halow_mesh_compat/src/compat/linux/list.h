/*
 * Copyright 2026 Warthog contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * <linux/list.h> compat — the classic Linux intrusive doubly-linked list,
 * trimmed to the operations the morse_driver dot11ah/mesh sources use.
 */
#ifndef _COMPAT_LINUX_LIST_H_
#define _COMPAT_LINUX_LIST_H_

#include <linux/types.h>
#include <linux/kernel.h>

struct list_head {
    struct list_head *next, *prev;
};

#define LIST_HEAD_INIT(name) { &(name), &(name) }
#define LIST_HEAD(name) struct list_head name = LIST_HEAD_INIT(name)

static inline void INIT_LIST_HEAD(struct list_head *list)
{
    list->next = list;
    list->prev = list;
}

static inline void __list_add(struct list_head *n,
                              struct list_head *prev,
                              struct list_head *next)
{
    next->prev = n;
    n->next = next;
    n->prev = prev;
    prev->next = n;
}

static inline void list_add(struct list_head *n, struct list_head *head)
{
    __list_add(n, head, head->next);
}

static inline void list_add_tail(struct list_head *n, struct list_head *head)
{
    __list_add(n, head->prev, head);
}

static inline void __list_del(struct list_head *prev, struct list_head *next)
{
    next->prev = prev;
    prev->next = next;
}

static inline void list_del(struct list_head *entry)
{
    __list_del(entry->prev, entry->next);
    entry->next = NULL;
    entry->prev = NULL;
}

static inline void list_del_init(struct list_head *entry)
{
    __list_del(entry->prev, entry->next);
    INIT_LIST_HEAD(entry);
}

static inline int list_empty(const struct list_head *head)
{
    return head->next == head;
}

#define list_entry(ptr, type, member) container_of(ptr, type, member)

#define list_first_entry(ptr, type, member) \
    list_entry((ptr)->next, type, member)

#define list_for_each(pos, head) \
    for (pos = (head)->next; pos != (head); pos = pos->next)

#define list_for_each_entry(pos, head, member)                          \
    for (pos = list_entry((head)->next, typeof(*pos), member);          \
         &pos->member != (head);                                        \
         pos = list_entry(pos->member.next, typeof(*pos), member))

#define list_for_each_entry_safe(pos, n, head, member)                  \
    for (pos = list_entry((head)->next, typeof(*pos), member),          \
         n = list_entry(pos->member.next, typeof(*pos), member);        \
         &pos->member != (head);                                        \
         pos = n, n = list_entry(n->member.next, typeof(*n), member))

/* mac80211 mesh uses hlist for the path table; provide a minimal alias. */
struct hlist_head { struct hlist_node *first; };
struct hlist_node { struct hlist_node *next, **pprev; };

#endif /* _COMPAT_LINUX_LIST_H_ */
