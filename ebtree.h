/*
 * ebtree.h : Elastic Binary Trees - macros and structures.
 * (C) 2002-2007 - Willy Tarreau - willy@ant-computing.com
 *
 * 2007/05/27: version 2: compact everything into one single struct
 * 2007/05/18: adapted the structure to support embedded nodes
 * 2007/05/13: adapted to mempools v2.
 *
 */



/*

  General idea:
  In a radix binary tree, we may have up to 2N-1 nodes for N values if all of
  them are leaves. If we find a way to differentiate intermediate nodes (called
  "link nodes") and final nodes (called "leaf nodes"), and we associate them
  by two, it is possible to build sort of a self-contained radix tree with
  intermediate nodes always present. It will not be as cheap as the ultree for
  optimal cases as shown below, but the optimal case almost never happens :

  Eg, to store 8, 10, 12, 13, 14 :

      ultree       this tree

        8              8
       / \            / \
      10 12          10 12
        /  \           /  \
       13  14         12  14
                     / \
                    12 13

   Note that on real-world tests (with a scheduler), is was verified that the
   case with data on an intermediate node never happens. This is because the
   population is too large for such coincidences to happen. It would require
   for instance that a task has its expiration time at an exact second, with
   other tasks sharing that second. This is too rare to try to optimize for it.

   What is interesting is that the link will only be added above the leaf when
   necessary, which implies that it will always remain somewhere above it. So
   both the leaf and the link can share the exact value of the node, because
   when going down the link, the bit mask will be applied to comparisons. So we
   are tempted to have one value for both nodes.

   The bit only serves the links, and the dups only serve the leaves. So we can
   put a lot of information in common. This results in one single node with two
   leaves and two parents, one for the link part, and one for the leaf part.
   The link may refer to its leaf counterpart in one of its leaves, which will
   be a solution to quickly distinguish between different nodes and common
   nodes.

   Here's what we find in an eb_node :

   struct eb_node {
       struct eb_node *link_p;  // link node's parent
       struct eb_node *leaf_p;  // leaf node's parent
       struct eb_node *leaf[2]; // link's leaf nodes
       struct list    dup;      // leaf duplicates
       int            bit;      // link's bit position. Maybe we should use a char ?
   };

   struct eb32_node {
       struct eb_node node;
       u32 val;
   };

   struct eb64_node {
       struct eb_node node;
       u64 val;
   };


   Algorithmic complexity (max and avg computed for a tree full of distinct values) :
     - lookup              : avg=O(logN), max = O(logN)
     - insertion from root : avg=O(logN), max = O(logN)
     - insertion of dups   : O(1) after lookup
     - moves               : not implemented yet, O(logN)
     - deletion            : max = O(1)
     - prev/next           : avg = 2, max = O(logN)
       N/2 nodes need 1 hop  => 1*N/2
       N/4 nodes need 2 hops => 2*N/4
       N/8 nodes need 3 hops => 3*N/8
       ...
       N/x nodes need log(x) hops => log2(x)*N/x
       Total cost for all N nodes : sum[i=1..N](log2(i)*N/i) = N*sum[i=1..N](log2(i)/i)
       Average cost across N nodes = total / N = sum[i=1..N](log2(i)/i) = 2

   Useful properties :
     - links are only provided above the leaf, never below. This implies that
       the nodes directly attached to the root do not use their link. It also
       enhances the probability that the link directly above a leaf are from
       the same node.

     - a link connected to its own leaf will have leaf_p = node = leaf[0|1].

     - leaf[0] can never be equal to leaf[1] except for the root which can have
       them both NULL.

     - link_p can never be equal to the same node.

     - two leaves can never point to the same location

     - duplicates do not use their link part, nor their leaf_p pointer.

     - links do not use their dup list.

     - those cannot be merged because a leaf at the head of a dup list needs
       both a link above it and a dup list.

     - a leaf-only node should have some easily distinguishable info in the
       link part, such as NULL or a pointer to the same node (which cannot
       happen in normal case). The NULL might be better to identify the root.

     - bit is necessarily > 0.

   Basic definitions (subject to change) :
     - for duplicate leaf nodes, leaf_p = NULL.
     - use bit == 0 to indicate a leaf node which is not used as a link
     - root->bit = INTBITS for the represented data type (eg: 32)
     - root->link_p = root->leaf_p = NULL
     - root->leaf[0,1] = NULL if branch is empty

   Deletion is not very complex:
     - it only applies to leaves
     - if the leaf is a duplicate, simply remove it from the list.
     - when a leaf is deleted, its parent must be unlinked (unless it is the root)
     - when a leaf is deleted, the link provided with the same node must be
       replaced if used, because it will not be available anymore. We put
       the one we freed instead.

   It is important to understand that once a link node is removed, it will
   never be needed anymore. If another node comes above and needs a link, it
   will provide its own.

   Also, when we delete a leaf attached to the root, we get no link back. It's
   not a problem because by definition, since a node can only provide links
   above it, it has no link in use.

 */

