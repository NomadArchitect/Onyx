// SPDX-License-Identifier: GPL-2.0
/*
 * KCSAN reporting.
 *
 * Copyright (C) 2019, Google LLC.
 */
// clang-format off
#if 0
#include <linux/debug_locks.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/kallsyms.h>
#include <linux/kernel.h>
#include <linux/lockdep.h>
#include <linux/preempt.h>
#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/stacktrace.h>
#endif

#include <onyx/perf_probe.h>
#include <onyx/cpu.h>
#include <onyx/irq.h>
#include <onyx/modules.h>

#include <onyx/clock.h>
#include <stdio.h>
#include <onyx/scheduler.h>

#include "kcsan.h"
#include "encoding.h"

/*
 * Max. number of stack entries to show in the report.
 */
#define NUM_STACK_ENTRIES 64

/* Common access info. */
struct access_info {
	const volatile void	*ptr;
	size_t			size;
	int			access_type;
	int			task_pid;
	int			cpu_id;
	unsigned long		ip;
};

/*
 * Other thread info: communicated from other racing thread to thread that set
 * up the watchpoint, which then prints the complete report atomically.
 */
struct other_info {
	struct access_info	ai;
	unsigned long		stack_entries[NUM_STACK_ENTRIES];
	int			num_stack_entries;

	/*
	 * Optionally pass @current. Typically we do not need to pass @current
	 * via @other_info since just @task_pid is sufficient. Passing @current
	 * has additional overhead.
	 *
	 * To safely pass @current, we must either use get_task_struct/
	 * put_task_struct, or stall the thread that populated @other_info.
	 *
	 * We cannot rely on get_task_struct/put_task_struct in case
	 * release_report() races with a task being released, and would have to
	 * free it in release_report(). This may result in deadlock if we want
	 * to use KCSAN on the allocators.
	 *
	 * Since we also want to reliably print held locks for
	 * CONFIG_KCSAN_VERBOSE, the current implementation stalls the thread
	 * that populated @other_info until it has been consumed.
	 */
	struct thread	*task;
};

/*
 * To never block any producers of struct other_info, we need as many elements
 * as we have watchpoints (upper bound on concurrent races to report).
 */
static struct other_info other_infos[CONFIG_KCSAN_NUM_WATCHPOINTS + NUM_SLOTS-1];

/*
 * Information about reported races; used to rate limit reporting.
 */
struct report_time {
	/*
	 * The last time the race was reported.
	 */
	unsigned long time;

	/*
	 * The frames of the 2 threads; if only 1 thread is known, one frame
	 * will be 0.
	 */
	unsigned long frame1;
	unsigned long frame2;
};

#define CONFIG_KCSAN_REPORT_ONCE_IN_MS 3000

/*
 * Since we also want to be able to debug allocators with KCSAN, to avoid
 * deadlock, report_times cannot be dynamically resized with krealloc in
 * rate_limit_report.
 *
 * Therefore, we use a fixed-size array, which at most will occupy a page. This
 * still adequately rate limits reports, assuming that a) number of unique data
 * races is not excessive, and b) occurrence of unique races within the
 * same time window is limited.
 */
#define REPORT_TIMES_MAX (PAGE_SIZE / sizeof(struct report_time))
#define REPORT_TIMES_SIZE                                                      \
	(CONFIG_KCSAN_REPORT_ONCE_IN_MS > REPORT_TIMES_MAX ?                   \
		 REPORT_TIMES_MAX :                                            \
		 CONFIG_KCSAN_REPORT_ONCE_IN_MS)
static struct report_time report_times[REPORT_TIMES_SIZE];

#define BUILD_BUG_ON(cond) _Static_assert(!(cond), #cond)

/*
 * Spinlock serializing report generation, and access to @other_infos. Although
 * it could make sense to have a finer-grained locking story for @other_infos,
 * report generation needs to be serialized either way, so not much is gained.
 */
static struct spinlock report_lock;

/*
 * Checks if the race identified by thread frames frame1 and frame2 has
 * been reported since (now - KCSAN_REPORT_ONCE_IN_MS).
 */
