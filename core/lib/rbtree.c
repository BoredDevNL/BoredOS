// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.

#include "rbtree.h"

static void rb_rotate_left(rb_root_t *root, rb_node_t *node, rb_augment_cb augment_cb) {
    rb_node_t *right = node->rb_right;
    node->rb_right = right->rb_left;

    if (right->rb_left) {
        right->rb_left->rb_parent = node;
    }

    right->rb_parent = node->rb_parent;

    if (!node->rb_parent) {
        root->rb_node = right;
    } else if (node == node->rb_parent->rb_left) {
        node->rb_parent->rb_left = right;
    } else {
        node->rb_parent->rb_right = right;
    }

    right->rb_left = node;
    node->rb_parent = right;

    if (augment_cb) {
        augment_cb(node);
        augment_cb(right);
    }
}

static void rb_rotate_right(rb_root_t *root, rb_node_t *node, rb_augment_cb augment_cb) {
    rb_node_t *left = node->rb_left;
    node->rb_left = left->rb_right;

    if (left->rb_right) {
        left->rb_right->rb_parent = node;
    }

    left->rb_parent = node->rb_parent;

    if (!node->rb_parent) {
        root->rb_node = left;
    } else if (node == node->rb_parent->rb_right) {
        node->rb_parent->rb_right = left;
    } else {
        node->rb_parent->rb_left = left;
    }

    left->rb_right = node;
    node->rb_parent = left;

    if (augment_cb) {
        augment_cb(node);
        augment_cb(left);
    }
}

void rb_insert_augmented(rb_root_t *root, rb_node_t *node, rb_augment_cb augment_cb) {
    while (node->rb_parent && node->rb_parent->rb_color == RB_RED) {
        rb_node_t *parent = node->rb_parent;
        rb_node_t *grandparent = parent->rb_parent;

        if (parent == grandparent->rb_left) {
            rb_node_t *uncle = grandparent->rb_right;

            if (uncle && uncle->rb_color == RB_RED) {
                parent->rb_color = RB_BLACK;
                uncle->rb_color = RB_BLACK;
                grandparent->rb_color = RB_RED;
                node = grandparent;
            } else {
                if (node == parent->rb_right) {
                    node = parent;
                    rb_rotate_left(root, node, augment_cb);
                    parent = node->rb_parent;
                    grandparent = parent->rb_parent;
                }
                parent->rb_color = RB_BLACK;
                grandparent->rb_color = RB_RED;
                rb_rotate_right(root, grandparent, augment_cb);
            }
        } else {
            rb_node_t *uncle = grandparent->rb_left;

            if (uncle && uncle->rb_color == RB_RED) {
                parent->rb_color = RB_BLACK;
                uncle->rb_color = RB_BLACK;
                grandparent->rb_color = RB_RED;
                node = grandparent;
            } else {
                if (node == parent->rb_left) {
                    node = parent;
                    rb_rotate_right(root, node, augment_cb);
                    parent = node->rb_parent;
                    grandparent = parent->rb_parent;
                }
                parent->rb_color = RB_BLACK;
                grandparent->rb_color = RB_RED;
                rb_rotate_left(root, grandparent, augment_cb);
            }
        }
    }
    if (root->rb_node) {
        root->rb_node->rb_color = RB_BLACK;
    }
}

void rb_insert_color(rb_root_t *root, rb_node_t *node) {
    rb_insert_augmented(root, node, NULL);
}

static void rb_erase_fixup(rb_root_t *root, rb_node_t *node, rb_node_t *parent, rb_augment_cb augment_cb) {
    while ((!node || node->rb_color == RB_BLACK) && node != root->rb_node) {
        if (node == parent->rb_left) {
            rb_node_t *sibling = parent->rb_right;

            if (sibling && sibling->rb_color == RB_RED) {
                sibling->rb_color = RB_BLACK;
                parent->rb_color = RB_RED;
                rb_rotate_left(root, parent, augment_cb);
                sibling = parent->rb_right;
            }

            if ((!sibling->rb_left || sibling->rb_left->rb_color == RB_BLACK) &&
                (!sibling->rb_right || sibling->rb_right->rb_color == RB_BLACK)) {
                sibling->rb_color = RB_RED;
                node = parent;
                parent = node->rb_parent;
            } else {
                if (!sibling->rb_right || sibling->rb_right->rb_color == RB_BLACK) {
                    if (sibling->rb_left) sibling->rb_left->rb_color = RB_BLACK;
                    sibling->rb_color = RB_RED;
                    rb_rotate_right(root, sibling, augment_cb);
                    sibling = parent->rb_right;
                }
                sibling->rb_color = parent->rb_color;
                parent->rb_color = RB_BLACK;
                if (sibling->rb_right) sibling->rb_right->rb_color = RB_BLACK;
                rb_rotate_left(root, parent, augment_cb);
                node = root->rb_node;
                break;
            }
        } else {
            rb_node_t *sibling = parent->rb_left;

            if (sibling && sibling->rb_color == RB_RED) {
                sibling->rb_color = RB_BLACK;
                parent->rb_color = RB_RED;
                rb_rotate_right(root, parent, augment_cb);
                sibling = parent->rb_left;
            }

            if ((!sibling->rb_left || sibling->rb_left->rb_color == RB_BLACK) &&
                (!sibling->rb_right || sibling->rb_right->rb_color == RB_BLACK)) {
                sibling->rb_color = RB_RED;
                node = parent;
                parent = node->rb_parent;
            } else {
                if (!sibling->rb_left || sibling->rb_left->rb_color == RB_BLACK) {
                    if (sibling->rb_right) sibling->rb_right->rb_color = RB_BLACK;
                    sibling->rb_color = RB_RED;
                    rb_rotate_left(root, sibling, augment_cb);
                    sibling = parent->rb_left;
                }
                sibling->rb_color = parent->rb_color;
                parent->rb_color = RB_BLACK;
                if (sibling->rb_left) sibling->rb_left->rb_color = RB_BLACK;
                rb_rotate_right(root, parent, augment_cb);
                node = root->rb_node;
                break;
            }
        }
    }
    if (node) node->rb_color = RB_BLACK;
}