#if defined(__i386__)
static inline int fls(int x)
{
	int r;
	__asm__("bsrl %1,%0\n\t"
	        "jnz 1f\n\t"
	        "movl $-1,%0\n"
	        "1:" : "=r" (r) : "rm" (x));
	return r+1;
}
#else
// returns 1 to 32 for 1<<0 to 1<<31. Undefined for 0.
#define fls(___a) ({ \
	register int ___x, ___bits = 0; \
	___x = (___a); \
	if (___x & 0xffff0000) { ___x &= 0xffff0000; ___bits += 16;} \
	if (___x & 0xff00ff00) { ___x &= 0xff00ff00; ___bits +=  8;} \
	if (___x & 0xf0f0f0f0) { ___x &= 0xf0f0f0f0; ___bits +=  4;} \
	if (___x & 0xcccccccc) { ___x &= 0xcccccccc; ___bits +=  2;} \
	if (___x & 0xaaaaaaaa) { ___x &= 0xaaaaaaaa; ___bits +=  1;} \
	___bits + 1; \
	})
#endif


#ifndef LIST_INIT
#define LIST_INIT(l) ((l)->n = (l)->p = (l))
#define LIST_ADDQ(lh, el) ({ (el)->p = (lh)->p; (el)->p->n = (lh)->p = (el); (el)->n = (lh); (el); })
#define LIST_DEL(el) ({ typeof(el) __ret = (el); (el)->n->p = (el)->p; (el)->p->n = (el)->n; (__ret); })
#define LIST_ELEM(lh, pt, el) ((pt)(((void *)(lh)) - ((void *)&((pt)NULL)->el)))

struct list {
	struct list *n;	/* next */
	struct list *p;	/* prev */
};
#endif


/*
 * Gcc >= 3 provides the ability for the programme to give hints to the
 * compiler about what branch of an if is most likely to be taken. This
 * helps the compiler produce the most compact critical paths, which is
 * generally better for the cache and to reduce the number of jumps.
 */
#if __GNUC__ < 3
#define __builtin_expect(x,y) (x)
#endif

#define likely(x) (__builtin_expect((x) != 0, 1))
#define unlikely(x) (__builtin_expect((x) != 0, 0))


typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;

/* 28 bytes per node on 32-bit machines. */
struct eb_node {
	struct list     dup;     /* leaf duplicates */
	struct eb_node *leaf_p;  /* leaf node's parent */
	struct eb_node *link_p;  /* link node's parent */
	struct eb_node *leaf[2]; /* link's leaf nodes */
	unsigned int    bit;     /* link's bit position. */
};

/* Those structs carry nodes and data. They must start with the eb_node so that
 * any eb*_node can be cast into an eb_node.
 */
struct eb32_node {
	struct eb_node node;
	u32 val;
};

struct eb64_node {
	struct eb_node node;
	u64 val;
};

/*
 * The root is a node initialized with :
 * - bit = 32, so that we split on bit 31 below it
 * - val = 0
 * - parent* = left = right = NULL
 * During its life, only left and right will change. Checking
 * that parent = NULL should be enough to retain from deleting it.
 * 
 */


/********************************************************************/

#define EB32_TREE_HEAD(name) 						\
	struct eb32_node name = { 					\
		.node = { .bit = 32, 					\
			  .link_p = NULL, .leaf_p = NULL,		\
			  .leaf = { [0] = NULL, [1] = NULL },		\
			  .dup = { .n = NULL, .p = NULL }},		\
		.val = 0,						\
	}



#undef STATS
#ifdef STATS
extern unsigned long total_jumps;
#define COUNT_STATS  total_jumps++;
#else
#define COUNT_STATS
#endif


/* Walks down link node <root> starting with <start> leaf, and always walking
 * on side <side>. It either returns the first leaf on that side, or NULL if
 * no leaf is left. Note that <root> may either be NULL or a link node, but
 * it must not be a leaf-only node. <start> may either be NULL, a link node,
 * or a heading leaf node (not a dup). The leaf (or NULL) is returned.
 */
static inline struct eb_node *
__eb_walk_down(struct eb_node *root, unsigned int side, struct eb_node *start)
{
	if (unlikely(!start))
		return start;	/* only possible at root */
	while (start->leaf_p != root) {
		root = start;
		start = start->leaf[side];
	};
	return start;
}