static bool rate_limit_report(unsigned long frame1, unsigned long frame2)
{
	struct report_time *use_entry = &report_times[0];
	unsigned long invalid_before;
	unsigned long i;

	BUILD_BUG_ON(CONFIG_KCSAN_REPORT_ONCE_IN_MS != 0 && REPORT_TIMES_SIZE == 0);

    return false;
	if (CONFIG_KCSAN_REPORT_ONCE_IN_MS == 0)
		return false;

	invalid_before = (clocksource_get_time() / NS_PER_MS) - (CONFIG_KCSAN_REPORT_ONCE_IN_MS);

	/* Check if a matching race report exists. */
	for (i = 0; i < REPORT_TIMES_SIZE; ++i) {
		struct report_time *rt = &report_times[i];

		/*
		 * Must always select an entry for use to store info as we
		 * cannot resize report_times; at the end of the scan, use_entry
		 * will be the oldest entry, which ideally also happened before
		 * KCSAN_REPORT_ONCE_IN_MS ago.
		 */
		if (rt->time < use_entry->time)
			use_entry = rt;

		/*
		 * Initially, no need to check any further as this entry as well
		 * as following entries have never been used.
		 */
		if (rt->time == 0)
			break;

		/* Check if entry expired. */
		if (rt->time < invalid_before)
			continue; /* before KCSAN_REPORT_ONCE_IN_MS ago */

		/* Reported recently, check if race matches. */
		if ((rt->frame1 == frame1 && rt->frame2 == frame2) ||
		    (rt->frame1 == frame2 && rt->frame2 == frame1))
			return true;
	}

	use_entry->time = clocksource_get_time() / NS_PER_MS;
	use_entry->frame1 = frame1;
	use_entry->frame2 = frame2;
	return false;
}

#define CONFIG_KCSAN_REPORT_VALUE_CHANGE_ONLY 1

/*
 * Special rules to skip reporting.
 */
static bool
skip_report(enum kcsan_value_change value_change, unsigned long top_frame)
{
	/* Should never get here if value_change==FALSE. */
	//WARN_ON_ONCE(value_change == KCSAN_VALUE_CHANGE_FALSE);

	/*
	 * The first call to skip_report always has value_change==TRUE, since we
	 * cannot know the value written of an instrumented access. For the 2nd
	 * call there are 6 cases with CONFIG_KCSAN_REPORT_VALUE_CHANGE_ONLY:
	 *
	 * 1. read watchpoint, conflicting write (value_change==TRUE): report;
	 * 2. read watchpoint, conflicting write (value_change==MAYBE): skip;
	 * 3. write watchpoint, conflicting write (value_change==TRUE): report;
	 * 4. write watchpoint, conflicting write (value_change==MAYBE): skip;
	 * 5. write watchpoint, conflicting read (value_change==MAYBE): skip;
	 * 6. write watchpoint, conflicting read (value_change==TRUE): report;
	 *
	 * Cases 1-4 are intuitive and expected; case 5 ensures we do not report
	 * data races where the write may have rewritten the same value; case 6
	 * is possible either if the size is larger than what we check value
	 * changes for or the access type is KCSAN_ACCESS_ASSERT.
	 */
     return false;
#if 0
	if (CONFIG_KCSAN_REPORT_VALUE_CHANGE_ONLY &&
	    value_change == KCSAN_VALUE_CHANGE_MAYBE) {
		/*
		 * The access is a write, but the data value did not change.
		 *
		 * We opt-out of this filter for certain functions at request of
		 * maintainers.
		 */
		char buf[64];
		int len = scnprintf(buf, sizeof(buf), "%ps", (void *)top_frame);

		if (!strnstr(buf, "rcu_", len) &&
		    !strnstr(buf, "_rcu", len) &&
		    !strnstr(buf, "_srcu", len))
			return true;
	}
#endif
	//return kcsan_skip_report_debugfs(top_frame);
}

