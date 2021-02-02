/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 * Copyright 2021 Oxide Computer Company
 */

/*
 * Copyright 2009 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 * Copyright 2018 Joyent, Inc.
 */

/* Copyright (c) 1984, 1986, 1987, 1988, 1989 AT&T */
/* All Rights Reserved */

/*
 * University Copyright- Copyright (c) 1982, 1986, 1988
 * The Regents of the University of California
 * All Rights Reserved
 *
 * University Acknowledgment- Portions of this document are derived from
 * software developed by the University of California, Berkeley, and its
 * contributors.
 */

#include <sys/types.h>
#include <sys/t_lock.h>
#include <sys/param.h>
#include <sys/buf.h>
#include <sys/uio.h>
#include <sys/proc.h>
#include <sys/systm.h>
#include <sys/mman.h>
#include <sys/cred.h>
#include <sys/vnode.h>
#include <sys/vm.h>
#include <sys/vmparam.h>
#include <sys/vtrace.h>
#include <sys/cmn_err.h>
#include <sys/cpuvar.h>
#include <sys/user.h>
#include <sys/kmem.h>
#include <sys/debug.h>
#include <sys/callb.h>
#include <sys/tnf_probe.h>
#include <sys/mem_cage.h>
#include <sys/time.h>
#include <sys/zone.h>
#include <sys/stdbool.h>

#include <vm/hat.h>
#include <vm/as.h>
#include <vm/seg.h>
#include <vm/page.h>
#include <vm/pvn.h>
#include <vm/seg_kmem.h>

/*
 * FREE MEMORY MANAGEMENT
 *
 * Management of the pool of free pages is a tricky business.  There are
 * several critical threshold values which constrain our allocation of new
 * pages and inform the rate of paging out of memory to swap.  These threshold
 * values, and the behaviour they induce, are described below in descending
 * order of size -- and thus increasing order of severity!
 *
 *   +---------------------------------------------------- physmem (all memory)
 *   |
 *   | Ordinarily there are no particular constraints placed on page
 *   v allocation.  The page scanner is not running and page_create_va()
 *   | will effectively grant all page requests (whether from the kernel
 *   | or from user processes) without artificial delay.
 *   |
 *   +------------------------ lotsfree (1.56% of physmem, min. 16MB, max. 2GB)
 *   |
 *   | When we have less than "lotsfree" pages, pageout_scanner() is
 *   v signalled by schedpaging() to begin looking for pages that can
 *   | be evicted to disk to bring us back above lotsfree.  At this
 *   | stage there is still no constraint on allocation of free pages.
 *   |
 *   | For small systems, we set a lower bound of 16MB for lotsfree;
 *   v this is the natural value for a system with 1GB memory.  This is
 *   | to ensure that the pageout reserve pool contains at least 4MB
 *   | for use by ZFS.
 *   |
 *   | For systems with a large amount of memory, we constrain lotsfree
 *   | to be at most 2GB (with a pageout reserve of around 0.5GB), as
 *   v at some point the required slack relates more closely to the
 *   | rate at which paging can occur than to the total amount of memory.
 *   |
 *   +------------------- desfree (1/2 of lotsfree, 0.78% of physmem, min. 8MB)
 *   |
 *   | When we drop below desfree, a number of kernel facilities will
 *   v wait before allocating more memory, under the assumption that
 *   | pageout or reaping will make progress and free up some memory.
 *   | This behaviour is not especially coordinated; look for comparisons
 *   | of desfree and freemem.
 *   |
 *   | In addition to various attempts at advisory caution, clock()
 *   | will wake up the thread that is ordinarily parked in sched().
 *   | This routine is responsible for the heavy-handed swapping out
 *   v of entire processes in an attempt to arrest the slide of free
 *   | memory.  See comments in sched.c for more details.
 *   |
 *   +----- minfree & throttlefree (3/4 of desfree, 0.59% of physmem, min. 6MB)
 *   |
 *   | These two separate tunables have, by default, the same value.
 *   v Various parts of the kernel use minfree to signal the need for
 *   | more aggressive reclamation of memory, and sched() is more
 *   | aggressive at swapping processes out.
 *   |
 *   | If free memory falls below throttlefree, page_create_va() will
 *   | use page_create_throttle() to begin holding most requests for
 *   | new pages while pageout and reaping free up memory.  Sleeping
 *   v allocations (e.g., KM_SLEEP) are held here while we wait for
 *   | more memory.  Non-sleeping allocations are generally allowed to
 *   | proceed, unless their priority is explicitly lowered with
 *   | KM_NORMALPRI.
 *   |
 *   +------- pageout_reserve (3/4 of throttlefree, 0.44% of physmem, min. 4MB)
 *   |
 *   | When we hit throttlefree, the situation is already dire.  The
 *   v system is generally paging out memory and swapping out entire
 *   | processes in order to free up memory for continued operation.
 *   |
 *   | Unfortunately, evicting memory to disk generally requires short
 *   | term use of additional memory; e.g., allocation of buffers for
 *   | storage drivers, updating maps of free and used blocks, etc.
 *   | As such, pageout_reserve is the number of pages that we keep in
 *   | special reserve for use by pageout() and sched() and by any
 *   v other parts of the kernel that need to be working for those to
 *   | make forward progress such as the ZFS I/O pipeline.
 *   |
 *   | When we are below pageout_reserve, we fail or hold any allocation
 *   | that has not explicitly requested access to the reserve pool.
 *   | Access to the reserve is generally granted via the KM_PUSHPAGE
 *   | flag, or by marking a thread T_PUSHPAGE such that all allocations
 *   | can implicitly tap the reserve.  For more details, see the
 *   v NOMEMWAIT() macro, the T_PUSHPAGE thread flag, the KM_PUSHPAGE
 *   | and VM_PUSHPAGE allocation flags, and page_create_throttle().
 *   |
 *   +---------------------------------------------------------- no free memory
 *   |
 *   | If we have arrived here, things are very bad indeed.  It is
 *   v surprisingly difficult to tell if this condition is even fatal,
 *   | as enough memory may have been granted to pageout() and to the
 *   | ZFS I/O pipeline that requests for eviction that have already been
 *   | made will complete and free up memory some time soon.
 *   |
 *   | If free memory does not materialise, the system generally remains
 *   | deadlocked.  The pageout_deadman() below is run once per second
 *   | from clock(), seeking to limit the amount of time a single request
 *   v to page out can be blocked before the system panics to get a crash
 *   | dump and return to service.
 *   |
 *   +-------------------------------------------------------------------------
 */

/*
 * The following parameters control operation of the page replacement
 * algorithm.  They are initialized to 0, and then computed at boot time
 * based on the size of the system.  If they are patched non-zero in
 * a loaded vmunix they are left alone and may thus be changed per system
 * using mdb on the loaded system.
 */
pgcnt_t		slowscan = 0;
pgcnt_t		fastscan = 0;

static pgcnt_t	handspreadpages = 0;
static int	loopfraction = 2;
static pgcnt_t	looppages;
/* See comment below describing 4% and 80% */
static int	min_percent_cpu = 4;
static int	max_percent_cpu = 80;
static pgcnt_t	maxfastscan = 0;
static pgcnt_t	maxslowscan = 100;

