/*
 * timers.c - timers and set of timers, mainly for WTP.
 *
 * See timers.h for a description of the interface.
 */

#include <signal.h>

#include "gwlib/gwlib.h"
#include "gw/wap-events.h"
#include "gw/timers.h"

/*
 * Internal functions
 */
static void abort_elapsed(Timer *timer);
static void heap_delete(List *heap, long index);
static int heap_adjust(List *heap, long index);
static void heap_insert(List *heap, Timer *timer);
static void heap_swap(List *heap, long index1, long index2);
static void lock(Timerset *set);
static void unlock(Timerset *set);
static void watch_timers(void *arg);  /* The timer thread */
static void elapse_timer(Timer *timer);


struct Timerset {
	/*
	 * This field is used to control the timer thread.
	 */
	volatile sig_atomic_t stopping;
	/*
	 * The entire set is locked for any operation on it.  This is
	 * not as expensive as it sounds because usually each set is
	 * used by one caller thread and one (internal) timer thread,
	 * and the timer thread does not wake up very often.
	 */
	Mutex *mutex;
	/*
	 * Active timers are stored here in a partially ordered structure.
	 * Each element i is the child of element i/2 (rounded down),
	 * and a child never elapses before its parent.  The result is
	 * that element 0, the top of the heap, is always the first timer
	 * to elapse.  The heap is kept in this partial order by all
	 * operations on timers.  (Maintaining a partial order is much
	 * cheaper than maintaining a sorted list.)
	 */
	List *heap;
	/*
	 * All timers contain an opaque pointer to caller's data.
	 * This pointer is produced on the output list when the
	 * timer elapses.  The timer is not considered to have
	 * elapsed completely until that pointer has also been
	 * consumed from this list (by the caller, presumably).
	 * That is why the timer code sometimes goes back and
	 * removes a pointer from the output list.
	 */
	List *output;
	long timer_thread;
};

struct Timer {
	/*
	 * The set this timer is associated with.
	 */
	Timerset *set;
	/*
	 * The timer is set to elapse at this time, expressed in
	 * Unix time format.  This field is set to -1 if the timer
	 * is not active (i.e. in the timer set's heap).
	 */
	long elapses;
	/*
	 * A duplicate of this event will be put on the output list
	 * when the timer elapses.  It can be NULL if the timer has
	 * not been started yet.
	 */
	WAPEvent *event;
	/*
	 * This field is normally NULL, but after the timer elapses
	 * it points to the event that was put on the output list.
	 * It is set back to NULL if the event was taken back from
	 * the list, or if it's confirmed that the event was consumed.
	 */
	WAPEvent *elapsed_event;
	/*
	 * Index in the timer set's heap.  This field is managed by
	 * the heap operations, and is used to make them faster.
	 * If this timer is not in the heap, this field is -1.
	 */
	long index;
};

Timerset *timerset_create(List *outputlist) {
	Timerset *set;

	set = gw_malloc(sizeof(*set));
	set->mutex = mutex_create();
	set->heap = list_create();
	set->output = outputlist;
	list_add_producer(outputlist);
	set->stopping = 0;
	set->timer_thread = gwthread_create(watch_timers, set);

	return set;
}

void timerset_destroy(Timerset *set) {
	if (set == NULL)
		return;

	/* Stop all timers.  */
	while (list_len(set->heap) > 0)
		timer_stop(list_get(set->heap, 0));

	/* Kill timer thread */
	set->stopping = 1;
	gwthread_wakeup(set->timer_thread);
	gwthread_join(set->timer_thread);

	/* Free resources */
	list_remove_producer(set->output);
	list_destroy(set->heap, NULL);
	mutex_destroy(set->mutex);
	gw_free(set);
}
	

Timer *timer_create(Timerset *set) {
	Timer *t;

	t = gw_malloc(sizeof(*t));
	t->set = set;
	t->elapses = -1;
	t->event = NULL;
	t->elapsed_event = NULL;
	t->index = -1;

	return t;
}

void timer_destroy(Timer *timer) {
	if (timer == NULL)
		return;

	timer_stop(timer);
	wap_event_destroy(timer->event);
	gw_free(timer);
}

void timer_start(Timer *timer, int interval, WAPEvent *event) {
	int wakeup = 0;

	gw_assert(timer != NULL);
	gw_assert(event != NULL || timer->event != NULL);

	lock(timer->set);

	/* Convert to absolute time */
	interval += time(NULL);

	if (timer->elapses > 0) {
		/* Resetting an existing timer.  Move it to its new
		 * position in the heap. */
		if (interval < timer->elapses && timer->index == 0)
			wakeup = 1;
		timer->elapses = interval;
		gw_assert(list_get(timer->set->heap, timer->index) == timer);
		wakeup |= heap_adjust(timer->set->heap, timer->index);

		/* Then set its new event, if necessary. */
		if (event != NULL) {
			wap_event_destroy(timer->event);
			timer->event = event;
		}
	} else {
		/* Setting a new timer, or resetting an elapsed one.
		 * First deal with a possible elapse event that may
		 * still be on the output list. */
		abort_elapsed(timer);

		/* Then activate the timer. */
		timer->elapses = interval;
		gw_assert(timer->index < 0);
		heap_insert(timer->set->heap, timer);
		wakeup = timer->index == 0; /* Do we have a new top? */
	}

	unlock(timer->set);

	if (wakeup)
		gwthread_wakeup(timer->set->timer_thread);
}

