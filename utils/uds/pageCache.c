/*
 * Copyright Red Hat
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA. 
 *
 * $Id: //eng/uds-releases/krusty/src/uds/pageCache.c#17 $
 */

#include "pageCache.h"

#include "atomicDefs.h"
#include "cacheCounters.h"
#include "chapterIndex.h"
#include "compiler.h"
#include "errors.h"
#include "geometry.h"
#include "hashUtils.h"
#include "indexConfig.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"
#include "recordPage.h"
#include "stringUtils.h"
#include "threads.h"
#include "zone.h"

/**********************************************************************/
int assert_page_in_cache(struct page_cache *cache, struct cached_page *page)
{
	int result = ASSERT((page->cp_physical_page < cache->num_index_entries),
			    "physical page %u is valid (< %u)",
			    page->cp_physical_page,
			    cache->num_index_entries);
	if (result != UDS_SUCCESS) {
		return result;
	}

	uint16_t page_index = cache->index[page->cp_physical_page];
	return ASSERT((page_index < cache->num_cache_entries) &&
			      (&cache->cache[page_index] == page),
		      "page is at expected location in cache");
}

/**
 * Clear a cache page.  Note: this does not clear read_pending - a read could
 * still be pending and the read thread needs to be able to proceed and restart
 * the requests regardless. This page will still be marked invalid, but it
 * won't get reused (see get_least_recent_page()) until the read_pending flag
 * is cleared. This is a valid case, e.g. the chapter gets forgotten and
 * replaced with a new one in LRU.  Restarting the requests will lead them to
 * not find the records in the MI.
 *
 * @param cache   the cache
 * @param page    the cached page to clear
 *
 **/
static void clear_cache_page(struct page_cache *cache,
			     struct cached_page *page)
{
	page->cp_physical_page = cache->num_index_entries;
	WRITE_ONCE(page->cp_last_used, 0);
}

/**
 * Get a page from the cache, but with no stats
 *
 * @param cache         the cache
 * @param physical_page the physical page to get
 * @param queue_index   the index of the page in the read queue if
 *                      queued, -1 otherwise
 * @param page_ptr      a pointer to hold the page
 *
 * @return UDS_SUCCESS or an error code
 **/
static int __must_check get_page_no_stats(struct page_cache *cache,
					  unsigned int physical_page,
					  int *queue_index,
					  struct cached_page **page_ptr)
{
	/*
	 * ASSERTION: We are either a zone thread holding a
	 * search_pending_counter, or we are any thread holding the
	 * readThreadsMutex.
	 *
	 * Holding only a search_pending_counter is the most frequent case.
	 */

	int result = ASSERT((physical_page < cache->num_index_entries),
			    "physical page %u is invalid",
			    physical_page);
	if (result != UDS_SUCCESS) {
		return result;
	}

	/*
	 * It would be unlikely that the compiler turns the usage of
	 * index_value into two reads of cache->index, but it would be
	 * possible and very bad if those reads did not return the
	 * same bits.
	 */
	uint16_t index_value = READ_ONCE(cache->index[physical_page]);
	bool queued = (index_value & VOLUME_CACHE_QUEUED_FLAG) != 0;
	uint16_t index = index_value & ~VOLUME_CACHE_QUEUED_FLAG;

	if (!queued && (index < cache->num_cache_entries)) {
		*page_ptr = &cache->cache[index];
		/*
		 * We have acquired access to the cached page, but
		 * unless we hold the readThreadsMutex, we need a read
		 * memory barrier now.  The corresponding write memory
		 * barrier is in put_page_in_cache.
		 */
		smp_rmb();
	} else {
		*page_ptr = NULL;
	}
	if (queue_index != NULL) {
		*queue_index = queued ? index : -1;
	}
	return UDS_SUCCESS;
}

/**
 * Wait for all pending searches on a page in the cache to complete
 *
 * @param cache          the page cache
 * @param physical_page  the page to check searches on
 **/
