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
 * Copyright (c) 2011, Joyent, Inc. All rights reserved.
 */

#include <sys/spa.h>
#include <sys/vdev_impl.h>
#include <sys/zfs_zone.h>

#ifndef _KERNEL

/*
 * Stubs for when compiling for user-land.
 */

void
zfs_zone_io_throttle(zfs_zone_iop_type_t type)
{
}

void
zfs_zone_zio_init(zio_t *zp)
{
}

void
zfs_zone_zio_start(zio_t *zp)
{
}

void
zfs_zone_zio_done(zio_t *zp)
{
}

void
zfs_zone_zio_dequeue(zio_t *zp)
{
}

void
zfs_zone_zio_enqueue(zio_t *zp)
{
}

/*ARGSUSED*/
void
zfs_zone_report_txg_sync(void *dp)
{
}

int
zfs_zone_txg_delay()
{
	return (1);
}

#else

/*
 * The real code.
 */

#include <sys/systm.h>
#include <sys/thread.h>
#include <sys/proc.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/time.h>
#include <sys/atomic.h>
#include <sys/zio.h>
#include <sys/zone.h>
#include <sys/avl.h>
#include <sys/sdt.h>
#include <sys/ddi.h>

/*
 * The zone throttle delays read and write operations from certain zones based
 * on each zone's IO utilitzation.  Once a cycle (defined by zfs_zone_cycle_time
 * below), the delays for each zone are recalculated based on the utilization
 * over the previous window.
 */
boolean_t	zfs_zone_delay_enable = B_TRUE;	/* enable IO throttle */
uint16_t	zfs_zone_delay_step = 5;	/* amount to change delay */
uint16_t	zfs_zone_delay_ceiling = 100;	/* longest possible delay */

hrtime_t	zfs_zone_last_checked = 0;

boolean_t	zfs_zone_priority_enable = B_TRUE;  /* enable IO priority */

/*
 * For certain workloads, one zone may be issuing primarily sequential I/O and
 * another primarily random I/O.  The sequential I/O will complete much more
 * quickly than the random I/O, driving the average system latency for those
 * operations way down.  As a result, the random I/O may be throttled back, even
 * though the sequential I/O should be throttled to allow the random I/O more
 * access to the disk.
 *
 * This tunable limits the discrepancy between the read and write system
 * latency.  If one becomes excessively high, this tunable prevents the I/O
 * throttler from exacerbating the imbalance.
 */
uint_t		zfs_zone_rw_lat_limit = 10;


/*
 * The I/O throttle will only start delaying zones when it detects disk
 * utilization has reached a certain level.  This tunable controls the threshold
 * at which the throttle will start delaying zones. The calculation should
 * correspond closely with the %b column from iostat.
 */
uint_t		zfs_zone_util_threshold = 80;

/*
 * Throughout this subsystem, our timestamps are in microseconds.  Our system
 * average cycle is one second or 1 million microseconds.  Our zone counter
 * update cycle is two seconds or 2 million microseconds.  We use a longer
 * duration for that cycle because some ops can see a little over two seconds of
 * latency when they are being starved by another zone.
 */
uint_t 		zfs_zone_sys_avg_cycle = 1000000;	/* 1 s */
uint_t 		zfs_zone_cycle_time = 2000000;		/* 2 s */

uint_t 		zfs_zone_adjust_time = 250000;		/* 250 ms */

typedef struct {
	hrtime_t	cycle_start;
	int		cycle_cnt;
	hrtime_t	cycle_lat;
	hrtime_t	sys_avg_lat;
} sys_lat_cycle_t;

typedef struct {
	hrtime_t zi_now;
	uint_t zi_avgrlat;
	uint_t zi_avgwlat;
	uint64_t zi_totpri;
	uint64_t zi_totutil;
	int zi_active;
	uint_t zi_diskutil;
} zoneio_stats_t;

static sys_lat_cycle_t	rd_lat;
static sys_lat_cycle_t	wr_lat;

/*
 * Some basic disk stats to determine disk utilization.
 */
kmutex_t	zfs_disk_lock;
uint_t		zfs_disk_rcnt;
hrtime_t	zfs_disk_rtime = 0;
hrtime_t	zfs_disk_rlastupdate = 0;

hrtime_t	zfs_disk_last_rtime = 0;

/*
 * Data used to keep track of how often txg flush is running.
 */
extern int	zfs_txg_timeout;
static uint_t	txg_last_check;
static uint_t	txg_cnt;
static uint_t	txg_flush_rate;

boolean_t	zfs_zone_schedule_enable = B_TRUE;	/* enable IO sched. */
/*
 * Threshold for when zio scheduling should kick in.
 *
 * This threshold is based on 1/2 of the zfs_vdev_max_pending value for the
 * number of I/Os that can be pending on a device.  If there are more than a
 * few ops already queued up, beyond those already issued to the vdev, then
 * use scheduling to get the next zio.
 */
int		zfs_zone_schedule_thresh = 5;

/*
 * Tunables for delay throttling when TxG flush is occurring.
 */
