/*
 * Interface for controlling IO bandwidth on a request queue
 *
 * Copyright (C) 2010 Vivek Goyal <vgoyal@redhat.com>
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/blktrace_api.h>
#include <linux/blk-cgroup.h>
#include <linux/delay.h>
#include "blk.h"
#include "blk-throttle.h"

/* Max dispatch from a group in 1 round */
static int throtl_grp_quantum = 8;

/* Total max dispatch from all groups in one round */
static int throtl_quantum = 32;

/* Throttling is performed over 100ms slice and after that slice is renewed */
static unsigned long throtl_slice = HZ/10;  /* 100 ms */

static struct blkcg_policy blkcg_policy_throtl;

/* A workqueue to queue throttle related work */
static struct workqueue_struct *kthrotld_workqueue;

/*
 * To implement hierarchical throttling, throtl_grps form a tree and bios
 * are dispatched upwards level by level until they reach the top and get
 * issued.  When dispatching bios from the children and local group at each
 * level, if the bios are dispatched into a single bio_list, there's a risk
 * of a local or child group which can queue many bios at once filling up
 * the list starving others.
 *
 * To avoid such starvation, dispatched bios are queued separately
 * according to where they came from.  When they are again dispatched to
 * the parent, they're popped in round-robin order so that no single source
 * hogs the dispatch window.
 *
 * throtl_qnode is used to keep the queued bios separated by their sources.
 * Bios are queued to throtl_qnode which in turn is queued to
 * throtl_service_queue and then dispatched in round-robin order.
 *
 * It's also used to track the reference counts on blkg's.  A qnode always
 * belongs to a throtl_grp and gets queued on itself or the parent, so
 * incrementing the reference of the associated throtl_grp when a qnode is
 * queued and decrementing when dequeued is enough to keep the whole blkg
 * tree pinned while bios are in flight.
 */

#define rb_entry_tg(node)   rb_entry((node), struct throtl_grp, rb_node)

/* Per-cpu group stats */
struct tg_stats_cpu {
    /* total bytes transferred */
    struct blkg_rwstat      service_bytes;
    /* total IOs serviced, post merge */
    struct blkg_rwstat      serviced;
};



struct throtl_data
{
    /* service tree for active throtl groups */
    struct throtl_service_queue service_queue;

    struct request_queue *queue;

    /* Total Number of queued bios on READ and WRITE lists */
    unsigned int nr_queued[2];

    /*
     * number of total undestroyed groups
     */
    unsigned int nr_undestroyed_grps;

    /* Work for dispatching throttled bios */
    struct work_struct dispatch_work;
};

/* list and work item to allocate percpu group stats */
static DEFINE_SPINLOCK(tg_stats_alloc_lock);
static LIST_HEAD(tg_stats_alloc_list);

static void tg_stats_alloc_fn(struct work_struct *);
static DECLARE_DELAYED_WORK(tg_stats_alloc_work, tg_stats_alloc_fn);

static void throtl_pending_timer_fn(unsigned long arg);

static inline struct throtl_grp *fake_d_to_tg(struct fake_device *fake_d)
{
//  return fake_d ? container_of(fake_d, struct throtl_grp, fake_d) : NULL;
    return fake_d->tg;
}

static inline struct throtl_grp *pd_to_tg(struct blkg_policy_data *pd)
{
    return pd ? container_of(pd, struct throtl_grp, pd) : NULL;
}

static inline struct throtl_grp *blkg_to_tg(struct blkcg_gq *blkg)
{
    return pd_to_tg(blkg_to_pd(blkg, &blkcg_policy_throtl));
}

struct blkcg_gq *tg_to_blkg(struct throtl_grp *tg)
{
    return pd_to_blkg(&tg->pd);
}

static inline struct throtl_grp *td_root_tg(struct throtl_data *td)
{
    return blkg_to_tg(td->queue->root_blkg);
}

/**
 * sq_to_tg - return the throl_grp the specified service queue belongs to
 * @sq: the throtl_service_queue of interest
 *
 * Return the throtl_grp @sq belongs to.  If @sq is the top-level one
 * embedded in throtl_data, %NULL is returned.
 */
struct throtl_grp *sq_to_tg(struct throtl_service_queue *sq)
{
    if (sq && sq->parent_sq)
        return container_of(sq, struct throtl_grp, service_queue);
    else
        return NULL;
}

/**
 * sq_to_td - return throtl_data the specified service queue belongs to
 * @sq: the throtl_service_queue of interest
 *
 * A service_queue can be embeded in either a throtl_grp or throtl_data.
 * Determine the associated throtl_data accordingly and return it.
 */
static struct throtl_data *sq_to_td(struct throtl_service_queue *sq)
{
    struct throtl_grp *tg = sq_to_tg(sq);

    if (tg)
        return tg->td;
    else
        return container_of(sq, struct throtl_data, service_queue);
}

/**
 * throtl_log - log debug message via blktrace
 * @sq: the service_queue being reported
 * @fmt: printf format string
 * @args: printf args
 *
 * The messages are prefixed with "throtl BLKG_NAME" if @sq belongs to a
 * throtl_grp; otherwise, just "throtl".
 *
 * TODO: this should be made a function and name formatting should happen
 * after testing whether blktrace is enabled.
 */