static void wait_for_pending_searches(struct page_cache *cache,
				      unsigned int physical_page)
{
	/*
	 * We hold the readThreadsMutex.  We are waiting for threads
	 * that do not hold the readThreadsMutex.  Those threads have
	 * "locked" their targeted page by setting the
	 * search_pending_counter.  The corresponding write memory
	 * barrier is in begin_pending_search.
	 */
	smp_mb();

	invalidate_counter_t initial_counters[MAX_ZONES];
	unsigned int i;
	for (i = 0; i < cache->zone_count; i++) {
		initial_counters[i] = get_invalidate_counter(cache, i);
	}
	for (i = 0; i < cache->zone_count; i++) {
		if (search_pending(initial_counters[i]) &&
		    (page_being_searched(initial_counters[i]) ==
		     physical_page)) {
			// There is an active search using the physical page.
			// We need to wait for the search to finish.
			while (initial_counters[i] ==
			       get_invalidate_counter(cache, i)) {
				yield_scheduler();
			}
		}
	}
}

/**
 * Invalidate a cache page
 *
 * @param cache   the cache
 * @param page    the cached page
 * @param reason  the reason for invalidation, for stats
 *
 * @return UDS_SUCCESS or an error code
 **/
static int __must_check
invalidate_page_in_cache(struct page_cache *cache,
			 struct cached_page *page,
			 enum invalidation_reason reason)
{
	// We hold the readThreadsMutex.
	if (page == NULL) {
		return UDS_SUCCESS;
	}

	if (page->cp_physical_page != cache->num_index_entries) {
		switch (reason) {
		case INVALIDATION_EVICT:
			cache->counters.evictions++;
			break;
		case INVALIDATION_EXPIRE:
			cache->counters.expirations++;
			break;
		default:
			break;
		}

		if (reason != INVALIDATION_ERROR) {
			int result = assert_page_in_cache(cache, page);
			if (result != UDS_SUCCESS) {
				return result;
			}
		}

		WRITE_ONCE(cache->index[page->cp_physical_page],
			   cache->num_cache_entries);
		wait_for_pending_searches(cache, page->cp_physical_page);
	}

	clear_cache_page(cache, page);

	return UDS_SUCCESS;
}

/**********************************************************************/
int find_invalidate_and_make_least_recent(struct page_cache *cache,
					  unsigned int physical_page,
					  struct queued_read *read_queue,
					  enum invalidation_reason reason,
					  bool must_find)
{
	// We hold the readThreadsMutex.
	if (cache == NULL) {
		return UDS_SUCCESS;
	}

	struct cached_page *page;
	int queued_index = -1;
	int result =
		get_page_no_stats(cache,
				  physical_page,
				  ((read_queue != NULL) ? &queued_index : NULL),
				  &page);
	if (result != UDS_SUCCESS) {
		return result;
	}

	if (page == NULL) {
		result = ASSERT(!must_find, "found page");
		if (result != UDS_SUCCESS) {
			return result;
		}

		if (queued_index > -1) {
			log_debug("setting pending read to invalid");
			read_queue[queued_index].invalid = true;
		}
		return UDS_SUCCESS;
	}

	// Invalidate the page and unmap it from the cache.
	result = invalidate_page_in_cache(cache, page, reason);
	if (result != UDS_SUCCESS) {
		return result;
	}

	// Move the cached page to the least recently used end of the list
	// so it will be replaced before any page with valid data.
	WRITE_ONCE(page->cp_last_used, 0);

	return UDS_SUCCESS;
}

/**********************************************************************/
static int __must_check initialize_page_cache(struct page_cache *cache,
					      const struct geometry *geometry,
					      unsigned int chapters_in_cache,
					      unsigned int read_queue_max_size,
					      unsigned int zone_count)
{
	cache->geometry = geometry;
	cache->num_index_entries = geometry->pages_per_volume + 1;
	cache->num_cache_entries =
		chapters_in_cache * geometry->record_pages_per_chapter;
	cache->read_queue_max_size = read_queue_max_size;
	cache->zone_count = zone_count;
	atomic64_set(&cache->clock, 1);

	int result = ALLOCATE(read_queue_max_size,
			      struct queued_read,
			      "volume read queue",
			      &cache->read_queue);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = ALLOCATE(cache->zone_count,
			  struct search_pending_counter,
			  "Volume Cache Zones",
			  &cache->search_pending_counters);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = ASSERT((cache->num_cache_entries <= VOLUME_CACHE_MAX_ENTRIES),
			"requested cache size, %u, within limit %u",
			cache->num_cache_entries,
			VOLUME_CACHE_MAX_ENTRIES);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = ALLOCATE(cache->num_index_entries,
			  uint16_t,
			  "page cache index",
			  &cache->index);
	if (result != UDS_SUCCESS) {
		return result;
	}

	// Initialize index values to invalid values.
	unsigned int i;
	for (i = 0; i < cache->num_index_entries; i++) {
		cache->index[i] = cache->num_cache_entries;
	}

	result = ALLOCATE(cache->num_cache_entries,
			  struct cached_page,
			  "page cache cache",
			  &cache->cache);
	if (result != UDS_SUCCESS) {
		return result;
	}

	for (i = 0; i < cache->num_cache_entries; i++) {
		struct cached_page *page = &cache->cache[i];
		result = initialize_volume_page(geometry, &page->cp_page_data);
		if (result != UDS_SUCCESS) {
			return result;
		}
		clear_cache_page(cache, page);
	}

	return UDS_SUCCESS;
}