/*
 * The operator may override these tunables to request a different minimum or
 * maximum lotsfree value, or to change the divisor we use for automatic
 * sizing.
 *
 * By default, we make lotsfree 1/64th of the total memory in the machine.  The
 * minimum and maximum are specified in bytes, rather than pages; a zero value
 * means the default values (below) are used.
 */
uint_t		lotsfree_fraction = 64;
pgcnt_t		lotsfree_min = 0;
pgcnt_t		lotsfree_max = 0;

pgcnt_t	maxpgio = 0;
pgcnt_t	minfree = 0;
pgcnt_t	desfree = 0;
pgcnt_t	lotsfree = 0;
pgcnt_t	needfree = 0;
pgcnt_t	throttlefree = 0;
pgcnt_t	pageout_reserve = 0;

pgcnt_t	deficit;
pgcnt_t	nscan;
pgcnt_t	desscan;

#define		MEGABYTES		(1024ULL * 1024ULL)

/*
 * pageout_threshold_style:
 *     set to 1 to use the previous default threshold size calculation;
 *     i.e., each threshold is half of the next largest value.
 */
uint_t		pageout_threshold_style = 0;

#define		LOTSFREE_MIN_DEFAULT	(16 * MEGABYTES)
#define		LOTSFREE_MAX_DEFAULT	(2048 * MEGABYTES)

/* kstats */
uint64_t low_mem_scan;
uint64_t zone_cap_scan;
uint64_t n_throttle;

/*
 * Values for min_pageout_nsec, max_pageout_nsec, pageout_nsec and
 * zone_pageout_nsec are the number of nanoseconds in each wakeup cycle
 * that gives the equivalent of some underlying %CPU duty cycle.
 *
 * min_pageout_nsec:
 *     nanoseconds/wakeup equivalent of min_percent_cpu.
 *
 * max_pageout_nsec:
 *     nanoseconds/wakeup equivalent of max_percent_cpu.
 *
 * pageout_nsec:
 *     Number of nanoseconds budgeted for each wakeup cycle.
 *     Computed each time around by schedpaging().
 *     Varies between min_pageout_nsec and max_pageout_nsec,
 *     depending on memory pressure or zones over their cap.
 *
 * zone_pageout_nsec:
 *     Number of nanoseconds budget for each cycle when a zone
 *     is over its memory cap. If this is zero, then the value
 *     of max_pageout_nsec is used instead.
 */

static hrtime_t		min_pageout_nsec;
static hrtime_t		max_pageout_nsec;
static hrtime_t		pageout_nsec;
static hrtime_t		zone_pageout_nsec;

#define	MAX_PSCAN_THREADS	16
static boolean_t reset_hands[MAX_PSCAN_THREADS];

/*
 * These can be tuned in /etc/system or set with mdb.
 * 'des_page_scanners' is the desired number of page scanner threads. The
 * system will bring the actual number of threads into line with the desired
 * number. If des_page_scanners is set to an invalid value, the system will
 * correct the setting.
 */
uint_t des_page_scanners;
uint_t pageout_reset_cnt = 64;	/* num. cycles for pageout_scanner hand reset */

uint_t n_page_scanners;
static pgcnt_t	pscan_region_sz; /* informational only */


#define	PAGES_POLL_MASK	1023

/*
 * pageout_sample_lim:
 *     The limit on the number of samples needed to establish a value
 *     for new pageout parameters, fastscan, slowscan, and handspreadpages.
 *
 * pageout_sample_cnt:
 *     Current sample number.  Once the sample gets large enough,
 *     set new values for handspreadpages, fastscan and slowscan.
 *
 * pageout_sample_pages:
 *     The accumulated number of pages scanned during sampling.
 *
 * pageout_sample_etime:
 *     The accumulated number of nanoseconds for the sample.
 *
 * pageout_rate:
 *     Rate in pages/second, computed at the end of sampling.
 *
 * pageout_new_spread:
 *     The new value to use for maxfastscan and (perhaps) handspreadpages.
 *     Intended to be the number pages that can be scanned per sec using ~10%
 *     of a CPU. Calculated after enough samples have been taken.
 *     pageout_rate / 10
 */

typedef hrtime_t hrrate_t;

static uint_t	pageout_sample_lim = 4;
static uint_t	pageout_sample_cnt = 0;
static pgcnt_t	pageout_sample_pages = 0;
static hrrate_t	pageout_rate = 0;
static pgcnt_t	pageout_new_spread = 0;

static hrtime_t	pageout_sample_etime = 0;

/* True if page scanner is first starting up */
#define	PAGE_SCAN_STARTUP	(pageout_sample_cnt < pageout_sample_lim)

/*
 * Record number of times a pageout_scanner wakeup cycle finished because it
 * timed out (exceeded its CPU budget), rather than because it visited
 * its budgeted number of pages. This is only done when scanning under low
 * free memory conditions, not when scanning for zones over their cap.
 */
uint64_t pageout_timeouts = 0;

#ifdef VM_STATS
static struct pageoutvmstats_str {
	ulong_t	checkpage[3];
} pageoutvmstats;
#endif /* VM_STATS */

/*
 * Threads waiting for free memory use this condition variable and lock until
 * memory becomes available.
 */
kmutex_t	memavail_lock;
kcondvar_t	memavail_cv;

typedef enum pageout_hand {
	POH_FRONT = 1,
	POH_BACK,
} pageout_hand_t;

typedef enum {
	CKP_INELIGIBLE,
	CKP_NOT_FREED,
	CKP_FREED,
} checkpage_result_t;

static checkpage_result_t checkpage(page_t *, pageout_hand_t);

static struct clockinit {
	bool ci_init;
	pgcnt_t ci_lotsfree_min;
	pgcnt_t ci_lotsfree_max;
	pgcnt_t ci_lotsfree;
	pgcnt_t ci_desfree;
	pgcnt_t ci_minfree;
	pgcnt_t ci_throttlefree;
	pgcnt_t ci_pageout_reserve;
	pgcnt_t ci_maxpgio;
	pgcnt_t ci_maxfastscan;
	pgcnt_t ci_fastscan;
	pgcnt_t ci_slowscan;
	pgcnt_t ci_handspreadpages;
} clockinit = { .ci_init = false };

static pgcnt_t
clamp(pgcnt_t value, pgcnt_t minimum, pgcnt_t maximum)
{
	if (value < minimum) {
		return (minimum);
	} else if (value > maximum) {
		return (maximum);
	} else {
		return (value);
	}
}

static pgcnt_t
tune(pgcnt_t initval, pgcnt_t initval_ceiling, pgcnt_t defval)
{
	if (initval == 0 || initval >= initval_ceiling) {
		return (defval);
	} else {
		return (initval);
	}
}

/*
 * Local boolean to control scanning when zones are over their cap. Avoids
 * accessing the zone_num_over_cap variable except within schedpaging(), which
 * only runs periodically. This is here only to reduce our access to
 * zone_num_over_cap, since it is already accessed a lot during paging, and
 * the page scanner accesses the zones_over variable on each page during a
 * scan. There is no lock needed for zone_num_over_cap since schedpaging()
 * doesn't modify the variable, it only cares if the variable is 0 or non-0.
 */
static boolean_t zones_over = B_FALSE;