#define eb_walk_down(root, side, start)				\
({								\
	__label__ __out;					\
	struct eb_node *__wp = (struct eb_node *)(root);	\
	struct eb_node *__wn = (struct eb_node *)(start);	\
	if (unlikely(!__wn))					\
		goto __out;	/* only possible at root */	\
	while (__wn->leaf_p != __wp) {				\
		__wp = __wn;					\
		__wn = __wn->leaf[(side)];			\
	};							\
__out:								\
	__wn;							\
})

#define eb_walk_down_left(node, start)			\
	eb_walk_down(node, 0, start)

#define eb_walk_down_right(node, start)			\
	eb_walk_down(node, 1, start)


/* Walks up starting from node <node> with parent <par>, which must be a valid
 * parent (ie: link_p or leaf_p, and <node> must not be a duplicate). It
 * follows side <side> for as long as possible, and stops when it reaches a
 * node which sees it on the other side, or before attempting to go beyond the
 * root. The pointer to the closest common ancestor is returned, which might be
 * NULL if none is found.
 */
static inline struct eb_node *
eb_walk_up(struct eb_node *node, int side, struct eb_node *par)
{
	while (par->leaf[side] == node) {
		node = par;
		par = par->link_p;
		if (unlikely(!par))
			break;
	}
	return par;
}

#define eb_walk_up_left_with_parent(node, par)			\
	eb_walk_up(node, 0, par)

#define eb_walk_up_right_with_parent(node, par)			\
	eb_walk_up(node, 1, par)


/* Walks up left starting from leaf node <node> */
#define eb_walk_up_left(node)					\
	eb_walk_up_left_with_parent((node), (node)->leaf_p)


/* Walks up right starting from leaf node <node> */
#define eb_walk_up_right(node)					\
	eb_walk_up_right_with_parent((node), (node)->leaf_p)


/* Returns the pointer to the other node sharing the same parent. This
 * method is tricky but avoids a test and is faster.
 */
#define eb_sibling_with_parent(node, par)			\
	((struct eb_node *)					\
	 (((unsigned long)(par)->leaf[0]) ^			\
	  ((unsigned long)(par)->leaf[1]) ^			\
	  ((unsigned long)(node))))

#define eb_sibling_with_parent_test(node, par)			\
	(((par)->leaf[1] == (node)) ? (par)->leaf[0] : (par)->leaf[1])


/* Returns first leaf in the tree starting at <root>, or NULL if none */
static inline struct eb_node *
eb_first_node(struct eb_node *root)
{
	unsigned int branch;
	struct eb_node *ret;
	for (branch = 0; branch <= 1; branch++) {
		ret = eb_walk_down_left((struct eb_node *)(root),
					((struct eb_node *)(root))->leaf[branch]);
		if (likely(ret))
			break;
	}
	return ret;
}

/* returns first leaf in the tree starting at <root>, or NULL if none */
#define eb_first(root)							\
	((typeof(root))eb_first_node((struct eb_node *)(root)))


/* returns last leaf in the tree starting at <root>, or NULL if none */
static inline struct eb_node *
eb_last_node(struct eb_node *root)
{
	unsigned int branch;
	struct eb_node *ret;
	for (branch = 1; branch <= 1; branch--) {
		ret = eb_walk_down_right((struct eb_node *)(root),
					((struct eb_node *)(root))->leaf[branch]);
		if (likely(ret))
			break;
	}
	return ret;
}

/* returns last leaf in the tree starting at <root>, or NULL if none */
#define eb_last(root)							\
	((typeof(root))eb_last_node((struct eb_node *)(root)))



/* returns next leaf node after an existing leaf node, or NULL if none. */
static inline struct eb_node *
eb_next_node(struct eb_node *node)
{
	if (node->dup.n != &node->dup) {
		/* let's return duplicates before going further */
		node = LIST_ELEM(node->dup.n, struct eb_node *, dup);
		if (unlikely(!node->leaf_p))
			return node;
		/* we returned to the list's head, let's walk up now */
	}
	node = eb_walk_up_right_with_parent(node, node->leaf_p);
	if (node)
		node = eb_walk_down_left(node, node->leaf[1]);
	return node;
}

#define eb_next(node)							\
	((typeof(node))eb_next_node((struct eb_node *)(node)))