static const char *get_access_type(int type)
{
	if (type & KCSAN_ACCESS_ASSERT) {
		if (type & KCSAN_ACCESS_SCOPED) {
			if (type & KCSAN_ACCESS_WRITE)
				return "assert no accesses (reordered)";
			else
				return "assert no writes (reordered)";
		} else {
			if (type & KCSAN_ACCESS_WRITE)
				return "assert no accesses";
			else
				return "assert no writes";
		}
	}

	switch (type) {
	case 0:
		return "read";
	case KCSAN_ACCESS_ATOMIC:
		return "read (marked)";
	case KCSAN_ACCESS_WRITE:
		return "write";
	case KCSAN_ACCESS_WRITE | KCSAN_ACCESS_ATOMIC:
		return "write (marked)";
	case KCSAN_ACCESS_COMPOUND | KCSAN_ACCESS_WRITE:
		return "read-write";
	case KCSAN_ACCESS_COMPOUND | KCSAN_ACCESS_WRITE | KCSAN_ACCESS_ATOMIC:
		return "read-write (marked)";
	case KCSAN_ACCESS_SCOPED:
		return "read (reordered)";
	case KCSAN_ACCESS_SCOPED | KCSAN_ACCESS_ATOMIC:
		return "read (marked, reordered)";
	case KCSAN_ACCESS_SCOPED | KCSAN_ACCESS_WRITE:
		return "write (reordered)";
	case KCSAN_ACCESS_SCOPED | KCSAN_ACCESS_WRITE | KCSAN_ACCESS_ATOMIC:
		return "write (marked, reordered)";
	case KCSAN_ACCESS_SCOPED | KCSAN_ACCESS_COMPOUND | KCSAN_ACCESS_WRITE:
		return "read-write (reordered)";
	case KCSAN_ACCESS_SCOPED | KCSAN_ACCESS_COMPOUND | KCSAN_ACCESS_WRITE | KCSAN_ACCESS_ATOMIC:
		return "read-write (marked, reordered)";
	default:
		panic("BUG");
	}
}

static const char *get_bug_type(int type)
{
	return (type & KCSAN_ACCESS_ASSERT) != 0 ? "assert: race" : "data-race";
}

/* Return thread description: in task or interrupt. */
static const char *get_thread_desc(int task_id)
{
	if (task_id != -1) {
		static char buf[32]; /* safe: protected by report_lock */

		snprintf(buf, sizeof(buf), "task %i", task_id);
		return buf;
	}
	return "interrupt";
}

/**
 * strnstr - Find the first substring in a length-limited string
 * @s1: The string to be searched
 * @s2: The string to search for
 * @len: the maximum number of characters to search
 */
char *strnstr(const char *s1, const char *s2, size_t len)
{
	size_t l2;

	l2 = strlen(s2);
	if (!l2)
		return (char *)s1;
	while (len >= l2) {
		len--;
		if (!memcmp(s1, s2, l2))
			return (char *)s1;
		s1++;
	}
	return NULL;
}

__always_inline size_t str_has_prefix(const char *str, const char *prefix)
{
	size_t len = strlen(prefix);
	return strncmp(str, prefix, len) == 0 ? len : 0;
}

/* Helper to skip KCSAN-related functions in stack-trace. */
static int get_stack_skipnr(const unsigned long stack_entries[], int num_entries)
{
	char buf[128];
	char *cur;
	int len, skip;


	for (skip = 0; skip < num_entries; ++skip) {
		len = snprintf(buf, sizeof(buf), "%ps", (void *)stack_entries[skip]);
		if ((size_t) len >= sizeof(buf))
			len = sizeof(buf) - 1;

		/* Never show tsan_* or {read,write}_once_size. */
		if (strnstr(buf, "tsan_", len) ||
		    strnstr(buf, "_once_size", len))
			continue;

		cur = strnstr(buf, "kcsan_", len);
		if (cur) {
			cur += strlen("kcsan_");
			if (!str_has_prefix(cur, "test"))
				continue; /* KCSAN runtime function. */
			/* KCSAN related test. */
		}

		/*
		 * No match for runtime functions -- @skip entries to skip to
		 * get to first frame of interest.
		 */
		break;
	}

	return skip;
}

#define kallsyms_lookup_size_offset sym_get_off_size

/*
 * Skips to the first entry that matches the function of @ip, and then replaces
 * that entry with @ip, returning the entries to skip with @replaced containing
 * the replaced entry.
 */
static int
replace_stack_entry(unsigned long stack_entries[], int num_entries, unsigned long ip,
		    unsigned long *replaced)
{
	unsigned long symbolsize, offset;
	unsigned long target_func;
	int skip;

	if (kallsyms_lookup_size_offset(ip, &symbolsize, &offset))
		target_func = ip - offset;
	else
		goto fallback;

	for (skip = 0; skip < num_entries; ++skip) {
		unsigned long func = stack_entries[skip];

		if (!kallsyms_lookup_size_offset(func, &symbolsize, &offset))
			goto fallback;
		func -= offset;

		if (func == target_func) {
			*replaced = stack_entries[skip];
			stack_entries[skip] = ip;
			return skip;
		}
	}

fallback:
	/* Should not happen; the resulting stack trace is likely misleading. */
	pr_warn("Cannot find frame for %pS in stack trace", (void *)ip);
	return get_stack_skipnr(stack_entries, num_entries);
}