void timer_stop(Timer *timer) {
	gw_assert(timer != NULL);
	lock(timer->set);

	/*
	 * If the timer is active, make it inactive and remove it from
	 * the heap.
	 */
	if (timer->elapses > 0) {
		timer->elapses = -1;
		gw_assert(list_get(timer->set->heap, timer->index) == timer);
		heap_delete(timer->set->heap, timer->index);
	}

	abort_elapsed(timer);

	unlock(timer->set);
}

static void lock(Timerset *set) {
	gw_assert(set != NULL);
	mutex_lock(set->mutex);
}

static void unlock(Timerset *set) {
	gw_assert(set != NULL);
	mutex_unlock(set->mutex);
}

/*
 * Go back and remove this timer's elapse event from the output list,
 * to pretend that it didn't elapse after all.  This is necessary
 * to deal with some races between the timer thread and the caller's
 * start/stop actions.
 */
static void abort_elapsed(Timer *timer) {
	long count;

	if (timer->elapsed_event == NULL)
		return;

	count = list_delete_equal(timer->set->output, timer->elapsed_event);
	if (count > 0)
		wap_event_destroy(timer->elapsed_event);
	timer->elapsed_event = NULL;
}

/*
 * Remove a timer from the heap.  Do this by swapping it with the element
 * in the last position, then shortening the heap, then moving the
 * swapped element up or down to maintain the partial ordering.
 */
static void heap_delete(List *heap, long index) {
	Timer *t;
	long last;

	t = list_get(heap, index);
	last = list_len(heap) - 1;
	if (index == last) {
		list_delete(heap, last, 1);
	} else {
		heap_swap(heap, index, last);
		list_delete(heap, last, 1);
		heap_adjust(heap, index);
	}
	t->index = -1;
}

/*
 * Add a timer to the heap.  Do this by adding it at the end, then
 * moving it up or down as necessary to achieve partial ordering.
 */
static void heap_insert(List *heap, Timer *timer) {
	list_append(heap, timer);
	timer->index = list_len(heap) - 1;
	heap_adjust(heap, timer->index);
}

/*
 * Swap two elements of the heap, and update their index fields.
 * This is the basic heap operation.
 */
static void heap_swap(List *heap, long index1, long index2) {
	Timer *t;

	list_swap(heap, index1, index2);
	t = list_get(heap, index1);
	t->index = index1;
	t = list_get(heap, index2);
	t->index = index2;
}

/*
 * The current element has broken the partial ordering of the
 * heap (see explanation in the definition of Timerset), and
 * it has to be moved up or down until the ordering is restored.
 * Return 1 if the timer at the heap's top is now earlier than
 * before this operation, otherwise 0.
 */
static int heap_adjust(List *heap, long index) {
	Timer *t;
	Timer *parent;
	Timer *child;
	Timer *child2;
	long child_index;

	/*
	 * We can assume that the heap was fine before this element's
	 * elapse time was changed.  There are three cases to deal
	 * with:
	 *  - Element's new elapse time is too small; it should be
	 *    moved toward the top.
	 *  - Element's new elapse time is too large; it should be
	 *    moved toward the bottom.
	 *  - Element's new elapse time still fits here, we don't
	 *    have to do anything.
	 */

	/* Move to top? */
	t = list_get(heap, index);
	parent = list_get(heap, index / 2);
	if (t->elapses < parent->elapses) {
		/* This will automatically terminate when it reaches
		 * the top, because in that case t == parent. */
		while (t->elapses < parent->elapses) {
			heap_swap(heap, index, index / 2);
			index = index / 2;
			parent = list_get(heap, index / 2);
		}
		/* We're done.  Return 1 if we changed the top. */
		return index == 0;
	}
	
	/* Move to bottom? */
	for (;;) {
		child_index = index * 2;
		if (child_index >= list_len(heap))
			return 0;  /* Already at bottom */
		child = list_get(heap, child_index);
		if (child_index == list_len(heap) - 1) {
			/* Only one child */
			if (child->elapses < t->elapses)
				heap_swap(heap, index, child_index);
			break;
		}

		/* Find first child */
		child2 = list_get(heap, child_index + 1);
		if (child2->elapses < child->elapses) {
			child = child2;
			child_index++;
		}

		if (child->elapses < t->elapses) {
			heap_swap(heap, index, child_index);
			index = child_index;
		} else {
			break;
		}
	}

	return 0;
}

/*
 * This timer has elapsed.  Do the housekeeping.  The timer has already
 * been deleted from its heap.  We have its set locked.
 */
static void elapse_timer(Timer *timer) {
	gw_assert(timer != NULL);
	gw_assert(timer->set != NULL);
	/* This must be true because abort_elapsed is always called
	 * before a timer is activated. */
	gw_assert(timer->elapsed_event == NULL);

	timer->elapsed_event = wap_event_duplicate(timer->event);
	list_produce(timer->set->output, timer->elapsed_event);
	timer->elapses = -1;
}

/*
 * Main function for timer thread.
 */
static void watch_timers(void *arg) {
	Timerset *set;
	Timer *top;
	long top_time;
	long now;

	set = arg;

	while (!set->stopping) {
		lock(set);

		/* Are there any timers to watch? */
		if (list_len(set->heap) == 0) {
			unlock(set);
			gwthread_sleep(1000000.0);  /* Sleep very long */
			continue;
		}

		/* Does the top timer elapse? */
		top = list_get(set->heap, 0);
		top_time = top->elapses;
		now = time(NULL);
		if (top_time <= now) {
			heap_delete(set->heap, 0);
			elapse_timer(top);
			unlock(set);
			continue;
		}

		/* Sleep until the top timer elapses (or we get woken up) */
		unlock(set);
		gwthread_sleep(top_time - now);
	}
}
