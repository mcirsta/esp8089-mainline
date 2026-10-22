// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>
#include <linux/kthread.h>
#include "esp_pub.h"

struct ownership_fixture {
	struct ieee80211_hw hw;
	struct esp_pub epub;
	struct ieee80211_sta *sta;
	struct completion entered, reader_done, remover_done;
	atomic_t reader_left;
	bool reader_found, saw_unpublished, returned_early;
	int reader_cpu, remover_cpu, index;
};

static const struct cfg80211_ops ownership_cfg_ops;

static void ownership_cleanup(void *data)
{
	struct ownership_fixture *f = data;

	wiphy_free(f->hw.wiphy);
}

static int ownership_init(struct kunit *test)
{
	struct ownership_fixture *f;
	struct esp_node *node;

	f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	if (!f)
		return -ENOMEM;
	f->sta = kunit_kzalloc(test, sizeof(*f->sta) + sizeof(*node), GFP_KERNEL);
	if (!f->sta)
		return -ENOMEM;
	f->hw.wiphy = wiphy_new(&ownership_cfg_ops, 0);
	if (!f->hw.wiphy)
		return -ENOMEM;
	f->hw.priv = &f->epub;
	f->epub.hw = &f->hw;
	spin_lock_init(&f->epub.tx_ampdu_lock);
	spin_lock_init(&f->epub.rx_ampdu_lock);
	f->sta->addr[0] = 0x02;
	f->sta->addr[5] = 0x31;
	node = (void *)f->sta->drv_priv;
	node->sta = f->sta;
	f->epub.enodes[0] = node;
	f->epub.enodes_map = BIT(0);
	f->epub.enodes_maps[0] = BIT(0);
	f->epub.rxampdu_map = BIT(0);
	f->epub.rxampdu_node[0] = node;
	init_completion(&f->entered);
	init_completion(&f->reader_done);
	init_completion(&f->remover_done);
	test->priv = f;
	return kunit_add_action_or_reset(test, ownership_cleanup, f);
}

static void ownership_detach_unpublishes(struct kunit *test)
{
	struct ownership_fixture *f = test->priv;
	struct esp_node *node = (void *)f->sta->drv_priv;
	int index;

	wiphy_lock(f->hw.wiphy);
	index = esp_node_detach(&f->hw, 0, f->sta);
	wiphy_unlock(f->hw.wiphy);
	KUNIT_EXPECT_EQ(test, index, 0);
	KUNIT_EXPECT_EQ(test, f->epub.enodes_map, 0);
	KUNIT_EXPECT_EQ(test, f->epub.enodes_maps[0], 0);
	KUNIT_EXPECT_PTR_EQ(test, f->epub.enodes[0], NULL);
	KUNIT_EXPECT_EQ(test, f->epub.rxampdu_map, 0);
	KUNIT_EXPECT_PTR_EQ(test, f->epub.rxampdu_node[0], NULL);
	KUNIT_EXPECT_PTR_EQ(test, node->sta, f->sta);
}

static int ownership_reader(void *data)
{
	struct ownership_fixture *f = data;
	struct esp_node *node;
	u64 deadline;

	rcu_read_lock();
	f->reader_cpu = get_cpu();
	put_cpu();
	node = esp_get_node_by_addr(&f->epub, f->sta->addr);
	f->reader_found = node == (void *)f->sta->drv_priv;
	complete(&f->entered);
	/* No sleeping in an RCU section on the non-preemptible test kernel.
	 * The other CPU can unpublish immediately. Bound any harness failure.
	 */
	deadline = ktime_get_ns() + 250 * NSEC_PER_MSEC;
	while (ktime_get_ns() < deadline) {
		if (!esp_get_node_by_addr(&f->epub, f->sta->addr)) {
			f->saw_unpublished = true;
			break;
		}
		cpu_relax();
	}
	/* Let removal try to return while our previously acquired pointer is
	 * still in use. The fixture keeps memory allocated even on failure.
	 */
	deadline = ktime_get_ns() + 20 * NSEC_PER_MSEC;
	while (ktime_get_ns() < deadline)
		cpu_relax();
	if (node)
		f->reader_found &= node->sta == f->sta;
	atomic_set(&f->reader_left, 1);
	rcu_read_unlock();
	complete(&f->reader_done);
	while (!kthread_should_stop())
		schedule_timeout_interruptible(1);
	return 0;
}

static int ownership_remover(void *data)
{
	struct ownership_fixture *f = data;

	wait_for_completion(&f->entered);
	f->remover_cpu = get_cpu();
	put_cpu();
	wiphy_lock(f->hw.wiphy);
	f->index = esp_node_detach(&f->hw, 0, f->sta);
	f->returned_early = !atomic_read(&f->reader_left);
	wiphy_unlock(f->hw.wiphy);
	complete(&f->remover_done);
	while (!kthread_should_stop())
		schedule_timeout_interruptible(1);
	return 0;
}

static void ownership_detach_waits_for_reader(struct kunit *test)
{
	struct ownership_fixture *f = test->priv;
	struct task_struct *reader, *remover;
	unsigned int cpu1, cpu2;

	if (num_online_cpus() < 2) {
		kunit_skip(test, "requires two online CPUs");
		return;
	}
	cpu1 = cpumask_first(cpu_online_mask);
	cpu2 = cpumask_next(cpu1, cpu_online_mask);
	reader = kthread_create(ownership_reader, f, "esp-owner-reader");
	KUNIT_ASSERT_FALSE(test, IS_ERR(reader));
	kthread_bind(reader, cpu1);
	remover = kthread_create(ownership_remover, f, "esp-owner-remove");
	if (IS_ERR(remover)) {
		kthread_stop(reader);
		KUNIT_FAIL(test, "cannot create remover");
		return;
	}
	kthread_bind(remover, cpu2);
	wake_up_process(remover);
	wake_up_process(reader);
	wait_for_completion(&f->reader_done);
	wait_for_completion(&f->remover_done);
	kthread_stop(reader);
	kthread_stop(remover);
	KUNIT_EXPECT_EQ(test, f->reader_cpu, (int)cpu1);
	KUNIT_EXPECT_EQ(test, f->remover_cpu, (int)cpu2);
	KUNIT_EXPECT_TRUE(test, f->reader_found);
	KUNIT_EXPECT_TRUE(test, f->saw_unpublished);
	KUNIT_EXPECT_EQ(test, f->index, 0);
	KUNIT_EXPECT_FALSE(test, f->returned_early);
	kunit_info(test, "reader CPU%u; removal CPU%u; removal waited=%d\n",
		   cpu1, cpu2, !f->returned_early);
}

static struct kunit_case ownership_cases[] = {
	KUNIT_CASE(ownership_detach_unpublishes),
	KUNIT_CASE(ownership_detach_waits_for_reader),
	{}
};

static struct kunit_suite ownership_suite = {
	.name = "esp8089-ownership",
	.init = ownership_init,
	.test_cases = ownership_cases,
};
kunit_test_suite(ownership_suite);
