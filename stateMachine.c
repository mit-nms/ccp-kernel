#include <net/tcp.h>

#include "tcp_ccp.h"

static void doSetCwndAbs(
    struct tcp_sock *tp, 
    uint32_t cwnd
) {
    // translate cwnd value back into packets
    cwnd /= tp->mss_cache;
    printk(KERN_INFO "cwnd %d -> %d (mss %d)\n", tp->snd_cwnd, cwnd, tp->mss_cache);
    tp->snd_cwnd = cwnd;
}

static void doSetRateAbs(
    struct sock *sk,
    uint32_t rate
) {
    struct ccp *ca = inet_csk_ca(sk);

    printk(KERN_INFO "rate (Bytes/s) -> %u\n", rate);
    ca->rate = rate;
    ccp_set_pacing_rate(sk);
}

static void doSetRateRel(
    struct sock *sk,
    uint32_t factor
) {
    struct ccp *ca = inet_csk_ca(sk);

    // factor is * 100
    uint64_t newrate = ca->rate * factor;
    do_div(newrate, 100);
    printk(KERN_INFO "rate -> %llu\n", newrate);
    ca->rate = (u32) newrate;
    ccp_set_pacing_rate(sk);
}

static void doReport(
    struct sock *sk
) {
    struct ccp *cpl = inet_csk_ca(sk);
    struct ccp_measurement mmt = cpl->mmt;

    pr_info("sending report\n");
    nl_send_measurement(cpl->ccp_index, mmt);

    cpl->mmt.rtt = 0;
    cpl->mmt.rin = 0;
    cpl->mmt.rout = 0;
    cpl->mmt.loss = 0;
}

static void doWaitAbs(
    struct sock *sk,
    uint32_t wait_us
) {
    struct ccp *cpl = inet_csk_ca(sk);
    pr_info("waiting %u us\n", wait_us);
    cpl->next_event_time = tcp_time_stamp + usecs_to_jiffies(wait_us);
}

static void doWaitRel(
    struct sock *sk,
    uint32_t rtt_factor
) {
    struct ccp *cpl = inet_csk_ca(sk);
    u64 rtt_us = cpl->mmt.rtt;
    u64 wait_us = rtt_factor * rtt_us;
    do_div(wait_us, 100);
    pr_info("waiting %llu us (%u/100 rtts) (rtt = %llu us)\n", wait_us, rtt_factor, rtt_us);
    cpl->next_event_time = tcp_time_stamp + usecs_to_jiffies(wait_us);
}

void sendStateMachine(struct sock *sk) {
    int ok;
    struct ccp *cpl = inet_csk_ca(sk);
    struct tcp_sock *tp = tcp_sk(sk);
    struct PatternEvent ev;
    if (cpl->numPatternEvents == 0) {
        // try contacting the CCP again
        // index of pointer back to this sock for IPC callback
        // first ack to expect
        ok = nl_send_conn_create(cpl->ccp_index, tp->snd_una);
        if (ok < 0) {
            pr_info("failed to send create message: %d", ok);
        }

        return;
    }

    if (unlikely(after(tcp_time_stamp, cpl->next_event_time))) {
        cpl->currPatternEvent = (cpl->currPatternEvent + 1) % cpl->numPatternEvents;
        pr_info("curr pattern event: %d\n", cpl->currPatternEvent);
    } else {
        return;
    }

    ev = cpl->pattern[cpl->currPatternEvent];
    switch (ev.type) {
    case SETRATEABS:
        doSetRateAbs(sk, ev.val);
        break;
    case SETCWNDABS:
        doSetCwndAbs(tp, ev.val);
        break;
    case SETRATEREL:
        doSetRateRel(sk, ev.val);
        break;
    case WAITREL:
        doWaitRel(sk, ev.val);
        break;
    case WAITABS:
        doWaitAbs(sk, ev.val);
        break;
    case REPORT:
        doReport(sk);
        break;
    }
}

static void log_sequence(struct PatternEvent *seq, int numEvents) {
    size_t  i;
    struct PatternEvent ev;
    pr_info("installed pattern:\n");
    for (i = 0; i < numEvents; i++) {
        ev = seq[i];
        switch (ev.type) {
        case SETRATEABS:
            pr_info("[ev %lu] set rate %u\n", i, ev.val);
            break;
        case SETCWNDABS:
            pr_info("[ev %lu] set cwnd %d\n", i, ev.val);
            break;
        case SETRATEREL:
            pr_info("[ev %lu] set rate factor %u/100\n", i, ev.val);
            break;
        case WAITREL:
            pr_info("[ev %lu] wait rtts %d/100\n", i, ev.val);
            break;
        case WAITABS:
            pr_info("[ev %lu] wait %d us\n", i, ev.val);
            break;
        case REPORT:
            pr_info("[ev %lu] send report\n", i);
            break;
        }
    }
}

void installPattern(
    struct sock *sk,
    int numEvents,
    struct PatternEvent *seq
) {
    struct ccp *ca = inet_csk_ca(sk);

    log_sequence(seq, numEvents);

    ca->numPatternEvents = numEvents;
    ca->currPatternEvent = numEvents - 1;
    ca->next_event_time = tcp_time_stamp;
    ca->pattern = seq;

    sendStateMachine(sk);
}
