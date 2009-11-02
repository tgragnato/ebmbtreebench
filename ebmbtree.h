/*
 * Elastic Binary Trees - macros and structures for Multi-Byte data nodes.
 * Version 5.0
 * (C) 2002-2009 - Willy Tarreau <w@1wt.eu>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifndef _EBMBTREE_H
#define _EBMBTREE_H

#include <string.h>
#include "ebtree.h"

/* Return the structure of type <type> whose member <member> points to <ptr> */
#define ebmb_entry(ptr, type, member) container_of(ptr, type, member)

#define EBMB_ROOT	EB_ROOT
#define EBMB_TREE_HEAD	EB_TREE_HEAD

/* This structure carries a node, a leaf, and a key. It must start with the
 * eb_node so that it can be cast into an eb_node. We could also have put some
 * sort of transparent union here to reduce the indirection level, but the fact
 * is, the end user is not meant to manipulate internals, so this is pointless.
 * The 'node.bit' value here works differently from scalar types, as it contains
 * the number of identical bits between the two branches.
 */
struct ebmb_node {
	struct eb_node node; /* the tree node, must be at the beginning */
	unsigned char key[0]; /* the key, its size depends on the application */
};

/*
 * Exported functions and macros.
 * Many of them are always inlined because they are extremely small, and
 * are generally called at most once or twice in a program.
 */

/* Return leftmost node in the tree, or NULL if none */
static forceinline struct ebmb_node *ebmb_first(struct eb_root *root)
{
	return ebmb_entry(eb_first(root), struct ebmb_node, node);
}

/* Return rightmost node in the tree, or NULL if none */
static forceinline struct ebmb_node *ebmb_last(struct eb_root *root)
{
	return ebmb_entry(eb_last(root), struct ebmb_node, node);
}

/* Return next node in the tree, or NULL if none */
static forceinline struct ebmb_node *ebmb_next(struct ebmb_node *ebmb)
{
	return ebmb_entry(eb_next(&ebmb->node), struct ebmb_node, node);
}

/* Return previous node in the tree, or NULL if none */
static forceinline struct ebmb_node *ebmb_prev(struct ebmb_node *ebmb)
{
	return ebmb_entry(eb_prev(&ebmb->node), struct ebmb_node, node);
}

/* Return next node in the tree, skipping duplicates, or NULL if none */
static forceinline struct ebmb_node *ebmb_next_unique(struct ebmb_node *ebmb)
{
	return ebmb_entry(eb_next_unique(&ebmb->node), struct ebmb_node, node);
}

/* Return previous node in the tree, skipping duplicates, or NULL if none */
static forceinline struct ebmb_node *ebmb_prev_unique(struct ebmb_node *ebmb)
{
	return ebmb_entry(eb_prev_unique(&ebmb->node), struct ebmb_node, node);
}

/* Delete node from the tree if it was linked in. Mark the node unused. Note
 * that this function relies on a non-inlined generic function: eb_delete.
 */
static forceinline void ebmb_delete(struct ebmb_node *ebmb)
{
	eb_delete(&ebmb->node);
}

/* The following functions are not inlined by default. They are declared
 * in ebmbtree.c, which simply relies on their inline version.
 */
REGPRM3 struct ebmb_node *ebmb_lookup(struct eb_root *root, const void *x, unsigned int len);
REGPRM3 struct ebmb_node *ebmb_insert(struct eb_root *root, struct ebmb_node *new, unsigned int len);

/* The following functions are less likely to be used directly, because their
 * code is larger. The non-inlined version is preferred.
 */

/* Delete node from the tree if it was linked in. Mark the node unused. */
static forceinline void __ebmb_delete(struct ebmb_node *ebmb)
{
	__eb_delete(&ebmb->node);
}

/* Find the first occurence of a key of <len> bytes in the tree <root>.
 * If none can be found, return NULL.
 */
