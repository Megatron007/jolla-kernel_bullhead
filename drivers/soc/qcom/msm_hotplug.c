/*
 * MSM Hotplug Driver
 *
 * Copyright (c) 2010-2014, The Linux Foundation. All rights reserved.
 * Copyright (c) 2013-2014, Fluxi <linflux@arcor.de>
 * Copyright (c) 2013-2015, Pranav Vashi <neobuddy89@gmail.com>
 * Copyright (c) 2016, jollaman999 <admin@jollaman999.com> / Adaptive for big.LITTLE
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/cpu.h>
#include <linux/init.h>
#include <linux/workqueue.h>
#include <linux/sched.h>
#include <linux/platform_device.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/cpufreq.h>
#include <linux/mutex.h>
#include <linux/math64.h>
#include <linux/kernel_stat.h>
#include <linux/fb.h>
#include <linux/tick.h>
#include <linux/hrtimer.h>
#include <asm-generic/cputime.h>
#include <linux/msm_hotplug.h>

// Change this value for your device
#define LITTLE_CORES	4
#define BIG_CORES	2

#define MSM_HOTPLUG			"msm_hotplug"
#define HOTPLUG_ENABLED			0
#define DEFAULT_UPDATE_RATE		200
#define START_DELAY			20000
#define DEFAULT_HISTORY_SIZE		10
#define DEFAULT_DOWN_LOCK_DUR		1000
#define DEFAULT_MIN_CPUS_ONLINE		2
#define DEFAULT_MAX_CPUS_ONLINE		LITTLE_CORES
#define DEFAULT_FAST_LANE_LOAD		99
#define DEFAULT_BIG_CORE_UP_DELAY	1200
#define DEFAULT_MAX_CPUS_ONLINE_SUSP	2

unsigned int msm_enabled = HOTPLUG_ENABLED;

struct notifier_block msm_hotplug_fb_notif;

/* HACK: Prevent big cluster turned off when changing governor settings. */
bool prevent_big_off = false;
EXPORT_SYMBOL(prevent_big_off);

// Use for msm_hotplug_resume_timeout
#define HOTPLUG_TIMEOUT			2000
static bool timeout_enabled = false;
static s64 pre_time;
bool msm_hotplug_scr_suspended = false;
bool msm_hotplug_fingerprint_called = false;

static bool big_up_called_before = false;

static void msm_hotplug_suspend(void);

static unsigned int debug = 0;
module_param_named(debug_mask, debug, uint, 0644);

#define dprintk(msg...)		\
do { 				\
	if (debug)		\
		pr_info(msg);	\
} while (0)

static struct cpu_hotplug {
	unsigned int suspended;
	unsigned int max_cpus_online_susp;
	unsigned int target_cpus;
	unsigned int min_cpus_online;
	unsigned int max_cpus_online;
	unsigned int offline_load;
	unsigned int down_lock_dur;
	unsigned int fast_lane_load;
	unsigned int big_core_up_delay;
	struct mutex msm_hotplug_mutex;
	struct notifier_block notif;
} hotplug = {
	.min_cpus_online = DEFAULT_MIN_CPUS_ONLINE,
	.max_cpus_online = DEFAULT_MAX_CPUS_ONLINE,
	.suspended = 0,
	.max_cpus_online_susp = DEFAULT_MAX_CPUS_ONLINE_SUSP,
	.down_lock_dur = DEFAULT_DOWN_LOCK_DUR,
	.fast_lane_load = DEFAULT_FAST_LANE_LOAD,
	.big_core_up_delay = DEFAULT_BIG_CORE_UP_DELAY
};

static struct workqueue_struct *hotplug_wq;
static struct delayed_work hotplug_work;

static unsigned int default_update_rates[] = { DEFAULT_UPDATE_RATE };
static bool big_core_up_ready_checked = false;
static s64 big_core_up_ready_time = 0;

static struct cpu_stats {
	unsigned int *update_rates;
	int nupdate_rates;
	spinlock_t update_rates_lock;
	unsigned int *load_hist;
	unsigned int hist_size;
	unsigned int hist_cnt;
	unsigned int min_cpus;
	unsigned int total_cpus;
	unsigned int online_cpus;
	unsigned int cur_avg_load;
	unsigned int cur_max_load;
	struct mutex stats_mutex;
} stats = {
	.update_rates = default_update_rates,
	.nupdate_rates = ARRAY_SIZE(default_update_rates),
	.hist_size = DEFAULT_HISTORY_SIZE,
	.min_cpus = 1,
	.total_cpus = LITTLE_CORES
};

struct down_lock {
	unsigned int locked;
	struct delayed_work lock_rem;
};

static DEFINE_PER_CPU(struct down_lock, lock_info);