/*
 * Set up the paging constants for the page scanner clock-hand algorithm.
 * Called at startup after the system is initialized and the amount of memory
 * and number of paging devices is known (recalc will be 0). Called again once
 * PAGE_SCAN_STARTUP is true after the scanner has collected enough samples
 * (recalc will be 1).
 *
 * Will also be called after a memory dynamic reconfiguration operation and
 * recalc will be 1 in those cases too.
 *
 * lotsfree is 1/64 of memory, but at least 512K (ha!).
 * desfree is 1/2 of lotsfree.
 * minfree is 1/2 of desfree.
 */
void
setupclock(void)
{
	uint_t i;
	pgcnt_t sz, tmp;
	pgcnt_t defval;
	bool half = (pageout_threshold_style == 1);
	bool recalc = true;

	looppages = total_pages;

	/*
	 * The operator may have provided specific values for some of the
	 * tunables via /etc/system.  On our first call, we preserve those
	 * values so that they can be used for subsequent recalculations.
	 *
	 * A value of zero for any tunable means we will use the default
	 * sizing.
	 */

	if (!clockinit.ci_init) {
		clockinit.ci_init = true;

		clockinit.ci_lotsfree_min = lotsfree_min;
		clockinit.ci_lotsfree_max = lotsfree_max;
		clockinit.ci_lotsfree = lotsfree;
		clockinit.ci_desfree = desfree;
		clockinit.ci_minfree = minfree;
		clockinit.ci_throttlefree = throttlefree;
		clockinit.ci_pageout_reserve = pageout_reserve;
		clockinit.ci_maxpgio = maxpgio;
		clockinit.ci_maxfastscan = maxfastscan;
		clockinit.ci_fastscan = fastscan;
		clockinit.ci_slowscan = slowscan;
		clockinit.ci_handspreadpages = handspreadpages;
		/*
		 * The first call does not trigger a recalculation, only
		 * subsequent calls.
		 */
		recalc = false;
	}

	/*
	 * Configure paging threshold values.  For more details on what each
	 * threshold signifies, see the comments at the top of this file.
	 */
	lotsfree_max = tune(clockinit.ci_lotsfree_max, looppages,
	    btop(LOTSFREE_MAX_DEFAULT));
	lotsfree_min = tune(clockinit.ci_lotsfree_min, lotsfree_max,
	    btop(LOTSFREE_MIN_DEFAULT));

	lotsfree = tune(clockinit.ci_lotsfree, looppages,
	    clamp(looppages / lotsfree_fraction, lotsfree_min, lotsfree_max));

	desfree = tune(clockinit.ci_desfree, lotsfree,
	    lotsfree / 2);

	minfree = tune(clockinit.ci_minfree, desfree,
	    half ? desfree / 2 : 3 * desfree / 4);

	throttlefree = tune(clockinit.ci_throttlefree, desfree,
	    minfree);

	pageout_reserve = tune(clockinit.ci_pageout_reserve, throttlefree,
	    half ? throttlefree / 2 : 3 * throttlefree / 4);

	/*
	 * Maxpgio thresholds how much paging is acceptable.
	 * This figures that 2/3 busy on an arm is all that is
	 * tolerable for paging.  We assume one operation per disk rev.
	 *
	 * XXX - Does not account for multiple swap devices.
	 */
	if (clockinit.ci_maxpgio == 0) {
		maxpgio = (DISKRPM * 2) / 3;
	} else {
		maxpgio = clockinit.ci_maxpgio;
	}

	/*
	 * When the system is in a low memory state, the page scan rate varies
	 * between fastscan and slowscan based on the amount of free memory
	 * available. When only zones are over their memory cap, the scan rate
	 * is always fastscan.
	 *
	 * The fastscan rate should be set based on the number pages that can
	 * be scanned per sec using ~10% of a CPU. Since this value depends on
	 * the processor, MMU, Ghz etc., it must be determined dynamically.
	 *
	 * When the scanner first starts up, fastscan will be set to 0 and
	 * maxfastscan will be set to MAXHANDSPREADPAGES (64MB, in pages).
	 * However, once the scanner has collected enough samples, then fastscan
	 * is set to be the smaller of 1/2 of memory (looppages / loopfraction)
	 * or maxfastscan (which is set from pageout_new_spread). Thus,
	 * MAXHANDSPREADPAGES is irrelevant after the scanner is fully
	 * initialized.
	 *
	 * pageout_new_spread is calculated when the scanner first starts
	 * running. During this initial sampling period the nscan_limit
	 * is set to the total_pages of system memory. Thus, the scanner could
	 * theoretically scan all of memory in one pass. However, each sample
	 * is also limited by the %CPU budget. This is controlled by
	 * pageout_nsec which is set in schedpaging(). During the sampling
	 * period, pageout_nsec is set to max_pageout_nsec. This value
	 * is derived from the max_percent_cpu (80%) described above. On a
	 * system with more than a small amount of memory (~8GB), the scanner's
	 * %CPU will be the limiting factor in calculating pageout_new_spread.
	 *
	 * At the end of the sampling period, the pageout_rate indicates how
	 * many pages could be scanned per second. The pageout_new_spread is
	 * then set to be 1/10th of that (i.e. approximating 10% of a CPU).
	 * Of course, this value could still be more than the physical memory
	 * on the system. If so, fastscan is set to 1/2 of memory, as
	 * mentioned above.
	 *
	 * All of this leads up to the setting of handspreadpages, which is
	 * set to fastscan. This is the distance, in pages, between the front
	 * and back hands during scanning. It will dictate which pages will
	 * be considered "hot" on the backhand and which pages will be "cold"
	 * and reclaimed
	 *
	 * If the scanner is limited by desscan, then at the highest rate it
	 * will scan up to fastscan/SCHEDPAGING_HZ pages per cycle. If the
	 * scanner is limited by the %CPU, then at the highest rate (20% of a
	 * CPU per cycle) the number of pages scanned could be much less.
	 *
	 * Thus, if the scanner is limited by desscan, then the handspreadpages
	 * setting means 1sec between the front and back hands, but if the
	 * scanner is limited by %CPU, it could be several seconds between the
	 * two hands.
	 *
	 * The basic assumption is that at the worst case, stealing pages
	 * not accessed within 1 sec seems reasonable and ensures that active
	 * user processes don't thrash. This is especially true when the system
	 * is in a low memory state.
	 *
	 * There are some additional factors to consider for the case of
	 * scanning when zones are over their cap. In this situation it is
	 * also likely that the machine will have a large physical memory which
	 * will take many seconds to fully scan (due to the %CPU and desscan
	 * limits per cycle). It is probable that there will be few (or 0)
	 * pages attributed to these zones in any single scanning cycle. The
	 * result is that reclaiming enough pages for these zones might take
	 * several additional seconds (this is generally not a problem since
	 * the zone physical cap is just a soft cap).
	 *
	 * This is similar to the typical multi-processor situation in which
	 * pageout is often unable to maintain the minimum paging thresholds
	 * under heavy load due to the fact that user processes running on
	 * other CPU's can be dirtying memory at a much faster pace than
	 * pageout can find pages to free.
	 *
	 * One potential approach to address both of these cases is to enable
	 * more than one CPU to run the page scanner, in such a manner that the
	 * various clock hands don't overlap. However, this also makes it more
	 * difficult to determine the values for fastscan, slowscan and
	 * handspreadpages. This is left as a future enhancement, if necessary.
	 *
	 * When free memory falls just below lotsfree, the scan rate goes from
	 * 0 to slowscan (i.e., the page scanner starts running).  This
	 * transition needs to be smooth and is achieved by ensuring that
	 * pageout scans a small number of pages to satisfy the transient
	 * memory demand.  This is set to not exceed 100 pages/sec (25 per
	 * wakeup) since scanning that many pages has no noticible impact
	 * on system performance.
	 *
	 * The swapper is currently used to free up memory when pageout is
	 * unable to meet memory demands. It does this by swapping out entire
	 * processes. In addition to freeing up memory, swapping also reduces
	 * the demand for memory because the swapped out processes cannot
	 * run, and thereby consume memory. However, this is a pathological
	 * state and performance will generally be considered unacceptable.
	 */
	if (clockinit.ci_maxfastscan == 0) {
		if (pageout_new_spread != 0) {
			maxfastscan = pageout_new_spread;
		} else {
			maxfastscan = MAXHANDSPREADPAGES;
		}
	} else {
		maxfastscan = clockinit.ci_maxfastscan;
	}

	if (clockinit.ci_fastscan == 0) {
		fastscan = MIN(looppages / loopfraction, maxfastscan);
	} else {
		fastscan = clockinit.ci_fastscan;
	}
	if (fastscan > looppages / loopfraction)
		fastscan = looppages / loopfraction;

	/*
	 * Set slow scan time to 1/10 the fast scan time, but
	 * not to exceed maxslowscan.
	 */
	if (clockinit.ci_slowscan == 0) {
		slowscan = MIN(fastscan / 10, maxslowscan);
	} else {
		slowscan = clockinit.ci_slowscan;
	}

	if (slowscan > fastscan / 2) {
		slowscan = fastscan / 2;
	}

	/*
	 * Handspreadpages is distance (in pages) between front and back
	 * pageout daemon hands.  The amount of time to reclaim a page
	 * once pageout examines it increases with this distance and
	 * decreases as the scan rate rises. It must be < the amount
	 * of pageable memory.
	 *
	 * Since pageout is limited to the %CPU per cycle, setting
	 * handspreadpages to be "fastscan" results in the front hand being
	 * a few secs (varies based on the processor speed) ahead of the back
	 * hand at fastscan rates.
	 *
	 * As a result, user processes have a much better chance of
	 * referencing their pages before the back hand examines them.
	 * This also significantly lowers the number of reclaims from
	 * the freelist since pageout does not end up freeing pages which
	 * may be referenced a sec later.
	 */
	if (clockinit.ci_handspreadpages == 0) {
		handspreadpages = fastscan;
	} else {
		handspreadpages = clockinit.ci_handspreadpages;
	}

	/*
	 * Make sure that back hand follows front hand by at least
	 * 1/SCHEDPAGING_HZ seconds.  Without this test, it is possible for the
	 * back hand to look at a page during the same wakeup of the pageout
	 * daemon in which the front hand cleared its ref bit.
	 */
	if (handspreadpages >= looppages) {
		handspreadpages = looppages - 1;
	}

	if (!recalc) {
		/*
		 * Setup basic values at initialization.
		 */
		pscan_region_sz = total_pages;
		des_page_scanners = n_page_scanners = 1;
		reset_hands[0] = B_TRUE;
		return;
	}

	/*
	 * Recalculating
	 *
	 * We originally set the number of page scanners to 1. Now that we
	 * know what the handspreadpages is for a scanner, figure out how many
	 * scanners we should run. We want to ensure that the regions don't
	 * overlap and that they are not touching.
	 *
	 * A default 64GB region size is used as the initial value to calculate
	 * how many scanner threads we should create on lower memory systems.
	 * The idea is to limit the number of threads to a practical value
	 * (e.g. a 64GB machine really only needs one scanner thread). For very
	 * large memory systems, we limit ourselves to MAX_PSCAN_THREADS
	 * threads.
	 *
	 * The scanner threads themselves are evenly spread out around the
	 * memory "clock" in pageout_scanner when we reset the hands, and each
	 * thread will scan all of memory.
	 */
	sz = (btop(64ULL * 0x40000000ULL));
	if (sz < handspreadpages) {
		/*
		 * 64GB is smaller than the separation between the front
		 * and back hands; use double handspreadpages.
		 */
		sz = handspreadpages << 1;
	}
	if (sz > total_pages) {
		sz = total_pages;
	}
	/* Record region size for inspection with mdb, otherwise unused */
	pscan_region_sz = sz;

	tmp = sz;
	for (i = 1; tmp < total_pages; i++) {
		tmp += sz;
	}

	if (i > MAX_PSCAN_THREADS)
		i = MAX_PSCAN_THREADS;

	des_page_scanners = i;
}

