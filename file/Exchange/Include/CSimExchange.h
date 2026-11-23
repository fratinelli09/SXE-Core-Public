#pragma once
#include <string>
#include <utility>
#include <vector>

#include "IExchange.h"

//=============================
// Simulated exchange (USDT-M perpetual contract of one symbol, Bitget rules)
//
//  - position mode (ExchangeRules::ePositionMode):
//      one-way: one direction at a time. An opposite order reduces the position and, if it is
//               larger, closes it and opens the rest in the opposite direction (a single order:
//               if the margin for the new part is not enough, all of it is rejected)
//      hedge:   long and short together, each with its own average price, margin and liquidation
//  - margin (ExchangeRules::eMarginMode):
//      isolated: each position is backed only by its own margin; liquidated, it loses that
//      crossed:  the whole balance backs the positions; liquidated, the balance is lost
//  - market orders: quantity = USDT * leverage / price, rounded to the step, filled at the
//    price +/- slippage (always against us); rejected below the minimum quantity and value or
//    if the available margin is not enough. In isolated, if an opening is short by a little
//    (fees and funding paid), the order uses all the available margin
//  - reductions (reduce-only) of part of the position: PnL realized on the closed part,
//    margin released in proportion, average price unchanged
//  - taker fee on market orders and stops, maker on filled limit orders
//  - limit orders: openings (buy below the market, sell above) and closes (of a long above
//    the market, of a short below): filled at the limit price, without slippage, when the
//    price goes through it by one tick (limit_fill = through) or touches it (touch). An opening
//    reserves margin and fee; if the margin is not enough at the fill it is cancelled.
//    A limit that would be filled at once is rejected (as post-only)
//  - stops (stop loss and take profit on a trigger): when the price is reached, they close at market
//  - when a position is completely closed its open close orders are cancelled
//  - available margin: balance - isolated margins, or equity - value / leverage of the
//    cross positions; minus what the opening orders reserved
//  - funding every 8 hours (00:00, 08:00, 16:00 UTC) on the value of the position, from the
//    history or with the fixed rate: with a positive rate the long pays and the short receives.
//    In isolated it goes through the margin of the position
//  - maintenance margin in progressive tiers (as Bitget since 10/11/2025): each band of the
//    value of the position pays the rate of its tier; in cross hedge the side with the higher
//    value counts
//  - liquidation when collateral + PnL <= maintenance margin + closing fee:
//      isolated: only that position, its margin is lost
//      crossed:  every order is cancelled, in hedge long and short are offset, then the position
//                steps down one tier at a time (market reduction), checking again at every
//                step; if that is not enough every position is liquidated and the balance goes to zero
//=============================
class CSimExchange : public IExchange
{
public:
	struct Stats
	{
		long long nOrders       = 0;	//orders executed (openings, increases, reductions, closes)
		long long nRejected     = 0;
		long long nClosed       = 0;	//positions closed completely by an order
		long long nWins         = 0;	//closed with a net gain (fees and funding included)
		long long nLiquidations = 0;
		double    dFeesPaid     = 0;
		double    dFundingPaid  = 0;	//negative if received
		double    dRealizedPnL  = 0;	//gross PnL of closes and reductions
		long long nLimitOrders  = 0;	//limit orders placed
		long long nLimitFills   = 0;	//limit orders filled
		long long nCancelled    = 0;
		double    dMakerFees    = 0;	//fees of the limit orders (inside dFeesPaid)
		long long nPartialLiquidations = 0;	//tier reductions and offsets in cross
		long long nStopOrders   = 0;	//stops placed
		long long nStopFills    = 0;	//stops triggered
	};

	//complete state, to save and resume a simulation
	struct Snapshot
	{
		long long nTime         = 0;	//last step
		double    dPrice        = 0;	//last price: values the positions before the first step
		double    dWallet       = 0;
		Position  position;				//long (or the only position in one-way)
		Position  shortPosition;		//short
		long long nFundingSlot  = -1;
		Stats     stats;
		std::vector<LimitOrder> vOrders;	//open orders
		long long nNextOrderId  = 1;
		double    dCycleFees    = 0;	//fees of the current cycle
		double    dCycleFunding = 0;	//funding of the current cycle
	};

public:
	CSimExchange(const ExchangeRules& rules, double dInitialCapital);