static int
sanitize_stack_entries(unsigned long stack_entries[], int num_entries, unsigned long ip,
		       unsigned long *replaced)
{
	return ip ? replace_stack_entry(stack_entries, num_entries, ip, replaced) :
			  get_stack_skipnr(stack_entries, num_entries);
}

/* Compares symbolized strings of addr1 and addr2. */
static int sym_strcmp(void *addr1, void *addr2)
{
	char buf1[64];
	char buf2[64];

	snprintf(buf1, sizeof(buf1), "%pS", addr1);
	snprintf(buf2, sizeof(buf2), "%pS", addr2);

	return strncmp(buf1, buf2, sizeof(buf1));
}

static void stack_trace_print(unsigned long stack_entries[], int num_entries, int spaces)
{
    for (int i = 0; i < num_entries; i++)
        pr_err(" %pS\n", (void *) stack_entries[i]);
    pr_err("\n");
}

static void
print_stack_trace(unsigned long stack_entries[], int num_entries, unsigned long reordered_to)
{
	stack_trace_print(stack_entries, num_entries, 0);
	if (reordered_to)
		pr_err("  |\n  +-> reordered to: %pS\n", (void *)reordered_to);
}

#define debug_show_held_locks(task)
#define print_irqtrace_events(task)

static void print_verbose_info(struct task_struct *task)
{
	if (!task)
		return;

	/* Restore IRQ state trace for printing. */
	kcsan_restore_irqtrace(task);

	pr_err("\n");
	debug_show_held_locks(task);
	print_irqtrace_events(task);
}

static void print_report(enum kcsan_value_change value_change,
			 const struct access_info *ai,
			 struct other_info *other_info,
			 u64 old, u64 new, u64 mask)
{
	unsigned long reordered_to = 0;
	unsigned long stack_entries[NUM_STACK_ENTRIES] = { 0 };
	int num_stack_entries = stack_trace_get((unsigned long *) __builtin_frame_address(0), stack_entries, NUM_STACK_ENTRIES);
	int skipnr = sanitize_stack_entries(stack_entries, num_stack_entries, ai->ip, &reordered_to);
	unsigned long this_frame = stack_entries[skipnr];
	unsigned long other_reordered_to = 0;
	unsigned long other_frame = 0;
	int other_skipnr = 0; /* silence uninit warnings */

	/*
	 * Must check report filter rules before starting to print.
	 */
	if (skip_report(KCSAN_VALUE_CHANGE_TRUE, stack_entries[skipnr]))
		return;

	if (other_info) {
		other_skipnr = sanitize_stack_entries(other_info->stack_entries,
						      other_info->num_stack_entries,
						      other_info->ai.ip, &other_reordered_to);
		other_frame = other_info->stack_entries[other_skipnr];

		/* @value_change is only known for the other thread */
		if (skip_report(value_change, other_frame))
			return;
	}

	if (rate_limit_report(this_frame, other_frame))
		return;

	/* Print report header. */
	pr_err("==================================================================\n");
	if (other_info) {
		int cmp;

		/*
		 * Order functions lexographically for consistent bug titles.
		 * Do not print offset of functions to keep title short.
		 */
		cmp = -1;
		pr_err("BUG: KCSAN: %s in %ps / %ps\n",
		       get_bug_type(ai->access_type | other_info->ai.access_type),
		       (void *)(cmp < 0 ? other_frame : this_frame),
		       (void *)(cmp < 0 ? this_frame : other_frame));
	} else {
		pr_err("BUG: KCSAN: %s in %pS\n", get_bug_type(ai->access_type),
		       (void *)this_frame);
	}

	pr_err("\n");

	/* Print information about the racing accesses. */
	if (other_info) {
		pr_err("%s to 0x%px of %zu bytes by %s on cpu %i:\n",
		       get_access_type(other_info->ai.access_type), other_info->ai.ptr,
		       other_info->ai.size, get_thread_desc(other_info->ai.task_pid),
		       other_info->ai.cpu_id);

		/* Print the other thread's stack trace. */
		print_stack_trace(other_info->stack_entries + other_skipnr,
				  other_info->num_stack_entries - other_skipnr,
				  other_reordered_to);
		if (0)
			print_verbose_info(NULL);

		pr_err("\n");
		pr_err("%s to 0x%px of %zu bytes by %s on cpu %i:\n",
		       get_access_type(ai->access_type), ai->ptr, ai->size,
		       get_thread_desc(ai->task_pid), ai->cpu_id);
	} else {
		pr_err("race at unknown origin, with %s to 0x%px of %zu bytes by %s on cpu %i:\n",
		       get_access_type(ai->access_type), ai->ptr, ai->size,
		       get_thread_desc(ai->task_pid), ai->cpu_id);
	}
	/* Print stack trace of this thread. */
	print_stack_trace(stack_entries + skipnr, num_stack_entries - skipnr, reordered_to);
	if (0)
		print_verbose_info(NULL);

	/* Print observed value change. */
	if (ai->size <= 8) {
		int hex_len = ai->size * 2;
		u64 diff = old ^ new;

		if (mask)
			diff &= mask;
		if (diff) {
			pr_err("\n");
			pr_err("value changed: 0x%0*lx -> 0x%0*lx\n",
			       hex_len, old, hex_len, new);
			if (mask) {
				pr_err(" bits changed: 0x%0*lx with mask 0x%0*lx\n",
				       hex_len, diff, hex_len, mask);
			}
		}
	}

	/* Print report footer. */
	pr_err("\n");
	pr_err("Reported by Kernel Concurrency Sanitizer on:\n");
	//dump_stack_print_info(KERN_DEFAULT);
	pr_err("==================================================================\n");
}