/*
 * Pageout scheduling.
 *
 * Schedpaging controls the rate at which the page out daemon runs by
 * setting the global variables pageout_nsec and desscan SCHEDPAGING_HZ
 * times a second. The pageout_nsec variable controls the percent of one
 * CPU that each page scanner thread should consume (see min_percent_cpu
 * and max_percent_cpu descriptions). The desscan variable records the number
 * of pages pageout should examine in its next pass; schedpaging sets this
 * value based on the amount of currently available memory. In addtition, the
 * nscan variable records the number of pages pageout has examined in its
 * current pass; schedpaging resets this value to zero each time it runs.
 */

#define	SCHEDPAGING_HZ	4		/* times/second */

/* held while pageout_scanner or schedpaging are modifying shared data */
static kmutex_t	pageout_mutex;

/*
 * Pool of available async pageout putpage requests.
 */
static struct async_reqs *push_req;
static struct async_reqs *req_freelist;	/* available req structs */
static struct async_reqs *push_list;	/* pending reqs */
static kmutex_t push_lock;		/* protects req pool */
static kcondvar_t push_cv;

/*
 * If pageout() is stuck on a single push for this many seconds,
 * pageout_deadman() will assume the system has hit a memory deadlock.  If set
 * to 0, the deadman will have no effect.
 *
 * Note that we are only looking for stalls in the calls that pageout() makes
 * to VOP_PUTPAGE().  These calls are merely asynchronous requests for paging
 * I/O, which should not take long unless the underlying strategy call blocks
 * indefinitely for memory.  The actual I/O request happens (or fails) later.
 */
uint_t pageout_deadman_seconds = 90;

static uint_t pageout_stucktime = 0;
static bool pageout_pushing = false;
static uint64_t pageout_pushcount = 0;
static uint64_t pageout_pushcount_seen = 0;

static int async_list_size = 256;	/* number of async request structs */

static void pageout_scanner(void *);

/*
 * If a page is being shared more than "po_share" times
 * then leave it alone- don't page it out.
 */
#define	MIN_PO_SHARE	(8)
#define	MAX_PO_SHARE	((MIN_PO_SHARE) << 24)
ulong_t	po_share = MIN_PO_SHARE;

/*
 * Schedule rate for paging.
 * Rate is linear interpolation between
 * slowscan with lotsfree and fastscan when out of memory.
 */
