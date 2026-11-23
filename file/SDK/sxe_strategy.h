/*
 * sxe_strategy.h - C contract between SXE Core and a strategy (SXE Core 2.2)
 *
 * A strategy is a shared library (Windows: lib<Name>.dll, Linux: lib<Name>.so). Besides the C++
 * strategies (CStrategyBase, SXE_EXPORT_STRATEGY), the engine loads libraries that export the C
 * functions below: plain C, so the library can be built with any compiler (MSVC, MinGW, clang,
 * gcc) and written in any language that exports C functions, or that a C library can host
 * (Python, C#, Rust...). Only this file is needed: the library does not link the engine.
 *
 * The library exports (SXE_STRATEGY_EXPORT):
 *
 *   int         sxe_strategy_api_version(void);                     SXE_STRATEGY_API_VERSION
 *   void*       sxe_strategy_create(const sxe_host* host);           NULL = failed
 *   void        sxe_strategy_info(void* s, sxe_info* info); name, version, description
 *   int         sxe_strategy_load_config(void* s, const char* ini);  1 = ok, 0 = failed
 *   int         sxe_strategy_init(void* s, const sxe_init* init);    1 = ok, 0 = failed
 *   int         sxe_strategy_step(void* s, sxe_step* step);          SXE_STEP_CONTINUE, _STOP, _ERROR
 *   void        sxe_strategy_term(void* s);
 *   const char* sxe_strategy_error(void* s);                         why the last call failed
 *   void        sxe_strategy_destroy(void* s);
 *
 * The engine calls them in this order: create, info, load_config (the strategy's .ini, [STRATEGY]
 * config), init (once), step (every step, until it returns STOP or ERROR or the data end), term,
 * destroy. One thread: the engine's. The strategy does not read the clock, the network or the
 * exchange: time and data come with the step, orders go through the host.
 *
 * A step is one price of the simulation (several per candle with [SIMULATION] intrabar). The
 * candle in progress is never visible: last_candle is the last CLOSED one, candle_closed = 1 in
 * the first step after it closed, so a strategy cannot see future prices.
 *
 * Orders: the host functions below, only during sxe_strategy_step(). The result comes at once,
 * and after an order the engine has already updated account, positions and open orders of the
 * sxe_step being run (the same pointer).
 *
 * Strings: UTF-8. Those given by the engine are valid until the call that received them returns
 * (the error of an order until the next order); those given by the strategy (info, error) must
 * stay valid until the next call to the strategy.
 *
 * Times: seconds since 1970-01-01 UTC. Percentages are fractions only where it is written.
 *
 * Licence: PolyForm Noncommercial 1.0.0 (see LICENSE): commercial use needs the permission of
 * the author.
 */
#ifndef SXE_STRATEGY_H
#define SXE_STRATEGY_H

#include <stdint.h>

#if defined(_WIN32)
  #define SXE_STRATEGY_EXPORT __declspec(dllexport)
#else
  #define SXE_STRATEGY_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* version of this contract: the engine refuses a library built with another one */
#define SXE_STRATEGY_API_VERSION 1

/* names of the functions, for the loaders */
#define SXE_STRATEGY_SYMBOL_VERSION     "sxe_strategy_api_version"
#define SXE_STRATEGY_SYMBOL_CREATE      "sxe_strategy_create"
#define SXE_STRATEGY_SYMBOL_INFO        "sxe_strategy_info"
#define SXE_STRATEGY_SYMBOL_LOAD_CONFIG "sxe_strategy_load_config"
#define SXE_STRATEGY_SYMBOL_INIT        "sxe_strategy_init"
#define SXE_STRATEGY_SYMBOL_STEP        "sxe_strategy_step"
#define SXE_STRATEGY_SYMBOL_TERM        "sxe_strategy_term"
#define SXE_STRATEGY_SYMBOL_ERROR       "sxe_strategy_error"
#define SXE_STRATEGY_SYMBOL_DESTROY     "sxe_strategy_destroy"

/* ---------- values (int, so that every compiler agrees on the size) ---------- */

/* what sxe_strategy_step() returns */
#define SXE_STEP_CONTINUE 0
#define SXE_STEP_STOP     1   /* the strategy asks to end the run */
#define SXE_STEP_ERROR    2   /* serious error: the engine stops (sxe_strategy_error) */

/* side of a position */
#define SXE_SIDE_LONG  0
#define SXE_SIDE_SHORT 1

/* side of an order */
#define SXE_ORDER_BUY   0   /* opens or increases the long (one-way: first reduces a short) */
#define SXE_ORDER_CLOSE 1   /* reduces or closes the position of position_side (reduce-only) */
#define SXE_ORDER_SELL  2   /* opens or increases the short (one-way: first reduces a long) */

