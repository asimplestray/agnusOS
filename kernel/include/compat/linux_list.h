/*
 * compat/linux_list.h — listas duplamente ligadas intrusivas estilo Linux.
 *
 * Fase 2 Dev 4 — Camada de Compatibilidade.
 *
 * O kernel nativo não possui list_head próprio; este header É a
 * implementação usada por drivers em port (mesmo algoritmo do Linux,
 * self-contained, sem depender de headers linux/ reais).
 */

#ifndef _COMPAT_LINUX_LIST_H_
#define _COMPAT_LINUX_LIST_H_

#include <compat/linux_types.h>

struct list_head {
    struct list_head *prev;
    struct list_head *next;
};

#define LIST_HEAD_INIT(name) { &(name), &(name) }

#define LIST_HEAD(name) \
    struct list_head name = LIST_HEAD_INIT(name)

static inline void INIT_LIST_HEAD(struct list_head *list)
{
    list->prev = list->next = list;
}

static inline int list_empty(const struct list_head *head)
{
    return head->next == head;
}

static inline int list_is_last(const struct list_head *list,
                               const struct list_head *head)
{
    return list->next == head;
}

/* Inserção interna: insere `entry` entre `prev` e `next` */
static inline void __list_add(struct list_head *entry,
                              struct list_head *prev,
                              struct list_head *next)
{
    next->prev = entry;
    entry->next = next;
    entry->prev = prev;
    prev->next = entry;
}

static inline void list_add(struct list_head *entry, struct list_head *head)
{
    __list_add(entry, head, head->next);
}

static inline void list_add_tail(struct list_head *entry, struct list_head *head)
{
    __list_add(entry, head->prev, head);
}

/* Remoção interna */
static inline void __list_del(struct list_head *prev, struct list_head *next)
{
    next->prev = prev;
    prev->next = next;
}

static inline void list_del(struct list_head *entry)
{
    __list_del(entry->prev, entry->next);
    entry->prev = entry->next = NULL;
}

static inline void list_del_init(struct list_head *entry)
{
    __list_del(entry->prev, entry->next);
    INIT_LIST_HEAD(entry);
}

/* Move para outra lista */
static inline void list_move(struct list_head *entry, struct list_head *head)
{
    __list_del(entry->prev, entry->next);
    list_add(entry, head);
}

static inline void list_move_tail(struct list_head *entry, struct list_head *head)
{
    __list_del(entry->prev, entry->next);
    list_add_tail(entry, head);
}

static inline void list_splice(const struct list_head *list, struct list_head *head)
{
    if (!list_empty(list)) {
        struct list_head *first = list->next;
        struct list_head *last  = list->prev;
        struct list_head *at    = head->next;

        first->prev = head;
        head->next  = first;

        last->next = at;
        at->prev   = last;
    }
}

/* Obtenção do struct hospedeiro */
#ifndef list_entry
#define list_entry(ptr, type, member) container_of(ptr, type, member)
#endif

#define list_first_entry(ptr, type, member) \
    list_entry((ptr)->next, type, member)

#define list_last_entry(ptr, type, member) \
    list_entry((ptr)->prev, type, member)

#define list_next_entry(pos, member) \
    list_entry((pos)->member.next, typeof(*(pos)), member)

#define list_prev_entry(pos, member) \
    list_entry((pos)->member.prev, typeof(*(pos)), member)

/* Iteração */
#define list_for_each(pos, head) \
    for (pos = (head)->next; pos != (head); pos = pos->next)

#define list_for_each_prev(pos, head) \
    for (pos = (head)->prev; pos != (head); pos = pos->prev)

#define list_for_each_safe(pos, n, head) \
    for (pos = (head)->next, n = pos->next; pos != (head); \
         pos = n, n = pos->next)

#define list_for_each_entry(pos, head, member)                          \
    for (pos = list_first_entry(head, typeof(*pos), member);            \
         &pos->member != (head);                                        \
         pos = list_next_entry(pos, member))

#define list_for_each_entry_reverse(pos, head, member)                  \
    for (pos = list_last_entry(head, typeof(*pos), member);             \
         &pos->member != (head);                                        \
         pos = list_prev_entry(pos, member))

#define list_for_each_entry_safe(pos, n, head, member)                  \
    for (pos = list_first_entry(head, typeof(*pos), member),            \
         n = list_next_entry(pos, member);                              \
         &pos->member != (head);                                        \
         pos = n, n = list_next_entry(n, member))

#endif /* _COMPAT_LINUX_LIST_H_ */