static void
schedpaging(void *arg)
{
	spgcnt_t vavail;

	if (freemem < lotsfree + needfree + kmem_reapahead)
		kmem_reap();

	if (freemem < lotsfree + needfree)
		seg_preap();

	if (kcage_on && (kcage_freemem < kcage_desfree || kcage_needfree))
		kcage_cageout_wakeup();

	(void) atomic_swap_ulong(&nscan, 0);
	vavail = freemem - deficit;
	if (pageout_new_spread != 0)
		vavail -= needfree;
	if (vavail < 0)
		vavail = 0;
	if (vavail > lotsfree)
		vavail = lotsfree;

	/*
	 * Fix for 1161438 (CRS SPR# 73922).  All variables
	 * in the original calculation for desscan were 32 bit signed
	 * ints.  As freemem approaches 0x0 on a system with 1 Gig or
	 * more of memory, the calculation can overflow.  When this
	 * happens, desscan becomes negative and pageout_scanner()
	 * stops paging out.
	 */
	if (needfree > 0 && pageout_new_spread == 0) {
		/*
		 * If we've not yet collected enough samples to
		 * calculate a spread, kick into high gear anytime
		 * needfree is non-zero. Note that desscan will not be
		 * the limiting factor for systems with larger memory;
		 * the %CPU will limit the scan. That will also be
		 * maxed out below.
		 */
		desscan = fastscan / SCHEDPAGING_HZ;
	} else {
		/*
		 * Once we've calculated a spread based on system
		 * memory and usage, just treat needfree as another
		 * form of deficit.
		 */
		spgcnt_t faststmp, slowstmp, result;

		slowstmp = slowscan * vavail;
		faststmp = fastscan * (lotsfree - vavail);
		result = (slowstmp + faststmp) /
		    nz(lotsfree) / SCHEDPAGING_HZ;
		desscan = (pgcnt_t)result;
	}

	/*
	 * If we've not yet collected enough samples to calculate a
	 * spread, also kick %CPU to the max.
	 */
	if (pageout_new_spread == 0) {
		pageout_nsec = max_pageout_nsec;
	} else {
		pageout_nsec = min_pageout_nsec +
		    (lotsfree - vavail) *
		    (max_pageout_nsec - min_pageout_nsec) /
		    nz(lotsfree);
	}

	if (pageout_new_spread != 0 && des_page_scanners != n_page_scanners) {
		/*
		 * We have finished the pagescan initialization and the desired
		 * number of page scanners has changed, either because
		 * initialization just finished, because of a memory DR, or
		 * because des_page_scanners has been modified on the fly (i.e.
		 * by mdb). If we need more scanners, start them now, otherwise
		 * the excess scanners will terminate on their own when they
		 * reset their hands.
		 */
		uint_t i;
		uint_t curr_nscan = n_page_scanners;
		pgcnt_t max = total_pages / handspreadpages;

		if (des_page_scanners > max)
			des_page_scanners = max;

		if (des_page_scanners > MAX_PSCAN_THREADS) {
			des_page_scanners = MAX_PSCAN_THREADS;
		} else if (des_page_scanners == 0) {
			des_page_scanners = 1;
		}

		/*
		 * Each thread has its own entry in the reset_hands array, so
		 * we don't need any locking in pageout_scanner to check the
		 * thread's reset_hands entry. Thus, we use a pre-allocated
		 * fixed size reset_hands array and upper limit on the number
		 * of pagescan threads.
		 *
		 * The reset_hands entries need to be true before we start new
		 * scanners, but if we're reducing, we don't want a race on the
		 * recalculation for the existing threads, so we set
		 * n_page_scanners first.
		 */
		n_page_scanners = des_page_scanners;
		for (i = 0; i < MAX_PSCAN_THREADS; i++) {
			reset_hands[i] = B_TRUE;
		}

		if (des_page_scanners > curr_nscan) {
			/* Create additional pageout scanner threads. */
			for (i = curr_nscan; i < des_page_scanners; i++) {
				(void) lwp_kernel_create(proc_pageout,
				    pageout_scanner, (void *)(uintptr_t)i,
				    TS_RUN, curthread->t_pri);
			}
		}
	}

	zones_over = B_FALSE;

	if (freemem < lotsfree + needfree || PAGE_SCAN_STARTUP) {
		if (!PAGE_SCAN_STARTUP)
			low_mem_scan++;
		DTRACE_PROBE(schedpage__wake__low);
		WAKE_PAGEOUT_SCANNER();

	} else if (zone_num_over_cap > 0) {
		/* One or more zones are over their cap. */

		/* No page limit */
		desscan = total_pages;

		/*
		 * Increase the scanning CPU% to the max. This implies
		 * 80% of one CPU/sec if the scanner can run each
		 * opportunity. Can also be tuned via setting
		 * zone_pageout_nsec in /etc/system or with mdb.
		 */
		pageout_nsec = (zone_pageout_nsec != 0) ?
		    zone_pageout_nsec : max_pageout_nsec;

		zones_over = B_TRUE;
		zone_cap_scan++;

		DTRACE_PROBE(schedpage__wake__zone);
		WAKE_PAGEOUT_SCANNER();

	} else {
		/*
		 * There are enough free pages, no need to
		 * kick the scanner thread.  And next time
		 * around, keep more of the `highly shared'
		 * pages.
		 */
		cv_signal_pageout();

		mutex_enter(&pageout_mutex);
		if (po_share > MIN_PO_SHARE) {
			po_share >>= 1;
		}
		mutex_exit(&pageout_mutex);
	}

	/*
	 * Signal threads waiting for available memory.
	 * NOTE: usually we need to grab memavail_lock before cv_broadcast, but
	 * in this case it is not needed - the waiters will be waken up during
	 * the next invocation of this function.
	 */
	if (kmem_avail() > 0)
		cv_broadcast(&memavail_cv);

	(void) timeout(schedpaging, arg, hz / SCHEDPAGING_HZ);
}

pgcnt_t		pushes;
ulong_t		push_list_size;		/* # of requests on pageout queue */

int dopageout = 1;	/* /etc/system tunable to disable page reclamation */

/*
 * The page out daemon, which runs as process 2.
 *
 * Page out occurs when either:
 * a) there is less than lotsfree pages,
 * b) there are one or more zones over their physical memory cap.
 *
 * The daemon treats physical memory as a circular array of pages and scans the
 * pages using a 'two-handed clock' algorithm. The front hand moves through
 * the pages, clearing the reference bit. The back hand travels a distance
 * (handspreadpages) behind the front hand, freeing the pages that have not
 * been referenced in the time since the front hand passed. If modified, they
 * are first written to their backing store before being freed.
 *
 * In order to make page invalidation more responsive on machines with larger
 * memory, multiple pageout_scanner threads may be created. In this case, the
 * threads are evenly distributed around the the memory "clock face" so that
 * memory can be reclaimed more quickly (that is, there can be large regions in
 * which no pages can be reclaimed by a single thread, leading to lag which
 * causes undesirable behavior such as htable stealing).
 *
 * As long as there are at least lotsfree pages, or no zones over their cap,
 * then pageout_scanner threads are not run. When pageout_scanner threads are
 * running for case (a), all pages are considered for pageout. For case (b),
 * only pages belonging to a zone over its cap will be considered for pageout.
 *
 * There are multiple threads that act on behalf of the pageout process.
 * A set of threads scan pages (pageout_scanner) and frees them up if
 * they don't require any VOP_PUTPAGE operation. If a page must be
 * written back to its backing store, the request is put on a list
 * and the other (pageout) thread is signaled. The pageout thread
 * grabs VOP_PUTPAGE requests from the list, and processes them.
 * Some filesystems may require resources for the VOP_PUTPAGE
 * operations (like memory) and hence can block the pageout
 * thread, but the pageout_scanner threads can still operate. There is still
 * no guarantee that memory deadlocks cannot occur.
 *
 * The pageout_scanner parameters are determined in schedpaging().
 */