	//bLiquidation = 0/1 ([SIMULATION] liquidation); dFundingRate used where the history is missing;
	//dFundingMultiplier multiplies every funding (2 = twice as much paid at every settlement)
	void SetSimRules(bool bLiquidation, double dFundingRate, double dFundingMultiplier);
	//bThrough: limit_fill = through (the price must go through the limit by dTickStep) or touch
	void SetLimitRules(bool bThrough, double dTickStep);
	//tiers (upper limit in USDT, rate), increasing; empty = rules.dMaintenanceMargin on everything
	void SetMaintenanceTiers(const std::vector<std::pair<double, double>>& vTiers);
	//maintenance margin of a position of the given value (USDT)
	double MaintenanceMargin(double dNotional) const;
	//funding history (timestamp,funding_rate); empty = only the fixed rate
	bool LoadFunding(const std::string& strFile, std::string& strError);
	size_t FundingCount() const { return vFunding.size(); }

	//order log (one line per order); bAppend = false empties it (simulation from the start).
	//The file is opened at every order: at the end of the cycle the engine moves it to the backup
	bool SetOrderLog(const std::string& strFile, bool bAppend);

	//before the first tick of every candle: pays the funding slots crossed
	void ApplyFunding(long long nCandleTime, double dMark);

	//new price: orders reached, then the liquidation
	bool Refresh(long long nTime, double dPrice) override;

	const Account&  GetAccount() const override  { return account; }
	const Position& GetPosition() const override;
	const Position& GetPosition(PositionSide eSide) const override { return vPositions[_Index(eSide)]; }
	bool AnyOpen() const override { return vPositions[0].bOpen || vPositions[1].bOpen; }
	const ExchangeRules& GetRules() const override { return rules; }
	bool TakeLiquidated() override;

	OrderResult MarketBuy(double dUsdt, double dLeverage) override;
	OrderResult MarketSell(double dUsdt, double dLeverage) override;
	OrderResult Close() override;
	OrderResult ClosePosition(PositionSide eSide) override;
	OrderResult Reduce(PositionSide eSide, double dQty) override;

	OrderResult LimitBuy(double dPrice, double dUsdt, double dLeverage) override;
	OrderResult LimitSell(double dPrice, double dUsdt, double dLeverage) override;
	OrderResult LimitClose(double dPrice) override;
	OrderResult LimitReduce(PositionSide eSide, double dPrice, double dQty) override;
	OrderResult StopClose(PositionSide eSide, double dTrigger, double dQty) override;
	OrderResult Move(long long nOrderId, double dPrice) override;
	bool Cancel(long long nOrderId) override;
	void CancelAll() override;

	const std::vector<LimitOrder>& GetOpenOrders() const override { return vOrders; }
	std::vector<Fill> TakeFills() override;
	std::vector<ExchangeEvent> TakeEvents() override;

	const Stats& GetStats() const { return stats; }
	//fees and funding of the current cycle (cycle still open at the end of the simulation)
	double CycleFees() const { return dCycleFees; }
	double CycleFunding() const { return dCycleFunding; }
	//changes at every change of the open orders: the engine copies the list again only then
	long long OrdersVersion() const { return nOrdersVersion; }

	Snapshot GetSnapshot() const;
	void Restore(const Snapshot& snapshot);

private:
	//state that a one-way order may have to undo (margin not enough for the new part)
	struct Saved
	{
		Account     account;
		Position    vPositions[2];
		Stats       stats;
		double      dCycleFees    = 0;
		double      dCycleFunding = 0;
		size_t      nEvents       = 0;
	};