int		zfs_zone_txg_throttle_scale = 2;
int		zfs_zone_txg_delay_ticks = 2;

typedef struct {
	int	zq_qdepth;
	int	zq_priority;
	int	zq_wt;
	zoneid_t zq_zoneid;
} zone_q_bump_t;

/*
 * This uses gethrtime() but returns a value in usecs.
 */
#define	GET_USEC_TIME		(gethrtime() / 1000)
#define	NANO_TO_MICRO(x)	(x / (NANOSEC / MICROSEC))

/*
 * Keep track of the zone's ZFS IOPs.
 *
 * If the number of ops is >1 then we can just use that value.  However,
 * if the number of ops is <2 then we might have a zone which is trying to do
 * IO but is not able to get any ops through the system.  We don't want to lose
 * track of this zone so we factor in its decayed count into the current count.
 *
 * Each cycle (zfs_zone_sys_avg_cycle) we want to update the decayed count.
 * However, since this calculation is driven by IO activity and since IO does
 * not happen at fixed intervals, we use a timestamp to see when the last update
 * was made.  If it was more than one cycle ago, then we need to decay the
 * historical count by the proper number of additional cycles in which no IO was
 * performed.
 *
 * Return true if we actually computed a new historical count.
 * If we're still within an active cycle there is nothing to do, return false.
 */
static hrtime_t
compute_historical_zone_cnt(hrtime_t unow, sys_zio_cntr_t *cp)
{
	hrtime_t delta;
	int	gen_cnt;

	/*
	 * Check if its time to recompute a new zone count.
	 * If we're still collecting data for the current cycle, return false.
	 */
	delta = unow - cp->cycle_start;
	if (delta < zfs_zone_cycle_time)
		return (delta);

	/* A previous cycle is past, compute the new zone count. */

	/*
	 * Figure out how many generations we have to decay the historical
	 * count, since multiple cycles may have elapsed since our last IO.
	 * We depend on int rounding here.
	 */
	gen_cnt = (int)(delta / zfs_zone_cycle_time);

	/* If more than 5 cycles since last the IO, reset count. */
	if (gen_cnt > 5) {
		cp->zone_avg_cnt = 0;
	} else {
		/* Update the count. */
		int	i;

		/*
		 * If the zone did more than 1 IO, just use its current count
		 * as the historical value, otherwise decay the historical
		 * count and factor that into the new historical count.  We
		 * pick a threshold > 1 so that we don't lose track of IO due
		 * to int rounding.
		 */
		if (cp->cycle_cnt > 1)
			cp->zone_avg_cnt = cp->cycle_cnt;
		else
			cp->zone_avg_cnt = cp->cycle_cnt +
			    (cp->zone_avg_cnt / 2);

		/*
		 * If more than one generation has elapsed since the last
		 * update, decay the values further.
		 */
		for (i = 1; i < gen_cnt; i++)
			cp->zone_avg_cnt = cp->zone_avg_cnt / 2;
	}

	/* A new cycle begins. */
	cp->cycle_start = unow;
	cp->cycle_cnt = 0;

	return (0);
}

/*
 * Add IO op data to the zone.
 */
static void
add_zone_iop(zone_t *zonep, hrtime_t unow, zfs_zone_iop_type_t op)
{
	switch (op) {
	case ZFS_ZONE_IOP_READ:
		(void) compute_historical_zone_cnt(unow, &zonep->zone_rd_ops);
		zonep->zone_rd_ops.cycle_cnt++;
		break;
	case ZFS_ZONE_IOP_WRITE:
		(void) compute_historical_zone_cnt(unow, &zonep->zone_wr_ops);
		zonep->zone_wr_ops.cycle_cnt++;
		break;
	case ZFS_ZONE_IOP_LOGICAL_WRITE:
		(void) compute_historical_zone_cnt(unow, &zonep->zone_lwr_ops);
		zonep->zone_lwr_ops.cycle_cnt++;
		break;
	}
}

/*
 * Use a decaying average to keep track of the overall system latency.
 *
 * We want to have the recent activity heavily weighted, but if the
 * activity decreases or stops, then the average should quickly decay
 * down to the new value.
 *
 * Each cycle (zfs_zone_sys_avg_cycle) we want to update the decayed average.
 * However, since this calculation is driven by IO activity and since IO does
 * not happen
 *
 * at fixed intervals, we use a timestamp to see when the last update was made.
 * If it was more than one cycle ago, then we need to decay the average by the
 * proper number of additional cycles in which no IO was performed.
 *
 * Return true if we actually computed a new system average.
 * If we're still within an active cycle there is nothing to do, return false.
 */