/* type of an order */
#define SXE_TYPE_MARKET 0   /* taker fee and slippage */
#define SXE_TYPE_LIMIT  1   /* maker fee, at its price */
#define SXE_TYPE_STOP   2   /* at its trigger it closes at market (stop loss, take profit) */

#define SXE_MODE_SIMULATED 0
#define SXE_MODE_REAL      1

#define SXE_MARGIN_CROSSED  0
#define SXE_MARGIN_ISOLATED 1

#define SXE_POSITIONS_ONE_WAY 0
#define SXE_POSITIONS_HEDGE   1

#define SXE_LEVEL_INFO    0
#define SXE_LEVEL_WARNING 1
#define SXE_LEVEL_ERROR   2

/* ---------- data ---------- */

typedef struct sxe_bar
{
    int64_t time;       /* open time of the candle */
    double  open;
    double  high;
    double  low;
    double  close;
    double  volume;
} sxe_bar;

typedef struct sxe_account
{
    double wallet;      /* balance, fees and funding already taken */
    double equity;      /* balance + unrealized PnL */
    double available;   /* margin free for a new order */
} sxe_account;

typedef struct sxe_position
{
    int    open;                /* 0 = no position (the other fields are 0) */
    int    side;                /* SXE_SIDE_ */
    double qty;                 /* in the base currency (e.g. BTC) */
    double avg_price;
    double margin;              /* initial margin (USDT) */
    double leverage;
    double unrealized_pnl;
    double liquidation_price;   /* 0 = cannot be computed */
    double fees_paid;
    double funding_paid;        /* negative if received */
    double realized_pnl;        /* gross PnL of the partial reductions already done */
} sxe_position;

typedef struct sxe_order_result
{
    int         ok;
    int         type;           /* SXE_TYPE_ */
    int64_t     order_id;       /* limit and stop orders: the id for move and cancel */
    double      fill_price;     /* 0 for a limit or stop order just placed */
    double      qty;
    double      fee;
    double      fee_rate;       /* fraction: fee_taker or fee_maker */
    const char* error;          /* why it was refused when ok = 0, "" otherwise */
} sxe_order_result;

/* an open limit or stop order */
typedef struct sxe_open_order
{
    int64_t id;
    int     side;               /* SXE_ORDER_ */
    int     type;               /* SXE_TYPE_LIMIT or SXE_TYPE_STOP */
    int     position_side;      /* SXE_SIDE_: the position it opens or closes */
    int     all;                /* close: the whole position when it fills */
    int     above;              /* stop: fires when the price rises to price (0: when it falls) */
    double  price;              /* limit price, or the trigger of a stop */
    double  usdt;               /* opening: margin asked */
    double  leverage;
    double  qty;                /* opening: quantity; close: how much (0 with all = 1) */
    double  reserved;           /* opening: margin and fee reserved */
    int64_t time;               /* when it was placed */
} sxe_open_order;

/* an open order that was filled (limit or stop) */
typedef struct sxe_execution
{
    int64_t order_id;
    int     side;               /* SXE_ORDER_ */
    int     type;               /* SXE_TYPE_ */
    int     position_side;      /* SXE_SIDE_ */
    int64_t time;
    double  price;
    double  qty;
    double  fee;
    double  fee_rate;
    double  wallet_before;
} sxe_execution;

/* rules of the contract ([EXCHANGE] of EngineConfig.ini) */
typedef struct sxe_rules
{
    int    margin_mode;         /* SXE_MARGIN_ */
    int    position_mode;       /* SXE_POSITIONS_ */
    double fee_taker;           /* fraction per side */
    double fee_maker;
    double slippage;            /* fraction */
    double maintenance_margin;
    double qty_step;
    double min_qty;
    double min_notional;        /* USDT */
} sxe_rules;

typedef struct sxe_info
{
    const char* name;
    const char* version;
    const char* description;
} sxe_info;

/* the situation at the start, given once to sxe_strategy_init() */
typedef struct sxe_init
{
    int          mode;          /* SXE_MODE_: only informative, the logic must not depend on it */
    int          resume;        /* the run goes on from a saved state: the strategy reloads its own */
    const char*  symbol;        /* e.g. BTCUSDT */
    int64_t      start_time;    /* simulation: first candle; real: start time */
    const char*  log_dir;       /* files of the cycle in progress (moved to backup_dir when it ends) */
    const char*  backup_dir;    /* closed cycles, read only */
    sxe_account  account;
    sxe_position position;      /* one-way: the open one, long or short; hedge: the long */
    sxe_position long_position;
    sxe_position short_position;
    sxe_rules    rules;
} sxe_init;

