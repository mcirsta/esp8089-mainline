// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>
#include <linux/kthread.h>
#include "esp_pub.h"
#include "esp_sip.h"

struct lifecycle_fixture {
	struct esp_pub epub;
	struct esp_sip sip;
	struct completion entered, release;
	atomic_t calls;
	int command_result;
	bool queued, transitioned;
};

static void lifecycle_work(struct work_struct *work)
{
	struct esp_pub *epub = container_of(work, struct esp_pub, tx_work);
	struct lifecycle_fixture *f = container_of(epub, struct lifecycle_fixture, epub);

	atomic_inc(&f->calls);
}

static void lifecycle_poll(struct work_struct *work) {}
static void lifecycle_timer(struct timer_list *timer) {}

static void lifecycle_cleanup(void *data)
{
	struct lifecycle_fixture *f = data;

	sip_stop(&f->sip);
	cancel_work_sync(&f->epub.tx_work);
	cancel_delayed_work_sync(&f->sip.reports.poll);
	timer_shutdown_sync(&f->sip.credit_timer);
	skb_queue_purge(&f->epub.ctrlq);
	destroy_workqueue(f->epub.esp_wkq);
}

static int lifecycle_init(struct kunit *test)
{
	struct lifecycle_fixture *f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);

	if (!f)
		return -ENOMEM;
	f->epub.esp_wkq = alloc_ordered_workqueue("esp8089-lifecycle-test", 0);
	if (!f->epub.esp_wkq)
		return -ENOMEM;
	f->epub.sip = &f->sip;
	f->sip.epub = &f->epub;
	spin_lock_init(&f->sip.state_lock);
	atomic_set(&f->sip.state, SIP_RUN);
	skb_queue_head_init(&f->epub.ctrlq);
	INIT_WORK(&f->epub.tx_work, lifecycle_work);
	INIT_DELAYED_WORK(&f->sip.reports.poll, lifecycle_poll);
	timer_setup(&f->sip.credit_timer, lifecycle_timer, 0);
	init_completion(&f->entered);
	init_completion(&f->release);
	test->priv = f;
	return kunit_add_action_or_reset(test, lifecycle_cleanup, f);
}

static void lifecycle_stop_is_terminal(struct kunit *test)
{
	struct lifecycle_fixture *f = test->priv;
	enum sip_state state;

	sip_stop(&f->sip);
	for (state = SIP_INIT; state < SIP_STOP; state++) {
		KUNIT_EXPECT_FALSE(test, sip_set_state(&f->sip, state));
		KUNIT_EXPECT_EQ(test, atomic_read(&f->sip.state), SIP_STOP);
	}
}

static void lifecycle_accepted_work_drains(struct kunit *test)
{
	struct lifecycle_fixture *f = test->priv;

	KUNIT_ASSERT_TRUE(test, sip_queue_work(&f->sip, &f->epub.tx_work));
	flush_workqueue(f->epub.esp_wkq);
	KUNIT_EXPECT_EQ(test, atomic_read(&f->calls), 1);
	sip_stop(&f->sip);
	cancel_work_sync(&f->epub.tx_work);
	KUNIT_EXPECT_FALSE(test, sip_queue_work(&f->sip, &f->epub.tx_work));
	flush_workqueue(f->epub.esp_wkq);
	KUNIT_EXPECT_EQ(test, atomic_read(&f->calls), 1);
}

static int lifecycle_late_producer(void *data)
{
	struct lifecycle_fixture *f = data;
	struct sk_buff *skb = alloc_skb(sizeof(struct sip_hdr), GFP_KERNEL);

	/* A callback has already begun and allocated its command when STOP
	 * cancels the consumer. It must not republish work or revive boot state.
	 */
	complete(&f->entered);
	wait_for_completion(&f->release);
	f->command_result = skb ? sip_cmd_enqueue(&f->sip, skb, ENQUEUE_PRIOR_TAIL) : -ENOMEM;
	f->queued = sip_queue_work(&f->sip, &f->epub.tx_work);
	sip_queue_report_poll(&f->sip);
	sip_rearm_credit_timer(&f->sip);
	f->transitioned = sip_set_state(&f->sip, SIP_WAIT_BOOTUP);
	while (!kthread_should_stop())
		schedule_timeout_interruptible(1);
	return 0;
}

static void lifecycle_late_producer_stays_stopped(struct kunit *test)
{
	struct lifecycle_fixture *f = test->priv;
	struct task_struct *producer;
	unsigned long entered;

	producer = kthread_run(lifecycle_late_producer, f, "esp8089-late-test");
	KUNIT_ASSERT_FALSE(test, IS_ERR(producer));
	entered = wait_for_completion_timeout(&f->entered, HZ);
	sip_stop(&f->sip);
	cancel_work_sync(&f->epub.tx_work);
	complete(&f->release);
	kthread_stop(producer);
	KUNIT_ASSERT_NE(test, entered, 0UL);
	flush_workqueue(f->epub.esp_wkq);
	KUNIT_EXPECT_EQ(test, f->command_result, -ESHUTDOWN);
	KUNIT_EXPECT_FALSE(test, f->queued);
	KUNIT_EXPECT_FALSE(test, f->transitioned);
	KUNIT_EXPECT_TRUE(test, skb_queue_empty(&f->epub.ctrlq));
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&f->sip.reports.poll));
	KUNIT_EXPECT_FALSE(test, timer_pending(&f->sip.credit_timer));
	KUNIT_EXPECT_EQ(test, atomic_read(&f->calls), 0);
	KUNIT_EXPECT_EQ(test, atomic_read(&f->sip.state), SIP_STOP);
}

static struct kunit_case lifecycle_cases[] = {
	KUNIT_CASE(lifecycle_stop_is_terminal),
	KUNIT_CASE(lifecycle_accepted_work_drains),
	KUNIT_CASE(lifecycle_late_producer_stays_stopped),
	{}
};

static struct kunit_suite lifecycle_suite = {
	.name = "esp8089-lifecycle",
	.init = lifecycle_init,
	.test_cases = lifecycle_cases,
};
kunit_test_suite(lifecycle_suite);