static int
compute_new_sys_avg(hrtime_t unow, sys_lat_cycle_t *cp)
{
	hrtime_t delta;
	int	gen_cnt;

	/*
	 * Check if its time to recompute a new average.
	 * If we're still collecting data for the current cycle, return false.
	 */
	delta = unow - cp->cycle_start;
	if (delta < zfs_zone_sys_avg_cycle)
		return (0);

	/* A previous cycle is past, compute a new system average. */

	/*
	 * Figure out how many generations we have to decay, since multiple
	 * cycles may have elapsed since our last IO.
	 * We count on int rounding here.
	 */
	gen_cnt = (int)(delta / zfs_zone_sys_avg_cycle);

	/* If more than 5 cycles since last the IO, reset average. */
	if (gen_cnt > 5) {
		cp->sys_avg_lat = 0;
	} else {
		/* Update the average. */
		int	i;

		cp->sys_avg_lat =
		    (cp->sys_avg_lat + cp->cycle_lat) / (1 + cp->cycle_cnt);

		/*
		 * If more than one generation has elapsed since the last
		 * update, decay the values further.
		 */
		for (i = 1; i < gen_cnt; i++)
			cp->sys_avg_lat = cp->sys_avg_lat / 2;
	}

	/* A new cycle begins. */
	cp->cycle_start = unow;
	cp->cycle_cnt = 0;
	cp->cycle_lat = 0;

	return (1);
}

static void
add_sys_iop(hrtime_t unow, int op, int lat)
{
	switch (op) {
	case ZFS_ZONE_IOP_READ:
		(void) compute_new_sys_avg(unow, &rd_lat);
		rd_lat.cycle_cnt++;
		rd_lat.cycle_lat += lat;
		break;
	case ZFS_ZONE_IOP_WRITE:
		(void) compute_new_sys_avg(unow, &wr_lat);
		wr_lat.cycle_cnt++;
		wr_lat.cycle_lat += lat;
		break;
	}
}

/*
 * Get the zone IO counts.
 */
static uint_t
calc_zone_cnt(hrtime_t unow, sys_zio_cntr_t *cp)
{
	hrtime_t delta;
	uint_t cnt;

	if ((delta = compute_historical_zone_cnt(unow, cp)) == 0) {
		/*
		 * No activity in the current cycle, we already have the
		 * historical data so we'll use that.
		 */
		cnt = cp->zone_avg_cnt;
	} else {
		/*
		 * If we're less than half way through the cycle then use
		 * the current count plus half the historical count, otherwise
		 * just use the current count.
		 */
		if (delta < (zfs_zone_cycle_time / 2))
			cnt = cp->cycle_cnt + (cp->zone_avg_cnt / 2);
		else
			cnt = cp->cycle_cnt;
	}

	return (cnt);
}

/*
 * Get the average read/write latency in usecs for the system.
 */
static uint_t
calc_avg_lat(hrtime_t unow, sys_lat_cycle_t *cp)
{
	if (compute_new_sys_avg(unow, cp)) {
		/*
		 * No activity in the current cycle, we already have the
		 * historical data so we'll use that.
		 */
		return (cp->sys_avg_lat);
	} else {
		/*
		 * We're within a cycle; weight the current activity higher
		 * compared to the historical data and use that.
		 */
		extern void __dtrace_probe_zfs__zone__calc__wt__avg(uintptr_t,
		    uintptr_t, uintptr_t);

		__dtrace_probe_zfs__zone__calc__wt__avg(
		    (uintptr_t)cp->sys_avg_lat,
		    (uintptr_t)cp->cycle_lat,
		    (uintptr_t)cp->cycle_cnt);

		return ((cp->sys_avg_lat + (cp->cycle_lat * 8)) /
		    (1 + (cp->cycle_cnt * 8)));
	}
}

/*
 * Account for the current IOP on the zone and for the system as a whole.
 * The latency parameter is in usecs.
 */
static void
add_iop(zone_t *zonep, hrtime_t unow, zfs_zone_iop_type_t op, hrtime_t lat)
{
	/* Add op to zone */
	add_zone_iop(zonep, unow, op);

	/* Track system latency */
	if (op != ZFS_ZONE_IOP_LOGICAL_WRITE)
		add_sys_iop(unow, op, lat);
}

/*
 * Calculate and return the total number of read ops, write ops and logical
 * write ops for the given zone.  If the zone has issued operations of any type
 * return a non-zero value, otherwise return 0.
 */
static int
get_zone_io_cnt(hrtime_t unow, zone_t *zonep, uint_t *rops, uint_t *wops,
    uint_t *lwops)
{
	*rops = calc_zone_cnt(unow, &zonep->zone_rd_ops);
	*wops = calc_zone_cnt(unow, &zonep->zone_wr_ops);
	*lwops = calc_zone_cnt(unow, &zonep->zone_lwr_ops);

	extern void __dtrace_probe_zfs__zone__io__cnt(uintptr_t,
	    uintptr_t, uintptr_t, uintptr_t);

	__dtrace_probe_zfs__zone__io__cnt((uintptr_t)zonep->zone_id,
	    (uintptr_t)(*rops), (uintptr_t)*wops, (uintptr_t)*lwops);

	return (*rops | *wops | *lwops);
}

/*
 * Get the average read/write latency in usecs for the system.
 */
