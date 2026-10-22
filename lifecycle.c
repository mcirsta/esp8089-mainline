// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/visibility.h>
#include "esp_pub.h"
#include "esp_sip.h"

/* A stop check followed by an unlocked queue_work() is insufficient:
 * quiesce can cancel the work between the check and publication. Serialize
 * both with STOP, then cancel without this lock held. Only a fully drained
 * sip_prepare_boot() may reopen the transport.
 */
void sip_stop(struct esp_sip *sip)
{
	spin_lock_bh(&sip->state_lock);
	atomic_set(&sip->state, SIP_STOP);
	spin_unlock_bh(&sip->state_lock);
}
EXPORT_SYMBOL_IF_KUNIT(sip_stop);

bool sip_set_state(struct esp_sip *sip, enum sip_state state)
{
	bool running;

	spin_lock_bh(&sip->state_lock);
	running = atomic_read(&sip->state) != SIP_STOP;
	if (running)
		atomic_set(&sip->state, state);
	spin_unlock_bh(&sip->state_lock);
	return running;
}
EXPORT_SYMBOL_IF_KUNIT(sip_set_state);

bool sip_queue_work(struct esp_sip *sip, struct work_struct *work)
{
	bool queued = false;

	spin_lock_bh(&sip->state_lock);
	if (atomic_read(&sip->state) != SIP_STOP)
		queued = queue_work(sip->epub->esp_wkq, work);
	spin_unlock_bh(&sip->state_lock);
	return queued;
}
EXPORT_SYMBOL_IF_KUNIT(sip_queue_work);

void sip_queue_report_poll(struct esp_sip *sip)
{
	spin_lock_bh(&sip->state_lock);
	if (atomic_read(&sip->state) != SIP_STOP)
		queue_delayed_work(sip->epub->esp_wkq, &sip->reports.poll,
				   msecs_to_jiffies(10));
	spin_unlock_bh(&sip->state_lock);
}
EXPORT_SYMBOL_IF_KUNIT(sip_queue_report_poll);

void sip_rearm_credit_timer(struct esp_sip *sip)
{
	spin_lock_bh(&sip->state_lock);
	if (atomic_read(&sip->state) != SIP_STOP)
		mod_timer(&sip->credit_timer, jiffies + msecs_to_jiffies(2000));
	spin_unlock_bh(&sip->state_lock);
}
EXPORT_SYMBOL_IF_KUNIT(sip_rearm_credit_timer);
