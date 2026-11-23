#pragma once
#include <string>
#include <vector>

#include "Types.h"

//=============================
// What the exchange reports to the engine, in order, for the cycle archive
//=============================
struct ExchangeEvent
{
	enum class Kind
	{
		Open,				//fill that opens or increases a position
		Flat,				//no position is left open: the cycle is over
		Liquidation,		//a position was liquidated (all of it)
		PartialLiquidation,	//cross: a position was reduced by one tier to avoid the liquidation
		Reduce,				//fill that reduces a position (it stays open)
		Close				//fill that closes a whole position
	};

	Kind         eKind         = Kind::Open;
	long long    nTime         = 0;
	PositionSide eSide         = PositionSide::Long;
	double       dPrice        = 0;		//Open: fill price; Flat: exit price; liquidation: price
	double       dQty          = 0;		//Open, PartialLiquidation: quantity
	double       dWalletBefore = 0;		//Open: balance before the fill
	double       dWalletAfter  = 0;		//Flat, liquidations: balance after
	double       dCloseFee     = 0;		//Flat: fee of the last close
	//Flat: the last position closed, with the fees and funding of the whole cycle (without dCloseFee);
	//liquidations: the position as it was before
	Position     positionBefore;
	std::string  strReason;				//Flat, Reduce, Close: "strategy", "limit", "stop", "liquidation"
	//fills (Open, Reduce, Close, liquidations): fee, gross realized PnL (Reduce, Close,
	//PartialLiquidation), type and id of the order (0 = market), position after
	double       dFee          = 0;
	double       dPnL          = 0;
	OrderType    eType         = OrderType::Market;
	long long    nOrderId      = 0;
	double       dPositionQty  = 0;
	double       dAvgPrice     = 0;
};

//=============================
// The exchange as the engine sees it: simulated (CSimExchange) or live (private plugin)
// The engine uses it only from its own thread.
//=============================
class IExchange
{
public:
	virtual ~IExchange(){}

	//updates account and positions at the current step.
	//Sim: applies the price; live: reads account and positions again.
	//false = data not available (e.g. network error): the step is skipped
	virtual bool Refresh(long long nTime, double dPrice) = 0;

	virtual const Account&  GetAccount() const = 0;
	//one-way: the position (long or short); hedge: the long position
	virtual const Position& GetPosition() const = 0;
	virtual const Position& GetPosition(PositionSide eSide) const = 0;
	//at least one position open
	virtual bool AnyOpen() const = 0;
	virtual const ExchangeRules& GetRules() const = 0;

	//true if a position was liquidated since the last call (the flag is cleared)
	virtual bool TakeLiquidated() = 0;

	virtual OrderResult MarketBuy(double dUsdt, double dLeverage) = 0;
	virtual OrderResult MarketSell(double dUsdt, double dLeverage) = 0;
	virtual OrderResult Close() = 0;
	virtual OrderResult ClosePosition(PositionSide eSide) = 0;
	virtual OrderResult Reduce(PositionSide eSide, double dQty) = 0;

	virtual OrderResult LimitBuy(double dPrice, double dUsdt, double dLeverage) = 0;
	virtual OrderResult LimitSell(double dPrice, double dUsdt, double dLeverage) = 0;
	virtual OrderResult LimitClose(double dPrice) = 0;
	virtual OrderResult LimitReduce(PositionSide eSide, double dPrice, double dQty) = 0;
	virtual OrderResult StopClose(PositionSide eSide, double dTrigger, double dQty) = 0;
	virtual OrderResult Move(long long nOrderId, double dPrice) = 0;
	virtual bool Cancel(long long nOrderId) = 0;
	virtual void CancelAll() = 0;

	virtual const std::vector<LimitOrder>& GetOpenOrders() const = 0;
	//open orders filled since the last call (the list is emptied)
	virtual std::vector<Fill> TakeFills() = 0;
	//events for the cycle archive since the last call, in order (the list is emptied)
	virtual std::vector<ExchangeEvent> TakeEvents() = 0;
};