static void
get_sys_avg_lat(hrtime_t unow, uint_t *rlat, uint_t *wlat)
{
	*rlat = calc_avg_lat(unow, &rd_lat);
	*wlat = calc_avg_lat(unow, &wr_lat);

	/*
	 * In an attempt to improve the accuracy of the throttling algorithm,
	 * assume that IO operations can't have zero latency.  Instead, assume
	 * a reasonable lower bound for each operation type. If the actual
	 * observed latencies are non-zero, use those latency values instead.
	 */
	if (*rlat == 0)
		*rlat = 1000;
	if (*wlat == 0)
		*wlat = 1000;

	extern void __dtrace_probe_zfs__zone__sys__avg__lat(uintptr_t,
	    uintptr_t);

	__dtrace_probe_zfs__zone__sys__avg__lat((uintptr_t)(*rlat),
	    (uintptr_t)*wlat);
}

/*
 * Find disk utilization for each zone and average utilization for all active
 * zones.
 */
static int
zfs_zone_wait_adjust_calculate_cb(zone_t *zonep, void *arg)
{
	zoneio_stats_t *sp = arg;
	uint_t rops, wops, lwops;

	if (zonep->zone_id == GLOBAL_ZONEID ||
	    get_zone_io_cnt(sp->zi_now, zonep, &rops, &wops, &lwops) == 0) {
		zonep->zone_io_util = 0;
		return (0);
	}

	zonep->zone_io_util = (rops * sp->zi_avgrlat) +
	    (wops * sp->zi_avgwlat) + (lwops * sp->zi_avgwlat);
	sp->zi_totutil += zonep->zone_io_util;

	if (zonep->zone_io_util > 0) {
		sp->zi_active++;
		sp->zi_totpri += zonep->zone_zfs_io_pri;
	}

	/*
	 * sdt:::zfs-zone-utilization
	 *
	 *	arg0: zone ID
	 *	arg1: read operations observed during time window
	 *	arg2: physical write operations observed during time window
	 *	arg3: logical write ops observed during time window
	 *	arg4: calculated utilization given read and write ops
	 *	arg5: I/O priority assigned to this zone
	 */
	extern void __dtrace_probe_zfs__zone__utilization(
	    uint_t, uint_t, uint_t, uint_t, uint_t, uint_t);

	__dtrace_probe_zfs__zone__utilization((uint_t)(zonep->zone_id),
	    (uint_t)rops, (uint_t)wops, (uint_t)lwops,
	    (uint_t)zonep->zone_io_util, (uint_t)zonep->zone_zfs_io_pri);

	return (0);
}

static void
zfs_zone_delay_inc(zone_t *zonep)
{
	if (zonep->zone_io_delay < zfs_zone_delay_ceiling)
		zonep->zone_io_delay += zfs_zone_delay_step;
}

static void
zfs_zone_delay_dec(zone_t *zonep)
{
	if (zonep->zone_io_delay > 0)
		zonep->zone_io_delay -= zfs_zone_delay_step;
}

/*
 * For all zones "far enough" away from the average utilization, increase that
 * zones delay.  Otherwise, reduce its delay.
 */
static int
zfs_zone_wait_adjust_delay_cb(zone_t *zonep, void *arg)
{
	zoneio_stats_t *sp = arg;
	uint16_t delay = zonep->zone_io_delay;
	uint_t fairutil = 0;

	zonep->zone_io_util_above_avg = B_FALSE;

	/*
	 * Given the calculated total utilitzation for all zones, calculate the
	 * fair share of I/O for this zone.
	 */
	if (zfs_zone_priority_enable && sp->zi_totpri > 0) {
		fairutil = (sp->zi_totutil * zonep->zone_zfs_io_pri) /
		    sp->zi_totpri;
	} else if (sp->zi_active > 0) {
		fairutil = sp->zi_totutil / sp->zi_active;
	}

	/*
	 * Adjust each IO's delay.  If the overall delay becomes too high, avoid
	 * increasing beyond the ceiling value.
	 */
	if (zonep->zone_io_util > fairutil &&
	    sp->zi_diskutil > zfs_zone_util_threshold) {
		zonep->zone_io_util_above_avg = B_TRUE;

		if (sp->zi_active > 1)
			zfs_zone_delay_inc(zonep);
	} else if (zonep->zone_io_util < fairutil || sp->zi_active <= 1) {
		zfs_zone_delay_dec(zonep);
	}

	/*
	 * sdt:::zfs-zone-throttle
	 *
	 *	arg0: zone ID
	 *	arg1: old delay for this zone
	 *	arg2: new delay for this zone
	 *	arg3: calculated fair I/O utilization
	 *	arg4: actual I/O utilization
	 */
	extern void __dtrace_probe_zfs__zone__throttle(
	    uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);

	__dtrace_probe_zfs__zone__throttle(
	    (uintptr_t)zonep->zone_id, (uintptr_t)delay,
	    (uintptr_t)zonep->zone_io_delay, (uintptr_t)fairutil,
	    (uintptr_t)zonep->zone_io_util);

	return (0);
}

/*
 * Examine the utilization between different zones, and adjust the delay for
 * each zone appropriately.
 */
