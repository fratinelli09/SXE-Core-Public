/*
 * The test strategy (CTestStrategy.cpp) written in C on the C contract (sxe_strategy.h):
 * the same rules, so the same references of the golden tests must come out identical.
 *
 * Opens if there is no position, closes at +tp% on the average price. stop_at_step > 0: at that
 * step it asks to stop. limit_dip > 0: market entry, limit increase limit_dip% against and limit
 * close at +tp% of the average price, moved when a fill changes the average price.
 * side: long, short, both. stop_loss > 0: at the opening a stop at -stop_loss%.
 */
#include <stdlib.h>
#include <string.h>

#include "sxe_strategy.h"
#include "sxe_test_ini.h"

typedef struct test_strategy
{
    const sxe_host* host;
    double tp;
    double leverage;
    double order_perc;
    double stop_at_step;
    double limit_dip;
    double stop_loss;
    int    is_long;
    int    is_short;
    char   error[256];
} test_strategy;

SXE_STRATEGY_EXPORT int sxe_strategy_api_version(void)
{
    return SXE_STRATEGY_API_VERSION;
}

SXE_STRATEGY_EXPORT void* sxe_strategy_create(const sxe_host* host)
{
    test_strategy* s = (test_strategy*)calloc(1, sizeof(test_strategy));
    if(!s)
        return NULL;
    s->host = host;
    s->tp = 2;
    s->leverage = 1;
    s->order_perc = 50;
    s->is_long = 1;
    return s;
}

SXE_STRATEGY_EXPORT void sxe_strategy_info(void* p, sxe_info* info)
{
    (void)p;
    info->name = "TestStrategyC";
    info->version = "2.0";
    info->description = "test strategy, C contract";
}

SXE_STRATEGY_EXPORT int sxe_strategy_load_config(void* p, const char* ini)
{
    test_strategy* s = (test_strategy*)p;
    if(!sxe_test_ini_double(ini, "TEST", "tp", &s->tp) || !sxe_test_ini_double(ini, "TEST", "leverage", &s->leverage) ||
       !sxe_test_ini_double(ini, "TEST", "order_perc", &s->order_perc) ||
       !sxe_test_ini_double(ini, "TEST", "stop_at_step", &s->stop_at_step))
    {
        strcpy(s->error, "keys missing in [TEST]");
        return 0;
    }
    sxe_test_ini_double(ini, "TEST", "limit_dip", &s->limit_dip);
    sxe_test_ini_double(ini, "TEST", "stop_loss", &s->stop_loss);
    char side[32] = "long";
    sxe_test_ini_get(ini, "TEST", "side", side, sizeof side);
    s->is_long = strcmp(side, "long") == 0 || strcmp(side, "both") == 0;
    s->is_short = strcmp(side, "short") == 0 || strcmp(side, "both") == 0;
    if(!s->is_long && !s->is_short)
    {
        strcpy(s->error, "side is not valid (long, short, both)");
        return 0;
    }
    return 1;
}

SXE_STRATEGY_EXPORT int sxe_strategy_init(void* p, const sxe_init* init)
{
    (void)p;
    (void)init;
    return 1;
}

static const sxe_position* position_of(const sxe_step* step, int side)
{
    return side == SXE_SIDE_LONG ? &step->long_position : &step->short_position;
}

/* price at +pct% in favour of the position (negative: against) */
static double target(int side, double avg, double pct)
{
    return side == SXE_SIDE_LONG ? avg * (1.0 + pct / 100.0) : avg * (1.0 - pct / 100.0);
}

static int open_position(test_strategy* s, sxe_step* step, int side)
{
    sxe_order_result r;
    const double usdt = step->account.available * s->order_perc / 100.0;
    if(side == SXE_SIDE_LONG)
        return s->host->market_buy(s->host->engine, usdt, s->leverage, &r);
    return s->host->market_sell(s->host->engine, usdt, s->leverage, &r);
}