void
pageout()
{
	struct async_reqs *arg;
	pri_t pageout_pri;
	int i;
	pgcnt_t max_pushes;
	callb_cpr_t cprinfo;

	proc_pageout = ttoproc(curthread);
	proc_pageout->p_cstime = 0;
	proc_pageout->p_stime =  0;
	proc_pageout->p_cutime =  0;
	proc_pageout->p_utime = 0;
	bcopy("pageout", PTOU(curproc)->u_psargs, 8);
	bcopy("pageout", PTOU(curproc)->u_comm, 7);

	/*
	 * Create pageout scanner thread
	 */
	mutex_init(&pageout_mutex, NULL, MUTEX_DEFAULT, NULL);
	mutex_init(&push_lock, NULL, MUTEX_DEFAULT, NULL);

	/*
	 * Allocate and initialize the async request structures
	 * for pageout.
	 */
	push_req = (struct async_reqs *)
	    kmem_zalloc(async_list_size * sizeof (struct async_reqs), KM_SLEEP);

	req_freelist = push_req;
	for (i = 0; i < async_list_size - 1; i++)
		push_req[i].a_next = &push_req[i + 1];

	pageout_pri = curthread->t_pri;

	/* Create the (first) pageout scanner thread. */
	(void) lwp_kernel_create(proc_pageout, pageout_scanner, (void *) 0,
	    TS_RUN, pageout_pri - 1);

	/*
	 * kick off pageout scheduler.
	 */
	schedpaging(NULL);

	/*
	 * Create kernel cage thread.
	 * The kernel cage thread is started under the pageout process
	 * to take advantage of the less restricted page allocation
	 * in page_create_throttle().
	 */
	kcage_cageout_init();

	/*
	 * Limit pushes to avoid saturating pageout devices.
	 */
	max_pushes = maxpgio / SCHEDPAGING_HZ;
	CALLB_CPR_INIT(&cprinfo, &push_lock, callb_generic_cpr, "pageout");

	for (;;) {
		mutex_enter(&push_lock);

		while ((arg = push_list) == NULL || pushes > max_pushes) {
			CALLB_CPR_SAFE_BEGIN(&cprinfo);
			cv_wait(&push_cv, &push_lock);
			pushes = 0;
			CALLB_CPR_SAFE_END(&cprinfo, &push_lock);
		}
		push_list = arg->a_next;
		arg->a_next = NULL;
		pageout_pushing = true;
		mutex_exit(&push_lock);

		DTRACE_PROBE(pageout__push);
		if (VOP_PUTPAGE(arg->a_vp, (offset_t)arg->a_off,
		    arg->a_len, arg->a_flags, arg->a_cred, NULL) == 0) {
			pushes++;
		}

		/* vp held by checkpage() */
		VN_RELE(arg->a_vp);

		mutex_enter(&push_lock);
		pageout_pushing = false;
		pageout_pushcount++;
		arg->a_next = req_freelist;	/* back on freelist */
		req_freelist = arg;
		push_list_size--;
		mutex_exit(&push_lock);
	}
}

/*
 * Kernel thread that scans pages looking for ones to free
 */