#define throtl_log(sq, fmt, args...)    do {                \
    struct throtl_grp *__tg = sq_to_tg((sq));           \
    struct throtl_data *__td = sq_to_td((sq));          \
                                    \
    (void)__td;                         \
    if ((__tg)) {                           \
        char __pbuf[128];                   \
                                    \
        blkg_path(tg_to_blkg(__tg), __pbuf, sizeof(__pbuf));    \
        blk_add_trace_msg(__td->queue, "throtl %s " fmt, __pbuf, ##args); \
    } else {                            \
        blk_add_trace_msg(__td->queue, "throtl " fmt, ##args);  \
    }                               \
} while (0)

static void tg_stats_init(struct tg_stats_cpu *tg_stats)
{
    blkg_rwstat_init(&tg_stats->service_bytes);
    blkg_rwstat_init(&tg_stats->serviced);
}

/*
 * Worker for allocating per cpu stat for tgs. This is scheduled on the
 * system_wq once there are some groups on the alloc_list waiting for
 * allocation.
 */
static void tg_stats_alloc_fn(struct work_struct *work)
{
    static struct tg_stats_cpu *stats_cpu;  /* this fn is non-reentrant */
    struct delayed_work *dwork = to_delayed_work(work);
    bool empty = false;

alloc_stats:
    if (!stats_cpu) {
        int cpu;

        stats_cpu = alloc_percpu(struct tg_stats_cpu);
        if (!stats_cpu) {
            /* allocation failed, try again after some time */
            schedule_delayed_work(dwork, msecs_to_jiffies(10));
            return;
        }
        for_each_possible_cpu(cpu)
            tg_stats_init(per_cpu_ptr(stats_cpu, cpu));
    }

    spin_lock_irq(&tg_stats_alloc_lock);

    if (!list_empty(&tg_stats_alloc_list)) {
        struct throtl_grp *tg = list_first_entry(&tg_stats_alloc_list,
                             struct throtl_grp,
                             stats_alloc_node);
        swap(tg->stats_cpu, stats_cpu);
        list_del_init(&tg->stats_alloc_node);
    }

    empty = list_empty(&tg_stats_alloc_list);
    spin_unlock_irq(&tg_stats_alloc_lock);
    if (!empty)
        goto alloc_stats;
}

static void throtl_qnode_init(struct throtl_qnode *qn, struct throtl_grp *tg)
{
    INIT_LIST_HEAD(&qn->node);
    bio_list_init(&qn->bios);
    qn->tg = tg;
}

/**
 * throtl_qnode_add_bio - add a bio to a throtl_qnode and activate it
 * @bio: bio being added
 * @qn: qnode to add bio to
 * @queued: the service_queue->queued[] list @qn belongs to
 *
 * Add @bio to @qn and put @qn on @queued if it's not already on.
 * @qn->tg's reference count is bumped when @qn is activated.  See the
 * comment on top of throtl_qnode definition for details.
 */
static void throtl_qnode_add_bio(struct bio *bio, struct throtl_qnode *qn,
                 struct list_head *queued)
{
    bio_list_add(&qn->bios, bio);
    if (list_empty(&qn->node)) {
        list_add_tail(&qn->node, queued);
        blkg_get(tg_to_blkg(qn->tg));
    }
}


/*
 * Added by zhoufang. Because fake_device has no blkg, 
 * we create a new funciton like throtl_qnode_add_bio
 */
static void throtl_qnode_add_bio_withoutblkg(struct bio *bio, struct throtl_qnode *qn,
                struct list_head *queued)
{
    bio_list_add(&qn->bios, bio);
    if (list_empty(&qn->node)) {
        list_add_tail(&qn->node, queued);
//      blkg_get(tg_to_blkg(qn->tg));
    }
}

/**
 * throtl_peek_queued - peek the first bio on a qnode list
 * @queued: the qnode list to peek
 */
static struct bio *throtl_peek_queued(struct list_head *queued)
{
    struct throtl_qnode *qn = list_first_entry(queued, struct throtl_qnode, node);
    struct bio *bio;

    if (list_empty(queued))
        return NULL;

    bio = bio_list_peek(&qn->bios);
    WARN_ON_ONCE(!bio);
    return bio;
}

/**
 * throtl_pop_queued - pop the first bio form a qnode list
 * @queued: the qnode list to pop a bio from
 * @tg_to_put: optional out argument for throtl_grp to put
 *
 * Pop the first bio from the qnode list @queued.  After popping, the first
 * qnode is removed from @queued if empty or moved to the end of @queued so
 * that the popping order is round-robin.
 *
 * When the first qnode is removed, its associated throtl_grp should be put
 * too.  If @tg_to_put is NULL, this function automatically puts it;
 * otherwise, *@tg_to_put is set to the throtl_grp to put and the caller is
 * responsible for putting it.
 */
static struct bio *throtl_pop_queued(struct list_head *queued,
                     struct throtl_grp **tg_to_put)
{
    struct throtl_qnode *qn = list_first_entry(queued, struct throtl_qnode, node);
    struct bio *bio;

    if (list_empty(queued))
        return NULL;

    bio = bio_list_pop(&qn->bios);
    WARN_ON_ONCE(!bio);

    if (bio_list_empty(&qn->bios)) {
        list_del_init(&qn->node);
        if (tg_to_put)
            *tg_to_put = qn->tg;
        else{
            if(!qn->tg->fake)
                blkg_put(tg_to_blkg(qn->tg));
        }
    } else {
        list_move_tail(&qn->node, queued);
    }

//  struct bio *next_bio = throtl_peek_queued(queued);
//  qn->tg->td->q = 

    return bio;
}

/* init a service_queue, assumes the caller zeroed it */
static void throtl_service_queue_init(struct throtl_service_queue *sq,
                      struct throtl_service_queue *parent_sq)
{
    INIT_LIST_HEAD(&sq->queued[0]);
    INIT_LIST_HEAD(&sq->queued[1]);
    sq->pending_tree = RB_ROOT;
    sq->parent_sq = parent_sq;
    setup_timer(&sq->pending_timer, throtl_pending_timer_fn,
            (unsigned long)sq);
}

static void throtl_service_queue_exit(struct throtl_service_queue *sq)
{
    del_timer_sync(&sq->pending_timer);
}

static void throtl_pd_init(struct blkcg_gq *blkg)
{
    struct throtl_grp *tg = blkg_to_tg(blkg);
    struct throtl_data *td = blkg->q->td;
    struct throtl_service_queue *parent_sq;
    unsigned long flags;
    int rw;

    /*
     * If on the default hierarchy, we switch to properly hierarchical
     * behavior where limits on a given throtl_grp are applied to the
     * whole subtree rather than just the group itself.  e.g. If 16M
     * read_bps limit is set on the root group, the whole system can't
     * exceed 16M for the device.
     *
     * If not on the default hierarchy, the broken flat hierarchy
     * behavior is retained where all throtl_grps are treated as if
     * they're all separate root groups right below throtl_data.
     * Limits of a group don't interact with limits of other groups
     * regardless of the position of the group in the hierarchy.
     */
    parent_sq = &td->service_queue;

    if (cgroup_on_dfl(blkg->blkcg->css.cgroup) && blkg->parent)
        parent_sq = &blkg_to_tg(blkg->parent)->service_queue;

    throtl_service_queue_init(&tg->service_queue, parent_sq);

    for (rw = READ; rw <= WRITE; rw++) {
        throtl_qnode_init(&tg->qnode_on_self[rw], tg);
        throtl_qnode_init(&tg->qnode_on_parent[rw], tg);
    }

    RB_CLEAR_NODE(&tg->rb_node);
    tg->td = td;

    tg->bps[READ] = -1;
    tg->bps[WRITE] = -1;
    tg->bps[RANDW] = -1;
    tg->iops[READ] = -1;
    tg->iops[WRITE] = -1;
    tg->iops[RANDW] = -1;
    tg->fake = false;

    /*
     * Ugh... We need to perform per-cpu allocation for tg->stats_cpu
     * but percpu allocator can't be called from IO path.  Queue tg on
     * tg_stats_alloc_list and allocate from work item.
     */
    spin_lock_irqsave(&tg_stats_alloc_lock, flags);
    list_add(&tg->stats_alloc_node, &tg_stats_alloc_list);
    schedule_delayed_work(&tg_stats_alloc_work, 0);
    spin_unlock_irqrestore(&tg_stats_alloc_lock, flags);
}

/*
 * Added by zhoufang.
 * init the fake device throtl_grp
 */
static void fd_throtl_init(struct blkcg *blkcg)
{
    struct throtl_grp *tg;
    struct throtl_data *td;
    struct throtl_service_queue *parent_sq;
    unsigned long flags;
    int rw;
    struct fake_device *fake_d= blkcg->fd_head;
    struct fake_device_member *fd_member;


    while(fake_d != NULL){
        tg = fake_d->tg;
        for (rw = READ; rw <= WRITE; rw++) {
            throtl_qnode_init(&tg->qnode_on_self[rw], tg);
            throtl_qnode_init(&tg->qnode_on_parent[rw], tg);
        }
        RB_CLEAR_NODE(&tg->rb_node);

        fd_member = fake_d->head;
        while(fd_member != NULL){
            tg = fd_member->tg;

            throtl_service_queue_init(&tg->service_queue, &(fd_member->queue->td->service_queue));

            for (rw = READ; rw <= WRITE; rw++) {
                throtl_qnode_init(&tg->qnode_on_self[rw], tg);
                throtl_qnode_init(&tg->qnode_on_parent[rw], tg);
            }
            RB_CLEAR_NODE(&tg->rb_node);

            spin_lock_irqsave(&tg_stats_alloc_lock, flags);
            list_add(&tg->stats_alloc_node, &tg_stats_alloc_list);
            schedule_delayed_work(&tg_stats_alloc_work, 0);
            spin_unlock_irqrestore(&tg_stats_alloc_lock, flags);

            fd_member = fd_member->next;    
        }

        fake_d = fake_d->next;
    }
}

/*
 * Set has_rules[] if @tg or any of its parents have limits configured.
 * This doesn't require walking up to the top of the hierarchy as the
 * parent's has_rules[] is guaranteed to be correct.
 */
static void tg_update_has_rules(struct throtl_grp *tg)
{
    struct throtl_grp *parent_tg = sq_to_tg(tg->service_queue.parent_sq);
    int rw;

    for (rw = READ; rw <= RANDW; rw++)
        tg->has_rules[rw] = (parent_tg && parent_tg->has_rules[rw]) ||
                    (tg->bps[rw] != -1 || tg->iops[rw] != -1);
}

/* Added by zhoufang
 * update has_rules[] if needed
 * We didn't consider parent tree assuming no parent-child scenes
 */
static void tg_fd_update_has_rules_recursively(struct fake_device *fake_d)
{
    int rw = 0;
    struct fake_device_member *fd_member = fake_d->head;
    struct throtl_grp *fd_tg = fake_d_to_tg(fake_d);
    struct throtl_grp *tg = fake_d_to_tg(fake_d);
    for (rw = READ; rw <= RANDW; rw++)
        tg->has_rules[rw] = (tg->bps[rw] != -1 || tg->iops[rw] != -1);

    while(fd_member != NULL){
        tg = fd_member->tg;
        
        for (rw = READ; rw <= RANDW; rw++){
            tg->bps[rw] = fd_tg->bps[rw];
            tg->iops[rw] = fd_tg->iops[rw];
            tg->has_rules[rw] = (tg->bps[rw] != -1 || tg->iops[rw] != -1);
        }
        fd_member = fd_member->next;
    }

}


static void throtl_pd_online(struct blkcg_gq *blkg)
{
    /*
     * We don't want new groups to escape the limits of its ancestors.
     * Update has_rules[] after a new group is brought online.
     */
    tg_update_has_rules(blkg_to_tg(blkg));
}

static void throtl_pd_exit(struct blkcg_gq *blkg)
{
    struct throtl_grp *tg = blkg_to_tg(blkg);
    unsigned long flags;

    spin_lock_irqsave(&tg_stats_alloc_lock, flags);
    list_del_init(&tg->stats_alloc_node);
    spin_unlock_irqrestore(&tg_stats_alloc_lock, flags);

    free_percpu(tg->stats_cpu);

    throtl_service_queue_exit(&tg->service_queue);
}

static void throtl_pd_reset_stats(struct blkcg_gq *blkg)
{
    struct throtl_grp *tg = blkg_to_tg(blkg);
    int cpu;

    if (tg->stats_cpu == NULL)
        return;

    for_each_possible_cpu(cpu) {
        struct tg_stats_cpu *sc = per_cpu_ptr(tg->stats_cpu, cpu);

        blkg_rwstat_reset(&sc->service_bytes);
        blkg_rwstat_reset(&sc->serviced);
    }
}

struct throtl_grp *throtl_lookup_tg(struct throtl_data *td,
                       struct blkcg *blkcg)
{
    /*
     * This is the common case when there are no blkcgs.  Avoid lookup
     * in this case
     */
    if (blkcg == &blkcg_root)
        return td_root_tg(td);

    return blkg_to_tg(blkg_lookup(blkcg, td->queue));
}

struct throtl_grp *throtl_lookup_create_tg(struct throtl_data *td,
                          struct blkcg *blkcg)
{
    struct request_queue *q = td->queue;
    struct throtl_grp *tg = NULL;

    /*
     * This is the common case when there are no blkcgs.  Avoid lookup
     * in this case
     */
    if (blkcg == &blkcg_root) {
        tg = td_root_tg(td);
    } else {
        struct blkcg_gq *blkg;

        blkg = blkg_lookup_create(blkcg, q);

        /* if %NULL and @q is alive, fall back to root_tg */
        if (!IS_ERR(blkg))
            tg = blkg_to_tg(blkg);
        else if (!blk_queue_dying(q))
            tg = td_root_tg(td);
    }

    return tg;
}

static struct throtl_grp *
throtl_rb_first(struct throtl_service_queue *parent_sq)
{
    /* Service tree is empty */
    if (!parent_sq->nr_pending)
        return NULL;

    if (!parent_sq->first_pending)
        parent_sq->first_pending = rb_first(&parent_sq->pending_tree);

    if (parent_sq->first_pending)
        return rb_entry_tg(parent_sq->first_pending);

    return NULL;
}

static void rb_erase_init(struct rb_node *n, struct rb_root *root)
{
    rb_erase(n, root);
    RB_CLEAR_NODE(n);
}

static void throtl_rb_erase(struct rb_node *n,
                struct throtl_service_queue *parent_sq)
{
    if (parent_sq->first_pending == n)
        parent_sq->first_pending = NULL;
    rb_erase_init(n, &parent_sq->pending_tree);
    --parent_sq->nr_pending;
}

static void update_min_dispatch_time(struct throtl_service_queue *parent_sq)
{
    struct throtl_grp *tg;

    tg = throtl_rb_first(parent_sq);
    if (!tg)
        return;

    parent_sq->first_pending_disptime = tg->disptime;
}

static void tg_service_queue_add(struct throtl_grp *tg)
{
    struct throtl_service_queue *parent_sq = tg->service_queue.parent_sq;
    struct rb_node **node = &parent_sq->pending_tree.rb_node;
    struct rb_node *parent = NULL;
    struct throtl_grp *__tg;
    unsigned long key = tg->disptime;
    int left = 1;

    while (*node != NULL) {
        parent = *node;
        __tg = rb_entry_tg(parent);

        if (time_before(key, __tg->disptime))
            node = &parent->rb_left;
        else {
            node = &parent->rb_right;
            left = 0;
        }
    }

    if (left)
        parent_sq->first_pending = &tg->rb_node;

    rb_link_node(&tg->rb_node, parent, node);
    rb_insert_color(&tg->rb_node, &parent_sq->pending_tree);
}

static void __throtl_enqueue_tg(struct throtl_grp *tg)
{
    tg_service_queue_add(tg);
    tg->flags |= THROTL_TG_PENDING;
    tg->service_queue.parent_sq->nr_pending++;
}

static void throtl_enqueue_tg(struct throtl_grp *tg)
{
    if (!(tg->flags & THROTL_TG_PENDING))
        __throtl_enqueue_tg(tg);
}

static void __throtl_dequeue_tg(struct throtl_grp *tg)
{
    throtl_rb_erase(&tg->rb_node, tg->service_queue.parent_sq);
    tg->flags &= ~THROTL_TG_PENDING;
}

static void throtl_dequeue_tg(struct throtl_grp *tg)
{
    if (tg->flags & THROTL_TG_PENDING)
        __throtl_dequeue_tg(tg);
}

/* Call with queue lock held */
static void throtl_schedule_pending_timer(struct throtl_service_queue *sq,
                      unsigned long expires)
{
    mod_timer(&sq->pending_timer, expires);
    throtl_log(sq, "schedule timer. delay=%lu jiffies=%lu",
           expires - jiffies, jiffies);
}

/**
 * throtl_schedule_next_dispatch - schedule the next dispatch cycle
 * @sq: the service_queue to schedule dispatch for
 * @force: force scheduling
 *
 * Arm @sq->pending_timer so that the next dispatch cycle starts on the
 * dispatch time of the first pending child.  Returns %true if either timer
 * is armed or there's no pending child left.  %false if the current
 * dispatch window is still open and the caller should continue
 * dispatching.
 *
 * If @force is %true, the dispatch timer is always scheduled and this
 * function is guaranteed to return %true.  This is to be used when the
 * caller can't dispatch itself and needs to invoke pending_timer
 * unconditionally.  Note that forced scheduling is likely to induce short
 * delay before dispatch starts even if @sq->first_pending_disptime is not
 * in the future and thus shouldn't be used in hot paths.
 */
static bool throtl_schedule_next_dispatch(struct throtl_service_queue *sq,
                      bool force)
{
    /* any pending children left? */
    if (!sq->nr_pending)
        return true;

    update_min_dispatch_time(sq);

    /* is the next dispatch time in the future? */
    if (force || time_after(sq->first_pending_disptime, jiffies)) {
        throtl_schedule_pending_timer(sq, sq->first_pending_disptime);
        return true;
    }

    /* tell the caller to continue dispatching */
    return false;
}

static inline void throtl_start_new_slice_with_credit(struct throtl_grp *tg,
        bool rw, unsigned long start)
{
    tg->bytes_disp[rw] = 0;
    tg->io_disp[rw] = 0;

    /*
     * Previous slice has expired. We must have trimmed it after last
     * bio dispatch. That means since start of last slice, we never used
     * that bandwidth. Do try to make use of that bandwidth while giving
     * credit.
     */
    if (time_after_eq(start, tg->slice_start[rw]))
        tg->slice_start[rw] = start;

    tg->slice_end[rw] = jiffies + throtl_slice;
//    throtl_log(&tg->service_queue,
//           "[%c] new slice with credit start=%lu end=%lu jiffies=%lu",
//           rw == READ ? 'R' : 'W', tg->slice_start[rw],
//           tg->slice_end[rw], jiffies);
}

static inline void throtl_start_new_slice(struct throtl_grp *tg, bool rw)
{
    tg->bytes_disp[rw] = 0;
    tg->io_disp[rw] = 0;
    tg->slice_start[rw] = jiffies;
    tg->slice_end[rw] = jiffies + throtl_slice;
//  throtl_log(&tg->service_queue,
//         "[%c] new slice start=%lu end=%lu jiffies=%lu",
//         rw == READ ? 'R' : 'W', tg->slice_start[rw],
//         tg->slice_end[rw], jiffies);
}

/*
 * Added by zhoufang
 */
static inline void throtl_start_new_slice_recursively(struct fake_device *fake_d,bool rw)
{
    struct fake_device_member *fd_member = fake_d->head;
    struct throtl *tg = fake_d_to_tg(fake_d);

    throtl_start_new_slice(tg,rw);

    while(fd_member != NULL){
        tg = fd_member->tg;
        
        throtl_start_new_slice(tg,rw);
        fd_member = fd_member->next;
    }
}


static inline void throtl_set_slice_end(struct throtl_grp *tg, bool rw,
                    unsigned long jiffy_end)
{
    tg->slice_end[rw] = roundup(jiffy_end, throtl_slice);
}

static inline void throtl_extend_slice(struct throtl_grp *tg, bool rw,
                       unsigned long jiffy_end)
{
    tg->slice_end[rw] = roundup(jiffy_end, throtl_slice);
//    throtl_log(&tg->service_queue,
//           "[%c] extend slice start=%lu end=%lu jiffies=%lu",
//           rw == READ ? 'R' : 'W', tg->slice_start[rw],
//           tg->slice_end[rw], jiffies);
}

/* Determine if previously allocated or extended slice is complete or not */
static bool throtl_slice_used(struct throtl_grp *tg, bool rw)
{
    if (time_in_range(jiffies, tg->slice_start[rw], tg->slice_end[rw]))
        return false;

    return 1;
}

/* Trim the used slices and adjust slice start accordingly */
 void throtl_trim_slice(struct throtl_grp *tg, unsigned int rw)
{
    unsigned long nr_slices, time_elapsed, io_trim;
    u64 bytes_trim, tmp;

    BUG_ON(time_before(tg->slice_end[rw], tg->slice_start[rw]));

    /*
     * If bps are unlimited (-1), then time slice don't get
     * renewed. Don't try to trim the slice if slice is used. A new
     * slice will start when appropriate.
     */
    if (throtl_slice_used(tg, rw))
        return;

    /*
     * A bio has been dispatched. Also adjust slice_end. It might happen
     * that initially cgroup limit was very low resulting in high
     * slice_end, but later limit was bumped up and bio was dispached
     * sooner, then we need to reduce slice_end. A high bogus slice_end
     * is bad because it does not allow new slice to start.
     */

    throtl_set_slice_end(tg, rw, jiffies + throtl_slice);

    time_elapsed = jiffies - tg->slice_start[rw];

    nr_slices = time_elapsed / throtl_slice;

    if (!nr_slices)
        return;
    tmp = tg->bps[rw] * throtl_slice * nr_slices;
    do_div(tmp, HZ);
    bytes_trim = tmp;

    io_trim = (tg->iops[rw] * throtl_slice * nr_slices)/HZ;

    if (!bytes_trim && !io_trim)
        return;

    if (tg->bytes_disp[rw] >= bytes_trim)
        tg->bytes_disp[rw] -= bytes_trim;
    else
        tg->bytes_disp[rw] = 0;

    if (tg->io_disp[rw] >= io_trim)
        tg->io_disp[rw] -= io_trim;
    else
        tg->io_disp[rw] = 0;

    tg->slice_start[rw] += nr_slices * throtl_slice;

//    throtl_log(&tg->service_queue,
//           "[%c] trim slice nr=%lu bytes=%llu io=%lu start=%lu end=%lu jiffies=%lu",
//           rw == READ ? 'R' : 'W', nr_slices, bytes_trim, io_trim,
//           tg->slice_start[rw], tg->slice_end[rw], jiffies);
}

void throtl_trim_slice_recursively(struct fake_device *fake_d, unsigned int rw)
{
    struct throtl_grp *tg = fake_d_to_tg(fake_d);
    struct fake_device_member *fd_member = fake_d->head;

    throtl_trim_slice(tg,rw);

    while(fd_member != NULL){
        tg = fd_member->tg;
        throtl_trim_slice(tg,rw);
        fd_member = fd_member->next;
    }
}

/* tg->iops[rw]!=-1 or tg->iops[RANDW]!=-1 both will lead to tg_with_in_iops_limit,should check tg->iops[] inside this function */
static bool tg_with_in_iops_limit(struct throtl_grp *tg, struct bio *bio,
                  unsigned long *wait)
{
    bool rw = bio_data_dir(bio);
    unsigned int io_allowed;
    unsigned long jiffy_elapsed, jiffy_wait, jiffy_elapsed_rnd;
    u64 tmp;

    if(tg->iops[rw]!=-1){

        /* Check origin rw limit */
        jiffy_elapsed = jiffy_elapsed_rnd = jiffies - tg->slice_start[rw];

        /* Slice has just started. Consider one slice interval */
        if (!jiffy_elapsed)
            jiffy_elapsed_rnd = throtl_slice;

        jiffy_elapsed_rnd = roundup(jiffy_elapsed_rnd, throtl_slice);

        /*
        * jiffy_elapsed_rnd should not be a big value as minimum iops can be
        * 1 then at max jiffy elapsed should be equivalent of 1 second as we
        * will allow dispatch after 1 second and after that slice should
        * have been trimmed.
        */

        tmp = (u64)tg->iops[rw] * jiffy_elapsed_rnd;
        do_div(tmp, HZ);

        if (tmp > UINT_MAX)
            io_allowed = UINT_MAX;
        else
            io_allowed = tmp;

        if (tg->io_disp[rw] + 1 <= io_allowed) {
            if (wait)
                *wait = 0;      /* Wait for RANDW check */
        }
        else{
            /* Calc approx time to dispatch */
            jiffy_wait = ((tg->io_disp[rw] + 1) * HZ)/tg->iops[rw] + 1;

            if (jiffy_wait > jiffy_elapsed)
                jiffy_wait = jiffy_wait - jiffy_elapsed;
            else
                jiffy_wait = 1;

            if (wait)
                *wait = jiffy_wait;
        }
    }

    if(tg->iops[RANDW]!=-1){

        /* Check for RANDW limit */
        jiffy_elapsed = jiffy_elapsed_rnd = jiffies - tg->slice_start[RANDW];

        /* Slice has just started. Consider one slice interval */
        if (!jiffy_elapsed)
            jiffy_elapsed_rnd = throtl_slice;

        jiffy_elapsed_rnd = roundup(jiffy_elapsed_rnd, throtl_slice);

        tmp = (u64)tg->iops[RANDW] * jiffy_elapsed_rnd;
        do_div(tmp, HZ);

        if (tmp > UINT_MAX)
            io_allowed = UINT_MAX;
        else
            io_allowed = tmp;

        if (tg->io_disp[RANDW] + 1 <= io_allowed) {
            /* Only RANDW limit exist,else this bio is in RANDW limit,wait depends on rw limit */
            if (wait && tg->iops[rw]==-1)
                *wait = 0;
        }
        else{
            /* Calc approx time to dispatch */
            jiffy_wait = ((tg->io_disp[RANDW] + 1) * HZ)/tg->iops[RANDW] + 1;

            if (jiffy_wait > jiffy_elapsed)
                jiffy_wait = jiffy_wait - jiffy_elapsed;
            else
                jiffy_wait = 1;

            if (wait && tg->iops[rw]!=-1)   /* RANDW limit and rw limit both exist */
                *wait = max(*wait,jiffy_wait);
            else if(wait)
                *wait = jiffy_wait;
        }
    }
    if(*wait == 0)
        return 1;
    else
        return 0;
}

/* tg->bps[rw]!=-1 or tg->bps[RANDW]!=-1 both will lead to tg_with_in_bps_limit,should check tg->bps[] inside this function */
static bool tg_with_in_bps_limit(struct throtl_grp *tg, struct bio *bio,
                 unsigned long *wait)
{
    bool rw = bio_data_dir(bio);
    u64 bytes_allowed, extra_bytes, tmp;
    unsigned long jiffy_elapsed, jiffy_wait, jiffy_elapsed_rnd;

    if(tg->bps[rw]!=-1){

        /* Check origin rw limit */
        jiffy_elapsed = jiffy_elapsed_rnd = jiffies - tg->slice_start[rw];

        /* Slice has just started. Consider one slice interval */
        if (!jiffy_elapsed)
            jiffy_elapsed_rnd = throtl_slice;

        jiffy_elapsed_rnd = roundup(jiffy_elapsed_rnd, throtl_slice);

        tmp = tg->bps[rw] * jiffy_elapsed_rnd;
        do_div(tmp, HZ);
        bytes_allowed = tmp;

        if (tg->bytes_disp[rw] + bio->bi_iter.bi_size <= bytes_allowed) {
            if (wait)
                *wait = 0;
        }
        else{
            /* Calc approx time to dispatch */
            extra_bytes = tg->bytes_disp[rw] + bio->bi_iter.bi_size - bytes_allowed;
            jiffy_wait = div64_u64(extra_bytes * HZ, tg->bps[rw]);

            if (!jiffy_wait)
                jiffy_wait = 1;

            /*
            * This wait time is without taking into consideration the rounding
            * up we did. Add that time also.
            */
            jiffy_wait = jiffy_wait + (jiffy_elapsed_rnd - jiffy_elapsed);
            if (wait)
                *wait = jiffy_wait;
        }
    }

    /* Check RANDW limit if exist*/
    if(tg->bps[RANDW]!=-1){

        jiffy_elapsed = jiffy_elapsed_rnd = jiffies - tg->slice_start[RANDW];

        /* Slice has just started. Consider one slice interval */
        if (!jiffy_elapsed)
            jiffy_elapsed_rnd = throtl_slice;

        jiffy_elapsed_rnd = roundup(jiffy_elapsed_rnd, throtl_slice);

        tmp = tg->bps[RANDW] * jiffy_elapsed_rnd;
        do_div(tmp, HZ);
        bytes_allowed = tmp;

        if (tg->bytes_disp[RANDW] + bio->bi_iter.bi_size <= bytes_allowed) {
            /* Only RANDW limit exist,else this bio is in RANDW limit,wait depends on rw limit */
            if (wait && tg->bps[rw]==-1)
                *wait = 0;
        }
        else{
            /* Calc approx time to dispatch */
            extra_bytes = tg->bytes_disp[RANDW] + bio->bi_iter.bi_size - bytes_allowed;
            jiffy_wait = div64_u64(extra_bytes * HZ, tg->bps[RANDW]);

            if (!jiffy_wait)
                jiffy_wait = 1;

            /*
            * This wait time is without taking into consideration the rounding
            * up we did. Add that time also.
            */
            jiffy_wait = jiffy_wait + (jiffy_elapsed_rnd - jiffy_elapsed);
            if (wait && tg->bps[rw]!=-1)    /* RANDW limit and rw limit both exist */
                *wait = max(*wait,jiffy_wait);
            else if(wait)
                *wait = jiffy_wait;
        }
    }

    if(*wait == 0)
        return 1;
    else
        return 0;
}

/*
 * Returns whether one can dispatch a bio or not. Also returns approx number
 * of jiffies to wait before this bio is with-in IO rate and can be dispatched
 */
static bool tg_may_dispatch(struct throtl_grp *tg, struct bio *bio,
                unsigned long *wait)
{
    bool rw = bio_data_dir(bio);
    unsigned long bps_wait = 0, iops_wait = 0, max_wait = 0;

    /*
     * Currently whole state machine of group depends on first bio
     * queued in the group bio list. So one should not be calling
     * this function with a different bio if there are other bios
     * queued.
     */
//    BUG_ON(tg->service_queue.nr_queued[rw] &&
//           bio != throtl_peek_queued(&tg->service_queue.queued[rw]));

    /* If tg->bps = -1, then BW is unlimited */
    if (tg->bps[rw] == -1 && tg->iops[rw] == -1 && tg->bps[RANDW] == -1 && tg->iops[RANDW] == -1) {
        if (wait)
            *wait = 0;
        printk("tg_mat_dispatch return 1 for no_rules,tg addr = %llu.\n",tg);
        return true;
    }

    /*
     * If previous slice expired, start a new one otherwise renew/extend
     * existing slice to make sure it is at least throtl_slice interval
     * long since now.
     */
    if (throtl_slice_used(tg, rw))
        throtl_start_new_slice(tg, rw);
    else {
        if (time_before(tg->slice_end[rw], jiffies + throtl_slice))
            throtl_extend_slice(tg, rw, jiffies + throtl_slice);
    }

    if (throtl_slice_used(tg, RANDW))
        throtl_start_new_slice(tg, RANDW);
    else {
        if (time_before(tg->slice_end[RANDW], jiffies + throtl_slice))
            throtl_extend_slice(tg, RANDW, jiffies + throtl_slice);
    }

    if (tg_with_in_bps_limit(tg, bio, &bps_wait) &&
        tg_with_in_iops_limit(tg, bio, &iops_wait)) {
        if (wait)
            *wait = 0;
        printk("tg_may_dispatch return 1 for within limit.tg->bps[0] = %llu,tg->slice_start[0]=%llu,tg->slice_end[0]=%llu, tg addr = %llu.\n",tg->bps[0],tg->slice_start[0],tg->slice_end[0],tg);
        return 1;
    }

    max_wait = max(bps_wait, iops_wait);

    if (wait)
        *wait = max_wait;

    if (time_before(tg->slice_end[rw], jiffies + max_wait))
        throtl_extend_slice(tg, rw, jiffies + max_wait);

    if (time_before(tg->slice_end[RANDW], jiffies + max_wait))
        throtl_extend_slice(tg, RANDW, jiffies + max_wait);

    return 0;
}

void throtl_update_dispatch_stats(struct blkcg_gq *blkg, u64 bytes,
                     int rw)
{
    struct throtl_grp *tg = blkg_to_tg(blkg);
    struct tg_stats_cpu *stats_cpu;
    unsigned long flags;

    /* If per cpu stats are not allocated yet, don't do any accounting. */
    if (tg->stats_cpu == NULL)
        return;

    /*
     * Disabling interrupts to provide mutual exclusion between two
     * writes on same cpu. It probably is not needed for 64bit. Not
     * optimizing that case yet.
     */
    local_irq_save(flags);

    stats_cpu = this_cpu_ptr(tg->stats_cpu);

    blkg_rwstat_add(&stats_cpu->serviced, rw, 1);
    blkg_rwstat_add(&stats_cpu->service_bytes, rw, bytes);

    local_irq_restore(flags);
}

/* Added by zhoufang
 * throtl_update_fd_dispatch_stats() designed for update
 * corresponding tg stata for according to fake_device
 */
void throtl_update_fd_dispatch_stats(struct fake_device *fake_d, u64 bytes,
        int rw)
{
    struct throtl_grp *tg = fake_d_to_tg(fake_d);

    struct tg_stats_cpu *stats_cpu;
    unsigned long flags;

    /* If per cpu stats are not allocated yet, don't do any accounting. */
    if (tg->stats_cpu == NULL)
        return;

    /*
     * Disabling interrupts to provide mutual exclusion between two
     * writes on same cpu. It probably is not needed for 64bit. Not
     * optimizing that case yet.
     */
    local_irq_save(flags);

    stats_cpu = this_cpu_ptr(tg->stats_cpu);

    blkg_rwstat_add(&stats_cpu->serviced, rw, 1);
    blkg_rwstat_add(&stats_cpu->service_bytes, rw, bytes);

    local_irq_restore(flags);
}


static void throtl_charge_bio(struct throtl_grp *tg, struct bio *bio)
{
    bool rw = bio_data_dir(bio);

    /* Charge the bio to the group */
    tg->bytes_disp[rw] += bio->bi_iter.bi_size;
    tg->bytes_disp[RANDW] += bio->bi_iter.bi_size;
    tg->io_disp[rw]++;
    tg->io_disp[RANDW]++;

    /*
     * REQ_THROTTLED is used to prevent the same bio to be throttled
     * more than once as a throttled bio will go through blk-throtl the
     * second time when it eventually gets issued.  Set it when a bio
     * is being charged to a tg.
     *
     * Dispatch stats aren't recursive and each @bio should only be
     * accounted by the @tg it was originally associated with.  Let's
     * update the stats when setting REQ_THROTTLED for the first time
     * which is guaranteed to be for the @bio's original tg.
     */
    if (!(bio->bi_rw & REQ_THROTTLED)) {
        bio->bi_rw |= REQ_THROTTLED;
        /* disable stats function. Modified by zhoufang*/
//      throtl_update_dispatch_stats(tg_to_blkg(tg),
//                       bio->bi_iter.bi_size, bio->bi_rw);
    }
}

static void throtl_charge_bio_recursively(struct fake_device *fake_d, struct bio *bio)
{
    struct fake_device_member *fd_member = fake_d->head;
    struct throtl_grp   *tg = fake_d_to_tg(fake_d);

    throtl_charge_bio(tg,bio);
    while(fd_member != NULL){
        tg = fd_member->tg;
        throtl_charge_bio(tg,bio);
        fd_member = fd_member->next;
    }
}


/**
 * throtl_add_bio_tg - add a bio to the specified throtl_grp
 * @bio: bio to add
 * @qn: qnode to use
 * @tg: the target throtl_grp
 *
 * Add @bio to @tg's service_queue using @qn.  If @qn is not specified,
 * tg->qnode_on_self[] is used.
 */
static void throtl_add_bio_tg(struct bio *bio, struct throtl_qnode *qn,
                  struct throtl_grp *tg)
{
    struct throtl_service_queue *sq = &tg->service_queue;
    bool rw = bio_data_dir(bio);

    if (!qn)
        qn = &tg->qnode_on_self[rw];

    /*
     * If @tg doesn't currently have any bios queued in the same
     * direction, queueing @bio can change when @tg should be
     * dispatched.  Mark that @tg was empty.  This is automatically
     * cleaered on the next tg_update_disptime().
     */
    if (!sq->nr_queued[rw])
        tg->flags |= THROTL_TG_WAS_EMPTY;

    throtl_qnode_add_bio(bio, qn, &sq->queued[rw]);

    sq->nr_queued[rw]++;
    throtl_enqueue_tg(tg);
}

static struct fake_device_member *queue_to_fd_member(struct fake_device *fake_d, 
                                        struct request_queue *q)
{
    struct fake_device_member *fd_member = fake_d->head;
    
    while (fd_member != NULL)
    {
        if(fd_member->queue == q)
            return fd_member;
        fd_member = fd_member->next;
    }

    return NULL;
}

/*
 * Added by zhoufang. fake_device_member's tg->service_queue->nr_queued might change
 * if pending_timer_fn was called. So we have update the number of queued bio in 
 * fake_d->tg, which counted the number of queued bio for eah fake_deivce_member->tg
 */
static void update_fd_queuenr(struct fake_device *fake_d)
{
    struct fake_device_member *fd_member;
    unsigned int total = 0;
    int rw = 0;
    for(rw = READ;rw <= WRITE; rw++){
        fd_member = fake_d->head;
        while(fd_member != NULL)
        {
            total += fd_member->tg->service_queue.nr_queued[rw];
            fd_member = fd_member->next;
        }
        if(total <= fake_d->tg->service_queue.nr_queued[rw])
            fake_d->tg->service_queue.nr_queued[rw] = total;
        else
            printk("the nr_queued total bigger than fake_d. total = %u, record = %u.\n",total,fake_d->tg->service_queue.nr_queued[rw]);
    }
    
}


static void throtl_add_bio_fd_tg(struct bio *bio, struct fake_device *fake_d, 
                    struct request_queue *q)
{  
  bool rw = bio_data_dir(bio);
  struct throtl_grp *tg = fake_d->tg;
  struct throtl_service_queue *sq = &tg->service_queue;
  struct throtl_qnode *qn;
  struct fake_device_member *fd_member;

  /*
   * If @tg doesn't currently have any bios queued in the same
   * direction, queueing @bio can change when @tg should be
   * dispatched.  Mark that @tg was empty.  This is automatically
   * cleaered on the next tg_update_disptime().
   */
  if (!sq->nr_queued[rw])
      tg->flags |= THROTL_TG_WAS_EMPTY;

  fd_member = queue_to_fd_member(fake_d, q);

  BUG_ON(!fd_member);

  tg = fd_member->tg;
  sq = &tg->service_queue;
  qn = &tg->qnode_on_self[rw];

  throtl_qnode_add_bio_withoutblkg(bio, qn, &sq->queued[rw]);

  sq->nr_queued[rw]++;
  fake_d->tg->service_queue.nr_queued[rw]++;
  throtl_enqueue_tg(tg);
}


static void tg_update_disptime(struct throtl_grp *tg)
{
    struct throtl_service_queue *sq = &tg->service_queue;
    unsigned long read_wait = -1, write_wait = -1, min_wait = -1, disptime;
    struct bio *bio;

    if ((bio = throtl_peek_queued(&sq->queued[READ])))
        tg_may_dispatch(tg, bio, &read_wait);

    if ((bio = throtl_peek_queued(&sq->queued[WRITE])))
        tg_may_dispatch(tg, bio, &write_wait);

    min_wait = min(read_wait, write_wait);
    disptime = jiffies + min_wait;

    /* Update dispatch time */
    throtl_dequeue_tg(tg);
    tg->disptime = disptime;
    throtl_enqueue_tg(tg);

    /* see throtl_add_bio_tg() */
    tg->flags &= ~THROTL_TG_WAS_EMPTY;
}

static void tg_update_disptime_recursively(struct fake_device *fake_d)
{
    struct throtl_service_queue *sq;
    unsigned long read_wait = -1, write_wait = -1, min_wait = -1, disptime;
    struct bio *bio;
        struct throtl_grp *tg;
    struct fake_device_member *fd_member = fake_d->head;

    while(fd_member != NULL){
        tg = fd_member->tg;
        sq = &tg->service_queue;
        if ((bio = throtl_peek_queued(&sq->queued[READ])))
            tg_may_dispatch(tg, bio, &read_wait);

        if ((bio = throtl_peek_queued(&sq->queued[WRITE])))
            tg_may_dispatch(tg, bio, &write_wait);

        min_wait = min(read_wait, min_wait);
        min_wait = min(write_wait, min_wait);

        fd_member = fd_member->next;
    }
    

    disptime = jiffies + min_wait;

    tg = fake_d->tg;
    /* Update dispatch time,no parent_sq so we don't need dequeue & enqueue */
    tg->disptime = disptime;

    /* see throtl_add_bio_tg() */
    tg->flags &= ~THROTL_TG_WAS_EMPTY;
        

    fd_member = fake_d->head;
    while(fd_member != NULL){
        tg = fd_member->tg;
        /* Update dispatch time */
        throtl_dequeue_tg(tg);
        tg->disptime = disptime;
        throtl_enqueue_tg(tg);

        /* see throtl_add_bio_tg() */
        tg->flags &= ~THROTL_TG_WAS_EMPTY;
        fd_member = fd_member->next;
    }

}


static void start_parent_slice_with_credit(struct throtl_grp *child_tg,
                    struct throtl_grp *parent_tg, bool rw)
{
    if (throtl_slice_used(parent_tg, rw)) {
        throtl_start_new_slice_with_credit(parent_tg, rw,
                child_tg->slice_start[rw]);
    }

}

static void tg_dispatch_one_bio(struct throtl_grp *tg, bool rw)
{
    struct throtl_service_queue *sq = &tg->service_queue;
    struct throtl_service_queue *parent_sq = sq->parent_sq;
    struct throtl_grp *parent_tg = sq_to_tg(parent_sq);
    struct throtl_grp *tg_to_put = NULL;
    struct bio *bio;
    struct blkcg *blkcg;
    struct fake_device *fake_d;

    /*
     * @bio is being transferred from @tg to @parent_sq.  Popping a bio
     * from @tg may put its reference and @parent_sq might end up
     * getting released prematurely.  Remember the tg to put and put it
     * after @bio is transferred to @parent_sq.
     */
    bio = throtl_pop_queued(&sq->queued[rw], &tg_to_put);
    sq->nr_queued[rw]--;
    blkcg = bio_blkcg(bio);
    fake_d = tg->fake_d;

    if(tg->fake)
        throtl_charge_bio_recursively(fake_d,bio);
    else
        throtl_charge_bio(tg, bio);

    /*
     * If our parent is another tg, we just need to transfer @bio to
     * the parent using throtl_add_bio_tg().  If our parent is
     * @td->service_queue, @bio is ready to be issued.  Put it on its
     * bio_lists[] and decrease total number queued.  The caller is
     * responsible for issuing these bios.
     */
     /*
      * Added by zhoufang. If tg corresponding to a fake_device, 
      * it's td depends on the first bio throttled in tg.
      */
    if (parent_tg) {
        throtl_add_bio_tg(bio, &tg->qnode_on_parent[rw], parent_tg);
        start_parent_slice_with_credit(tg, parent_tg, rw);
        start_parent_slice_with_credit(tg, parent_tg, RANDW);
    } else {
        if (tg->fake_d)
        {
            struct request_queue *q = bdev_get_queue(bio->bi_bdev);
            struct throtl_data *td = q->td;
            struct throtl_service_queue *td_sq = &td->service_queue;
            throtl_qnode_add_bio_withoutblkg(bio, &tg->qnode_on_parent[rw],
                &td_sq->queued[rw]);
            BUG_ON(td->nr_queued[rw] <= 0);
            td->nr_queued[rw]--;
        }
        else
        {
            throtl_qnode_add_bio(bio, &tg->qnode_on_parent[rw],
                     &parent_sq->queued[rw]);
            BUG_ON(tg->td->nr_queued[rw] <= 0);
            tg->td->nr_queued[rw]--;
        }       
    }

    if(tg->fake){
        if(tg->has_rules[rw])
            throtl_trim_slice_recursively(fake_d, rw);
        if(tg->has_rules[RANDW])
            throtl_trim_slice_recursively(fake_d, RANDW);
    }
    else{
        if(tg->has_rules[rw])
            throtl_trim_slice(tg, rw);
        if(tg->has_rules[RANDW])
            throtl_trim_slice(tg, RANDW);
    }

    if (tg_to_put && !tg->fake)
        blkg_put(tg_to_blkg(tg_to_put));
}

static int throtl_dispatch_tg(struct throtl_grp *tg)
{
    struct throtl_service_queue *sq = &tg->service_queue;
    unsigned int nr_reads = 0, nr_writes = 0;
    unsigned int max_nr_reads = throtl_grp_quantum*3/4;
    unsigned int max_nr_writes = throtl_grp_quantum - max_nr_reads;
    struct bio *bio;

    /* Try to dispatch 75% READS and 25% WRITES */

    while ((bio = throtl_peek_queued(&sq->queued[READ])) &&
           tg_may_dispatch(tg, bio, NULL)) {

        tg_dispatch_one_bio(tg, bio_data_dir(bio));
        nr_reads++;

        if (nr_reads >= max_nr_reads)
            break;
    }

    while ((bio = throtl_peek_queued(&sq->queued[WRITE])) &&
           tg_may_dispatch(tg, bio, NULL)) {

        tg_dispatch_one_bio(tg, bio_data_dir(bio));
        nr_writes++;

        if (nr_writes >= max_nr_writes)
            break;
    }

    return nr_reads + nr_writes;
}

static int throtl_select_dispatch(struct throtl_service_queue *parent_sq)
{
    unsigned int nr_disp = 0;

    while (1) {
        struct throtl_grp *tg = throtl_rb_first(parent_sq);
        struct throtl_service_queue *sq = &tg->service_queue;

        if (!tg)
            break;

        if (time_before(jiffies, tg->disptime))
            break;

        throtl_dequeue_tg(tg);

        nr_disp += throtl_dispatch_tg(tg);

        if (sq->nr_queued[0] || sq->nr_queued[1])
            tg_update_disptime(tg);

        if (nr_disp >= throtl_quantum)
            break;
    }

    return nr_disp;
}

/**
 * throtl_pending_timer_fn - timer function for service_queue->pending_timer
 * @arg: the throtl_service_queue being serviced
 *
 * This timer is armed when a child throtl_grp with active bio's become
 * pending and queued on the service_queue's pending_tree and expires when
 * the first child throtl_grp should be dispatched.  This function
 * dispatches bio's from the children throtl_grps to the parent
 * service_queue.
 *
 * If the parent's parent is another throtl_grp, dispatching is propagated
 * by either arming its pending_timer or repeating dispatch directly.  If
 * the top-level service_tree is reached, throtl_data->dispatch_work is
 * kicked so that the ready bio's are issued.
 */
static void throtl_pending_timer_fn(unsigned long arg)
{
    struct throtl_service_queue *sq = (void *)arg;
    struct throtl_grp *tg = sq_to_tg(sq);
    struct throtl_data *td = sq_to_td(sq);
    struct request_queue *q = td->queue;
    struct throtl_service_queue *parent_sq;
    bool dispatched;
    int ret;

    spin_lock_irq(q->queue_lock);
again:
    parent_sq = sq->parent_sq;
    dispatched = false;

    while (true) {
        throtl_log(sq, "dispatch nr_queued=%u read=%u write=%u",
               sq->nr_queued[READ] + sq->nr_queued[WRITE],
               sq->nr_queued[READ], sq->nr_queued[WRITE]);

        ret = throtl_select_dispatch(sq);
        if (ret) {
            throtl_log(sq, "bios disp=%u", ret);
            dispatched = true;
        }

        if (throtl_schedule_next_dispatch(sq, false))
            break;

        /* this dispatch windows is still open, relax and repeat */
        spin_unlock_irq(q->queue_lock);
        cpu_relax();
        spin_lock_irq(q->queue_lock);
    }

    if (!dispatched)
        goto out_unlock;

    if (parent_sq) {
        /* @parent_sq is another throl_grp, propagate dispatch */
        if (tg->flags & THROTL_TG_WAS_EMPTY) {
            tg_update_disptime(tg);
            if (!throtl_schedule_next_dispatch(parent_sq, false)) {
                /* window is already open, repeat dispatching */
                sq = parent_sq;
                tg = sq_to_tg(sq);
                goto again;
            }
        }
    } else {
        /* reached the top-level, queue issueing */
        queue_work(kthrotld_workqueue, &td->dispatch_work);
    }
out_unlock:
    spin_unlock_irq(q->queue_lock);
}

/**
 * blk_throtl_dispatch_work_fn - work function for throtl_data->dispatch_work
 * @work: work item being executed
 *
 * This function is queued for execution when bio's reach the bio_lists[]
 * of throtl_data->service_queue.  Those bio's are ready and issued by this
 * function.
 */
static void blk_throtl_dispatch_work_fn(struct work_struct *work)
{
    struct throtl_data *td = container_of(work, struct throtl_data,
                          dispatch_work);
    struct throtl_service_queue *td_sq = &td->service_queue;
    struct request_queue *q = td->queue;
    struct bio_list bio_list_on_stack;
    struct bio *bio;
    struct blk_plug plug;
    int rw;

    bio_list_init(&bio_list_on_stack);

    spin_lock_irq(q->queue_lock);
    for (rw = READ; rw <= WRITE; rw++)
        while ((bio = throtl_pop_queued(&td_sq->queued[rw], NULL)))
            bio_list_add(&bio_list_on_stack, bio);
    spin_unlock_irq(q->queue_lock);

    if (!bio_list_empty(&bio_list_on_stack)) {
        blk_start_plug(&plug);
        while((bio = bio_list_pop(&bio_list_on_stack)))
            generic_make_request(bio);
        blk_finish_plug(&plug);
    }
}

static u64 tg_prfill_cpu_rwstat(struct seq_file *sf,
                struct blkg_policy_data *pd, int off)
{
    struct throtl_grp *tg = pd_to_tg(pd);
    struct blkg_rwstat rwstat = { }, tmp;
    int i, cpu;

    if (tg->stats_cpu == NULL)
        return 0;

    for_each_possible_cpu(cpu) {
        struct tg_stats_cpu *sc = per_cpu_ptr(tg->stats_cpu, cpu);

        tmp = blkg_rwstat_read((void *)sc + off);
        for (i = 0; i < BLKG_RWSTAT_NR; i++)
            rwstat.cnt[i] += tmp.cnt[i];
    }

    return __blkg_prfill_rwstat(sf, pd, &rwstat);
}

static int tg_print_cpu_rwstat(struct seq_file *sf, void *v)
{
    blkcg_print_blkgs(sf, css_to_blkcg(seq_css(sf)), tg_prfill_cpu_rwstat,
              &blkcg_policy_throtl, seq_cft(sf)->private, true);
    return 0;
}

static u64 tg_prfill_conf_u64(struct seq_file *sf, struct blkg_policy_data *pd,
                  int off)
{
    struct throtl_grp *tg = pd_to_tg(pd);
    u64 v = *(u64 *)((void *)tg + off);

    if (v == -1)
        return 0;
    return __blkg_prfill_u64(sf, pd, v);
}

static u64 tg_prfill_conf_uint(struct seq_file *sf, struct blkg_policy_data *pd,
                   int off)
{
    struct throtl_grp *tg = pd_to_tg(pd);
    unsigned int v = *(unsigned int *)((void *)tg + off);

    if (v == -1)
        return 0;
    return __blkg_prfill_u64(sf, pd, v);
}

static int tg_print_conf_u64(struct seq_file *sf, void *v)
{
    blkcg_print_blkgs(sf, css_to_blkcg(seq_css(sf)), tg_prfill_conf_u64,
              &blkcg_policy_throtl, seq_cft(sf)->private, false);
    return 0;
}

static int tg_print_conf_uint(struct seq_file *sf, void *v)
{
    blkcg_print_blkgs(sf, css_to_blkcg(seq_css(sf)), tg_prfill_conf_uint,
              &blkcg_policy_throtl, seq_cft(sf)->private, false);
    return 0;
}

static ssize_t tg_set_conf(struct kernfs_open_file *of,
               char *buf, size_t nbytes, loff_t off, bool is_u64)
{
    struct blkcg *blkcg = css_to_blkcg(of_css(of));
    struct blkg_conf_ctx ctx;
    struct throtl_grp *tg;
    struct throtl_service_queue *sq;
    struct blkcg_gq *blkg;
    struct cgroup_subsys_state *pos_css;
    int ret;

    ret = blkg_conf_prep(blkcg, &blkcg_policy_throtl, buf, &ctx);
    if (ret)
        return ret;

    tg = blkg_to_tg(ctx.blkg);
    sq = &tg->service_queue;

    if (!ctx.v)
        ctx.v = -1;

    if (is_u64)
        *(u64 *)((void *)tg + of_cft(of)->private) = ctx.v;
    else
        *(unsigned int *)((void *)tg + of_cft(of)->private) = ctx.v;

    //throtl_log(&tg->service_queue,"limit change rbps=%llu wbps=%llu rwbps=%llu riops=%u wiops=%u rwiops=%u",tg->bps[READ], tg->bps[WRITE],tg->bps[RANDW],tg->iops[READ], tg->iops[WRITE],tg->iops[RANDW],);

    /*
     * Update has_rules[] flags for the updated tg's subtree.  A tg is
     * considered to have rules if either the tg itself or any of its
     * ancestors has rules.  This identifies groups without any
     * restrictions in the whole hierarchy and allows them to bypass
     * blk-throttle.
     */
    blkg_for_each_descendant_pre(blkg, pos_css, ctx.blkg)
        tg_update_has_rules(blkg_to_tg(blkg));

    /*
     * We're already holding queue_lock and know @tg is valid.  Let's
     * apply the new config directly.
     *
     * Restart the slices for both READ and WRITES. It might happen
     * that a group's limit are dropped suddenly and we don't want to
     * account recently dispatched IO with new low rate.
     */
    throtl_start_new_slice(tg, 0);
    throtl_start_new_slice(tg, 1);
    throtl_start_new_slice(tg, 2);

    if (tg->flags & THROTL_TG_PENDING) {
        tg_update_disptime(tg);
        throtl_schedule_next_dispatch(sq->parent_sq, true);
    }

    blkg_conf_finish(&ctx);
    return nbytes;
}

static ssize_t tg_set_conf_u64(struct kernfs_open_file *of,
                   char *buf, size_t nbytes, loff_t off)
{
    return tg_set_conf(of, buf, nbytes, off, true);
}

static ssize_t tg_set_conf_uint(struct kernfs_open_file *of,
                char *buf, size_t nbytes, loff_t off)
{
    return tg_set_conf(of, buf, nbytes, off, false);
}

/* Added by zhoufang
 * 
 */
                
static ssize_t tg_fd_set_conf(struct kernfs_open_file *of,
                char *buf, size_t nbytes, loff_t off, bool is_u64)

{   
    struct blkcg *blkcg = css_to_blkcg(of_css(of));
    struct blkg_fd_conf_ctx fd_ctx;
    struct throtl_grp *tg;
    struct throtl_service_queue *sq;
    struct blkcg_gq *blkg;
    struct cgroup_subsys_state *pos_css;
    int ret;

    printk("the blkcg addr in conf is:%llu\n",blkcg);
    ret = blkg_fd_conf_prep(blkcg, &blkcg_policy_throtl, buf, &fd_ctx);
    printk("the ret of blkg_fd_conf_prep is : %d\n",ret);
    if (ret)
    {
        printk("the ret exist, ret = %d.\n",ret);
        return ret;
    }

//  tg = blkg_to_tg(ctx.blkg);
//  sq = &tg->service_queue;

    if (!fd_ctx.v)
        fd_ctx.v = -1;
    printk("now fd_ctx.v = %d.\n",fd_ctx.v);

    tg = fake_d_to_tg(fd_ctx.fake_d);
    printk("get tg from fake_d_to_tg. tg->bps[0] = %llu.\n",tg->bps[0]);

    if (is_u64)
        *(u64 *)((void *)tg + of_cft(of)->private) = fd_ctx.v;
    else
        *(unsigned int *)((void *)tg + of_cft(of)->private) = fd_ctx.v;
    printk("the parameter is_u64 = %d.\n",is_u64);
    printk("tg->private = %llu,tg->bps[0] = %llu,tg addr=%llu.\n",*(unsigned int *)((void *)tg + of_cft(of)->private),tg->bps[0],tg);
    printk("in set_conf, fake_d addr = %llu, blkcg->fd_head addr = %llu.\n",fd_ctx.fake_d,blkcg->fd_head);

    //throtl_log(&tg->service_queue,"limit change rbps=%llu wbps=%llu rwbps=%llu riops=%u wiops=%u rwiops=%u",tg->bps[READ], tg->bps[WRITE],tg->bps[RANDW],tg->iops[READ], tg->iops[WRITE],tg->iops[RANDW],);

    /*
     * Update has_rules[] flags for the updated tg's subtree.  A tg is
     * considered to have rules if either the tg itself or any of its
     * ancestors has rules.  This identifies groups without any
     * restrictions in the whole hierarchy and allows them to bypass
     * blk-throttle.
     */
//  blkg_for_each_descendant_pre(blkg, pos_css, ctx.blkg)
        tg_fd_update_has_rules_recursively(fd_ctx.fake_d);
    printk("update rules. tg->has_rules[0] = %d,tg->has_rules[1] = %d,tg->has_rules[2] = %d.\n",tg->has_rules[0],tg->has_rules[1],tg->has_rules[2]);

    /*
     * We're already holding queue_lock and know @tg is valid.  Let's
     * apply the new config directly.
     *
     * Restart the slices for both READ and WRITES. It might happen
     * that a group's limit are dropped suddenly and we don't want to
     * account recently dispatched IO with new low rate.
     */
    fd_throtl_init(blkcg);
    
    throtl_start_new_slice_recursively(fd_ctx.fake_d, 0);
    printk("throtl_start_new_slice(tg, 0)\n");
    throtl_start_new_slice_recursively(fd_ctx.fake_d, 1);
    printk("throtl_start_new_slice(tg, 1)\n");
    throtl_start_new_slice_recursively(fd_ctx.fake_d, 2);
    printk("throtl_start_new_slice(tg, 2)\n");

    if (tg->flags & THROTL_TG_PENDING) {
        tg_update_disptime_recursively(fd_ctx.fake_d);
        printk("update_disaptime for pending.\n");
//      throtl_schedule_next_dispatch(sq->parent_sq, true);
    }

    blkg_fd_conf_finish(&fd_ctx);
    printk("conf_read_finish.\n");
    return nbytes;
}

/*  Added by zhoufang
 *  tg_fd_set_conf() is designed for parse config file hybrid_**_bps_device
 */
static ssize_t tg_fd_set_conf_u64(struct kernfs_open_file *of,
                char *buf, size_t nbytes, loff_t off)
{
    return tg_fd_set_conf(of, buf, nbytes, off, true);
}

                
static ssize_t tg_fd_set_conf_uint(struct kernfs_open_file *of,
                char *buf, size_t nbytes, loff_t off)
{
    return tg_fd_set_conf(of, buf, nbytes, off, false);
}


/* Added by zhoufang */
/* 
 * 
 */

bool queue_in_fake_d(struct fake_device *fake_d, struct request_queue *q)
{
    struct fake_device_member *fd_member = fake_d->head;

    while (fd_member != NULL)
    {
        if(fd_member->queue == q)
            return true;
        fd_member = fd_member->next;
    }

    return false;
}
                    

bool fake_d_has_limit(struct fake_device *fake_d, unsigned int rw,struct request_queue *q)
{
    if(queue_in_fake_d(fake_d,q))
    {
        struct throtl_grp *tg = fake_d_to_tg(fake_d);
        return tg->has_rules[rw];
    }
            

    return false;
}





/* Added by zhoufang */
/* 
 * throttle.rw_bps_device : per cgroup per device, R&W limit, in bps
 * throttle.rw_iops_devic : per cgroup per device, R&W limit, in iops
 * throttle.hybrid_read_bps_device: per cgroup, read limit, in bps
 */

static struct cftype throtl_files[] = {
    {
        .name = "throttle.read_bps_device",
        .private = offsetof(struct throtl_grp, bps[READ]),
        .seq_show = tg_print_conf_u64,
        .write = tg_set_conf_u64,
    },
    {
        .name = "throttle.write_bps_device",
        .private = offsetof(struct throtl_grp, bps[WRITE]),
        .seq_show = tg_print_conf_u64,
        .write = tg_set_conf_u64,
    },
    {
        .name = "throttle.rw_bps_device",
        .private = offsetof(struct throtl_grp, bps[RANDW]),
        .seq_show = tg_print_conf_u64,
        .write = tg_set_conf_u64,
    },
    {
        .name = "throttle.read_iops_device",
        .private = offsetof(struct throtl_grp, iops[READ]),
        .seq_show = tg_print_conf_uint,
        .write = tg_set_conf_uint,
    },
    {
        .name = "throttle.write_iops_device",
        .private = offsetof(struct throtl_grp, iops[WRITE]),
        .seq_show = tg_print_conf_uint,
        .write = tg_set_conf_uint,
    },
    {
        .name = "throttle.rw_iops_device",
        .private = offsetof(struct throtl_grp, iops[RANDW]),
        .seq_show = tg_print_conf_uint,
        .write = tg_set_conf_uint,
    },
    {
        .name = "throttle.io_service_bytes",
        .private = offsetof(struct tg_stats_cpu, service_bytes),
        .seq_show = tg_print_cpu_rwstat,
    },
    {
        .name = "throttle.io_serviced",
        .private = offsetof(struct tg_stats_cpu, serviced),
        .seq_show = tg_print_cpu_rwstat,
    },
    {
        .name = "throttle.hybrid_read_bps_device",
        .private = offsetof(struct throtl_grp, bps[READ]),
        .write = tg_fd_set_conf_u64,
    },
    {
        .name = "throttle.hybrid_write_bps_device",
        .private = offsetof(struct throtl_grp, bps[WRITE]),
        .write = tg_fd_set_conf_u64,
    },
    { } /* terminate */
};

static void throtl_shutdown_wq(struct request_queue *q)
{
    struct throtl_data *td = q->td;

    cancel_work_sync(&td->dispatch_work);
}

static struct blkcg_policy blkcg_policy_throtl = {
    .pd_size        = sizeof(struct throtl_grp),
    .cftypes        = throtl_files,

    .pd_init_fn     = throtl_pd_init,
    .pd_online_fn       = throtl_pd_online,
    .pd_exit_fn     = throtl_pd_exit,
    .pd_reset_stats_fn  = throtl_pd_reset_stats,
};



bool blk_throtl_bio(struct request_queue *q, struct bio *bio)
{
    struct throtl_data *td = q->td;
    struct throtl_qnode *qn = NULL;
    struct throtl_grp *tg;
    struct throtl_service_queue *sq;
    bool rw = bio_data_dir(bio);
    struct blkcg *blkcg;
    struct fake_device *fake_d;
    bool throttled = false;

    printk("BLK_THROTL_BIO:now in blk_throtl_bio function.\n");
//   msleep(15000);
    /* see throtl_charge_bio() */
    if (bio->bi_rw & REQ_THROTTLED)
        goto out;
    printk("BLK_THROTL_BIO:pass goto out test.\n");
//    msleep(15000);

    /*
     * A throtl_grp pointer retrieved under rcu can be used to access
     * basic fields like stats and io rates. If a group has no rules,
     * just update the dispatch stats in lockless manner and return.
     */
    rcu_read_lock();
    blkcg = bio_blkcg(bio);
    printk("BLK_THROTL_BIO:blkcg_addr = %llu\n",blkcg);
  //  msleep(15000);
    tg = throtl_lookup_tg(td, blkcg);
    if (tg) {
        bool without_limit = true;
    
        if (!tg->has_rules[rw] && !tg->has_rules[RANDW]) {
            throtl_update_dispatch_stats(tg_to_blkg(tg),
                    bio->bi_iter.bi_size, bio->bi_rw);

            /* check whether we should update some tg in which q
             * was included.
             */
            printk("BLK_THROTL_BIO:check whether tg has_rules was done.\n");
//            msleep(15000);
            fake_d = blkcg->fd_head;
            printk("BLK_THROTL_BIO:blkcg->fd_head addr = %llu\n",blkcg->fd_head);
//            msleep(15000);
            while(fake_d != NULL)
            {
                printk("BLK_THROTL_BIO:fake_d: id=%d,r_bps=%d,w_bps=%d,rw_bps=%d\n",fake_d->id,fake_d->tg->bps[0],fake_d->tg->bps[1],fake_d->tg->bps[2]);
//                msleep(15000);
                if(queue_in_fake_d(fake_d,q))
                {
//                  if(!fake_d_has_limit(fake_d,rw,q) && !fake_d_has_limit(fake_d,RANDW,q))
//                      throtl_update_fd_dispatch_stats(fake_d,bio->bi_iter.bi_size, bio->bi_rw);
                    if(fake_d_has_limit(fake_d,rw,q) || fake_d_has_limit(fake_d,RANDW,q))
                        without_limit = false;
                }
                fake_d = fake_d->next;
                printk("BLK_THROTL_BIO:now in fake_d has_rules check loop.\n");
//                msleep(15000);
            }
            if(without_limit)
                goto out_unlock_rcu;
        }
    }

    /*
     * Either group has not been allocated yet or it is not an unlimited
     * IO group
     */
    spin_lock_irq(q->queue_lock);
    printk("BLK_THROTL_BIO:now we has got queue spin_lock.\n");
    //msleep(15000);
    tg = throtl_lookup_create_tg(td, blkcg);
    if (unlikely(!tg))
        goto fake_device_check;

    sq = &tg->service_queue;

    printk("BLK_THROTL_BIO:next, go tg dispatch loop.\n");
    while (true) {
        /* throtl is FIFO - if bios are already queued, should queue */
        if (sq->nr_queued[rw])
            break;

        /* if above limits, break to queue */
        if (!tg_may_dispatch(tg, bio, NULL))
            break;

        /* within limits, let's charge and dispatch directly */
        throtl_charge_bio(tg, bio);
        printk("BLK_THROTL_BIO: within limit, origin tg was charged.\n");
        //msleep(15000);

        /*
         * We need to trim slice even when bios are not being queued
         * otherwise it might happen that a bio is not queued for
         * a long time and slice keeps on extending and trim is not
         * called for a long time. Now if limits are reduced suddenly
         * we take into account all the IO dispatched so far at new
         * low rate and * newly queued IO gets a really long dispatch
         * time.
         *
         * So keep on trimming slice even if bio is not queued.
         */
        if(tg->has_rules[rw])
            throtl_trim_slice(tg, rw);
        if(tg->has_rules[RANDW])
            throtl_trim_slice(tg, RANDW);
        printk("BLK_THROTL_BIO: trim origin tg's slice\n");
        //msleep(15000);

        /*
         * @bio passed through this layer without being throttled.
         * Climb up the ladder.  If we''re already at the top, it
         * can be executed directly.
         */
        qn = &tg->qnode_on_parent[rw];
        sq = sq->parent_sq;
        tg = sq_to_tg(sq);
        if (!tg){
            printk("BLK_THROTL_BIO: parent tg not exist.\n");
           // msleep(15000);
            if(blkcg->fd_head != NULL){
		printk("blkcg->fd_head exist.\n");
                goto fake_device_check;
            }
	    else
                goto out_unlock;
        }
    }

    /* out-of-limit, queue to @tg */
    throtl_log(sq, "[%c] bio. bdisp=%llu rwbdisp=%llu sz=%u bps=%llu rwbps=%llu iodisp=%u rwiodisp=%u iops=%u rwiops=%uqueued=%d/%d",
           rw == READ ? 'R' : 'W',
           tg->bytes_disp[rw], tg->bytes_disp[RANDW], bio->bi_iter.bi_size, tg->bps[rw], tg->bps[RANDW],
           tg->io_disp[rw], tg->io_disp[RANDW], tg->iops[rw], tg->iops[RANDW],
           sq->nr_queued[READ], sq->nr_queued[WRITE]);

    bio_associate_current(bio);
    tg->td->nr_queued[rw]++;
    throtl_add_bio_fd_tg(bio, fake_d, q);
    throttled = true;

    /*
     * Update @tg's dispatch time and force schedule dispatch if @tg
     * was empty before @bio.  The forced scheduling isn't likely to
     * cause undue delay as @bio is likely to be dispatched directly if
     * its @tg's disptime is not in the future.
     */
    if (tg->flags & THROTL_TG_WAS_EMPTY) {
        tg_update_disptime(tg);
        throtl_schedule_next_dispatch(tg->service_queue.parent_sq, true);
    }

fake_device_check:
    printk("BLK_THROTL_BIO: now we come to fake_device_check.\n");
//    msleep(15000);
    /* throttled bio was associated with native cgroup tg
     * if so, we should charge this bio in the relevant fake_d tg
     */
    if (throttled) {
        fake_d = blkcg->fd_head;
        printk("BLK_THROTL_BIO: bio was throttled by origin tg.\n");
  //      //msleep(15000);
        while(fake_d != NULL) {
            if (queue_in_fake_d(fake_d, q) && fake_d_has_limit(fake_d, rw, q)) {
                printk("BLK_THROTL_BIO: queue_in_fake_d, we will charge this bio recursively.\n");
                //msleep(15000);
                throtl_charge_bio_recursively(fake_d, bio);
            }
            fake_d = fake_d->next;
        }
        goto out_unlock;
    }
    else {
        fake_d = blkcg->fd_head;
        printk("in blk_throtl_bio, blkcg->fd_head addr = %llu.\n",fake_d);
        printk("BLK_THROTL_BIO: bio was not throttled by origin tg.\n");
//        msleep(15000);
        while(true) {
            printk("BLK_THROTL_BIO: fake_d not null, next we will update queuenr.\n");
 //           msleep(15000);
           update_fd_queuenr(fake_d);
            if (fake_d_has_limit(fake_d, rw, q)) {
                printk("BLK_THROTL_BIO: current fake_d has limit on queue.\n");
              //msleep(15000);
                tg = fake_d_to_tg(fake_d);
                printk("in blk_throtl_bio, fake_d_to_tg addr = %llu,tg->bps[0]=%llu.\n",tg,tg->bps[0]);
                sq = &tg->service_queue;
                if (sq->nr_queued[rw]){
		    printk("break fake_d check because sq->nr_queued[rw] = %d.\n",sq->nr_queued[rw]);
                    break;
		}

               /* if above limits, break to queue */
                if (!tg_may_dispatch(tg, bio, NULL)){
                   printk("BLK_THROTL_BIO: over fake_d limit, next break loop.\n");
                   //msleep(15000);
                    break;
                }

               /* within limits, let's charge and dispatch directly */
                printk("BLK_THROTL_BIO: within fake_d limit, charge fake_d recursively.\n");
               //msleep(15000);
                throtl_charge_bio_recursively(fake_d, bio);

                /*
                 * We need to trim slice even when bios are not being queued
                 * otherwise it might happen that a bio is not queued for
                 * a long time and slice keeps on extending and trim is not
                 * called for a long time. Now if limits are reduced suddenly
                 * we take into account all the IO dispatched so far at new
                 * low rate and * newly queued IO gets a really long dispatch
                 * time.
                 *
                 * So keep on trimming slice even if bio is not queued.
                 */
                if(tg->has_rules[rw])
                    throtl_trim_slice_recursively(fake_d, rw);
                if(tg->has_rules[RANDW])
                    throtl_trim_slice_recursively(fake_d, RANDW);
                printk("BLK_THROTL_BIO: trim fake_d's tg recursively.\n");
               // msleep(15000);

            }
            fake_d = fake_d->next;
            if(fake_d == NULL){
		printk("fake_d == NULL, goto out_unlock.\n");
		goto out_unlock;
	    }
	    printk("fake_d is not null, go next round.\n");
        }

        printk("BLK_THROTL_BIO: next associate bio with current process.\n");
        //msleep(15000);
        bio_associate_current(bio);
        (q->td)->nr_queued[rw]++;
        printk("BLK_THROTL_BIO: add bio to fake_device_member's tg.\n");
        //msleep(15000);
        throtl_add_bio_tg(bio, qn, queue_to_fd_member(fake_d, q)->tg);
        throttled = true;

//        if (tg->flags & THROTL_TG_WAS_EMPTY) {
//            printk("BLK_THROTL_BIO: tg_update_disptime_recursively for target fake_d.\n");
            //msleep(15000);
            tg_update_disptime_recursively(fake_d);
            printk("BLK_THROTL_BIO: fake_d's tg->disptime = %lu.\n",fake_d->tg->disptime);
            struct fake_device_member *fd_member = queue_to_fd_member(fake_d, q);
            BUG_ON(!fd_member);
            tg = fd_member->tg;
            printk("BLK_THROTL_BIO: throtl_schedule_next_dispatch for fake_device_member's tg.\n");
            //msleep(15000);
            throtl_schedule_next_dispatch(tg->service_queue.parent_sq, true);
//        }
        
    }

out_unlock:
    printk("try to unlock queue_lock.\n");
    spin_unlock_irq(q->queue_lock);
out_unlock_rcu:
    printk("try to unlock ruc_read_lock.\n");
    rcu_read_unlock();
out:
    /*
     * As multiple blk-throtls may stack in the same issue path, we
     * don't want bios to leave with the flag set.  Clear the flag if
     * being issued.
     */
    if (!throttled)
        bio->bi_rw &= ~REQ_THROTTLED;
    printk("return value of throttled = %d.\n",throttled);
    return throttled;
}

/*
 * Dispatch all bios from all children tg's queued on @parent_sq.  On
 * return, @parent_sq is guaranteed to not have any active children tg's
 * and all bios from previously active tg's are on @parent_sq->bio_lists[].
 */
static void tg_drain_bios(struct throtl_service_queue *parent_sq)
{
    struct throtl_grp *tg;

    while ((tg = throtl_rb_first(parent_sq))) {
        struct throtl_service_queue *sq = &tg->service_queue;
        struct bio *bio;

        throtl_dequeue_tg(tg);

        while ((bio = throtl_peek_queued(&sq->queued[READ])))
            tg_dispatch_one_bio(tg, bio_data_dir(bio));
        while ((bio = throtl_peek_queued(&sq->queued[WRITE])))
            tg_dispatch_one_bio(tg, bio_data_dir(bio));
    }
}

/**
 * blk_throtl_drain - drain throttled bios
 * @q: request_queue to drain throttled bios for
 *
 * Dispatch all currently throttled bios on @q through ->make_request_fn().
 */
void blk_throtl_drain(struct request_queue *q)
    __releases(q->queue_lock) __acquires(q->queue_lock)
{
    struct throtl_data *td = q->td;
    struct blkcg_gq *blkg;
    struct cgroup_subsys_state *pos_css;
    struct bio *bio;
    int rw;

    queue_lockdep_assert_held(q);
    rcu_read_lock();

    /*
     * Drain each tg while doing post-order walk on the blkg tree, so
     * that all bios are propagated to td->service_queue.  It'd be
     * better to walk service_queue tree directly but blkg walk is
     * easier.
     */
    blkg_for_each_descendant_post(blkg, pos_css, td->queue->root_blkg)
        tg_drain_bios(&blkg_to_tg(blkg)->service_queue);

    /* finally, transfer bios from top-level tg's into the td */
    tg_drain_bios(&td->service_queue);

    rcu_read_unlock();
    spin_unlock_irq(q->queue_lock);

    /* all bios now should be in td->service_queue, issue them */
    for (rw = READ; rw <= WRITE; rw++)
        while ((bio = throtl_pop_queued(&td->service_queue.queued[rw],
                        NULL)))
            generic_make_request(bio);

    spin_lock_irq(q->queue_lock);
}

int blk_throtl_init(struct request_queue *q)
{
    struct throtl_data *td;
    int ret;

    td = kzalloc_node(sizeof(*td), GFP_KERNEL, q->node);
    if (!td)
        return -ENOMEM;

    INIT_WORK(&td->dispatch_work, blk_throtl_dispatch_work_fn);
    throtl_service_queue_init(&td->service_queue, NULL);

    q->td = td;
    td->queue = q;

    /* activate policy */
    ret = blkcg_activate_policy(q, &blkcg_policy_throtl);
    if (ret)
        kfree(td);
    return ret;
}

void blk_throtl_exit(struct request_queue *q)
{
    BUG_ON(!q->td);
    throtl_shutdown_wq(q);
    blkcg_deactivate_policy(q, &blkcg_policy_throtl);
    kfree(q->td);
}

static int __init throtl_init(void)
{
    kthrotld_workqueue = alloc_workqueue("kthrotld", WQ_MEM_RECLAIM, 0);
    if (!kthrotld_workqueue)
        panic("Failed to create kthrotld\n");

    return blkcg_policy_register(&blkcg_policy_throtl);
}

module_init(throtl_init);

