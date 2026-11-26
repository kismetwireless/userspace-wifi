/*
    This file is part of Kismet

    Kismet is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    Kismet is distributed in the hope that it will be useful,
      but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Kismet; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

/* cfg80211's wiphy work, as net/wireless/core.c: queued items run one at a time on a worker
 * that holds the wiphy mutex, so they're serialized with every other wiphy-locked path.
 * Cancel and flush are called with the mutex held; flush runs pending items in the caller. */

#include "local.h"

static struct ieee80211_local *kc_wiphy_local(struct wiphy *wiphy) {
    return wiphy_priv(wiphy);
}

static void kc_wiphy_work_run(struct work_struct *work) {
    struct ieee80211_local *local = container_of(work, struct ieee80211_local, wiphy_work);
    struct wiphy *wiphy = local->hw.wiphy;
    struct wiphy_work *wk;

    wiphy_lock(wiphy);

    spin_lock(&local->wiphy_work_lock);
    wk = list_first_entry_or_null(&local->wiphy_work_list, struct wiphy_work, entry);
    if (wk) {
        list_del_init(&wk->entry);
        if (!list_empty(&local->wiphy_work_list))
            queue_work(local->wiphy_wq, &local->wiphy_work);
    }
    spin_unlock(&local->wiphy_work_lock);

    if (wk)
        wk->func(wiphy, wk);

    wiphy_unlock(wiphy);
}

int kc_wiphy_work_init(struct ieee80211_local *local, const char *name) {
    spin_lock_init(&local->wiphy_work_lock);
    INIT_LIST_HEAD(&local->wiphy_work_list);
    INIT_WORK(&local->wiphy_work, kc_wiphy_work_run);
    local->wiphy_wq = alloc_ordered_workqueue("%s-wiphy", 0, name);
    return local->wiphy_wq ? 0 : -ENOMEM;
}

void kc_wiphy_work_exit(struct ieee80211_local *local) {
    if (local->wiphy_wq == NULL)
        return;
    cancel_work_sync(&local->wiphy_work);
    destroy_workqueue(local->wiphy_wq);
    local->wiphy_wq = NULL;
}

void wiphy_work_queue(struct wiphy *wiphy, struct wiphy_work *work) {
    struct ieee80211_local *local = kc_wiphy_local(wiphy);

    spin_lock(&local->wiphy_work_lock);
    if (list_empty(&work->entry))
        list_add_tail(&work->entry, &local->wiphy_work_list);
    spin_unlock(&local->wiphy_work_lock);

    queue_work(local->wiphy_wq, &local->wiphy_work);
}

void wiphy_work_cancel(struct wiphy *wiphy, struct wiphy_work *work) {
    struct ieee80211_local *local = kc_wiphy_local(wiphy);

    spin_lock(&local->wiphy_work_lock);
    if (!list_empty(&work->entry))
        list_del_init(&work->entry);
    spin_unlock(&local->wiphy_work_lock);
}

/* Runs queued items in order until work has run, or all of them for NULL */
void wiphy_work_flush(struct wiphy *wiphy, struct wiphy_work *work) {
    struct ieee80211_local *local = kc_wiphy_local(wiphy);
    struct wiphy_work *wk;

    for (;;) {
        spin_lock(&local->wiphy_work_lock);
        if (work && list_empty(&work->entry)) {
            spin_unlock(&local->wiphy_work_lock);
            return;
        }
        wk = list_first_entry_or_null(&local->wiphy_work_list, struct wiphy_work, entry);
        if (wk == NULL) {
            spin_unlock(&local->wiphy_work_lock);
            return;
        }
        list_del_init(&wk->entry);
        spin_unlock(&local->wiphy_work_lock);

        wk->func(wiphy, wk);

        if (wk == work)
            return;
    }
}

void wiphy_delayed_work_timer(struct timer_list *t) {
    struct wiphy_delayed_work *dwork = timer_container_of(dwork, t, timer);

    wiphy_work_queue(dwork->wiphy, &dwork->work);
}

void wiphy_delayed_work_queue(struct wiphy *wiphy, struct wiphy_delayed_work *dwork,
        unsigned long delay) {
    if (!delay) {
        timer_delete(&dwork->timer);
        wiphy_work_queue(wiphy, &dwork->work);
        return;
    }

    dwork->wiphy = wiphy;
    mod_timer(&dwork->timer, jiffies + delay);
}

void wiphy_delayed_work_cancel(struct wiphy *wiphy, struct wiphy_delayed_work *dwork) {
    timer_delete_sync(&dwork->timer);
    wiphy_work_cancel(wiphy, &dwork->work);
}

void wiphy_delayed_work_flush(struct wiphy *wiphy, struct wiphy_delayed_work *dwork) {
    timer_delete_sync(&dwork->timer);
    wiphy_work_flush(wiphy, &dwork->work);
}