/* the situation at every step, given to sxe_strategy_step() */
typedef struct sxe_step
{
    int64_t      time;          /* simulation: time of the step; real: now */
    int64_t      step;          /* from 1 */
    int64_t      cycle;         /* cycle in progress (or the next one if no position is open) */
    double       price;         /* price now */
    sxe_bar      last_candle;   /* the last CLOSED candle */
    int          candle_closed; /* 1 in the first step after last_candle closed */
    int          liquidated;    /* a position was liquidated since the last step (its orders cancelled) */
    sxe_account  account;
    sxe_position position;      /* one-way: the open one, long or short; hedge: the long */
    sxe_position long_position;
    sxe_position short_position;
    const sxe_open_order* open_orders;   /* the open limit and stop orders */
    int64_t      open_order_count;
    const sxe_execution*  executions;    /* open orders filled in this step, before the strategy ran */
    int64_t      execution_count;
} sxe_step;

/* ---------- what the engine offers to the strategy ----------
 * `engine` is given back to every function. The order functions write their result in `out`
 * (never NULL) and return out->ok.
 */
typedef struct sxe_host
{
    int   size;                 /* sizeof(sxe_host) of the engine: newer engines may add fields at the end */
    void* engine;

    /* --- at market (taker fee, slippage against) --- */
    /* buy: usdt of margin, leverage. Opens or increases the long; one-way with a short open: reduces
       it and, if larger, closes it and opens the rest long */
    int (*market_buy)(void* engine, double usdt, double leverage, sxe_order_result* out);
    /* sell: as market_buy on the other side (opens or increases the short) */
    int (*market_sell)(void* engine, double usdt, double leverage, sxe_order_result* out);
    /* closes every open position */
    int (*close_all)(void* engine, sxe_order_result* out);
    /* closes the whole position of a side */
    int (*close_position)(void* engine, int side, sxe_order_result* out);
    /* reduces the position of a side by qty (reduce-only); qty >= position closes it */
    int (*reduce)(void* engine, int side, double qty, sxe_order_result* out);

    /* --- limit orders: at their price, no slippage, maker fee ([SIMULATION] limit_fill says when).
       A limit that would fill at once is refused --- */
    int (*limit_buy)(void* engine, double price, double usdt, double leverage, sxe_order_result* out);
    int (*limit_sell)(void* engine, double price, double usdt, double leverage, sxe_order_result* out);
    /* closes the whole position (one-way: the open one; hedge: the long): above the market for a
       long, below for a short. One at a time */
    int (*limit_close)(void* engine, double price, sxe_order_result* out);
    /* closes qty of the position of a side at the limit price (partial take profit, reduce-only) */
    int (*limit_reduce)(void* engine, int side, double price, double qty, sxe_order_result* out);

    /* --- stop (stop loss, take profit at a trigger): at the trigger they close at market. qty 0 =
       the whole position when it fires. Fires when the price rises to trigger if it is above the
       market, when it falls if it is below --- */
    int (*stop_close)(void* engine, int side, double trigger, double qty, sxe_order_result* out);

    /* new price (or trigger) of an open order (opening: same margin, quantity computed again) */
    int (*move)(void* engine, int64_t order_id, double price, sxe_order_result* out);
    /* 1 = cancelled, 0 = no such open order */
    int  (*cancel)(void* engine, int64_t order_id);
    void (*cancel_all)(void* engine);

    /* --- log: the engine's log (log.txt, error.txt, the callbacks of sxe.h), also outside a step --- */
    void (*log)(void* engine, int level, const char* message);
    /* as log (warning and error), but the same message is not written again until another one
       comes or clear_once is called: a problem that repeats at every step is written once */
    void (*log_once)(void* engine, int level, const char* message);
    /* the problem of log_once is over: the next occurrence is written again */
    void (*clear_once)(void* engine);
} sxe_host;

/* the functions of a strategy, for the loaders */
typedef int         (*sxe_strategy_api_version_fn)(void);
typedef void*       (*sxe_strategy_create_fn)(const sxe_host* host);
typedef void        (*sxe_strategy_info_fn)(void* strategy, sxe_info* info);
typedef int         (*sxe_strategy_load_config_fn)(void* strategy, const char* ini_path);
typedef int         (*sxe_strategy_init_fn)(void* strategy, const sxe_init* init);
typedef int         (*sxe_strategy_step_fn)(void* strategy, sxe_step* step);
typedef void        (*sxe_strategy_term_fn)(void* strategy);
typedef const char* (*sxe_strategy_error_fn)(void* strategy);
typedef void        (*sxe_strategy_destroy_fn)(void* strategy);

#ifdef __cplusplus
}
#endif

#endif