struct cpu_load_data {
	u64 prev_cpu_idle;
	u64 prev_cpu_wall;
	unsigned int avg_load_maxfreq;
	unsigned int cur_load_maxfreq;
	unsigned int samples;
	unsigned int window_size;
	cpumask_var_t related_cpus;
};

static DEFINE_PER_CPU(struct cpu_load_data, cpuload);

static bool io_is_busy;

static int num_online_little_cpus(void)
{
	int cpu;
	unsigned int online_cpus = 0;

	for (cpu = 0; cpu < LITTLE_CORES; cpu++) {
		if (cpu_online(cpu))
			online_cpus++;
	}

	return online_cpus;
}

static int num_online_big_cpus(void)
{
	int cpu;
	unsigned int online_cpus = 0;

	for (cpu = LITTLE_CORES; cpu < LITTLE_CORES + BIG_CORES; cpu++) {
		if (cpu_online(cpu))
			online_cpus++;
	}

	return online_cpus;
}

static int update_average_load(unsigned int cpu)
{
	int ret;
	unsigned int idle_time, wall_time;
	unsigned int cur_load, load_max_freq;
	u64 cur_wall_time, cur_idle_time;
	struct cpu_load_data *pcpu = &per_cpu(cpuload, cpu);
	struct cpufreq_policy policy;

	ret = cpufreq_get_policy(&policy, cpu);
	if (ret)
		return -EINVAL;

	cur_idle_time = get_cpu_idle_time(cpu, &cur_wall_time, io_is_busy);

	wall_time = (unsigned int) (cur_wall_time - pcpu->prev_cpu_wall);
	pcpu->prev_cpu_wall = cur_wall_time;

	idle_time = (unsigned int) (cur_idle_time - pcpu->prev_cpu_idle);
	pcpu->prev_cpu_idle = cur_idle_time;

	if (unlikely(!wall_time || wall_time < idle_time))
		return 0;

	cur_load = 100 * (wall_time - idle_time) / wall_time;

	/* Calculate the scaled load across cpu */
	load_max_freq = (cur_load * policy.cur) / policy.max;

	if (!pcpu->avg_load_maxfreq) {
		/* This is the first sample in this window */
		pcpu->avg_load_maxfreq = load_max_freq;
		pcpu->window_size = wall_time;
	} else {
		/*
		 * The is already a sample available in this window.
		 * Compute weighted average with prev entry, so that
		 * we get the precise weighted load.
		 */
		pcpu->avg_load_maxfreq =
			((pcpu->avg_load_maxfreq * pcpu->window_size) +
			(load_max_freq * wall_time)) /
			(wall_time + pcpu->window_size);

		pcpu->window_size += wall_time;
	}

	return 0;
}

static unsigned int load_at_max_freq(void)
{
	int cpu;
	unsigned int total_load = 0, max_load = 0;
	struct cpu_load_data *pcpu;

	for (cpu = 0; cpu < LITTLE_CORES; cpu++) {
		if (!cpu_online(cpu))
			continue;
		pcpu = &per_cpu(cpuload, cpu);
		update_average_load(cpu);
		total_load += pcpu->avg_load_maxfreq;
		pcpu->cur_load_maxfreq = pcpu->avg_load_maxfreq;
		max_load = max(max_load, pcpu->avg_load_maxfreq);
		pcpu->avg_load_maxfreq = 0;
	}
	stats.cur_max_load = max_load;

	return total_load;
}

static void update_load_stats(void)
{
	unsigned int i, j;
	unsigned int load = 0;

	mutex_lock(&stats.stats_mutex);
	stats.online_cpus = num_online_little_cpus();

	if (stats.hist_size > 1) {
		stats.load_hist[stats.hist_cnt] = load_at_max_freq();
	} else {
		stats.cur_avg_load = load_at_max_freq();
		mutex_unlock(&stats.stats_mutex);
		return;
	}

	for (i = 0, j = stats.hist_cnt; i < stats.hist_size; i++, j--) {
		load += stats.load_hist[j];

		if (j == 0)
			j = stats.hist_size;
	}

	if (++stats.hist_cnt == stats.hist_size)
		stats.hist_cnt = 0;

	stats.cur_avg_load = load / stats.hist_size;
	mutex_unlock(&stats.stats_mutex);
}

struct loads_tbl {
	unsigned int up_threshold;
	unsigned int down_threshold;
};

#define LOAD_SCALE(u, d)     \
{                            \
	.up_threshold = u,   \
	.down_threshold = d, \
}

static struct loads_tbl loads[] = {
	LOAD_SCALE(400, 0),
	LOAD_SCALE(65, 0),
	LOAD_SCALE(120, 50),
	LOAD_SCALE(190, 100),
	LOAD_SCALE(410, 170),
	LOAD_SCALE(0, 0),
};