static void
zfs_zone_wait_adjust(hrtime_t unow)
{
	zoneio_stats_t stats;

	(void) bzero(&stats, sizeof (stats));

	stats.zi_now = unow;
	get_sys_avg_lat(unow, &stats.zi_avgrlat, &stats.zi_avgwlat);

	if (stats.zi_avgrlat > stats.zi_avgwlat * zfs_zone_rw_lat_limit)
		stats.zi_avgrlat = stats.zi_avgwlat * zfs_zone_rw_lat_limit;
	else if (stats.zi_avgrlat * zfs_zone_rw_lat_limit < stats.zi_avgwlat)
		stats.zi_avgwlat = stats.zi_avgrlat * zfs_zone_rw_lat_limit;

	if (zone_walk(zfs_zone_wait_adjust_calculate_cb, &stats) != 0)
		return;

	/*
	 * Calculate disk utilization for the most recent period.
	 */
	if (zfs_disk_last_rtime == 0 || unow - zfs_zone_last_checked <= 0) {
		stats.zi_diskutil = 0;
	} else {
		stats.zi_diskutil =
		    ((zfs_disk_rtime - zfs_disk_last_rtime) * 100) /
		    ((unow - zfs_zone_last_checked) * 1000);
	}
	zfs_disk_last_rtime = zfs_disk_rtime;

	/*
	 * sdt:::zfs-zone-stats
	 *
	 * Statistics observed over the last period:
	 *
	 *	arg0: average system read latency
	 *	arg1: average system write latency
	 *	arg2: number of active zones
	 *	arg3: total I/O 'utilization' for all zones
	 *	arg4: total I/O priority of all active zones
	 *	arg5: calculated disk utilization
	 */
	extern void __dtrace_probe_zfs__zone__stats(
	    uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);

	__dtrace_probe_zfs__zone__stats((uintptr_t)(stats.zi_avgrlat),
	    (uintptr_t)(stats.zi_avgwlat),
	    (uintptr_t)(stats.zi_active),
	    (uintptr_t)(stats.zi_totutil),
	    (uintptr_t)(stats.zi_totpri),
	    (uintptr_t)(stats.zi_diskutil));

	(void) zone_walk(zfs_zone_wait_adjust_delay_cb, &stats);
}

/*
 * Callback used to calculate a zone's IO schedule priority.
 *
 * We scan the zones looking for ones with ops in the queue.  Out of those,
 * we pick the one that calculates to the highest schedule priority.
 */
static int
get_sched_pri_cb(zone_t *zonep, void *arg)
{
	int pri;
	zone_q_bump_t *qbp = arg;

	extern void __dtrace_probe_zfs__zone__enqueued(uintptr_t, uintptr_t);
	__dtrace_probe_zfs__zone__enqueued((uintptr_t)(zonep->zone_id),
	    (uintptr_t)(zonep->zone_zfs_queued));

	if (zonep->zone_zfs_queued == 0) {
		zonep->zone_zfs_weight = 0;
		return (0);
	}

	/*
	 * On each pass, increment the zone's weight.  We use this as input
	 * to the calculation to prevent starvation.  The value is reset
	 * each time we issue an IO for this zone so zones which haven't
	 * done any IO over several iterations will see their weight max
	 * out.
	 */
	if (zonep->zone_zfs_weight < 20)
		zonep->zone_zfs_weight++;

	/*
	 * This zone's IO priority is the inverse of the number of IOs
	 * the zone has enqueued * zone's configured priority * weight.
	 * The queue depth has already been scaled by 10 to avoid problems
	 * with int rounding.
	 *
	 * This means that zones with fewer IOs in the queue will get
	 * preference unless other zone's assigned priority pulls them
	 * ahead.  The weight is factored in to help ensure that zones
	 * which haven't done IO in a while aren't getting starved.
	 */
	pri = (qbp->zq_qdepth / zonep->zone_zfs_queued) *
	    zonep->zone_zfs_io_pri * zonep->zone_zfs_weight;

	/*
	 * If this zone has a higher priority than what we found so far,
	 * schedule it next.
	 */
	if (pri > qbp->zq_priority) {
		qbp->zq_zoneid = zonep->zone_id;
		qbp->zq_priority = pri;
		qbp->zq_wt = zonep->zone_zfs_weight;
	}
	return (0);
}

/*
 * See if we need to bump a zone's zio to the head of the queue.
 *
 * For single-threaded synchronous workloads a zone cannot get more than
 * 1 op into the queue at a time unless the zone is running multiple workloads
 * in parallel.  This can cause an imbalance in performance if there are zones
 * with many parallel workloads (and ops in the queue) vs. other zones which
 * are doing simple single-threaded workloads, such as interactive tasks in the
 * shell.  These zones can get backed up behind a deep queue and their IO
 * performance will appear to be very poor as a result.  This can make the
 * zone work badly for interactive behavior.
 *
 * The scheduling algorithm kicks in once we start to get a deeper queue.
 * Once that occurs, we look at all of the zones to see which one calculates
 * to the highest priority.  We bump that zone's first zio to the head of the
 * queue.
 *
 * We use a counter on the zone so that we can quickly find how many ops each
 * zone has in the queue without having to search the entire queue itself.
 * This scales better since the number of zones is expected to be on the
 * order of 10-100 whereas the queue depth can be in the range of 50-2000.
 * In addition, since the zio's in the queue only have the zoneid, we would
 * have to look up the zone for each zio enqueued and that means the overhead
 * for scanning the queue each time would be much higher.
 *
 * In all cases, we fall back to simply pulling the next op off the queue
 * if something should go wrong.
 */
