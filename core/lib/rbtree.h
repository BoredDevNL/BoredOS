// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.

#ifndef BOREDOS_RBTREE_H
#define BOREDOS_RBTREE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define RB_RED   0
#define RB_BLACK 1

typedef struct rb_node {
    struct rb_node *rb_parent;
    struct rb_node *rb_left;
    struct rb_node *rb_right;
    uint32_t rb_color;
} rb_node_t;

typedef struct rb_root {
    rb_node_t *rb_node;
} rb_root_t;

#define RB_ROOT (rb_root_t){ .rb_node = NULL }

#define rb_parent(r)   ((r)->rb_parent)
#define rb_color(r)    ((r)->rb_color)
#define rb_is_red(r)   ((r) && (r)->rb_color == RB_RED)
#define rb_is_black(r) (!(r) || (r)->rb_color == RB_BLACK)
#define rb_set_red(r)   do { if (r) (r)->rb_color = RB_RED; } while (0)
#define rb_set_black(r) do { if (r) (r)->rb_color = RB_BLACK; } while (0)

#ifndef container_of
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))
#endif

#define rb_entry(ptr, type, member) container_of(ptr, type, member)

typedef void (*rb_augment_cb)(rb_node_t *node);

static inline void rb_link_node(rb_node_t *node, rb_node_t *parent, rb_node_t **rb_link) {
    node->rb_parent = parent;
    node->rb_color = RB_RED;
    node->rb_left = NULL;
    node->rb_right = NULL;
    *rb_link = node;
}

void rb_insert_color(rb_root_t *root, rb_node_t *node);
void rb_insert_augmented(rb_root_t *root, rb_node_t *node, rb_augment_cb augment_cb);

void rb_erase(rb_root_t *root, rb_node_t *node);
void rb_erase_augmented(rb_root_t *root, rb_node_t *node, rb_augment_cb augment_cb);

void rb_replace_node(rb_root_t *root, rb_node_t *victim, rb_node_t *new_node);

rb_node_t *rb_first(const rb_root_t *root);
rb_node_t *rb_last(const rb_root_t *root);
rb_node_t *rb_next(const rb_node_t *node);
rb_node_t *rb_prev(const rb_node_t *node);

bool rb_verify(const rb_root_t *root);

#endif // BOREDOS_RBTREE_H