static void apply_down_lock(unsigned int cpu)
{
	struct down_lock *dl = &per_cpu(lock_info, cpu);

	dl->locked = 1;
	queue_delayed_work_on(0, hotplug_wq, &dl->lock_rem,
			      msecs_to_jiffies(hotplug.down_lock_dur));
}

static void remove_down_lock(struct work_struct *work)
{
	struct down_lock *dl = container_of(work, struct down_lock,
					    lock_rem.work);
	dl->locked = 0;
}

static int check_down_lock(unsigned int cpu)
{
	struct down_lock *dl = &per_cpu(lock_info, cpu);

	return dl->locked;
}

static int get_lowest_load_cpu(void)
{
	int cpu, lowest_cpu = 0;
	unsigned int lowest_load = UINT_MAX;
	unsigned int cpu_load[stats.total_cpus];
	unsigned int proj_load;
	struct cpu_load_data *pcpu;

	// Skip cpu 0
	for (cpu = 1; cpu < LITTLE_CORES; cpu++) {
		if (!cpu_online(cpu))
			continue;
		pcpu = &per_cpu(cpuload, cpu);
		cpu_load[cpu] = pcpu->cur_load_maxfreq;
		if (cpu_load[cpu] < lowest_load) {
			lowest_load = cpu_load[cpu];
			lowest_cpu = cpu;
		}
	}

	proj_load = stats.cur_avg_load - lowest_load;
	if (proj_load > loads[stats.online_cpus - 1].up_threshold)
		return -EPERM;

	if (hotplug.offline_load && lowest_load >= hotplug.offline_load)
		return -EPERM;

	return lowest_cpu;
}

static void big_up(unsigned int target_big)
{
	int cpu;

	if (big_up_called_before)
		goto skip_dealy;

	big_up_called_before = true;

	if (!big_core_up_ready_checked) {
		big_core_up_ready_checked = true;
		big_core_up_ready_time = ktime_to_ms(ktime_get());
		return;
	}

	if (ktime_to_ms(ktime_get()) - big_core_up_ready_time > hotplug.big_core_up_delay) {
skip_dealy:
		for (cpu = LITTLE_CORES; cpu < LITTLE_CORES + BIG_CORES; cpu++) {
			if (cpu_online(cpu))
				continue;
			if (target_big <= num_online_big_cpus())
				break;
			cpu_up(cpu);
			apply_down_lock(cpu);
		}

		big_core_up_ready_checked = false;
		return;
	}
}

static void big_down(unsigned int target_big)
{
	int cpu;
	unsigned int target_big_off; // how many big cores to offline

	big_up_called_before = false;

	target_big_off = BIG_CORES - target_big;

	for (cpu = LITTLE_CORES; cpu < LITTLE_CORES + BIG_CORES; cpu++) {
		/* HACK: Prevent big cluster turned off when changing governor settings. */
		if (prevent_big_off && cpu == LITTLE_CORES) {
			// Turn on first of big cores
			if (!cpu_online(LITTLE_CORES))
				cpu_up(LITTLE_CORES);
			continue;
		}

		if (!cpu_online(cpu))
			continue;
		if (check_down_lock(cpu))
			continue;
		if (target_big_off <= BIG_CORES - num_online_big_cpus())
			break;
		cpu_down(cpu);
	}
}

static void big_updown(void)
{
	unsigned int online_little, online_big;
	unsigned int target_big;

	online_little = num_online_little_cpus();
	online_big = num_online_big_cpus();

	// If LITTLE_CORES is 4 and BIG_CORES is 2.
	// online_little == 4 -> Turn on all of big cores. (2)
	// online_little == 3 -> Turn on half of big cores. (1)
	// else               -> Turn off all of big cores. (0)
	if (online_little == LITTLE_CORES)
		target_big = BIG_CORES;
	else if (online_little == LITTLE_CORES - 1)
		target_big = BIG_CORES / 2;
	else
		target_big = 0;

	if (online_big != target_big) {
		if (target_big > online_big)
			big_up(target_big);
		else if (target_big < online_big)
			big_down(target_big);
	}
}

static void little_up(void)
{
	int cpu;

	// Skip cpu 0
	for (cpu = 1; cpu < LITTLE_CORES; cpu++) {
		if (cpu_online(cpu))
			continue;
		if (hotplug.target_cpus <= num_online_little_cpus())
			break;
		cpu_up(cpu);
		apply_down_lock(cpu);
	}
}

static void little_down(void)
{
	int cpu, lowest_cpu;

	// Skip cpu 0
	for (cpu = 1; cpu < LITTLE_CORES; cpu++) {
		if (!cpu_online(cpu))
			continue;
		lowest_cpu = get_lowest_load_cpu();
		if (lowest_cpu > 0 && lowest_cpu <= stats.total_cpus) {
			if (check_down_lock(lowest_cpu))
				break;
			cpu_down(lowest_cpu);
		}
		if (hotplug.target_cpus >= num_online_little_cpus())
			break;
	}
}