static forceinline struct ebmb_node *__ebmb_lookup(struct eb_root *root, const void *x, unsigned int len)
{
	struct ebmb_node *node;
	eb_troot_t *troot;
	unsigned int bit;

	troot = root->b[EB_LEFT];
	if (unlikely(troot == NULL))
		return NULL;

	bit = 0;
	while (1) {
		if ((eb_gettag(troot) == EB_LEAF)) {
			node = container_of(eb_untag(troot, EB_LEAF),
					    struct ebmb_node, node.branches);
			if (memcmp(node->key, x, len) == 0)
				return node;
			else
				return NULL;
		}
		node = container_of(eb_untag(troot, EB_NODE),
				    struct ebmb_node, node.branches);

		if (node->node.bit < 0) {
			/* We have a dup tree now. Either it's for the same
			 * value, and we walk down left, or it's a different
			 * one and we don't have our key.
			 */
			if (memcmp(node->key, x, len) != 0)
				return NULL;

			troot = node->node.branches.b[EB_LEFT];
			while (eb_gettag(troot) != EB_LEAF)
				troot = (eb_untag(troot, EB_NODE))->b[EB_LEFT];
			node = container_of(eb_untag(troot, EB_LEAF),
					    struct ebmb_node, node.branches);
			return node;
		}

		/* OK, normal data node, let's walk down */
		bit = equal_bits(x, node->key, bit, node->node.bit);
		if (bit < node->node.bit)
			return NULL; /* no more common bits */

		troot = node->node.branches.b[(((unsigned char*)x)[node->node.bit >> 3] >>
					       (~node->node.bit & 7)) & 1];
	}
}

/* Insert ebmb_node <new> into subtree starting at node root <root>.
 * Only new->key needs be set with the key. The ebmb_node is returned.
 * If root->b[EB_RGHT]==1, the tree may only contain unique keys. The
 * len is specified in bytes.
 */