static void run_limit(test_strategy* s, sxe_step* step, int side)
{
    const sxe_host* h = s->host;
    sxe_order_result r;
    if(!position_of(step, side)->open)
    {
        if(s->is_long != s->is_short)
        {
            if(step->open_order_count > 0)
                h->cancel_all(h->engine);
        }
        else
        {
            /* the ids before cancelling: every cancellation updates the orders of the step */
            int64_t ids[64];
            int64_t n = 0;
            for(int64_t i = 0; i < step->open_order_count && n < 64; ++i)
                if(step->open_orders[i].position_side == side)
                    ids[n++] = step->open_orders[i].id;
            for(int64_t i = 0; i < n; ++i)
                h->cancel(h->engine, ids[i]);
        }
        if(!open_position(s, step, side))
            return;
        const double avg = position_of(step, side)->avg_price;
        const double usdt = step->account.available * s->order_perc / 100.0;
        if(side == SXE_SIDE_LONG)
        {
            h->limit_buy(h->engine, target(side, avg, -s->limit_dip), usdt, s->leverage, &r);
            h->limit_close(h->engine, target(side, avg, s->tp), &r);
        }
        else
        {
            h->limit_sell(h->engine, target(side, avg, -s->limit_dip), usdt, s->leverage, &r);
            h->limit_reduce(h->engine, side, target(side, avg, s->tp), position_of(step, side)->qty * 1000, &r);
        }
        if(s->stop_loss > 0)
            h->stop_close(h->engine, side, target(side, avg, -s->stop_loss), 0, &r);
        return;
    }
    int added = 0;
    for(int64_t i = 0; i < step->execution_count; ++i)
        added = added || (step->executions[i].position_side == side && step->executions[i].side != SXE_ORDER_CLOSE);
    int64_t close_id = 0;
    for(int64_t i = 0; i < step->open_order_count; ++i)
    {
        const sxe_open_order* o = &step->open_orders[i];
        if(o->side == SXE_ORDER_CLOSE && o->type == SXE_TYPE_LIMIT && o->position_side == side)
            close_id = o->id;
    }
    /* increase executed: the close limit follows the average price */
    if(added && close_id > 0)
        h->move(h->engine, close_id, target(side, position_of(step, side)->avg_price, s->tp), &r);
}

static void run_side(test_strategy* s, sxe_step* step, int side)
{
    const sxe_host* h = s->host;
    sxe_order_result r;
    if(s->limit_dip > 0)
    {
        run_limit(s, step, side);
        return;
    }
    const sxe_position* p = position_of(step, side);
    if(!p->open)
    {
        if(open_position(s, step, side) && s->stop_loss > 0)
            h->stop_close(h->engine, side, target(side, position_of(step, side)->avg_price, -s->stop_loss), 0, &r);
    }
    else if(side == SXE_SIDE_LONG ? step->price >= target(side, p->avg_price, s->tp)
                                  : step->price <= target(side, p->avg_price, s->tp))
    {
        h->close_position(h->engine, side, &r);
    }
}

SXE_STRATEGY_EXPORT int sxe_strategy_step(void* p, sxe_step* step)
{
    test_strategy* s = (test_strategy*)p;
    if(s->is_long)
        run_side(s, step, SXE_SIDE_LONG);
    if(s->is_short)
        run_side(s, step, SXE_SIDE_SHORT);
    if(s->stop_at_step > 0 && step->step == (int64_t)s->stop_at_step)
        return SXE_STEP_STOP;
    return SXE_STEP_CONTINUE;
}

SXE_STRATEGY_EXPORT void sxe_strategy_term(void* p)
{
    (void)p;
}

SXE_STRATEGY_EXPORT const char* sxe_strategy_error(void* p)
{
    return ((test_strategy*)p)->error;
}

SXE_STRATEGY_EXPORT void sxe_strategy_destroy(void* p)
{
    free(p);
}