static void online_cpu(unsigned int target)
{
	unsigned int online_little;

	online_little = num_online_little_cpus();

	/* 
	 * Do not online more CPUs if max_cpus_online reached 
	 * and cancel online task if target already achieved.
	 */
	if (target <= online_little ||
		online_little >= hotplug.max_cpus_online)
		return;

	hotplug.target_cpus = target;
	little_up();
}

static void offline_cpu(unsigned int target)
{
	unsigned int online_little;

	online_little = num_online_little_cpus();

	/* 
	 * Do not offline more CPUs if min_cpus_online reached
	 * and cancel offline task if target already achieved.
	 */
	if (target >= online_little ||
		online_little <= hotplug.min_cpus_online)
		return;

	hotplug.target_cpus = target;
	little_down();
}

static unsigned int load_to_update_rate(unsigned int load)
{
	int i, ret;
	unsigned long flags;

	spin_lock_irqsave(&stats.update_rates_lock, flags);

	for (i = 0; i < stats.nupdate_rates - 1 &&
			load >= stats.update_rates[i+1]; i += 2)
		;

	ret = stats.update_rates[i];
	spin_unlock_irqrestore(&stats.update_rates_lock, flags);
	return ret;
}

static void reschedule_hotplug_work(void)
{
	int delay = load_to_update_rate(stats.cur_avg_load);
	queue_delayed_work_on(0, hotplug_wq, &hotplug_work,
			      msecs_to_jiffies(delay));
}

static void msm_hotplug_work(struct work_struct *work)
{
	unsigned int i, target = 0;

	if (!msm_enabled)
		return;

	if (hotplug.suspended) {
		dprintk("%s: suspended.\n", MSM_HOTPLUG);
		return;
	}

	/* HACK: Prevent big cluster turned off when changing governor settings. */
	// Turn on first of big cores
	if (prevent_big_off) {
		if (!cpu_online(LITTLE_CORES))
			cpu_up(LITTLE_CORES);
	}

	if (timeout_enabled) {
		if (ktime_to_ms(ktime_get()) - pre_time > HOTPLUG_TIMEOUT) {
			if (msm_hotplug_scr_suspended) {
				msm_hotplug_suspend();
				return;
			}

			timeout_enabled = false;
			msm_hotplug_fingerprint_called = false;
		}
		goto reschedule;
	}

	update_load_stats();

	if (stats.cur_max_load >= hotplug.fast_lane_load) {
		/* Enter the fast lane */
		online_cpu(hotplug.max_cpus_online);
		goto reschedule;
	}

	/* If number of cpus locked, break out early */
	if (hotplug.min_cpus_online == stats.total_cpus) {
		if (stats.online_cpus != hotplug.min_cpus_online)
			online_cpu(hotplug.min_cpus_online);
		goto reschedule;
	} else if (hotplug.max_cpus_online == stats.min_cpus) {
		if (stats.online_cpus != hotplug.max_cpus_online)
			offline_cpu(hotplug.max_cpus_online);
		goto reschedule;
	}

	for (i = stats.min_cpus; loads[i].up_threshold; i++) {
		if (stats.cur_avg_load <= loads[i].up_threshold
		    && stats.cur_avg_load > loads[i].down_threshold) {
			target = i;
			break;
		}
	}

	if (target > hotplug.max_cpus_online)
		target = hotplug.max_cpus_online;
	else if (target < hotplug.min_cpus_online)
		target = hotplug.min_cpus_online;

	if (stats.online_cpus != target) {
		if (target > stats.online_cpus)
			online_cpu(target);
		else if (target < stats.online_cpus)
			offline_cpu(target);
	}

reschedule:
	big_updown();

	dprintk("%s: cur_avg_load: %3u online_cpus: %u target: %u\n", MSM_HOTPLUG,
		stats.cur_avg_load, stats.online_cpus, target);
	reschedule_hotplug_work();
}

static void msm_hotplug_suspend(void)
{
	int cpu;

	mutex_lock(&hotplug.msm_hotplug_mutex);
	hotplug.suspended = 1;
	mutex_unlock(&hotplug.msm_hotplug_mutex);

	/* Flush hotplug workqueue */
	if (timeout_enabled) {
		timeout_enabled = false;
		msm_hotplug_fingerprint_called = false;
	} else {
		flush_workqueue(hotplug_wq);
		cancel_delayed_work_sync(&hotplug_work);
	}

	// Turn off little cores but remain max_cpus_online_susp
	// Skip cpu 0
	for (cpu = 1; cpu < LITTLE_CORES; cpu++) {
		if (hotplug.max_cpus_online_susp == num_online_little_cpus())
			break;
		cpu_down(cpu);
	}

	// Turn off all of big cores
	for (cpu = LITTLE_CORES; cpu < LITTLE_CORES + BIG_CORES; cpu++)
		cpu_down(cpu);

	pr_info("%s: suspended.\n", MSM_HOTPLUG);

	return;
}

