/*-
 * Copyright (c) 2014-2015 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_evict_file --
 *	Discard pages for a specific file.
 */
int
__wt_evict_file(WT_SESSION_IMPL *session, int syncop)
{
	WT_BTREE *btree;
	WT_DECL_RET;
	WT_PAGE *page;
	WT_REF *next_ref, *ref;
	int eviction_enabled;

	btree = S2BT(session);
	eviction_enabled = !F_ISSET(btree, WT_BTREE_NO_EVICTION);

	/*
	 * We need exclusive access to the file -- disable ordinary eviction
	 * and drain any blocks already queued.
	 */
	if (eviction_enabled)
		WT_RET(__wt_evict_file_exclusive_on(session));

	/* Make sure the oldest transaction ID is up-to-date. */
	__wt_txn_update_oldest(session);

	/* Walk the tree, discarding pages. */
	next_ref = NULL;
	WT_ERR(__wt_tree_walk(
	    session, &next_ref, WT_READ_CACHE | WT_READ_NO_EVICT));
	while ((ref = next_ref) != NULL) {
		page = ref->page;

		/*
		 * Eviction can fail when a page in the evicted page's subtree
		 * switches state.  For example, if we don't evict a page marked
		 * empty, because we expect it to be merged into its parent, it
		 * might no longer be empty after it's reconciled, in which case
		 * eviction of its parent would fail.  We can either walk the
		 * tree multiple times (until it's finally empty), or reconcile
		 * each page to get it to its final state before considering if
		 * it's an eviction target or will be merged into its parent.
		 *
		 * Don't limit this test to any particular page type, that tends
		 * to introduce bugs when the reconciliation of other page types
		 * changes, and there's no advantage to doing so.
		 *
		 * Eviction can also fail because an update cannot be written.
		 * If sessions have disjoint sets of files open, updates in a
		 * no-longer-referenced file may not yet be globally visible,
		 * and the write will fail with EBUSY.  Our caller handles that
		 * error, retrying later.
		 */
		if (syncop == WT_SYNC_CLOSE && __wt_page_is_modified(page))
			WT_ERR(__wt_reconcile(session, ref, NULL, WT_EVICTING));

		/*
		 * We can't evict the page just returned to us (it marks our
		 * place in the tree), so move the walk to one page ahead of
		 * the page being evicted.  Note, we reconciled the returned
		 * page first: if reconciliation of that page were to change
		 * the shape of the tree, and we did the next walk call before
		 * the reconciliation, the next walk call could miss a page in
		 * the tree.
		 */
		WT_ERR(__wt_tree_walk(
		    session, &next_ref, WT_READ_CACHE | WT_READ_NO_EVICT));

		switch (syncop) {
		case WT_SYNC_CLOSE:
			/*
			 * Evict the page.
			 * Do not attempt to evict pages expected to be merged
			 * into their parents, with the exception that the root
			 * page can't be merged, it must be written.
			 */
			if (__wt_ref_is_root(ref) ||
			    page->modify == NULL ||
			    !F_ISSET(page->modify, WT_PM_REC_EMPTY))
				WT_ERR(__wt_evict(session, ref, 1));
			break;
		case WT_SYNC_DISCARD:
		case WT_SYNC_DISCARD_FORCE:
			/*
			 * Discard the page, whether clean or dirty.
			 *
			 * Clean the page, both to keep statistics correct, and
			 * to let the page-discard function assert no dirty page
			 * is ever discarded.
			 */
			if (__wt_page_is_modified(page)) {
				page->modify->write_gen = 0;
				__wt_cache_dirty_decr(session, page);
			}
			/*
			 * If the page contains an update that is too recent to
			 * evict, stop.  This should never happen during
			 * connection close, and in other paths our caller
			 * should be prepared to deal with this case.
			 */
			if (syncop == WT_SYNC_DISCARD &&
			    page->modify != NULL &&
			    !__wt_txn_visible_all(session,
			    page->modify->rec_max_txn))
				WT_ERR(EBUSY);

			if (syncop == WT_SYNC_DISCARD_FORCE)
				F_SET(session, WT_SESSION_DISCARD_FORCE);
			__wt_rec_page_clean_update(session, ref);
			F_CLR(session, WT_SESSION_DISCARD_FORCE);
			break;
		WT_ILLEGAL_VALUE_ERR(session);
		}
	}

	if (0) {
err:		/* On error, clear any left-over tree walk. */
		if (next_ref != NULL)
			WT_TRET(__wt_page_release(
			    session, next_ref, WT_READ_NO_EVICT));
	}

	if (eviction_enabled)
		__wt_evict_file_exclusive_off(session);

	return (ret);
}