/**********************************************************************/
int make_page_cache(const struct geometry  *geometry,
		    unsigned int chapters_in_cache,
		    unsigned int read_queue_max_size,
		    unsigned int zone_count,
		    struct page_cache **cache_ptr)
{
	if (chapters_in_cache < 1) {
		return log_warning_strerror(UDS_BAD_STATE,
					    "cache size must be"
					    " at least one chapter");
	}
	if (read_queue_max_size <= 0) {
		return log_warning_strerror(UDS_INVALID_ARGUMENT,
					    "read queue max size must be"
					    " greater than 0");
	}
	if (zone_count < 1) {
		return log_warning_strerror(UDS_INVALID_ARGUMENT,
					    "cache must have at least one zone");
	}

	struct page_cache *cache;
	int result = ALLOCATE(1, struct page_cache, "volume cache", &cache);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = initialize_page_cache(cache,
				       geometry,
				       chapters_in_cache,
				       read_queue_max_size,
				       zone_count);
	if (result != UDS_SUCCESS) {
		free_page_cache(cache);
		return result;
	}

	*cache_ptr = cache;
	return UDS_SUCCESS;
}

/**********************************************************************/
void free_page_cache(struct page_cache *cache)
{
	if (cache == NULL) {
		return;
	}
	if (cache->cache != NULL) {
		unsigned int i;
		for (i = 0; i < cache->num_cache_entries; i++) {
			destroy_volume_page(&cache->cache[i].cp_page_data);
		}
	}
	FREE(cache->index);
	FREE(cache->cache);
	FREE(cache->search_pending_counters);
	FREE(cache->read_queue);
	FREE(cache);
}

/**********************************************************************/
int invalidate_page_cache_for_chapter(struct page_cache *cache,
				      unsigned int chapter,
				      unsigned int pages_per_chapter,
				      enum invalidation_reason reason)
{
	// We hold the readThreadsMutex.
	if ((cache == NULL) || (cache->cache == NULL)) {
		return UDS_SUCCESS;
	}

	int result;
	unsigned int i;
	for (i = 0; i < pages_per_chapter; i++) {
		unsigned int physical_page =
			1 + (pages_per_chapter * chapter) + i;
		result =
		  find_invalidate_and_make_least_recent(cache,
							physical_page,
							cache->read_queue,
							reason,
							false);
		if (result != UDS_SUCCESS) {
			return result;
		}
	}

	return UDS_SUCCESS;
}

/**********************************************************************/
void make_page_most_recent(struct page_cache *cache, struct cached_page *page)
{
	// ASSERTION: We are either a zone thread holding a
	// search_pending_counter, or we are any thread holding the
	// readThreadsMutex.
	if (atomic64_read(&cache->clock) != READ_ONCE(page->cp_last_used)) {
		WRITE_ONCE(page->cp_last_used,
			   atomic64_inc_return(&cache->clock));
	}
}

/**
 * Get the least recent valid page from the cache.
 *
 * @param cache    the cache
 * @param page_ptr  a pointer to hold the new page (will be set to NULL
 *                 if the page was not found)
 *
 * @return UDS_SUCCESS or an error code
 **/
static int __must_check get_least_recent_page(struct page_cache *cache,
					      struct cached_page **page_ptr)
{
	// We hold the readThreadsMutex.
	int oldest_index = 0;
	// Our first candidate is any page that does have a pending read.  We
	// ensure above that there are more entries than read threads, so there
	// must be one.
	unsigned int i;
	for (i = 0;; i++) {
		if (i >= cache->num_cache_entries) {
			// This should never happen.
			return ASSERT(false, "oldest page is not NULL");
		}
		if (!cache->cache[i].cp_read_pending) {
			oldest_index = i;
			break;
		}
	}
	// Now find the least recently used page that does not have a pending
	// read.
	for (i = 0; i < cache->num_cache_entries; i++) {
		if (!cache->cache[i].cp_read_pending &&
		    (READ_ONCE(cache->cache[i].cp_last_used) <=
		     READ_ONCE(cache->cache[oldest_index].cp_last_used))) {
			oldest_index = i;
		}
	}
	*page_ptr = &cache->cache[oldest_index];
	return UDS_SUCCESS;
}