static void msm_hotplug_resume(void)
{
	int cpu, required_reschedule = 0, required_wakeup = 0;

	if (hotplug.suspended) {
		mutex_lock(&hotplug.msm_hotplug_mutex);
		hotplug.suspended = 0;
		mutex_unlock(&hotplug.msm_hotplug_mutex);
		required_wakeup = 1;
		/* Initiate hotplug work if it was cancelled */
		required_reschedule = 1;
		INIT_DELAYED_WORK(&hotplug_work, msm_hotplug_work);
	}

	if (required_wakeup) {
		/* Fire up all CPUs */
		for_each_cpu_not(cpu, cpu_online_mask) {
			if (cpu == 0)
				continue;
			cpu_up(cpu);
			if (!timeout_enabled)
				apply_down_lock(cpu);
		}
	}

	/* Resume hotplug workqueue if required */
	if (required_reschedule)
		reschedule_hotplug_work();

	pr_info("%s: resumed.\n", MSM_HOTPLUG);

	return;
}

void msm_hotplug_resume_timeout(void)
{
	if (timeout_enabled || !hotplug.suspended)
		return;

	timeout_enabled = true;
	pre_time = ktime_to_ms(ktime_get());
	msm_hotplug_resume();
}
EXPORT_SYMBOL(msm_hotplug_resume_timeout);

static int msm_hotplug_start(int start_immediately)
{
	int cpu, ret = 0;
	struct down_lock *dl;

	hotplug_wq =
	    alloc_workqueue("msm_hotplug_wq", WQ_HIGHPRI | WQ_FREEZABLE, 0);
	if (!hotplug_wq) {
		pr_err("%s: Failed to allocate hotplug workqueue\n",
		       MSM_HOTPLUG);
		ret = -ENOMEM;
		goto err_out;
	}

	stats.load_hist = kmalloc(sizeof(stats.hist_size), GFP_KERNEL);
	if (!stats.load_hist) {
		pr_err("%s: Failed to allocate memory\n", MSM_HOTPLUG);
		ret = -ENOMEM;
		goto err_dev;
	}

	mutex_init(&stats.stats_mutex);
	mutex_init(&hotplug.msm_hotplug_mutex);

	INIT_DELAYED_WORK(&hotplug_work, msm_hotplug_work);
	for_each_possible_cpu(cpu) {
		dl = &per_cpu(lock_info, cpu);
		INIT_DELAYED_WORK(&dl->lock_rem, remove_down_lock);
	}

	/* Fire up all CPUs */
	for_each_cpu_not(cpu, cpu_online_mask) {
		if (cpu == 0)
			continue;
		cpu_up(cpu);
		apply_down_lock(cpu);
	}

	if (start_immediately)
		queue_delayed_work_on(0, hotplug_wq, &hotplug_work, 0);
	else
		queue_delayed_work_on(0, hotplug_wq, &hotplug_work, START_DELAY);

	return ret;
err_dev:
	destroy_workqueue(hotplug_wq);
err_out:
	msm_enabled = 0;
	return ret;
}

static void msm_hotplug_stop(void)
{
	int cpu;
	struct down_lock *dl;

	flush_workqueue(hotplug_wq);
	for_each_possible_cpu(cpu) {
		dl = &per_cpu(lock_info, cpu);
		cancel_delayed_work_sync(&dl->lock_rem);
	}
	cancel_delayed_work_sync(&hotplug_work);

	mutex_destroy(&hotplug.msm_hotplug_mutex);
	mutex_destroy(&stats.stats_mutex);
	kfree(stats.load_hist);

	hotplug.notif.notifier_call = NULL;

	destroy_workqueue(hotplug_wq);

	/* Fire up all CPUs */
	for_each_cpu_not(cpu, cpu_online_mask) {
		if (cpu == 0)
			continue;
		cpu_up(cpu);
	}
}

static unsigned int *get_tokenized_data(const char *buf, int *num_tokens)
{
	const char *cp;
	int i;
	int ntokens = 1;
	int *tokenized_data;
	int err = -EINVAL;

	cp = buf;
	while ((cp = strpbrk(cp + 1, " :")))
		ntokens++;

	if (!(ntokens & 0x1))
		goto err;

	tokenized_data = kmalloc(ntokens * sizeof(int), GFP_KERNEL);
	if (!tokenized_data) {
		err = -ENOMEM;
		goto err;
	}

	cp = buf;
	i = 0;
	while (i < ntokens) {
		if (sscanf(cp, "%d", &tokenized_data[i++]) != 1)
			goto err_kfree;

		cp = strpbrk(cp, " :");
		if (!cp)
			break;
		cp++;
	}

	if (i != ntokens)
		goto err_kfree;

	*num_tokens = ntokens;
	return tokenized_data;

err_kfree:
	kfree(tokenized_data);
err:
	return ERR_PTR(err);
}

