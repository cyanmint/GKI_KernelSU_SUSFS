// SPDX-License-Identifier: GPL-2.0
/*
 * lkm4ctr_log - implementation of the ring buffer declared in
 * common/lkm4ctr_log.h. See that header for the full rationale.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/ktime.h>

#include "lkm4ctr_log.h"

struct lkm4ctr_log_entry {
	u64	timestamp_ns;
	u8	level;
	char	tag[LKM4CTR_LOG_TAG_MAX];
	char	msg[LKM4CTR_LOG_LINE_MAX];
};

static struct lkm4ctr_log_entry lkm4ctr_log_ring[LKM4CTR_LOG_CAPACITY];
/* Index the *next* write will land on; entries wrap once full. */
static unsigned int lkm4ctr_log_head;
/* Total entries ever written, capped at LKM4CTR_LOG_CAPACITY for iteration. */
static unsigned int lkm4ctr_log_count;
static DEFINE_SPINLOCK(lkm4ctr_log_lock);

static const char *lkm4ctr_log_level_str(u8 level)
{
	switch (level) {
	case LKM4CTR_LOG_WARN:
		return "warn";
	case LKM4CTR_LOG_ERR:
		return "error";
	default:
		return "info";
	}
}

void lkm4ctr_log(const char *tag, enum lkm4ctr_log_level level,
		  const char *fmt, ...)
{
	struct lkm4ctr_log_entry *e;
	unsigned long flags;
	va_list args;

	spin_lock_irqsave(&lkm4ctr_log_lock, flags);
	e = &lkm4ctr_log_ring[lkm4ctr_log_head];
	e->timestamp_ns = ktime_get_boottime_ns();
	e->level = (u8)level;
	strscpy(e->tag, tag ? tag : "lkm4ctr", sizeof(e->tag));

	va_start(args, fmt);
	vsnprintf(e->msg, sizeof(e->msg), fmt, args);
	va_end(args);

	lkm4ctr_log_head = (lkm4ctr_log_head + 1) % LKM4CTR_LOG_CAPACITY;
	if (lkm4ctr_log_count < LKM4CTR_LOG_CAPACITY)
		lkm4ctr_log_count++;
	spin_unlock_irqrestore(&lkm4ctr_log_lock, flags);

#ifdef CONFIG_LKM4CTR_LOG_DMESG
	switch (level) {
	case LKM4CTR_LOG_WARN:
		pr_warn("lkm4ctr: %s: %s\n", tag ? tag : "lkm4ctr", e->msg);
		break;
	case LKM4CTR_LOG_ERR:
		pr_err("lkm4ctr: %s: %s\n", tag ? tag : "lkm4ctr", e->msg);
		break;
	default:
		pr_info("lkm4ctr: %s: %s\n", tag ? tag : "lkm4ctr", e->msg);
		break;
	}
#endif
}
EXPORT_SYMBOL_GPL(lkm4ctr_log);

/*
 * lkm4ctr_log_snprintf() - see header. Takes a private snapshot copy of the
 * matching entries under the spinlock (fixed-size on-stack-sized array
 * would be too large; use a bounded local scan instead: walk the ring
 * directly under the lock, formatting straight into @buf). This holds the
 * lock across vscnprintf() into @buf, which is acceptable here: @buf is a
 * plain kernel buffer (no page faults), and the whole scan is at most
 * LKM4CTR_LOG_CAPACITY iterations of fixed-size formatting -- bounded,
 * non-blocking work, matching what other spinlock-protected fixed-size
 * kernel ring buffers (e.g. printk's own logbuf) already do.
 */
size_t lkm4ctr_log_snprintf(const char *tag, char *buf, size_t buflen)
{
	unsigned long flags;
	unsigned int i, start, n;
	size_t pos = 0;

	spin_lock_irqsave(&lkm4ctr_log_lock, flags);
	n = lkm4ctr_log_count;
	start = (lkm4ctr_log_head + LKM4CTR_LOG_CAPACITY - n) % LKM4CTR_LOG_CAPACITY;

	for (i = 0; i < n; i++) {
		struct lkm4ctr_log_entry *e =
			&lkm4ctr_log_ring[(start + i) % LKM4CTR_LOG_CAPACITY];

		if (tag && strcmp(tag, e->tag))
			continue;

		pos += scnprintf(buf + pos, pos < buflen ? buflen - pos : 0,
				  "[%llu.%06llu][%s][%s] %s\n",
				  e->timestamp_ns / NSEC_PER_SEC,
				  (e->timestamp_ns % NSEC_PER_SEC) / NSEC_PER_USEC,
				  e->tag, lkm4ctr_log_level_str(e->level), e->msg);
	}
	spin_unlock_irqrestore(&lkm4ctr_log_lock, flags);

	return pos;
}
EXPORT_SYMBOL_GPL(lkm4ctr_log_snprintf);