/**********************************************************************/
int get_page_from_cache(struct page_cache *cache,
			unsigned int physical_page,
			int probe_type,
			struct cached_page **page_ptr)
{
	// ASSERTION: We are in a zone thread.
	// ASSERTION: We holding a search_pending_counter or the
	// readThreadsMutex.
	if (cache == NULL) {
		return log_warning_strerror(UDS_BAD_STATE,
					    "cannot get page with NULL cache");
	}

	// Get the cache page from the index
	struct cached_page *page;
	int queue_index = -1;
	int result =
		get_page_no_stats(cache, physical_page, &queue_index, &page);
	if (result != UDS_SUCCESS) {
		return result;
	}

	enum cache_result_kind cache_result = ((page != NULL) ?
					       CACHE_RESULT_HIT :
						((queue_index != -1) ?
						 CACHE_RESULT_QUEUED :
						 CACHE_RESULT_MISS));
	increment_cache_counter(&cache->counters, probe_type, cache_result);

	if (page_ptr != NULL) {
		*page_ptr = page;
	}
	return UDS_SUCCESS;
}

/**********************************************************************/
int enqueue_read(struct page_cache *cache,
		 Request *request,
		 unsigned int physical_page)
{
	// We hold the readThreadsMutex.
	uint16_t first = cache->read_queue_first;
	uint16_t last = cache->read_queue_last;
	uint16_t next = (last + 1) % cache->read_queue_max_size;
	uint16_t read_queue_pos;

	if ((cache->index[physical_page] & VOLUME_CACHE_QUEUED_FLAG) == 0) {
		/* Not seen before, add this to the read queue and mark it as
		 * queued */
		if (next == first) {
			/* queue is full */
			return UDS_SUCCESS;
		}
		/* fill the read queue entry */
		cache->read_queue[last].physical_page = physical_page;
		cache->read_queue[last].invalid = false;

		/* point the cache index to it */
		read_queue_pos = last;
		WRITE_ONCE(cache->index[physical_page],
			   read_queue_pos | VOLUME_CACHE_QUEUED_FLAG);
		cache->read_queue[read_queue_pos].request_list.first = NULL;
		cache->read_queue[read_queue_pos].request_list.last = NULL;
		/* bump the last pointer */
		cache->read_queue_last = next;
	} else {
		/* It's already queued, just add on to it */
		read_queue_pos =
			cache->index[physical_page] & ~VOLUME_CACHE_QUEUED_FLAG;
	}

	int result = ASSERT((read_queue_pos < cache->read_queue_max_size),
			    "queue is not overfull");
	if (result != UDS_SUCCESS) {
		return result;
	}

	request->next_request = NULL;
	if (cache->read_queue[read_queue_pos].request_list.first == NULL) {
		cache->read_queue[read_queue_pos].request_list.first = request;
	} else {
		cache->read_queue[read_queue_pos].request_list.last->next_request = request;
	}
	cache->read_queue[read_queue_pos].request_list.last = request;
	return UDS_QUEUED;
}

/**********************************************************************/
bool reserve_read_queue_entry(struct page_cache *cache,
			      unsigned int *queue_pos,
			      Request **first_request,
			      unsigned int *physical_page,
			      bool *invalid)
{
	// We hold the readThreadsMutex.
	uint16_t last_read = cache->read_queue_last_read;

	// No items to dequeue
	if (last_read == cache->read_queue_last) {
		return false;
	}

	unsigned int page_no = cache->read_queue[last_read].physical_page;
	bool is_invalid = cache->read_queue[last_read].invalid;

	uint16_t index_value = cache->index[page_no];
	bool queued = (index_value & VOLUME_CACHE_QUEUED_FLAG) != 0;

	// ALB-1429 ... need to check to see if its still queued before
	// resetting
	if (is_invalid && queued) {
		// invalidate cache index slot
		WRITE_ONCE(cache->index[page_no], cache->num_cache_entries);
	}

	// If a sync read has taken this page, set invalid to true so we don't
	// overwrite, we simply just requeue requests.
	if (!queued) {
		is_invalid = true;
	}

	cache->read_queue[last_read].reserved = true;

	*queue_pos = last_read;
	*first_request = cache->read_queue[last_read].request_list.first;
	*physical_page = page_no;
	*invalid = is_invalid;
	cache->read_queue_last_read =
		(last_read + 1) % cache->read_queue_max_size;

	return true;
}