static void release_report(unsigned long *flags, struct other_info *other_info)
{
	/*
	 * Use size to denote valid/invalid, since KCSAN entirely ignores
	 * 0-sized accesses.
	 */
	other_info->ai.size = 0;
	spin_unlock_irqrestore(&report_lock, *flags);
}

/*
 * Sets @other_info->task and awaits consumption of @other_info.
 *
 * Precondition: report_lock is held.
 * Postcondition: report_lock is held.
 */
static void set_other_info_task_blocking(unsigned long *flags,
					 const struct access_info *ai,
					 struct other_info *other_info)
{
	/*
	 * We may be instrumenting a code-path where current->state is already
	 * something other than TASK_RUNNING.
	 */
	/*
	 * To avoid deadlock in case we are in an interrupt here and this is a
	 * race with a task on the same CPU (KCSAN_INTERRUPT_WATCHER), provide a
	 * timeout to ensure this works in all contexts.
	 *
	 * Await approximately the worst case delay of the reporting thread (if
	 * we are not interrupted).
	 */
	int timeout = kcsan_udelay_task > kcsan_udelay_interrupt ? kcsan_udelay_task : kcsan_udelay_interrupt;

	other_info->task = get_current_thread();
	do {
		spin_unlock_irqrestore(&report_lock, *flags);
		/*
		 * We cannot call schedule() since we also cannot reliably
		 * determine if sleeping here is permitted -- see in_atomic().
		 */

		udelay(1);
		*flags = spin_lock_irqsave(&report_lock);
		if (timeout-- < 0) {
			/*
			 * Abort. Reset @other_info->task to NULL, since it
			 * appears the other thread is still going to consume
			 * it. It will result in no verbose info printed for
			 * this task.
			 */
			other_info->task = NULL;
			break;
		}
		/*
		 * If invalid, or @ptr nor @current matches, then @other_info
		 * has been consumed and we may continue. If not, retry.
		 */
	} while (other_info->ai.size && other_info->ai.ptr == ai->ptr &&
		 other_info->task == get_current_thread());
}

/* Populate @other_info; requires that the provided @other_info not in use. */
static void prepare_report_producer(unsigned long *flags,
				    const struct access_info *ai,
				    struct other_info *other_info)
{
	*flags = spin_lock_irqsave(&report_lock);

	/*
	 * The same @other_infos entry cannot be used concurrently, because
	 * there is a one-to-one mapping to watchpoint slots (@watchpoints in
	 * core.c), and a watchpoint is only released for reuse after reporting
	 * is done by the consumer of @other_info. Therefore, it is impossible
	 * for another concurrent prepare_report_producer() to set the same
	 * @other_info, and are guaranteed exclusivity for the @other_infos
	 * entry pointed to by @other_info.
	 *
	 * To check this property holds, size should never be non-zero here,
	 * because every consumer of struct other_info resets size to 0 in
	 * release_report().
	 */
	//WARN_ON(other_info->ai.size);

	other_info->ai = *ai;
	other_info->num_stack_entries = stack_trace_get(__builtin_frame_address(0), other_info->stack_entries, NUM_STACK_ENTRIES);

#if 0
	if (IS_ENABLED(CONFIG_KCSAN_VERBOSE))
		set_other_info_task_blocking(flags, ai, other_info);
#endif