static void
pageout_scanner(void *a)
{
	struct page *fronthand, *backhand;
	uint_t count, iter = 0;
	callb_cpr_t cprinfo;
	pgcnt_t	nscan_cnt, nscan_limit;
	pgcnt_t	pcount;
	uint_t inst = (uint_t)(uintptr_t)a;
	hrtime_t sample_start, sample_end;
	kmutex_t pscan_mutex;

	VERIFY3U(inst, <, MAX_PSCAN_THREADS);

	mutex_init(&pscan_mutex, NULL, MUTEX_DEFAULT, NULL);

	CALLB_CPR_INIT(&cprinfo, &pscan_mutex, callb_generic_cpr, "poscan");
	mutex_enter(&pscan_mutex);

	/*
	 * Establish the minimum and maximum length of time to be spent
	 * scanning pages per wakeup, limiting the scanner duty cycle.  The
	 * input percentage values (0-100) must be converted to a fraction of
	 * the number of nanoseconds in a second of wall time, then further
	 * scaled down by the number of scanner wakeups in a second:
	*/
	min_pageout_nsec = MAX(1,
	    NANOSEC * min_percent_cpu / 100 / SCHEDPAGING_HZ);
	max_pageout_nsec = MAX(min_pageout_nsec,
	    NANOSEC * max_percent_cpu / 100 / SCHEDPAGING_HZ);

loop:
	cv_signal_pageout();

	CALLB_CPR_SAFE_BEGIN(&cprinfo);
	cv_wait(&proc_pageout->p_cv, &pscan_mutex);
	CALLB_CPR_SAFE_END(&cprinfo, &pscan_mutex);

	if (!dopageout)
		goto loop;

	if (reset_hands[inst]) {
		struct page *first;
		pgcnt_t offset = total_pages / n_page_scanners;

		reset_hands[inst] = B_FALSE;
		if (inst >= n_page_scanners) {
			/*
			 * The desired number of page scanners has been
			 * reduced and this instance is no longer wanted.
			 * Exit the lwp.
			 */
			VERIFY3U(inst, !=, 0);
			mutex_exit(&pscan_mutex);
			mutex_enter(&curproc->p_lock);
			lwp_exit();
		}

		/*
		 * The reset case repositions the hands at the proper place
		 * on the memory clock face to prevent creep into another
		 * thread's active region or when the number of threads has
		 * changed.
		 *
		 * Set the two clock hands to be separated by a reasonable
		 * amount, but no more than 360 degrees apart.
		 *
		 * If inst == 0, backhand starts at first page, otherwise
		 * it is (inst * offset) around the memory "clock face" so that
		 * we spread out each scanner instance evenly.
		 */
		first = page_first();
		backhand = page_nextn(first, offset * inst);
		if (handspreadpages >= total_pages) {
			fronthand = page_nextn(backhand, total_pages - 1);
		} else {
			fronthand = page_nextn(backhand, handspreadpages);
		}
	}

	/*
	 * This CPU kstat is only incremented here and we're obviously on this
	 * CPU, so no lock.
	 */
	CPU_STATS_ADDQ(CPU, vm, pgrrun, 1);
	count = 0;

	/* Kernel probe */
	TNF_PROBE_2(pageout_scan_start, "vm pagedaemon", /* CSTYLED */,
	    tnf_ulong, pages_free, freemem, tnf_ulong, pages_needed, needfree);

	pcount = 0;
	nscan_cnt = 0;
	if (PAGE_SCAN_STARTUP) {
		nscan_limit = total_pages;
	} else {
		nscan_limit = desscan;
	}

	DTRACE_PROBE4(pageout__start, pgcnt_t, nscan_limit, uint_t, inst,
	    page_t *, backhand, page_t *, fronthand);

	sample_start = gethrtime();

	/*
	 * Scan the appropriate number of pages for a single duty cycle.
	 * Only scan while at least one of these is true:
	 * 1) one or more zones is over its cap
	 * 2) there is not enough free memory
	 * 3) during page scan startup when determining sample data
	 */
	while (nscan_cnt < nscan_limit &&
	    (zones_over ||
	    freemem < lotsfree + needfree ||
	    PAGE_SCAN_STARTUP)) {
		checkpage_result_t rvfront, rvback;

		DTRACE_PROBE2(pageout__loop, pgcnt_t, pcount, uint_t, inst);

		/*
		 * Check to see if we have exceeded our %CPU budget
		 * for this wakeup, but not on every single page visited,
		 * just every once in a while.
		 */
		if ((pcount & PAGES_POLL_MASK) == PAGES_POLL_MASK) {
			hrtime_t pageout_cycle_nsec;

			pageout_cycle_nsec = gethrtime() - sample_start;
			if (pageout_cycle_nsec >= pageout_nsec) {
				/*
				 * This is where we normally break out of the
				 * loop when scanning zones or sampling.
				 */
				if (!zones_over) {
					atomic_inc_64(&pageout_timeouts);
				}
				DTRACE_PROBE1(pageout__timeout, uint_t, inst);
				break;
			}
		}

		/*
		 * If checkpage manages to add a page to the free list,
		 * we give ourselves another couple of trips around memory.
		 */
		if ((rvfront = checkpage(fronthand, POH_FRONT)) == CKP_FREED)
			count = 0;
		if ((rvback = checkpage(backhand, POH_BACK)) == CKP_FREED)
			count = 0;

		++pcount;

		/*
		 * This CPU kstat is only incremented here and we're obviously
		 * on this CPU, so no lock.
		 */
		CPU_STATS_ADDQ(CPU, vm, scan, 1);

		/*
		 * Don't include ineligible pages in the number scanned.
		 */
		if (rvfront != CKP_INELIGIBLE || rvback != CKP_INELIGIBLE)
			nscan_cnt++;

		backhand = page_next(backhand);
		fronthand = page_next(fronthand);

		/*
		 * The front hand has wrapped around to the first page in the
		 * loop.
		*/
		if (fronthand == page_first())	{
			DTRACE_PROBE1(pageout__wrap__front, uint_t, inst);

			/*
			 * Every 64 wraps we reposition our hands within our
			 * region to prevent creep into another thread.
			 */
			if ((++iter % pageout_reset_cnt) == 0)
				reset_hands[inst] = B_TRUE;

			/*
			 * This CPU kstat is only incremented here and we're
			 * obviously on this CPU, so no lock.
			 */
			CPU_STATS_ADDQ(CPU, vm, rev, 1);

			/*
			 * If scanning because the system is low on memory,
			 * then when we wraparound memory we want to try to
			 * reclaim more pages.
			 * If scanning only because zones are over their cap,
			 * then wrapping is common and we simply keep going.
			 */
			if (freemem < lotsfree + needfree && ++count > 1) {
				/*
				 * The system is low on memory.
				 * Extremely unlikely, but it happens.
				 * We went around memory at least once
				 * and didn't reclaim enough.
				 * If we are still skipping `highly shared'
				 * pages, skip fewer of them.  Otherwise,
				 * give up till the next clock tick.
				 */
				mutex_enter(&pageout_mutex);
				if (po_share < MAX_PO_SHARE) {
					po_share <<= 1;
					mutex_exit(&pageout_mutex);
				} else {
					/*
					 * Really a "goto loop", but if someone
					 * is tracing or TNF_PROBE_ing, hit
					 * those probes first.
					 */
					mutex_exit(&pageout_mutex);
					break;
				}
			}
		}
	}

	atomic_add_long(&nscan, nscan_cnt);

	sample_end = gethrtime();

	DTRACE_PROBE3(pageout__loop__end, pgcnt_t, nscan_cnt, pgcnt_t, pcount,
	    uint_t, inst);

	/* Kernel probe */
	TNF_PROBE_2(pageout_scan_end, "vm pagedaemon", /* CSTYLED */,
	    tnf_ulong, pages_scanned, nscan_cnt, tnf_ulong, pages_free,
	    freemem);

	/*
	 * The following two blocks are only relevant when the scanner is
	 * first started up. After the scanner runs for a while, neither of
	 * the conditions will ever be true again.
	 *
	 * The global variables used below are only modified by this thread and
	 * only during initial scanning when there is a single page scanner
	 * thread running. Thus, we don't use any locking.
	 */
	if (PAGE_SCAN_STARTUP) {
		VERIFY3U(inst, ==, 0);
		pageout_sample_pages += pcount;
		pageout_sample_etime += sample_end - sample_start;
		++pageout_sample_cnt;

	} else if (pageout_new_spread == 0) {
		/*
		 * We have run enough samples, set the spread.
		 */
		VERIFY3U(inst, ==, 0);
		pageout_rate = (hrrate_t)pageout_sample_pages *
		    (hrrate_t)(NANOSEC) / pageout_sample_etime;
		pageout_new_spread = pageout_rate / 10;
		setupclock();
	}

	goto loop;
}

/*
 * The pageout deadman is run once per second by clock().
 */
void
pageout_deadman(void)
{
	if (panicstr != NULL) {
		/*
		 * There is no pageout after panic.
		 */
		return;
	}

	if (pageout_deadman_seconds == 0) {
		/*
		 * The deadman is not enabled.
		 */
		return;
	}

	if (!pageout_pushing) {
		goto reset;
	}

	/*
	 * We are pushing a page.  Check to see if it is the same call we saw
	 * last time we looked:
	 */
	if (pageout_pushcount != pageout_pushcount_seen) {
		/*
		 * It is a different call from the last check, so we are not
		 * stuck.
		 */
		goto reset;
	}

	if (++pageout_stucktime >= pageout_deadman_seconds) {
		panic("pageout_deadman: stuck pushing the same page for %d "
		    "seconds (freemem is %lu)", pageout_deadman_seconds,
		    freemem);
	}

	return;

reset:
	/*
	 * Reset our tracking state to reflect that we are not stuck:
	 */
	pageout_stucktime = 0;
	pageout_pushcount_seen = pageout_pushcount;
}

/*
 * Look at the page at hand.  If it is locked (e.g., for physical i/o),
 * system (u., page table) or free, then leave it alone.  Otherwise,
 * if we are running the front hand, turn off the page's reference bit.
 * If running the back hand, check whether the page has been reclaimed.
 * If not, free the page, pushing it to disk first if necessary.
 *
 * Return values:
 *     CKP_INELIGIBLE if the page is not a candidate at all,
 *     CKP_NOT_FREED  if the page was not freed, or
 *     CKP_FREED      if we freed it.
 */