/************************** sysfs interface ************************/

static ssize_t show_enable_hotplug(struct device *dev,
				   struct device_attribute *msm_hotplug_attrs,
				   char *buf)
{
	return sprintf(buf, "%u\n", msm_enabled);
}

static ssize_t store_enable_hotplug(struct device *dev,
				    struct device_attribute *msm_hotplug_attrs,
				    const char *buf, size_t count)
{
	int ret;
	unsigned int val;

	ret = sscanf(buf, "%u", &val);
	if (ret != 1 || val < 0 || val > 1)
		return -EINVAL;

	if (val == msm_enabled)
		return count;

	msm_enabled = val;

	if (msm_enabled)
		msm_hotplug_start(1);
	else
		msm_hotplug_stop();

	return count;
}

static ssize_t show_down_lock_duration(struct device *dev,
				       struct device_attribute
				       *msm_hotplug_attrs, char *buf)
{
	return sprintf(buf, "%u\n", hotplug.down_lock_dur);
}

static ssize_t store_down_lock_duration(struct device *dev,
					struct device_attribute
					*msm_hotplug_attrs, const char *buf,
					size_t count)
{
	int ret;
	unsigned int val;

	ret = sscanf(buf, "%u", &val);
	if (ret != 1)
		return -EINVAL;

	hotplug.down_lock_dur = val;

	return count;
}

static ssize_t show_update_rates(struct device *dev,
				struct device_attribute *msm_hotplug_attrs,
				char *buf)
{
	int i;
	ssize_t ret = 0;
	unsigned long flags;

	spin_lock_irqsave(&stats.update_rates_lock, flags);

	for (i = 0; i < stats.nupdate_rates; i++)
		ret += sprintf(buf + ret, "%u%s", stats.update_rates[i],
			       i & 0x1 ? ":" : " ");

	sprintf(buf + ret - 1, "\n");
	spin_unlock_irqrestore(&stats.update_rates_lock, flags);
	return ret;
}

static ssize_t store_update_rates(struct device *dev,
				 struct device_attribute *msm_hotplug_attrs,
				 const char *buf, size_t count)
{
	int ntokens;
	unsigned int *new_update_rates = NULL;
	unsigned long flags;

	new_update_rates = get_tokenized_data(buf, &ntokens);
	if (IS_ERR(new_update_rates))
		return PTR_RET(new_update_rates);

	spin_lock_irqsave(&stats.update_rates_lock, flags);
	if (stats.update_rates != default_update_rates)
		kfree(stats.update_rates);
	stats.update_rates = new_update_rates;
	stats.nupdate_rates = ntokens;
	spin_unlock_irqrestore(&stats.update_rates_lock, flags);
	return count;
}

static ssize_t show_load_levels(struct device *dev,
				struct device_attribute *msm_hotplug_attrs,
				char *buf)
{
	int i, len = 0;

	if (!buf)
		return -EINVAL;

	for (i = 0; loads[i].up_threshold; i++) {
		len += sprintf(buf + len, "%u ", i);
		len += sprintf(buf + len, "%u ", loads[i].up_threshold);
		len += sprintf(buf + len, "%u\n", loads[i].down_threshold);
	}

	return len;
}

static ssize_t store_load_levels(struct device *dev,
				 struct device_attribute *msm_hotplug_attrs,
				 const char *buf, size_t count)
{
	int ret;
	unsigned int val[3];

	ret = sscanf(buf, "%u %u %u", &val[0], &val[1], &val[2]);
	if (ret != ARRAY_SIZE(val) || val[2] > val[1])
		return -EINVAL;

	loads[val[0]].up_threshold = val[1];
	loads[val[0]].down_threshold = val[2];

	return count;
}

static ssize_t show_min_cpus_online(struct device *dev,
				    struct device_attribute *msm_hotplug_attrs,
				    char *buf)
{
	return sprintf(buf, "%u\n", hotplug.min_cpus_online);
}

static ssize_t store_min_cpus_online(struct device *dev,
				     struct device_attribute *msm_hotplug_attrs,
				     const char *buf, size_t count)
{
	int ret;
	unsigned int val;

	ret = sscanf(buf, "%u", &val);
	if (ret != 1 || val < 1 || val > stats.total_cpus)
		return -EINVAL;

	if (hotplug.max_cpus_online < val)
		hotplug.max_cpus_online = val;

	hotplug.min_cpus_online = val;

	return count;
}

