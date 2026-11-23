#pragma once
#include <string>
#include <vector>

#include "Logger.h"
#include "Types.h"

//=============================
// What the engine gives the strategy
// The strategy asks nothing of the exchange: it gets everything here, in the same
// form in simulation and live.
//=============================

//name, version and description of the strategy (written in the log at start)
struct StrategyInfo
{
	std::string strName;
	std::string strVersion;
	std::string strDescription;
};

//=============================
// Orders: the result comes at once and, after an executed order, ctx.position
// and ctx.account are already up to date
//=============================
class IOrders
{
public:
	virtual ~IOrders(){}

	//--- market (taker fee, slippage against) ---
	//buy: dUsdt of margin, dLeverage of leverage. Opens or increases the long; in one-way with a
	//short open it reduces it and, if the order is larger, closes it and opens the rest long
	virtual OrderResult MarketBuy(double dUsdt, double dLeverage) = 0;
	//sell: as MarketBuy on the other side (opens or increases the short)
	virtual OrderResult MarketSell(double dUsdt, double dLeverage) = 0;
	//closes every open position
	virtual OrderResult Close() = 0;
	//closes the whole position of one side
	virtual OrderResult ClosePosition(PositionSide eSide) = 0;
	//reduces the position of one side by dQty (reduce-only); dQty >= position = closes it
	virtual OrderResult Reduce(PositionSide eSide, double dQty) = 0;

	//--- limit orders: filled at their price, without slippage, with the maker fee ---
	//([SIMULATION] limit_fill says when). A limit that would be filled at once is rejected.
	//buy below the market: dUsdt of margin (reserved while the order is open), dLeverage of leverage
	virtual OrderResult LimitBuy(double dPrice, double dUsdt, double dLeverage) = 0;
	//sell above the market (opens or increases the short)
	virtual OrderResult LimitSell(double dPrice, double dUsdt, double dLeverage) = 0;
	//close of the whole position (one-way: the open one; hedge: the long): above the market for
	//a long, below for a short. One at a time
	virtual OrderResult LimitClose(double dPrice) = 0;
	//close of dQty of the position of one side at the limit price (partial take profit, reduce-only)
	virtual OrderResult LimitReduce(PositionSide eSide, double dPrice, double dQty) = 0;

	//--- stop (stop loss, take profit on a trigger): when dTrigger is reached they close at market ---
	//dQty 0 = the whole position when it triggers. It triggers when the price rises to dTrigger
	//if it is above the market, when it falls to it if it is below
	virtual OrderResult StopClose(PositionSide eSide, double dTrigger, double dQty) = 0;

	//new price (or trigger) of an open order (opening: same margin, quantity recalculated)
	virtual OrderResult Move(long long nOrderId, double dPrice) = 0;
	virtual bool Cancel(long long nOrderId) = 0;
	virtual void CancelAll() = 0;
};

//=============================
// InitContext: the situation at start, given once to Init()
//
// Cycle = from the opening of the position to its complete close.
// At the close the engine creates <strBackupDir>/b<N> and moves into it every file
// in strLogDir (log.txt, error.txt, orders and the strategy's files; only the
// simulation state stays).
// So the strategy writes its cycle files in strLogDir, opening them at every
// write (append): at the end of the cycle they are archived by themselves.
//=============================
struct InitContext
{
	Mode          eMode;			//for information only: the logic must not depend on it
	bool          bResume;			//the run resumes from a saved state: the strategy reloads its own;
									//false = a fresh start, the state of earlier runs must be ignored
	std::string   strSymbol;		//e.g. BTCUSDT
	long long     nStartTime;		//sim: first candle; live: start time
	std::string   strLogDir;		//Target/Log: files of the current cycle
	std::string   strBackupDir;		//Target/Backup: closed cycles (read only for the strategy)

	Account       account;			//account at start
	Position      position;			//position already open (resume) or bOpen = false; one-way: the
									//open one, long or short; hedge: the long
	Position      longPosition;		//long position (bOpen = false if there is none)
	Position      shortPosition;	//short position
	ExchangeRules rules;			//rules of the contract (EngineConfig.ini [EXCHANGE]): margin,
									//position mode (one-way or hedge), fees

	Logger&       log;
};

//=============================
// StepContext: the situation at every step, given to Run()
//
// lastCandle is the last CLOSED candle: the current candle is not visible,
// so the strategy cannot see future prices. bCandleClosed is true
// in the first step after a new candle closed.
//=============================
struct StepContext
{
	//--- time and market ---
	long long     nTime;			//sim: time of the step; live: current time
	long long     nStep;			//step counter, from 1
	long long     nCycle;			//number of the current cycle (or of the next one if the position is closed)
	double        dPrice;			//current price
	Candle        lastCandle;
	bool          bCandleClosed;

	//--- account and positions ---
	Account       account;
	Position      position;			//one-way: the open position, long or short (eSide); hedge: the long
	Position      longPosition;		//long position (bOpen = false if there is none)
	Position      shortPosition;	//short position

	//--- events since the previous step ---
	bool          bLiquidated;		//a position was liquidated by the exchange (orders cancelled)

	//--- open orders (limit and stop) ---
	std::vector<LimitOrder> vOpenOrders;	//open
	std::vector<Fill>       vFills;			//filled in this step, before Run()

	//--- actions ---
	IOrders&      orders;
	Logger&       log;
};
