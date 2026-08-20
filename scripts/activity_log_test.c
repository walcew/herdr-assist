/* Teste de host de activity_log (classify + buffer em anel). Sem ESP/FreeRTOS:
 * define ACTIVITY_HOST_TEST (stub dos locks) e inclui o .c direto. MSVC local. */
#define ACTIVITY_HOST_TEST
#include "../src/activity_log.c"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK_EQ(expr, expected)                                            \
    do {                                                                    \
        long _g = (long)(expr), _e = (long)(expected);                      \
        if (_g != _e) {                                                     \
            printf("FALHOU: %s => %ld (esperado %ld)\n", #expr, _g, _e);    \
            failures++;                                                     \
        }                                                                   \
    } while (0)

#define CHECK_STR(expr, expected)                                           \
    do {                                                                    \
        const char *_g = (expr);                                           \
        if (strcmp(_g, expected) != 0) {                                   \
            printf("FALHOU: %s => \"%s\" (esperado \"%s\")\n", #expr, _g, expected); \
            failures++;                                                     \
        }                                                                   \
    } while (0)

int main(void)
{
    /* --- classify --- */
    CHECK_EQ(activity_classify("idle", "working"),    ACT_STARTED);
    CHECK_EQ(activity_classify("working", "blocked"), ACT_BLOCKED);
    CHECK_EQ(activity_classify("working", "idle"),    ACT_DONE);
    CHECK_EQ(activity_classify("blocked", "idle"),    ACT_DONE);
    CHECK_EQ(activity_classify("", "working"),        ACT_STARTED);   /* novo agente */
    CHECK_EQ(activity_classify("", "blocked"),        ACT_BLOCKED);
    CHECK_EQ(activity_classify("working", ""),        ACT_DONE);      /* sumiu working */
    CHECK_EQ(activity_classify("blocked", ""),        ACT_DONE);
    CHECK_EQ(activity_classify("idle", ""),           ACT_NONE);
    CHECK_EQ(activity_classify("idle", "idle"),       ACT_NONE);
    CHECK_EQ(activity_classify("working", "working"), ACT_NONE);
    CHECK_EQ(activity_classify("blocked", "working"), ACT_STARTED);   /* retomou */
    CHECK_EQ(activity_classify(NULL, "working"),      ACT_STARTED);   /* NULL == "" */

    /* --- anel: ordem mais-recente-primeiro --- */
    activity_log_init();
    activity_log_note(ACT_STARTED, 0, "a", "p1", 0, 10);
    activity_log_note(ACT_BLOCKED, 0, "b", "p2", 0, 20);
    activity_log_note(ACT_DONE,    1, "c", "p3", 42, 30);
    activity_event_t buf[ACTIVITY_CAP];
    CHECK_EQ(activity_log_get(buf, ACTIVITY_CAP), 3);
    CHECK_EQ(buf[0].epoch, 30);  CHECK_STR(buf[0].agent, "c");  CHECK_EQ(buf[0].dur, 42);
    CHECK_EQ(buf[1].epoch, 20);  CHECK_STR(buf[1].agent, "b");
    CHECK_EQ(buf[2].epoch, 10);  CHECK_STR(buf[2].agent, "a");

    /* max menor que o total: só os mais recentes */
    CHECK_EQ(activity_log_get(buf, 2), 2);
    CHECK_EQ(buf[0].epoch, 30);
    CHECK_EQ(buf[1].epoch, 20);

    /* --- anel: estouro descarta os mais antigos --- */
    activity_log_init();
    for (uint32_t i = 0; i <= ACTIVITY_CAP + 3; i++) {
        activity_log_note(ACT_STARTED, 0, "x", "p", 0, i);
    }
    int n = activity_log_get(buf, ACTIVITY_CAP);
    CHECK_EQ(n, ACTIVITY_CAP);
    CHECK_EQ(buf[0].epoch, ACTIVITY_CAP + 3);              /* mais recente */
    CHECK_EQ(buf[ACTIVITY_CAP - 1].epoch, 4);             /* 0..3 descartados */

    /* --- resumo do dia (agrega epoch >= since) --- */
    activity_log_init();
    activity_log_note(ACT_STARTED, 0, "a", "p", 0,   1000);
    activity_log_note(ACT_BLOCKED, 0, "a", "p", 0,   1100);
    activity_log_note(ACT_DONE,    0, "a", "p", 60,  1200);
    activity_log_note(ACT_DONE,    0, "b", "p", 120, 1300);
    activity_log_note(ACT_STARTED, 0, "c", "p", 0,   500);   /* antes do "since" */
    int m = activity_log_get(buf, ACTIVITY_CAP);
    activity_summary_t sm = activity_summarize(buf, m, 1000);
    CHECK_EQ(sm.started, 1);
    CHECK_EQ(sm.blocked, 1);
    CHECK_EQ(sm.done, 2);
    CHECK_EQ(sm.active_secs, 180);
    sm = activity_summarize(buf, m, 0);   /* since=0 pega tudo */
    CHECK_EQ(sm.started, 2);
    CHECK_EQ(sm.done, 2);

    if (failures) {
        printf("activity_log: %d FALHA(S)\n", failures);
        return 1;
    }
    printf("activity_log: OK (todos os testes passaram)\n");
    return 0;
}