static ssize_t show_max_cpus_online(struct device *dev,
				    struct device_attribute *msm_hotplug_attrs,
				    char *buf)
{
	return sprintf(buf, "%u\n",hotplug.max_cpus_online);
}

static ssize_t store_max_cpus_online(struct device *dev,
				     struct device_attribute *msm_hotplug_attrs,
				     const char *buf, size_t count)
{
	int ret;
	unsigned int val;

	ret = sscanf(buf, "%u", &val);
	if (ret != 1 || val < 1 || val > stats.total_cpus)
		return -EINVAL;

	if (hotplug.min_cpus_online > val)
		hotplug.min_cpus_online = val;

	hotplug.max_cpus_online = val;

	return count;
}

static ssize_t show_max_cpus_online_susp(struct device *dev,
				    struct device_attribute *msm_hotplug_attrs,
				    char *buf)
{
	return sprintf(buf, "%u\n",hotplug.max_cpus_online_susp);
}

static ssize_t store_max_cpus_online_susp(struct device *dev,
				     struct device_attribute *msm_hotplug_attrs,
				     const char *buf, size_t count)
{
	int ret;
	unsigned int val;

	ret = sscanf(buf, "%u", &val);
	if (ret != 1 || val < 1 || val > stats.total_cpus)
		return -EINVAL;

	hotplug.max_cpus_online_susp = val;

	return count;
}

static ssize_t show_offline_load(struct device *dev,
				 struct device_attribute *msm_hotplug_attrs,
				 char *buf)
{
	return sprintf(buf, "%u\n", hotplug.offline_load);
}

static ssize_t store_offline_load(struct device *dev,
				  struct device_attribute *msm_hotplug_attrs,
				  const char *buf, size_t count)
{
	int ret;
	unsigned int val;

	ret = sscanf(buf, "%u", &val);
	if (ret != 1)
		return -EINVAL;

	hotplug.offline_load = val;

	return count;
}

static ssize_t show_fast_lane_load(struct device *dev,
				   struct device_attribute *msm_hotplug_attrs,
				   char *buf)
{
	return sprintf(buf, "%u\n", hotplug.fast_lane_load);
}

static ssize_t store_fast_lane_load(struct device *dev,
				    struct device_attribute *msm_hotplug_attrs,
				    const char *buf, size_t count)
{
	int ret;
	unsigned int val;

	ret = sscanf(buf, "%u", &val);
	if (ret != 1)
		return -EINVAL;

	hotplug.fast_lane_load = val;

	return count;
}

static ssize_t show_big_core_up_delay(struct device *dev,
				   struct device_attribute *msm_hotplug_attrs,
				   char *buf)
{
	return sprintf(buf, "%u\n", hotplug.big_core_up_delay);
}

static ssize_t store_big_core_up_delay(struct device *dev,
				    struct device_attribute *msm_hotplug_attrs,
				    const char *buf, size_t count)
{
	int ret;
	unsigned int val;

	ret = sscanf(buf, "%u", &val);
	if (ret != 1)
		return -EINVAL;

	hotplug.big_core_up_delay = val;

	return count;
}

static ssize_t show_io_is_busy(struct device *dev,
				   struct device_attribute *msm_hotplug_attrs,
				   char *buf)
{
	return sprintf(buf, "%u\n", io_is_busy);
}

static ssize_t store_io_is_busy(struct device *dev,
				    struct device_attribute *msm_hotplug_attrs,
				    const char *buf, size_t count)
{
	int ret;
	unsigned int val;

	ret = sscanf(buf, "%u", &val);
	if (ret != 1 || val < 0 || val > 1)
		return -EINVAL;

	io_is_busy = val ? true : false;

	return count;
}

static ssize_t show_current_load(struct device *dev,
				 struct device_attribute *msm_hotplug_attrs,
				 char *buf)
{
	return sprintf(buf, "%u\n", stats.cur_avg_load);
}

static DEVICE_ATTR(msm_enabled, 644, show_enable_hotplug, store_enable_hotplug);
static DEVICE_ATTR(down_lock_duration, 644, show_down_lock_duration,
		   store_down_lock_duration);
static DEVICE_ATTR(update_rates, 644, show_update_rates, store_update_rates);
static DEVICE_ATTR(load_levels, 644, show_load_levels, store_load_levels);
static DEVICE_ATTR(min_cpus_online, 644, show_min_cpus_online,
		   store_min_cpus_online);
static DEVICE_ATTR(max_cpus_online, 644, show_max_cpus_online,
		   store_max_cpus_online);
static DEVICE_ATTR(max_cpus_online_susp, 644, show_max_cpus_online_susp,
		   store_max_cpus_online_susp);
