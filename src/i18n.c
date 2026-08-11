#include "i18n.h"

#include <stdio.h>
#include <string.h>

/* Uma linha por chave, as duas traduções lado a lado — gerada da mesma lista
   que produziu o enum, então índice e chave não têm como divergir. */
static const char *const S[STR_COUNT][LANG_COUNT] = {
#define X(id, en, pt) { en, pt },
    I18N_STRINGS(X)
#undef X
};

static const char *const WEEKDAYS[LANG_COUNT][7] = {
    { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" },
    { "dom", "seg", "ter", "qua", "qui", "sex", "sáb" },
};

static const char *const MONTHS[LANG_COUNT][12] = {
    { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" },
    { "jan", "fev", "mar", "abr", "mai", "jun",
      "jul", "ago", "set", "out", "nov", "dez" },
};

static ui_lang_t s_lang = LANG_EN;

void i18n_set_lang(ui_lang_t lang)
{
    s_lang = lang < LANG_COUNT ? lang : LANG_EN;
}

ui_lang_t i18n_lang(void)
{
    return s_lang;
}

const char *i18n_lang_name(ui_lang_t lang)
{
    return lang == LANG_PT ? "Português" : "English";
}

const char *i18n_get(str_id_t id)
{
    if (id >= STR_COUNT) {
        return "";
    }
    return S[id][s_lang];
}

const char *i18n_status(const char *status)
{
    if (strcmp(status, "working") == 0) return T(STR_ST_WORKING);
    if (strcmp(status, "blocked") == 0) return T(STR_ST_BLOCKED);
    if (strcmp(status, "idle") == 0)    return T(STR_ST_IDLE);
    if (strcmp(status, "done") == 0)    return T(STR_ST_DONE);
    return status;   /* status novo aparece cru, não some */
}

const char *i18n_weekday(int wday)
{
    if (wday < 0 || wday > 6) {
        return "";
    }
    return WEEKDAYS[s_lang][wday];
}

const char *i18n_month(int mon)
{
    if (mon < 0 || mon > 11) {
        return "";
    }
    return MONTHS[s_lang][mon];
}

void i18n_format_date(char *buf, size_t size, const struct tm *tm)
{
    const char *wd = i18n_weekday(tm->tm_wday);
    const char *mo = i18n_month(tm->tm_mon);
    /* a ordem dia/mês inverte entre os idiomas, e o printf da LVGL não tem
       argumento posicional (%1$s) para resolver isso dentro do formato */
    if (s_lang == LANG_PT) {
        snprintf(buf, size, "%s, %d %s", wd, tm->tm_mday, mo);
    } else {
        snprintf(buf, size, "%s, %s %d", wd, mo, tm->tm_mday);
    }
}