/**********************************************************************/
void release_read_queue_entry(struct page_cache *cache, unsigned int queue_pos)
{
	// We hold the readThreadsMutex.
	cache->read_queue[queue_pos].reserved = false;

	uint16_t last_read = cache->read_queue_last_read;

	// Move the read_queue_first pointer along when we can
	while ((cache->read_queue_first != last_read) &&
	       (!cache->read_queue[cache->read_queue_first].reserved)) {
		cache->read_queue_first = (cache->read_queue_first + 1) %
					  cache->read_queue_max_size;
	}
}

/**********************************************************************/
int select_victim_in_cache(struct page_cache *cache,
			   struct cached_page **page_ptr)
{
	// We hold the readThreadsMutex.
	if (cache == NULL) {
		return log_warning_strerror(UDS_BAD_STATE,
					    "cannot put page in NULL cache");
	}

	struct cached_page *page = NULL;
	int result = get_least_recent_page(cache, &page);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = ASSERT((page != NULL), "least recent page was not NULL");
	if (result != UDS_SUCCESS) {
		return result;
	}

	// If the page is currently being pointed to by the page map, clear
	// it from the page map, and update cache stats
	if (page->cp_physical_page != cache->num_index_entries) {
		cache->counters.evictions++;
		WRITE_ONCE(cache->index[page->cp_physical_page],
			   cache->num_cache_entries);
		wait_for_pending_searches(cache, page->cp_physical_page);
	}

	page->cp_read_pending = true;

	*page_ptr = page;

	return UDS_SUCCESS;
}

/**********************************************************************/
int put_page_in_cache(struct page_cache *cache,
		      unsigned int physical_page,
		      struct cached_page *page)
{
	// We hold the readThreadsMutex.
	if (cache == NULL) {
		return log_warning_strerror(UDS_BAD_STATE,
					    "cannot complete page in NULL cache");
	}

	int result = ASSERT((page != NULL), "page to install exists");
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = ASSERT((page->cp_read_pending),
			"page to install has a pending read");
	if (result != UDS_SUCCESS) {
		return result;
	}

	clear_cache_page(cache, page);

	page->cp_physical_page = physical_page;

	// Figure out the index into the cache array using pointer arithmetic
	uint16_t value = page - cache->cache;
	result = ASSERT((value < cache->num_cache_entries),
			"cache index is valid");
	if (result != UDS_SUCCESS) {
		return result;
	}

	make_page_most_recent(cache, page);

	page->cp_read_pending = false;

	/*
	 * We hold the readThreadsMutex, but we must have a write memory
	 * barrier before making the cached_page available to the readers
	 * that do not hold the mutex.  The corresponding read memory
	 * barrier is in get_page_no_stats.
	 */
	smp_wmb();

	// Point the page map to the new page. Will clear queued flag
	WRITE_ONCE(cache->index[physical_page], value);

	return UDS_SUCCESS;
}

/**********************************************************************/
void cancel_page_in_cache(struct page_cache *cache,
			  unsigned int physical_page,
			  struct cached_page *page)
{
	// We hold the readThreadsMutex.
	if (cache == NULL) {
		log_warning("cannot cancel page in NULL cache");
		return;
	}

	int result = ASSERT((page != NULL), "page to install exists");
	if (result != UDS_SUCCESS) {
		return;
	}

	result = ASSERT((page->cp_read_pending),
			"page to install has a pending read");
	if (result != UDS_SUCCESS) {
		return;
	}

	clear_cache_page(cache, page);
	page->cp_read_pending = false;

	// Clear the page map for the new page. Will clear queued flag
	WRITE_ONCE(cache->index[physical_page], cache->num_cache_entries);
}

/**********************************************************************/
size_t get_page_cache_size(struct page_cache *cache)
{
	if (cache == NULL) {
		return 0;
	}
	return sizeof(struct delta_index_page) * cache->num_cache_entries;
}