	static int _Index(PositionSide eSide) { return eSide == PositionSide::Long ? 0 : 1; }
	static PositionSide _Other(PositionSide eSide) { return eSide == PositionSide::Long ? PositionSide::Short : PositionSide::Long; }
	Position& _Pos(PositionSide eSide) { return vPositions[_Index(eSide)]; }
	bool _Hedge() const { return rules.ePositionMode == PositionMode::Hedge; }
	//the open position when there is only one (the long if there are two)
	const Position& _OpenPosition() const { return vPositions[0].bOpen ? vPositions[0] : vPositions[1]; }
	bool _Isolated() const { return rules.eMarginMode == MarginMode::Isolated; }

	void _Update();								//equity, available, PnL and liquidation prices at the current price
	double _Collateral(const Position& p) const;
	double _PnL(const Position& p, double dAt) const;
	double _LiquidationPrice(const Position& p) const;
	double _CrossHedgeLiquidationPrice() const;
	double _FundingRate(long long nSlot) const;
	bool _Unsafe(const Position& p) const;		//a single position: collateral + PnL <= maintenance + fee
	bool _CrossHedgeUnsafe() const;
	void _CheckLiquidation();
	void _Liquidate(PositionSide eSide);
	void _LiquidateCross();
	OrderResult _Reject(const std::string& strError, OrderType eType = OrderType::Market);
	void _LogOrder(const char* pszType, const OrderResult& r, double dLimitPrice = 0, PositionSide eSide = PositionSide::Long);
	double _RoundQty(double dQty) const;
	double _FillPrice(bool bBuy) const;			//price of a market order: + slippage when buying, - when selling
	OrderResult _MarketOpen(PositionSide eSide, double dUsdt, double dLeverage);
	OrderResult _ApplyOpen(PositionSide eSide, double dQty, double dFill, double dLeverage, double dFeeRate, OrderType eType);
	OrderResult _ApplyReduce(PositionSide eSide, double dQty, double dFill, double dFeeRate, OrderType eType, const char* pszReason);
	OrderResult _ApplyClose(PositionSide eSide, double dFill, double dFeeRate, OrderType eType, const char* pszReason);
	OrderResult _Execute(PositionSide eSide, double dQty, double dFill, double dLeverage, double dFeeRate, OrderType eType,
	                     const char* pszReason, double dWalletBefore);
	void _OnFlat(const Position& closed, double dExitPrice, double dCloseFee, const char* pszReason);
	void _CheckOrders();
	bool _Triggered(const LimitOrder& order) const;
	double _Reserved() const;
	double _OpenQty(double dPrice, double dUsdt, double dLeverage) const;
	std::string _CheckLimit(const LimitOrder& order) const;
	OrderResult _PlaceOpenLimit(PositionSide eSide, double dLimit, double dUsdt, double dLeverage);
	OrderResult _PlaceClose(PositionSide eSide, OrderType eType, double dPrice, double dQty);
	void _Remove(size_t nIndex, const std::string& strReason);
	void _CancelCloseOrders(PositionSide eSide, const std::string& strReason);
	Saved _Save() const;
	void _Restore(const Saved& saved);

	ExchangeRules rules;
	Account       account;
	Position      vPositions[2];				//[0] long, [1] short
	Stats         stats;
	std::vector<std::pair<double, double>> vTiers;
	std::vector<std::pair<long long, double>> vFunding;	//(8-hour slot, rate)
	std::string   strOrderLog;

	bool      bLiquidation;
	double    dFundingRate;
	double    dFundingMultiplier;
	long long nFundingSlot;
	bool      bLiquidated;
	long long nTime;
	double    dPrice;

	bool      bLimitThrough;
	double    dLimitTick;
	std::vector<LimitOrder> vOrders;
	std::vector<Fill>       vFills;
	std::vector<ExchangeEvent> vEvents;
	long long nNextOrderId;
	long long nOrdersVersion;

	double    dCycleFees;
	double    dCycleFunding;
	long long nFillOrderId;		//open order being filled (for the events), 0 = market
};
