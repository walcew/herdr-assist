#include "activity_log.h"

#include <string.h>

#ifndef ACTIVITY_HOST_TEST
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
#define ACT_LOCK()   taskENTER_CRITICAL(&s_mux)
#define ACT_UNLOCK() taskEXIT_CRITICAL(&s_mux)
#else
#define ACT_LOCK()
#define ACT_UNLOCK()
#endif

static activity_event_t s_ring[ACTIVITY_CAP];
static int s_head;      /* próximo índice de escrita */
static int s_count;     /* eventos válidos (<= ACTIVITY_CAP) */

static int eq(const char *s, const char *w)
{
    return s && strcmp(s, w) == 0;
}

activity_type_t activity_classify(const char *old_status, const char *new_status)
{
    const char *o = old_status ? old_status : "";
    const char *n = new_status ? new_status : "";

    if (eq(n, "blocked") && !eq(o, "blocked")) {
        return ACT_BLOCKED;
    }
    if (eq(n, "working") && !eq(o, "working")) {
        return ACT_STARTED;
    }
    if ((eq(o, "working") || eq(o, "blocked")) && (eq(n, "idle") || n[0] == '\0')) {
        return ACT_DONE;
    }
    return ACT_NONE;
}

void activity_log_init(void)
{
    ACT_LOCK();
    s_head = 0;
    s_count = 0;
    ACT_UNLOCK();
}

static void copy_str(char *dst, size_t cap, const char *src)
{
    if (!src) {
        src = "";
    }
    size_t i = 0;
    for (; i + 1 < cap && src[i]; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

void activity_log_note(activity_type_t type, uint8_t host, const char *agent,
                       const char *project, uint32_t dur, uint32_t epoch)
{
    if (type == ACT_NONE) {
        return;
    }
    ACT_LOCK();
    activity_event_t *e = &s_ring[s_head];
    e->epoch = epoch;
    e->dur = dur;
    e->type = (uint8_t)type;
    e->host = host;
    copy_str(e->agent, ACTIVITY_AGENT_LEN, agent);
    copy_str(e->project, ACTIVITY_PROJ_LEN, project);
    s_head = (s_head + 1) % ACTIVITY_CAP;
    if (s_count < ACTIVITY_CAP) {
        s_count++;
    }
    ACT_UNLOCK();
}

int activity_log_get(activity_event_t *out, int max)
{
    if (max <= 0) {
        return 0;
    }
    ACT_LOCK();
    int n = s_count < max ? s_count : max;
    int idx = s_head - 1;
    for (int i = 0; i < n; i++) {
        if (idx < 0) {
            idx += ACTIVITY_CAP;
        }
        out[i] = s_ring[idx];
        idx--;
    }
    ACT_UNLOCK();
    return n;
}

activity_summary_t activity_summarize(const activity_event_t *ev, int n, uint32_t since)
{
    activity_summary_t s = {0, 0, 0, 0};
    for (int i = 0; i < n; i++) {
        if (ev[i].epoch < since) {
            continue;
        }
        switch (ev[i].type) {
        case ACT_STARTED:
            s.started++;
            break;
        case ACT_BLOCKED:
            s.blocked++;
            break;
        case ACT_DONE:
            s.done++;
            s.active_secs += ev[i].dur;
            break;
        default:
            break;
        }
    }
    return s;
}