static DEVICE_ATTR(offline_load, 644, show_offline_load, store_offline_load);
static DEVICE_ATTR(fast_lane_load, 644, show_fast_lane_load,
		   store_fast_lane_load);
static DEVICE_ATTR(big_core_up_delay, 644, show_big_core_up_delay,
		   store_big_core_up_delay);
static DEVICE_ATTR(io_is_busy, 644, show_io_is_busy, store_io_is_busy);
static DEVICE_ATTR(current_load, 444, show_current_load, NULL);

static struct attribute *msm_hotplug_attrs[] = {
	&dev_attr_msm_enabled.attr,
	&dev_attr_down_lock_duration.attr,
	&dev_attr_update_rates.attr,
	&dev_attr_load_levels.attr,
	&dev_attr_min_cpus_online.attr,
	&dev_attr_max_cpus_online.attr,
	&dev_attr_max_cpus_online_susp.attr,
	&dev_attr_offline_load.attr,
	&dev_attr_fast_lane_load.attr,
	&dev_attr_big_core_up_delay.attr,
	&dev_attr_io_is_busy.attr,
	&dev_attr_current_load.attr,
	NULL,
};

static struct attribute_group attr_group = {
	.attrs = msm_hotplug_attrs,
};

/************************** sysfs end ************************/

static int msm_hotplug_probe(struct platform_device *pdev)
{
	int ret = 0;
	struct kobject *module_kobj;

	module_kobj = kset_find_obj(module_kset, MSM_HOTPLUG);
	if (!module_kobj) {
		pr_err("%s: Cannot find kobject for module\n", MSM_HOTPLUG);
		goto err_dev;
	}

	ret = sysfs_create_group(module_kobj, &attr_group);
	if (ret) {
		pr_err("%s: Failed to create sysfs: %d\n", MSM_HOTPLUG, ret);
		goto err_dev;
	}

	if (msm_enabled) {
		ret = msm_hotplug_start(0);
		if (ret != 0)
			goto err_dev;
	}

	return ret;
err_dev:
	module_kobj = NULL;
	return ret;
}

static struct platform_device msm_hotplug_device = {
	.name = MSM_HOTPLUG,
	.id = -1,
};

static int msm_hotplug_remove(struct platform_device *pdev)
{
	if (msm_enabled)
		msm_hotplug_stop();

	return 0;
}

static struct platform_driver msm_hotplug_driver = {
	.probe = msm_hotplug_probe,
	.remove = msm_hotplug_remove,
	.driver = {
		.name = MSM_HOTPLUG,
		.owner = THIS_MODULE,
	},
};

static int msm_hotplug_fb_notifier_callback(struct notifier_block *self,
				unsigned long event, void *data)
{
	struct fb_event *evdata = data;
	int *blank;

	if (!msm_enabled)
		return 0;

	if (event == FB_EVENT_BLANK) {
		blank = evdata->data;

		switch (*blank) {
		case FB_BLANK_UNBLANK:
			msm_hotplug_scr_suspended = false;
			msm_hotplug_resume();
			break;
		case FB_BLANK_POWERDOWN:
			prevent_big_off = false;
			msm_hotplug_scr_suspended = true;
			msm_hotplug_suspend();
			break;
		}
	}

	return 0;
}

struct notifier_block msm_hotplug_fb_notif = {
	.notifier_call = msm_hotplug_fb_notifier_callback,
};

static int __init msm_hotplug_init(void)
{
	int ret;

	if (LITTLE_CORES + BIG_CORES != NR_CPUS) {
		pr_info("%s: Little cores and big cores are not match with this device: %d\n",
										MSM_HOTPLUG, ret);
		return -EPERM;
	}

	ret = fb_register_client(&msm_hotplug_fb_notif);
	if (ret) {
		pr_info("%s: FB register failed: %d\n", MSM_HOTPLUG, ret);
		return ret;
	}

	ret = platform_driver_register(&msm_hotplug_driver);
	if (ret) {
		pr_info("%s: Driver register failed: %d\n", MSM_HOTPLUG, ret);
		return ret;
	}

	ret = platform_device_register(&msm_hotplug_device);
	if (ret) {
		pr_info("%s: Device register failed: %d\n", MSM_HOTPLUG, ret);
		return ret;
	}

	pr_info("%s: Device init\n", MSM_HOTPLUG);

	return ret;
}

static void __exit msm_hotplug_exit(void)
{
	platform_device_unregister(&msm_hotplug_device);
	platform_driver_unregister(&msm_hotplug_driver);
	fb_unregister_client(&msm_hotplug_fb_notif);
}

late_initcall(msm_hotplug_init);
module_exit(msm_hotplug_exit);

MODULE_AUTHOR("jollaman999 <admin@jollaman999.com>");
MODULE_DESCRIPTION("MSM Hotplug Driver for big.LITTLE");
MODULE_LICENSE("GPLv2");