static zio_t *
get_next_zio(vdev_queue_t *vq, int qdepth)
{
	zone_q_bump_t qbump;
	zio_t *zp = NULL, *zphead;
	int cnt = 0;

	ASSERT(MUTEX_HELD(&vq->vq_lock));

	/* To avoid problems with int rounding, scale the queue depth by 10 */
	qbump.zq_qdepth = qdepth * 10;
	qbump.zq_priority = 0;
	qbump.zq_zoneid = 0;
	(void) zone_walk(get_sched_pri_cb, &qbump);

	zphead = avl_first(&vq->vq_deadline_tree);

	/* Check if the scheduler didn't pick a zone for some reason!? */
	if (qbump.zq_zoneid != 0) {
		for (zp = avl_first(&vq->vq_deadline_tree); zp != NULL;
		    zp = avl_walk(&vq->vq_deadline_tree, zp, AVL_AFTER)) {
			if (zp->io_zoneid == qbump.zq_zoneid)
				break;
			cnt++;
		}
	}

	if (zp == NULL) {
		zp = zphead;
	} else if (zp != zphead) {
		/*
		 * Only fire the probe if we actually picked a different zio
		 * than the one already at the head of the queue.
		 */
		extern void __dtrace_probe_zfs__zone__sched__bump(uintptr_t,
		    uintptr_t, uintptr_t, uintptr_t);
		__dtrace_probe_zfs__zone__sched__bump(
		    (uintptr_t)(zp->io_zoneid), (uintptr_t)(cnt),
		    (uintptr_t)(qbump.zq_priority), (uintptr_t)(qbump.zq_wt));
	}

	return (zp);
}

/*
 * Add our zone ID to the zio so we can keep track of which zones are doing
 * what, even when the current thread processing the zio is not associated
 * with the zone (e.g. the kernel taskq which pushes out RX groups).
 */
void
zfs_zone_zio_init(zio_t *zp)
{
	zone_t	*zonep = curzone;

	zp->io_zoneid = zonep->zone_id;
}

/*
 * Track IO operations per zone.  Called from dmu_tx_count_write for write ops
 * and dmu_read_uio for read ops.  For each operation, increment that zone's
 * counter based on the type of operation.
 *
 * There are three basic ways that we can see write ops:
 * 1) An application does write syscalls.  Those ops go into a TXG which
 *    we'll count here.  Sometime later a kernel taskq thread (we'll see the
 *    vdev IO as zone 0) will perform some number of physical writes to commit
 *    the TXG to disk.  Those writes are not associated with the zone which
 *    made the write syscalls and the number of operations is not correlated
 *    between the taskq and the zone.
 * 2) An application opens a file with O_SYNC.  Each write will result in
 *    an operation which we'll see here plus a low-level vdev write from
 *    that zone.
 * 3) An application does write syscalls followed by an fsync().  We'll
 *    count the writes going into a TXG here.  We'll also see some number
 *    (usually much smaller, maybe only 1) of low-level vdev writes from this
 *    zone when the fsync is performed, plus some other low-level vdev writes
 *    from the taskq in zone 0 (are these metadata writes?).
 *
 * 4) In addition to the above, there are misc. system-level writes, such as
 *    writing out dirty pages to swap, or sync(2) calls, which will be handled
 *    by the global zone and which we count but don't generally worry about.
 *
 * Because of the above, we can see writes twice because this is called
 * at a high level by a zone thread, but we also will count the phys. writes
 * that are performed at a low level via zfs_zone_zio_start.
 *
 * Without this, it can look like a non-global zone never writes (case 1).
 * Depending on when the TXG is flushed, the counts may be in the same sample
 * bucket or in a different one.
 *
 * Tracking read operations is simpler due to their synchronous semantics.  The
 * zfs_read function -- called as a result of a read(2) syscall -- will always
 * retrieve the data to be read through dmu_read_uio.
 */