static forceinline struct ebmb_node *
__ebmb_insert(struct eb_root *root, struct ebmb_node *new, unsigned int len)
{
	struct ebmb_node *old;
	unsigned int side;
	eb_troot_t *troot;
	eb_troot_t *root_right = root;
	int diff;
	int bit;

	side = EB_LEFT;
	troot = root->b[EB_LEFT];
	root_right = root->b[EB_RGHT];
	if (unlikely(troot == NULL)) {
		/* Tree is empty, insert the leaf part below the left branch */
		root->b[EB_LEFT] = eb_dotag(&new->node.branches, EB_LEAF);
		new->node.leaf_p = eb_dotag(root, EB_LEFT);
		new->node.node_p = NULL; /* node part unused */
		return new;
	}

	len <<= 3;

	/* The tree descent is fairly easy :
	 *  - first, check if we have reached a leaf node
	 *  - second, check if we have gone too far
	 *  - third, reiterate
	 * Everywhere, we use <new> for the node node we are inserting, <root>
	 * for the node we attach it to, and <old> for the node we are
	 * displacing below <new>. <troot> will always point to the future node
	 * (tagged with its type). <side> carries the side the node <new> is
	 * attached to below its parent, which is also where previous node
	 * was attached.
	 */

	bit = 0;
	while (1) {
		if (unlikely(eb_gettag(troot) == EB_LEAF)) {
			eb_troot_t *new_left, *new_rght;
			eb_troot_t *new_leaf, *old_leaf;

			old = container_of(eb_untag(troot, EB_LEAF),
					    struct ebmb_node, node.branches);

			new_left = eb_dotag(&new->node.branches, EB_LEFT);
			new_rght = eb_dotag(&new->node.branches, EB_RGHT);
			new_leaf = eb_dotag(&new->node.branches, EB_LEAF);
			old_leaf = eb_dotag(&old->node.branches, EB_LEAF);

			new->node.node_p = old->node.leaf_p;

			/* Right here, we have 3 possibilities :
			 * - the tree does not contain the key, and we have
			 *   new->key < old->key. We insert new above old, on
			 *   the left ;
			 *
			 * - the tree does not contain the key, and we have
			 *   new->key > old->key. We insert new above old, on
			 *   the right ;
			 *
			 * - the tree does contain the key, which implies it
			 *   is alone. We add the new key next to it as a
			 *   first duplicate.
			 *
			 * The last two cases can easily be partially merged.
			 */
			bit = equal_bits(new->key, old->key, bit, len);
			diff = cmp_bits(new->key, old->key, bit);

			if (diff < 0) {
				new->node.leaf_p = new_left;
				old->node.leaf_p = new_rght;
				new->node.branches.b[EB_LEFT] = new_leaf;
				new->node.branches.b[EB_RGHT] = old_leaf;
			} else {
				/* we may refuse to duplicate this key if the tree is
				 * tagged as containing only unique keys.
				 */
				if (diff == 0 && eb_gettag(root_right))
					return old;

				/* new->key >= old->key, new goes the right */
				old->node.leaf_p = new_left;
				new->node.leaf_p = new_rght;
				new->node.branches.b[EB_LEFT] = old_leaf;
				new->node.branches.b[EB_RGHT] = new_leaf;

				if (diff == 0) {
					new->node.bit = -1;
					root->b[side] = eb_dotag(&new->node.branches, EB_NODE);
					return new;
				}
			}
			break;
		}

		/* OK we're walking down this link */
		old = container_of(eb_untag(troot, EB_NODE),
				   struct ebmb_node, node.branches);

		/* Stop going down when we don't have common bits anymore. We
		 * also stop in front of a duplicates tree because it means we
		 * have to insert above. Note: we can compare more bits than
		 * the current node's because as long as they are identical, we
		 * know we descend along the correct side.
		 */
		if (old->node.bit < 0) {
			/* we're above a duplicate tree, we must compare till the end */
			bit = equal_bits(new->key, old->key, bit, len);
			goto dup_tree;
		}
		else if (bit < old->node.bit) {
			bit = equal_bits(new->key, old->key, bit, old->node.bit);
		}

		if (bit < old->node.bit) { /* we don't have all bits in common */
			/* The tree did not contain the key, so we insert <new> before the node
			 * <old>, and set ->bit to designate the lowest bit position in <new>
			 * which applies to ->branches.b[].
			 */
			eb_troot_t *new_left, *new_rght;
			eb_troot_t *new_leaf, *old_node;

		dup_tree:
			new_left = eb_dotag(&new->node.branches, EB_LEFT);
			new_rght = eb_dotag(&new->node.branches, EB_RGHT);
			new_leaf = eb_dotag(&new->node.branches, EB_LEAF);
			old_node = eb_dotag(&old->node.branches, EB_NODE);

			new->node.node_p = old->node.node_p;

			diff = cmp_bits(new->key, old->key, bit);
			if (diff < 0) {
				new->node.leaf_p = new_left;
				old->node.node_p = new_rght;
				new->node.branches.b[EB_LEFT] = new_leaf;
				new->node.branches.b[EB_RGHT] = old_node;
			}
			else if (diff > 0) {
				old->node.node_p = new_left;
				new->node.leaf_p = new_rght;
				new->node.branches.b[EB_LEFT] = old_node;
				new->node.branches.b[EB_RGHT] = new_leaf;
			}
			else {
				struct eb_node *ret;
				ret = eb_insert_dup(&old->node, &new->node);
				return container_of(ret, struct ebmb_node, node);
			}
			break;
		}

		/* walk down */
		root = &old->node.branches;
		side = (new->key[old->node.bit >> 3] >> (~old->node.bit & 7)) & 1;
		troot = root->b[side];
	}

	/* Ok, now we are inserting <new> between <root> and <old>. <old>'s
	 * parent is already set to <new>, and the <root>'s branch is still in
	 * <side>. Update the root's leaf till we have it. Note that we can also
	 * find the side by checking the side of new->node.node_p.
	 */

	/* We need the common higher bits between new->key and old->key.
	 * This number of bits is already in <bit>.
	 */
	new->node.bit = bit;
	root->b[side] = eb_dotag(&new->node.branches, EB_NODE);
	return new;
}

#endif /* _EBMBTREE_H */

