#include <string>
#include <vector>

#include "Settings.h"
#include "StrategyExport.h"

//=============================
// Test strategy: opens if there is no position, closes at +tp% on the average price.
// It has no internal state (everything comes from account and positions), so a simulation
// stopped and resumed must give exactly the same cycles as a continuous one.
// stop_at_step > 0: at that step it asks the engine to stop (resume test)
// limit_dip > 0: limit orders. Market entry, limit increase limit_dip% against and limit close
// at +tp% of the average price, moved when a fill changes the average price
// side: long (default), short, both (long and short together: position_mode = hedge)
// stop_loss > 0: at the opening a stop that closes the whole position at -stop_loss%
//=============================
class CTestStrategy : public CStrategyBase
{
public:
	StrategyInfo GetInfo() const override
	{
		return StrategyInfo{"TestStrategy", "2.0", "test strategy"};
	}

	bool LoadConfig(const std::string& strConfigFile) override
	{
		CReader ini(strConfigFile, false);
		if(!ini.Get("TEST", "tp", dTp) || !ini.Get("TEST", "leverage", dLeverage) ||
		   !ini.Get("TEST", "order_perc", dOrderPerc) || !ini.Get("TEST", "stop_at_step", dStopAtStep))
		{
			strLastError = "missing keys in [TEST]";
			return false;
		}
		ini.Get("TEST", "limit_dip", dLimitDip);
		ini.Get("TEST", "stop_loss", dStopLoss);
		std::string strSide = "long";
		ini.Get("TEST", "side", strSide);
		bLong  = strSide == "long" || strSide == "both";
		bShort = strSide == "short" || strSide == "both";
		if(!bLong && !bShort)
		{
			strLastError = "side = " + strSide + " is not valid (long, short, both)";
			return false;
		}
		return true;
	}

	bool Init(const InitContext&) override { return true; }

	RunResult Run(StepContext& ctx) override
	{
		if(bLong)
			_RunSide(ctx, PositionSide::Long);
		if(bShort)
			_RunSide(ctx, PositionSide::Short);

		if(dStopAtStep > 0 && ctx.nStep == static_cast<long long>(dStopAtStep))
			return RunResult::Stop;
		return RunResult::Continue;
	}

	void Term() override {}

private:
	static const Position& _Pos(const StepContext& ctx, PositionSide eSide)
	{
		return eSide == PositionSide::Long ? ctx.longPosition : ctx.shortPosition;
	}

	//price at +pct% in favour of the position (negative: against)
	static double _Target(PositionSide eSide, double dAvg, double dPct)
	{
		return eSide == PositionSide::Long ? dAvg * (1.0 + dPct / 100.0) : dAvg * (1.0 - dPct / 100.0);
	}

	OrderResult _Open(StepContext& ctx, PositionSide eSide)
	{
		const double dUsdt = ctx.account.dAvailable * dOrderPerc / 100.0;
		return eSide == PositionSide::Long ? ctx.orders.MarketBuy(dUsdt, dLeverage) : ctx.orders.MarketSell(dUsdt, dLeverage);
	}

	void _RunSide(StepContext& ctx, PositionSide eSide)
	{
		if(dLimitDip > 0)
		{
			_RunLimit(ctx, eSide);
			return;
		}
		const Position& p = _Pos(ctx, eSide);
		if(!p.bOpen)
		{
			if(_Open(ctx, eSide).bOk && dStopLoss > 0)
				ctx.orders.StopClose(eSide, _Target(eSide, _Pos(ctx, eSide).dAvgPrice, -dStopLoss), 0);
		}
		else if(eSide == PositionSide::Long ? ctx.dPrice >= _Target(eSide, p.dAvgPrice, dTp)
		                                    : ctx.dPrice <= _Target(eSide, p.dAvgPrice, dTp))
		{
			ctx.orders.ClosePosition(eSide);
		}
	}

	void _RunLimit(StepContext& ctx, PositionSide eSide)
	{
		const Position& p = _Pos(ctx, eSide);
		if(!p.bOpen)
		{
			if(bLong != bShort)
			{
				if(!ctx.vOpenOrders.empty())
					ctx.orders.CancelAll();
			}
			else
			{
				for(const LimitOrder& order : std::vector<LimitOrder>(ctx.vOpenOrders))
					if(order.ePositionSide == eSide)
						ctx.orders.Cancel(order.nId);
			}
			if(!_Open(ctx, eSide).bOk)
				return;
			const double dAvg  = _Pos(ctx, eSide).dAvgPrice;
			const double dUsdt = ctx.account.dAvailable * dOrderPerc / 100.0;
			if(eSide == PositionSide::Long)
			{
				ctx.orders.LimitBuy(_Target(eSide, dAvg, -dLimitDip), dUsdt, dLeverage);
				ctx.orders.LimitClose(_Target(eSide, dAvg, dTp));
			}
			else
			{
				ctx.orders.LimitSell(_Target(eSide, dAvg, -dLimitDip), dUsdt, dLeverage);
				ctx.orders.LimitReduce(eSide, _Target(eSide, dAvg, dTp), _Pos(ctx, eSide).dQty * 1000);
			}
			if(dStopLoss > 0)
				ctx.orders.StopClose(eSide, _Target(eSide, dAvg, -dStopLoss), 0);
			return;
		}
		bool bAdded = false;
		for(const Fill& fill : ctx.vFills)
			bAdded = bAdded || (fill.ePositionSide == eSide && fill.eSide != OrderSide::Close);
		long long nClose = 0;
		for(const LimitOrder& order : ctx.vOpenOrders)
			if(order.eSide == OrderSide::Close && order.eType == OrderType::Limit && order.ePositionSide == eSide)
				nClose = order.nId;
		//increase executed: the close limit follows the average price
		if(bAdded && nClose > 0)
			ctx.orders.Move(nClose, _Target(eSide, p.dAvgPrice, dTp));
	}

	double dTp         = 2;
	double dLeverage   = 1;
	double dOrderPerc  = 50;
	double dStopAtStep = 0;
	double dLimitDip   = 0;
	double dStopLoss   = 0;
	bool   bLong       = true;
	bool   bShort      = false;
};

SXE_EXPORT_STRATEGY(CTestStrategy)