void
zfs_zone_io_throttle(zfs_zone_iop_type_t type)
{
	zone_t *zonep = curzone;
	hrtime_t unow;
	uint16_t wait;

	unow = GET_USEC_TIME;

	/*
	 * Only bump the counters for logical operations here.  The counters for
	 * tracking physical IO operations are handled in zfs_zone_zio_done.
	 */
	if (type == ZFS_ZONE_IOP_LOGICAL_WRITE) {
		mutex_enter(&zonep->zone_stg_io_lock);
		add_iop(zonep, unow, type, 0);
		mutex_exit(&zonep->zone_stg_io_lock);
	}

	if (!zfs_zone_delay_enable)
		return;

	/*
	 * XXX There's a potential race here in that more than one thread may
	 * update the zone delays concurrently.  The worst outcome is corruption
	 * of our data to track each zone's IO, so the algorithm may make
	 * incorrect throttling decisions until the data is refreshed.
	 */
	if ((unow - zfs_zone_last_checked) > zfs_zone_adjust_time) {
		zfs_zone_wait_adjust(unow);
		zfs_zone_last_checked = unow;
	}

	if ((wait = zonep->zone_io_delay) > 0) {
		/*
		 * If this is a write and we're doing above normal TxG
		 * flushing, then throttle for longer than normal.
		 */
		if (type == ZFS_ZONE_IOP_LOGICAL_WRITE &&
		    (txg_cnt > 1 || txg_flush_rate > 1))
			wait *= zfs_zone_txg_throttle_scale;

		/*
		 * sdt:::zfs-zone-wait
		 *
		 *	arg0: zone ID
		 *	arg1: type of IO operation
		 *	arg2: time to delay (in us)
		 */
		extern void __dtrace_probe_zfs__zone__wait(
		    uintptr_t, uintptr_t, uintptr_t);

		__dtrace_probe_zfs__zone__wait((uintptr_t)(zonep->zone_id),
		    (uintptr_t)type, (uintptr_t)wait);

		drv_usecwait(wait);

		if (zonep->zone_vfs_stats != NULL) {
			atomic_inc_64(&zonep->zone_vfs_stats->
			    zv_delay_cnt.value.ui64);
			atomic_add_64(&zonep->zone_vfs_stats->
			    zv_delay_time.value.ui64, wait);
		}
	}
}

/*
 * XXX Ignore the pool pointer parameter for now.
 *
 * Keep track to see if the TxG flush rate is running above the expected rate.
 * If so, this implies that we are filling TxG's at a high rate due to a heavy
 * write workload.  We use this as input into the zone throttle.
 *
 * This function is called every 5 seconds (zfs_txg_timeout) under a normal
 * write load.  In this case, the flush rate is going to be 1.  When there
 * is a heavy write load, TxG's fill up fast and the sync thread will write
 * the TxG more frequently (perhaps once a second).  In this case the rate
 * will be > 1.  The flush rate is a lagging indicator since it can be up
 * to 5 seconds old.  We use the txg_cnt to keep track of the rate in the
 * current 5 second interval and txg_flush_rate to keep track of the previous
 * 5 second interval.  In that way we don't have a period (1 or more seconds)
 * where the txg_cnt == 0 and we cut back on throttling even though the rate
 * is still high.
 */
/*ARGSUSED*/
void
zfs_zone_report_txg_sync(void *dp)
{
	uint_t now;

	txg_cnt++;
	now = (uint_t)(gethrtime() / NANOSEC);
	if ((now - txg_last_check) >= zfs_txg_timeout) {
		txg_flush_rate = txg_cnt / 2;
		txg_cnt = 0;
		txg_last_check = now;
	}
}

int
zfs_zone_txg_delay()
{
	zone_t	*zonep = curzone;
	int delay = 1;

	if (zonep->zone_io_util_above_avg)
		delay = zfs_zone_txg_delay_ticks;

	extern void __dtrace_probe_zfs__zone__txg__delay(uintptr_t, uintptr_t);

	__dtrace_probe_zfs__zone__txg__delay((uintptr_t)(zonep->zone_id),
	    (uintptr_t)delay);

	return (delay);
}

/*
 * Called from zio_vdev_io_start when an IO hits the end of the zio pipeline
 * and is issued.
 * Keep track of start time for latency calculation in zfs_zone_zio_done.
 */
void
zfs_zone_zio_start(zio_t *zp)
{
	zone_t	*zonep;

	/*
	 * I/Os of type ZIO_TYPE_IOCTL are used to flush the disk cache, not for
	 * an actual I/O operation.  Ignore those operations as they relate to
	 * throttling and scheduling.
	 */
	if (zp->io_type == ZIO_TYPE_IOCTL)
		return;

	if ((zonep = zone_find_by_id(zp->io_zoneid)) == NULL)
		return;

	mutex_enter(&zonep->zone_zfs_lock);
	if (zp->io_type == ZIO_TYPE_READ)
		kstat_runq_enter(&zonep->zone_zfs_rwstats);
	zonep->zone_zfs_weight = 0;
	mutex_exit(&zonep->zone_zfs_lock);

	mutex_enter(&zfs_disk_lock);
	zp->io_dispatched = gethrtime();

	if (zfs_disk_rcnt++ != 0)
		zfs_disk_rtime += (zp->io_dispatched - zfs_disk_rlastupdate);
	zfs_disk_rlastupdate = zp->io_dispatched;
	mutex_exit(&zfs_disk_lock);

	zone_rele(zonep);
}

/*
 * Called from vdev_queue_io_done when an IO completes.
 * Increment our counter for zone ops.
 * Calculate the IO latency avg. for this zone.
 */