static checkpage_result_t
checkpage(struct page *pp, pageout_hand_t whichhand)
{
	int ppattr;
	int isfs = 0;
	int isexec = 0;
	int pagesync_flag;
	zoneid_t zid = ALL_ZONES;

	/*
	 * Skip pages:
	 *	- associated with the kernel vnode since
	 *	    they are always "exclusively" locked.
	 *	- that are free
	 *	- that are shared more than po_share'd times
	 *	- its already locked
	 *
	 * NOTE:  These optimizations assume that reads are atomic.
	 */

	if (PP_ISKAS(pp) || PAGE_LOCKED(pp) || PP_ISFREE(pp) ||
	    pp->p_lckcnt != 0 || pp->p_cowcnt != 0 ||
	    hat_page_checkshare(pp, po_share)) {
		return (CKP_INELIGIBLE);
	}

	if (!page_trylock(pp, SE_EXCL)) {
		/*
		 * Skip the page if we can't acquire the "exclusive" lock.
		 */
		return (CKP_INELIGIBLE);
	} else if (PP_ISFREE(pp)) {
		/*
		 * It became free between the above check and our actually
		 * locking the page.  Oh well, there will be other pages.
		 */
		page_unlock(pp);
		return (CKP_INELIGIBLE);
	}

	/*
	 * Reject pages that cannot be freed. The page_struct_lock
	 * need not be acquired to examine these
	 * fields since the page has an "exclusive" lock.
	 */
	if (pp->p_lckcnt != 0 || pp->p_cowcnt != 0) {
		page_unlock(pp);
		return (CKP_INELIGIBLE);
	}

	if (zones_over) {
		ASSERT(pp->p_zoneid == ALL_ZONES ||
		    pp->p_zoneid >= 0 && pp->p_zoneid <= MAX_ZONEID);
		if (pp->p_zoneid == ALL_ZONES ||
		    zone_pdata[pp->p_zoneid].zpers_over == 0) {
			/*
			 * Cross-zone shared page, or zone not over it's cap.
			 * Leave the page alone.
			 */
			page_unlock(pp);
			return (CKP_INELIGIBLE);
		}
		zid = pp->p_zoneid;
	}

	/*
	 * Maintain statistics for what we are freeing
	 */

	if (pp->p_vnode != NULL) {
		if (pp->p_vnode->v_flag & VVMEXEC)
			isexec = 1;

		if (!IS_SWAPFSVP(pp->p_vnode))
			isfs = 1;
	}

	/*
	 * Turn off REF and MOD bits with the front hand.
	 * The back hand examines the REF bit and always considers
	 * SHARED pages as referenced.
	 */
	if (whichhand == POH_FRONT)
		pagesync_flag = HAT_SYNC_ZERORM;
	else
		pagesync_flag = HAT_SYNC_DONTZERO | HAT_SYNC_STOPON_REF |
		    HAT_SYNC_STOPON_SHARED;

	ppattr = hat_pagesync(pp, pagesync_flag);

recheck:
	/*
	 * If page is referenced; fronthand makes unreferenced and reclaimable.
	 * For the backhand, a process referenced the page since the front hand
	 * went by, so it's not a candidate for freeing up.
	 */
	if (ppattr & P_REF) {
		DTRACE_PROBE2(pageout__isref, page_t *, pp, int, whichhand);
		if (whichhand == POH_FRONT) {
			hat_clrref(pp);
		}
		page_unlock(pp);
		return (CKP_NOT_FREED);
	}

	/*
	 * This page is not referenced, so it must be reclaimable and we can
	 * add it to the free list. This can be done by either hand.
	 */

	VM_STAT_ADD(pageoutvmstats.checkpage[0]);

	/*
	 * If large page, attempt to demote it. If successfully demoted,
	 * retry the checkpage.
	 */
	if (pp->p_szc != 0) {
		if (!page_try_demote_pages(pp)) {
			VM_STAT_ADD(pageoutvmstats.checkpage[1]);
			page_unlock(pp);
			return (CKP_INELIGIBLE);
		}
		ASSERT(pp->p_szc == 0);
		VM_STAT_ADD(pageoutvmstats.checkpage[2]);
		/*
		 * Since page_try_demote_pages() could have unloaded some
		 * mappings it makes sense to reload ppattr.
		 */
		ppattr = hat_page_getattr(pp, P_MOD | P_REF);
	}

	/*
	 * If the page is currently dirty, we have to arrange to have it
	 * cleaned before it can be freed.
	 *
	 * XXX - ASSERT(pp->p_vnode != NULL);
	 */
	if ((ppattr & P_MOD) && pp->p_vnode != NULL) {
		struct vnode *vp = pp->p_vnode;
		u_offset_t offset = pp->p_offset;

		/*
		 * Note: There is no possibility to test for process being
		 * swapped out or about to exit since we can't get back to
		 * process(es) from the page.
		 */

		/*
		 * Hold the vnode before releasing the page lock to
		 * prevent it from being freed and re-used by some
		 * other thread.
		 */
		VN_HOLD(vp);
		page_unlock(pp);

		/*
		 * Queue I/O request for the pageout thread.
		 */
		if (!queue_io_request(vp, offset)) {
			VN_RELE(vp);
			return (CKP_NOT_FREED);
		}
		if (isfs) {
			zone_pageout_stat(zid, ZPO_DIRTY);
		} else {
			zone_pageout_stat(zid, ZPO_ANONDIRTY);
		}
		return (CKP_FREED);
	}

	/*
	 * Now we unload all the translations and put the page back on to the
	 * free list.  If the page was used (referenced or modified) after the
	 * pagesync but before it was unloaded we catch it and handle the page
	 * properly.
	 */
	DTRACE_PROBE2(pageout__free, page_t *, pp, pageout_hand_t, whichhand);
	(void) hat_pageunload(pp, HAT_FORCE_PGUNLOAD);
	ppattr = hat_page_getattr(pp, P_MOD | P_REF);
	if ((ppattr & P_REF) || ((ppattr & P_MOD) && pp->p_vnode != NULL))
		goto recheck;

	VN_DISPOSE(pp, B_FREE, 0, kcred);

	CPU_STATS_ADD_K(vm, dfree, 1);

	if (isfs) {
		if (isexec) {
			CPU_STATS_ADD_K(vm, execfree, 1);
		} else {
			CPU_STATS_ADD_K(vm, fsfree, 1);
		}
		zone_pageout_stat(zid, ZPO_FS);
	} else {
		CPU_STATS_ADD_K(vm, anonfree, 1);
		zone_pageout_stat(zid, ZPO_ANON);
	}

	return (CKP_FREED);		/* freed a page! */
}

/*
 * Queue async i/o request from pageout_scanner and segment swapout
 * routines on one common list.  This ensures that pageout devices (swap)
 * are not saturated by pageout_scanner or swapout requests.
 * The pageout thread empties this list by initiating i/o operations.
 */
int
queue_io_request(vnode_t *vp, u_offset_t off)
{
	struct async_reqs *arg;

	/*
	 * If we cannot allocate an async request struct,
	 * skip this page.
	 */
	mutex_enter(&push_lock);
	if ((arg = req_freelist) == NULL) {
		mutex_exit(&push_lock);
		return (0);
	}
	req_freelist = arg->a_next;		/* adjust freelist */
	push_list_size++;

	arg->a_vp = vp;
	arg->a_off = off;
	arg->a_len = PAGESIZE;
	arg->a_flags = B_ASYNC | B_FREE;
	arg->a_cred = kcred;		/* always held */

	/*
	 * Add to list of pending write requests.
	 */
	arg->a_next = push_list;
	push_list = arg;

	if (req_freelist == NULL) {
		/*
		 * No free async requests left. The lock is held so we
		 * might as well signal the pusher thread now.
		 */
		cv_signal(&push_cv);
	}
	mutex_exit(&push_lock);
	return (1);
}

/*
 * Wakeup pageout to initiate i/o if push_list is not empty.
 */
void
cv_signal_pageout()
{
	if (push_list != NULL) {
		mutex_enter(&push_lock);
		cv_signal(&push_cv);
		mutex_exit(&push_lock);
	}
}