void rb_erase_augmented(rb_root_t *root, rb_node_t *node, rb_augment_cb augment_cb) {
    if (!root || !node || !root->rb_node) return;

    rb_node_t *node_to_remove = node;
    rb_node_t *child = NULL;
    rb_node_t *parent = NULL;
    uint32_t color = node_to_remove->rb_color;

    if (!node->rb_left) {
        child = node->rb_right;
        parent = node->rb_parent;

        if (!parent) root->rb_node = child;
        else if (parent->rb_left == node) parent->rb_left = child;
        else parent->rb_right = child;

        if (child) child->rb_parent = parent;
    } else if (!node->rb_right) {
        child = node->rb_left;
        parent = node->rb_parent;

        if (!parent) root->rb_node = child;
        else if (parent->rb_left == node) parent->rb_left = child;
        else parent->rb_right = child;

        if (child) child->rb_parent = parent;
    } else {
        node_to_remove = node->rb_right;
        while (node_to_remove->rb_left) {
            node_to_remove = node_to_remove->rb_left;
        }

        color = node_to_remove->rb_color;
        child = node_to_remove->rb_right;
        parent = node_to_remove->rb_parent;

        if (child) child->rb_parent = parent;

        if (parent == node) {
            parent = node_to_remove;
        } else {
            if (parent->rb_left == node_to_remove) parent->rb_left = child;
            else parent->rb_right = child;
        }

        if (!node->rb_parent) root->rb_node = node_to_remove;
        else if (node->rb_parent->rb_left == node) node->rb_parent->rb_left = node_to_remove;
        else node->rb_parent->rb_right = node_to_remove;

        node_to_remove->rb_parent = node->rb_parent;
        node_to_remove->rb_left = node->rb_left;
        node_to_remove->rb_right = (node->rb_right == node_to_remove) ? child : node->rb_right;
        node_to_remove->rb_color = node->rb_color;

        if (node->rb_left) node->rb_left->rb_parent = node_to_remove;
        if (node->rb_right && node->rb_right != node_to_remove) node->rb_right->rb_parent = node_to_remove;
    }

    if (augment_cb) {
        rb_node_t *p = parent;
        while (p) {
            augment_cb(p);
            p = p->rb_parent;
        }
    }

    if (color == RB_BLACK) {
        rb_erase_fixup(root, child, parent, augment_cb);
    }
}

void rb_erase(rb_root_t *root, rb_node_t *node) {
    rb_erase_augmented(root, node, NULL);
}

void rb_replace_node(rb_root_t *root, rb_node_t *victim, rb_node_t *new_node) {
    rb_node_t *parent = victim->rb_parent;

    if (parent) {
        if (parent->rb_left == victim) parent->rb_left = new_node;
        else parent->rb_right = new_node;
    } else {
        root->rb_node = new_node;
    }

    if (victim->rb_left) victim->rb_left->rb_parent = new_node;
    if (victim->rb_right) victim->rb_right->rb_parent = new_node;

    *new_node = *victim;
}

rb_node_t *rb_first(const rb_root_t *root) {
    if (!root || !root->rb_node) return NULL;
    rb_node_t *n = root->rb_node;
    while (n->rb_left) n = n->rb_left;
    return n;
}

rb_node_t *rb_last(const rb_root_t *root) {
    if (!root || !root->rb_node) return NULL;
    rb_node_t *n = root->rb_node;
    while (n->rb_right) n = n->rb_right;
    return n;
}

rb_node_t *rb_next(const rb_node_t *node) {
    if (!node) return NULL;
    if (node->rb_right) {
        const rb_node_t *n = node->rb_right;
        while (n->rb_left) n = n->rb_left;
        return (rb_node_t *)n;
    }
    const rb_node_t *p = node->rb_parent;
    const rb_node_t *curr = node;
    while (p && curr == p->rb_right) {
        curr = p;
        p = p->rb_parent;
    }
    return (rb_node_t *)p;
}

rb_node_t *rb_prev(const rb_node_t *node) {
    if (!node) return NULL;
    if (node->rb_left) {
        const rb_node_t *n = node->rb_left;
        while (n->rb_right) n = n->rb_right;
        return (rb_node_t *)n;
    }
    const rb_node_t *p = node->rb_parent;
    const rb_node_t *curr = node;
    while (p && curr == p->rb_left) {
        curr = p;
        p = p->rb_parent;
    }
    return (rb_node_t *)p;
}

static bool rb_verify_node_internal(const rb_node_t *node, int *black_height, int current_height) {
    if (!node) {
        if (*black_height == -1) {
            *black_height = current_height;
        } else if (current_height != *black_height) {
            return false;
        }
        return true;
    }

    if (node->rb_color == RB_RED) {
        if (node->rb_left && node->rb_left->rb_color == RB_RED) return false;
        if (node->rb_right && node->rb_right->rb_color == RB_RED) return false;
    } else {
        current_height++;
    }

    return rb_verify_node_internal(node->rb_left, black_height, current_height) &&
           rb_verify_node_internal(node->rb_right, black_height, current_height);
}

bool rb_verify(const rb_root_t *root) {
    if (!root || !root->rb_node) return true;
    if (root->rb_node->rb_color != RB_BLACK) return false;

    int black_height = -1;
    return rb_verify_node_internal(root->rb_node, &black_height, 0);
}