void
zfs_zone_zio_done(zio_t *zp)
{
	zone_t	*zonep;
	hrtime_t now, unow, udelta;

	if (zp->io_type == ZIO_TYPE_IOCTL)
		return;

	if ((zonep = zone_find_by_id(zp->io_zoneid)) == NULL)
		return;

	now = gethrtime();
	unow = NANO_TO_MICRO(now);
	udelta = unow - NANO_TO_MICRO(zp->io_dispatched);

	mutex_enter(&zonep->zone_zfs_lock);

	/*
	 * To calculate the wsvc_t average, keep a cumulative sum of all the
	 * wait time before each I/O was dispatched.  Since most writes are
	 * asynchronous, only track the wait time for read I/Os.
	 */
	if (zp->io_type == ZIO_TYPE_READ) {
		zonep->zone_zfs_rwstats.reads++;
		zonep->zone_zfs_rwstats.nread += zp->io_size;

		zonep->zone_zfs_stats->zz_waittime.value.ui64 +=
		    zp->io_dispatched - zp->io_start;

		kstat_runq_exit(&zonep->zone_zfs_rwstats);
	} else {
		zonep->zone_zfs_rwstats.writes++;
		zonep->zone_zfs_rwstats.nwritten += zp->io_size;
	}

	mutex_exit(&zonep->zone_zfs_lock);

	mutex_enter(&zfs_disk_lock);
	zfs_disk_rcnt--;
	zfs_disk_rtime += (now - zfs_disk_rlastupdate);
	zfs_disk_rlastupdate = now;
	mutex_exit(&zfs_disk_lock);

	if (zfs_zone_delay_enable) {
		mutex_enter(&zonep->zone_stg_io_lock);
		add_iop(zonep, unow, zp->io_type == ZIO_TYPE_READ ?
		    ZFS_ZONE_IOP_READ : ZFS_ZONE_IOP_WRITE, udelta);
		mutex_exit(&zonep->zone_stg_io_lock);
	}

	zone_rele(zonep);

	/*
	 * sdt:::zfs-zone-latency
	 *
	 *	arg0: zone ID
	 *	arg1: type of I/O operation
	 *	arg2: I/O latency (in us)
	 */
	extern void __dtrace_probe_zfs__zone__latency(
	    uintptr_t, uintptr_t, uintptr_t);

	__dtrace_probe_zfs__zone__latency((uintptr_t)(zp->io_zoneid),
	    (uintptr_t)(zp->io_type), (uintptr_t)(udelta));
}

void
zfs_zone_zio_dequeue(zio_t *zp)
{
	zone_t	*zonep;

	if ((zonep = zone_find_by_id(zp->io_zoneid)) == NULL)
		return;

	mutex_enter(&zonep->zone_stg_io_lock);
	ASSERT(zonep->zone_zfs_queued > 0);
	if (zonep->zone_zfs_queued == 0)
		cmn_err(CE_WARN, "zfs_zone_zio_dequeue: count==0");
	else
		zonep->zone_zfs_queued--;
	mutex_exit(&zonep->zone_stg_io_lock);
	zone_rele(zonep);
}

void
zfs_zone_zio_enqueue(zio_t *zp)
{
	zone_t	*zonep;

	if ((zonep = zone_find_by_id(zp->io_zoneid)) == NULL)
		return;

	mutex_enter(&zonep->zone_stg_io_lock);
	zonep->zone_zfs_queued++;
	mutex_exit(&zonep->zone_stg_io_lock);
	zone_rele(zonep);
}

/*
 * Called from vdev_queue_io_to_issue.  This function is where zio's are found
 * at the head of the queue (by avl_first), then pulled off (by
 * vdev_queue_io_remove) and issued.  We do our scheduling here to find the
 * next zio to issue.
 *
 * The vq->vq_lock mutex is held when we're executing this function so we
 * can safely access the "last zone" variable on the queue.
 */
zio_t *
zfs_zone_schedule(vdev_queue_t *vq)
{
	int cnt;
	zoneid_t last_zone;
	zio_t *zp;

	ASSERT(MUTEX_HELD(&vq->vq_lock));

	cnt = avl_numnodes(&vq->vq_deadline_tree);
	last_zone = vq->vq_last_zone_id;

	/*
	 * If there are only a few ops in the queue then just issue the head.
	 * If there are more than a few ops already queued up, then use
	 * scheduling to get the next zio.
	 */
	if (!zfs_zone_schedule_enable || cnt < zfs_zone_schedule_thresh)
		zp = avl_first(&vq->vq_deadline_tree);
	else
		zp = get_next_zio(vq, cnt);

	vq->vq_last_zone_id = zp->io_zoneid;

	/*
	 * Probe with 3 args; the number of IOs in the queue, the zone that
	 * was last scheduled off this queue, and the zone that was associated
	 * with the next IO that is scheduled.
	 */
	extern void __dtrace_probe_zfs__zone__sched(uintptr_t, uintptr_t,
	    uintptr_t);

	__dtrace_probe_zfs__zone__sched((uintptr_t)(cnt),
	    (uintptr_t)(last_zone), (uintptr_t)(zp->io_zoneid));

	return (zp);
}

#endif