/* returns previous leaf node before an existing leaf node, or NULL if none. */
static inline struct eb_node *
eb_prev_node(struct eb_node *node)
{
	if (node->dup.p != &node->dup) {
		/* let's return duplicates before going further */
		node = LIST_ELEM(node->dup.p, struct eb_node *, dup);
		if (unlikely(!node->leaf_p))
			return node;
		/* we returned to the list's head, let's walk up now */
	}
	node = eb_walk_up_left_with_parent(node, node->leaf_p);
	if (node)
		node = eb_walk_down_right(node, node->leaf[0]);
	return node;
}


/* returns previous leaf node before an existing leaf node, or NULL if none. */
#define eb_prev(node)							\
	((typeof(node))eb_prev_node((struct eb_node *)(node)))


/* Removes a leaf node from the tree, and returns zero after deleting the
 * last node. Otherwise, non-zero is returned.
 */
static inline int
__eb_delete(struct eb_node *node)
{
	__label__ replace_link;
	unsigned int l;
	struct eb_node *newlink, *parent, *gparent;

	parent = node->leaf_p;

	/* Duplicates are simply unlinked, because we know they are not linked
	 * to anything. Also, we know the tree is not empty afterwards.
	 */
	if (!parent) {
		LIST_DEL(&node->dup);
		return 1;
	}

	/* List heads are copied then removed. The parent pointing to them is
	 * updated to point to the first duplicate. Since the heads may have
	 * their link part used somewhere else, and we know that the duplicate
	 * cannot have his in use, we can switch over to using his.
	 */
	if (node->dup.n != &node->dup) {
		newlink = LIST_ELEM(node->dup.n, struct eb_node *, dup);
		LIST_DEL(&node->dup);
		newlink->leaf_p = parent;

		l = (parent->leaf[1] == node);
		parent->leaf[l] = newlink;
	
		/* keep newlink intact as we can use it for the replacement */
		goto replace_link;
	}

	/* Here, we know that the node is not part of any duplicate list.
	 * We likely have to release the parent link, unless it's the root,
	 * in which case we only set our branch to NULL.
	 */
	gparent = parent->link_p;
	if (unlikely(gparent == NULL)) {
		l = (parent->leaf[1] == node);
		parent->leaf[l] = NULL;
		return !!parent->leaf[l ^ 1];
	}

	/* To release the parent, we have to identify our sibling, and reparent
	 * it directly to/from the grand parent. Note that the sibling can
	 * either be a link or a leaf.
	 */
	newlink = eb_sibling_with_parent(node, parent);
	if (newlink->leaf_p == parent)
		newlink->leaf_p = gparent;
	else
		newlink->link_p = gparent;

	l = (gparent->leaf[1] == parent);
	gparent->leaf[l] = newlink;

	/* Mark the parent unused. Note that we do not check if the parent is
	 * our own link, but that's not a problem because if it is, it will be
	 * marked unused at the same time, which we'll use below to know we can
	 * safely remove it.
	 */
	parent->bit = 0;

	/* The parent link has been detached, and is unused. So we can use it
	 * if we need to replace the node's link somewhere else.
	 */
	newlink = parent;

 replace_link:
	/* check whether our link part is in use */
	if (!node->bit)
		return 1; /* tree is not empty yet */

	/* From now on, node and newlink are necessarily different, and the
	 * node's link part is in use. By definition, <newlink> is at least
	 * below <link>, so keeping its value for the bit string is OK.
	 */

	newlink->link_p = node->link_p;
	newlink->leaf[0] = node->leaf[0];
	newlink->leaf[1] = node->leaf[1];
	newlink->bit = node->bit;

	/* We must now update the new link's parent */
	gparent = node->link_p;
	if (gparent->leaf[0] == node)
		gparent->leaf[0] = newlink;
	else
		gparent->leaf[1] = newlink;

	/* ... and the link's leaves */
	for (l = 0; l <= 1; l++) {
		if (newlink->leaf[l]->leaf_p == node)
			newlink->leaf[l]->leaf_p = newlink;
		else
			newlink->leaf[l]->link_p = newlink;
	}

	/* Now the node has been completely unlinked */
	return 1; /* tree is not empty yet */
}

/* Removes a leaf node from the tree, and returns zero after deleting the
 * last node. Otherwise, non-zero is returned.
 */
#define eb_delete(node)							\
	((typeof(node))__eb_delete((struct eb_node *)(node)))


/********************************************************************/
/*         The following functions are data type-specific           */
/********************************************************************/


/*
 * Finds the first occurence of a value in the tree <root>. If none can be
 * found, NULL is returned.
 */