	spin_unlock_irqrestore(&report_lock, *flags);
}

/* Awaits producer to fill @other_info and then returns. */
static bool prepare_report_consumer(unsigned long *flags,
				    const struct access_info *ai,
				    struct other_info *other_info)
{

	*flags = spin_lock_irqsave(&report_lock);
	while (!other_info->ai.size) { /* Await valid @other_info. */
		spin_unlock_irqrestore(&report_lock, *flags);
		cpu_relax();
		*flags = spin_lock_irqsave(&report_lock);
	}

	/* Should always have a matching access based on watchpoint encoding. */
	if (!matching_access((unsigned long)other_info->ai.ptr & WATCHPOINT_ADDR_MASK, other_info->ai.size,
				     (unsigned long)ai->ptr & WATCHPOINT_ADDR_MASK, ai->size))
		goto discard;

	if (!matching_access((unsigned long)other_info->ai.ptr, other_info->ai.size,
			     (unsigned long)ai->ptr, ai->size)) {
		/*
		 * If the actual accesses to not match, this was a false
		 * positive due to watchpoint encoding.
		 */
		atomic_long_inc(&kcsan_counters[KCSAN_COUNTER_ENCODING_FALSE_POSITIVES]);
		goto discard;
	}

	return true;

discard:
	release_report(flags, other_info);
	return false;
}

static struct access_info prepare_access_info(const volatile void *ptr, size_t size,
					      int access_type, unsigned long ip)
{
	return (struct access_info) {
		.ptr		= ptr,
		.size		= size,
		.access_type	= access_type,
		.task_pid	= get_current_thread() && !is_in_interrupt() ? get_current_thread()->id : -1,
		.cpu_id		= get_cpu_nr(),
		/* Only replace stack entry with @ip if scoped access. */
		.ip		= (access_type & KCSAN_ACCESS_SCOPED) ? ip : 0,
	};
}

void kcsan_disable_current(void);
void kcsan_enable_current(void);

#define lockdep_off()
#define lockdep_on()


void kcsan_report_set_info(const volatile void *ptr, size_t size, int access_type,
			   unsigned long ip, int watchpoint_idx)
{
	const struct access_info ai = prepare_access_info(ptr, size, access_type, ip);
	unsigned long flags;

	kcsan_disable_current();
	lockdep_off(); /* See kcsan_report_known_origin(). */

	prepare_report_producer(&flags, &ai, &other_infos[watchpoint_idx]);

	lockdep_on();
	kcsan_enable_current();
}

void kcsan_report_known_origin(const volatile void *ptr, size_t size, int access_type,
			       unsigned long ip, enum kcsan_value_change value_change,
			       int watchpoint_idx, u64 old, u64 new, u64 mask)
{
	const struct access_info ai = prepare_access_info(ptr, size, access_type, ip);
	struct other_info *other_info = &other_infos[watchpoint_idx];
	unsigned long flags = 0;

	kcsan_disable_current();
	/*
	 * Because we may generate reports when we're in scheduler code, the use
	 * of printk() could deadlock. Until such time that all printing code
	 * called in print_report() is scheduler-safe, accept the risk, and just
	 * get our message out. As such, also disable lockdep to hide the
	 * warning, and avoid disabling lockdep for the rest of the kernel.
	 */
	lockdep_off();

	if (!prepare_report_consumer(&flags, &ai, other_info))
		goto out;
	/*
	 * Never report if value_change is FALSE, only when it is
	 * either TRUE or MAYBE. In case of MAYBE, further filtering may
	 * be done once we know the full stack trace in print_report().
	 */
	if (value_change != KCSAN_VALUE_CHANGE_FALSE)
		print_report(value_change, &ai, other_info, old, new, mask);

	release_report(&flags, other_info);
out:
	lockdep_on();
	kcsan_enable_current();
}

void kcsan_report_unknown_origin(const volatile void *ptr, size_t size, int access_type,
				 unsigned long ip, u64 old, u64 new, u64 mask)
{
	const struct access_info ai = prepare_access_info(ptr, size, access_type, ip);
	unsigned long flags;

	kcsan_disable_current();
	lockdep_off(); /* See kcsan_report_known_origin(). */

	flags = spin_lock_irqsave(&report_lock);
	print_report(KCSAN_VALUE_CHANGE_TRUE, &ai, NULL, old, new, mask);
	spin_unlock_irqrestore(&report_lock, flags);

	lockdep_on();
	kcsan_enable_current();
}