static inline struct eb32_node *
__eb32_lookup(struct eb32_node *root, unsigned long x)
{
	struct eb_node *parent;

	while (1) {
		parent = (struct eb_node *)root;

		// Don't ask why this slows down like hell ! Gcc completely
		// changes all the loop sequencing !
		// root = (struct eb32_node *)parent->leaf[((x >> (parent->bit - 1)) & 1)];

		if ((x >> (parent->bit - 1)) & 1)
			root = (struct eb32_node *)parent->leaf[1];
		else
			root = (struct eb32_node *)parent->leaf[0];

		/* may only happen in tree root */
		if (unlikely(!root))
			return NULL;

		if (unlikely(root->node.leaf_p == parent)) {
			/* reached a leaf */
			if (root->val == x)
				return root;
			else
				return NULL;
		}
#if 1
		/* Optimization 1: if x is equal to the exact value of the node,
		 * it implies that this node contains a leaf with this exact
		 * value, so we can return it now.
		 * This can boost by up to 50% on randomly inserted values, but may
		 * degrade by 5-10% when values have been carefully inserted in order,
		 * which is not exactly what we try to use anyway.
		 */
		if (unlikely((x ^ root->val) == 0))
			return root;
#endif
#if 1
		/* Optimization 2: if there are no bits in common anymore, let's
		 * stop right now instead of going down to the leaf.
		 * This one greatly improves performance in sparse trees, but it
		 * appears that repeating this test at every level in complete
		 * trees instead degrades performance by about 5%. Anyway, it
		 * generally is worth it.
		 */
		if (unlikely((x ^ root->val) >> root->node.bit))
			return NULL;
#endif

	}
}


/* Inserts node <new> into subtree starting at link node <root>.
 * Only new->leaf.val needs be set with the value.
 * The node is returned.
 */

static inline struct eb32_node *
__eb32_insert(struct eb32_node *root, struct eb32_node *new) {
	struct eb32_node *next;
	unsigned int l;
	u32 x;

	x = new->val;
	next = root;

	next = (struct eb32_node *)root->node.leaf[(x >> 31) & 1];
	if (unlikely(next == NULL)) {
		root->node.leaf[(x >> 31) & 1] = (struct eb_node *)new;
		/* This can only happen on the root node. */
		/* We'll have to insert our new leaf node here. */
		new->node.leaf_p = (struct eb_node *)root;
		LIST_INIT(&new->node.dup);
		new->node.bit = 0; /* link part unused */
		return new;
	}

	while (1) {
		COUNT_STATS;

		if (unlikely(next->node.leaf_p == (struct eb_node *)root)) {
			/* we're on a leaf node */
			if (unlikely(next->val == x)) {
				/* We are inserting a value we already have.
				 * We just have to join the duplicates list.
				 */
				LIST_ADDQ(&next->node.dup, &new->node.dup);
				new->node.leaf_p = NULL; /* we're a duplicate, no parent */
				new->node.bit = 0; /* link part unused */
				return new;
			}
			break;
		}

		/* Stop going down when we don't have common bits anymore. */
		if (unlikely(((x ^ next->val) >> next->node.bit) != 0))
			break;

		/* walk down */
		root = next;
		l = (x >> (next->node.bit - 1)) & 1;
		next = (struct eb32_node *)next->node.leaf[l];
	}

	/* Ok, now we know that we must insert between <root> and <next>. For
	 * this, we must insert the link part and chain it to the leaf part.
	 */

	/* We need the common higher bits between x and next->val.
	 * What differences are there between x and the node here ?
	 * NOTE that bit(new) is always < bit(root) because highest
	 * bit of x and next->val are identical here (otherwise they
	 * would sit on different branches).
	 */

	new->node.link_p = (struct eb_node *)root;
	new->node.leaf_p = (struct eb_node *)new;

	new->node.bit = fls(x ^ next->val);   /* lower identical bit */
	new->val = x;

	/* This optimization is a bit tricky. The goal is to put new->leaf as well
	 * as the other leaf on the right branch of the new parent link, depending
	 * on which one is bigger.
	 */
	l = (x > next->val);
	new->node.leaf[l ^ 1] = (struct eb_node *)next;
	new->node.leaf[l] = (struct eb_node *)new;

	/* now we build the leaf part and chain it directly below the link node */
	LIST_INIT(&new->node.dup);

	/* Now, change the links. Note that this could be done anywhere.
	 * Updating the <next> node above which we're inserting is a bit harder
	 * because it can be both a link and a leaf. We have no way but to check.
	 */
	l = (root->node.leaf[1] == (struct eb_node *)next);
	root->node.leaf[l] = (struct eb_node *)new;

	if (next->node.leaf_p == (struct eb_node *)root)
		next->node.leaf_p = (struct eb_node *)new;
	else
		next->node.link_p = (struct eb_node *)new;

	return new;
}


/********************************************************************/




/*********************************************************************/



/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */