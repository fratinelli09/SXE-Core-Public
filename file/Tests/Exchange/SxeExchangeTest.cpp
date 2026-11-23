//=============================
// Test of the simulated exchange: short, one-way and hedge, cross and isolated, reductions, stops,
// tiered liquidation, state saving. The expected values are calculated by hand
// (formulas in the comments), not taken from the program.
//=============================
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "CSimExchange.h"
#include "CSimStateFile.h"

namespace
{
	int nFailed = 0;
	int nChecks = 0;

	void Check(bool b, const std::string& strWhat, int nLine)
	{
		++nChecks;
		if(!b)
		{
			++nFailed;
			std::printf("FAILED (line %d): %s\n", nLine, strWhat.c_str());
		}
	}

	void Near(double dGot, double dWant, const std::string& strWhat, int nLine, double dTol = 1e-6)
	{
		++nChecks;
		if(!(std::fabs(dGot - dWant) <= dTol * std::max(1.0, std::fabs(dWant))))
		{
			++nFailed;
			std::printf("FAILED (line %d): %s = %.10f, expected %.10f\n", nLine, strWhat.c_str(), dGot, dWant);
		}
	}

#define CHECK(b)          Check((b), #b, __LINE__)
#define NEAR(a, b)        Near((a), (b), #a, __LINE__)

	ExchangeRules Rules(MarginMode eMargin, PositionMode eMode, double dSlippage = 0)
	{
		ExchangeRules r;
		r.eMarginMode        = eMargin;
		r.ePositionMode      = eMode;
		r.dFeeTaker          = 0.0006;
		r.dFeeMaker          = 0.0002;
		r.dSlippage          = dSlippage;
		r.dMaintenanceMargin = 0.004;
		r.dQtyStep           = 0.001;
		r.dMinQty            = 0.001;
		r.dMinNotional       = 5;
		return r;
	}

	std::vector<std::pair<double, double>> Tiers()
	{
		return {{200000, 0.004}, {1000000, 0.005}, {5000000, 0.007}};
	}

	int Count(const std::vector<ExchangeEvent>& v, ExchangeEvent::Kind eKind)
	{
		int n = 0;
		for(const ExchangeEvent& e : v)
			n += e.eKind == eKind;
		return n;
	}

	//short opened and closed at market: slippage in favour of the buyer, PnL inverted
	void ShortOpenClose()
	{
		CSimExchange ex(Rules(MarginMode::Isolated, PositionMode::OneWay, 0.0001), 10000);
		ex.Refresh(1000, 100);
		const OrderResult open = ex.MarketSell(1000, 5);			//qty = 1000*5/100 = 50
		CHECK(open.bOk);
		const Position& s = ex.GetPosition(PositionSide::Short);
		CHECK(s.bOpen && s.eSide == PositionSide::Short);
		NEAR(s.dQty, 50);
		NEAR(s.dAvgPrice, 99.99);									//100 * (1 - 0.0001)
		NEAR(s.dMargin, 50 * 99.99 / 5);
		NEAR(ex.GetAccount().dWallet, 10000 - 50 * 99.99 * 0.0006);
		CHECK(ex.GetPosition().eSide == PositionSide::Short);		//one-way: the open position
		CHECK(!ex.GetPosition(PositionSide::Long).bOpen);

		ex.Refresh(2000, 90);
		NEAR(s.dUnrealizedPnL, (99.99 - 90) * 50);
		const OrderResult close = ex.Close();
		CHECK(close.bOk);
		const double dFill = 90 * 1.0001;
		NEAR(close.dFillPrice, dFill);
		NEAR(ex.GetAccount().dWallet, 10000 - 50 * 99.99 * 0.0006 + (99.99 - dFill) * 50 - 50 * dFill * 0.0006);
		CHECK(!ex.AnyOpen());
		CHECK(ex.GetStats().nWins == 1);

		//opening, close (fill) and end of the cycle
		const std::vector<ExchangeEvent> v = ex.TakeEvents();
		CHECK(v.size() == 3 && v[0].eKind == ExchangeEvent::Kind::Open && v[1].eKind == ExchangeEvent::Kind::Close &&
		      v[2].eKind == ExchangeEvent::Kind::Flat);
		if(v.size() == 3)
		{
			NEAR(v[0].dFee, 50 * 99.99 * 0.0006);
			NEAR(v[1].dPnL, (99.99 - dFill) * 50);
			NEAR(v[1].dFee, 50 * dFill * 0.0006);
			CHECK(v[2].strReason == "strategy");
			NEAR(v[2].dCloseFee, 50 * dFill * 0.0006);
			NEAR(v[2].positionBefore.dFeesPaid, 50 * 99.99 * 0.0006);
			CHECK(v[2].positionBefore.eSide == PositionSide::Short);
		}
	}

	//positive funding: the short receives, in isolated the margin grows
	void ShortFunding()
	{
		CSimExchange ex(Rules(MarginMode::Isolated, PositionMode::OneWay), 10000);
		ex.SetSimRules(true, 0.0001, 1);
		ex.ApplyFunding(0, 100);									//first slot: only recorded
		ex.Refresh(10, 100);
		ex.MarketSell(1000, 10);									//100 contracts
		const double dWallet = ex.GetAccount().dWallet;
		const double dMargin = ex.GetPosition(PositionSide::Short).dMargin;
		ex.ApplyFunding(8 * 3600, 100);
		NEAR(ex.GetAccount().dWallet, dWallet + 100 * 100 * 0.0001);
		NEAR(ex.GetPosition(PositionSide::Short).dMargin, dMargin + 1);
		NEAR(ex.GetPosition(PositionSide::Short).dFundingPaid, -1);
		NEAR(ex.CycleFunding(), -1);
	}

	//one-way: a sell larger than the long closes it and opens a short with the rest
	void OneWayFlip()
	{
		CSimExchange ex(Rules(MarginMode::Isolated, PositionMode::OneWay), 10000);
		ex.Refresh(1000, 100);
		ex.MarketBuy(100, 10);										//long 10
		ex.TakeEvents();
		const OrderResult r = ex.MarketSell(150, 10);				//15: closes 10, short 5
		CHECK(r.bOk);
		NEAR(r.dQty, 15);
		CHECK(!ex.GetPosition(PositionSide::Long).bOpen);
		CHECK(ex.GetPosition(PositionSide::Short).bOpen);
		NEAR(ex.GetPosition(PositionSide::Short).dQty, 5);
		NEAR(ex.GetPosition(PositionSide::Short).dMargin, 5 * 100 / 10.0);
		CHECK(ex.GetStats().nClosed == 1);

		const std::vector<ExchangeEvent> v = ex.TakeEvents();
		CHECK(v.size() == 3 && v[0].eKind == ExchangeEvent::Kind::Close && v[1].eKind == ExchangeEvent::Kind::Flat &&
		      v[2].eKind == ExchangeEvent::Kind::Open);
		if(v.size() == 3)
		{
			//the new cycle starts from the balance after the close of the long
			NEAR(v[2].dWalletBefore, v[1].dWalletAfter);
			CHECK(v[2].eSide == PositionSide::Short);
			NEAR(v[2].dPositionQty, 5);
		}

		//a sell smaller than the short increases it; a smaller buy reduces it
		ex.MarketBuy(20, 10);										//2: short from 5 to 3
		NEAR(ex.GetPosition(PositionSide::Short).dQty, 3);
		CHECK(!ex.GetPosition(PositionSide::Long).bOpen);
	}

	//one-way: if the margin is not enough for the new part, the order does nothing
	void OneWayFlipRejected()
	{
		CSimExchange ex(Rules(MarginMode::Isolated, PositionMode::OneWay), 200);
		ex.Refresh(1000, 100);
		ex.MarketBuy(150, 10);										//long 15, margin 150
		ex.TakeEvents();
		const double dWallet = ex.GetAccount().dWallet;
		const OrderResult r = ex.MarketSell(1000, 10);				//100: closes 15, 85 short would be needed
		CHECK(!r.bOk);
		CHECK(ex.GetPosition(PositionSide::Long).bOpen);
		NEAR(ex.GetPosition(PositionSide::Long).dQty, 15);
		NEAR(ex.GetAccount().dWallet, dWallet);
		CHECK(ex.TakeEvents().empty());
		CHECK(ex.GetStats().nClosed == 0);
	}

	//hedge isolated: long and short together; the short liquidated does not touch the long
	void HedgeIsolated()
	{
		CSimExchange ex(Rules(MarginMode::Isolated, PositionMode::Hedge), 10000);
		ex.SetMaintenanceTiers(Tiers());
		ex.Refresh(1000, 100);
		ex.MarketBuy(500, 10);										//long 50
		ex.MarketSell(500, 20);										//short 100, margin 500
		CHECK(ex.GetPosition(PositionSide::Long).bOpen && ex.GetPosition(PositionSide::Short).bOpen);
		CHECK(ex.GetPosition().eSide == PositionSide::Long);		//hedge: GetPosition() is the long

		//liquidation price of the short: margin + qty*(avg - P) = 0.004*qty*P + 0.0006*qty*P
		const Position& s = ex.GetPosition(PositionSide::Short);
		const double dLiq = (s.dMargin + s.dQty * s.dAvgPrice) / (s.dQty * (1 + 0.004 + 0.0006));
		NEAR(s.dLiquidationPrice, dLiq);
		ex.TakeEvents();

		const double dWallet = ex.GetAccount().dWallet;
		ex.Refresh(2000, dLiq * 0.9999);
		CHECK(ex.GetPosition(PositionSide::Short).bOpen);
		ex.Refresh(3000, dLiq * 1.0001);
		CHECK(!ex.GetPosition(PositionSide::Short).bOpen);
		CHECK(ex.GetPosition(PositionSide::Long).bOpen);
		NEAR(ex.GetAccount().dWallet, dWallet - 500);
		CHECK(ex.TakeLiquidated());
		const std::vector<ExchangeEvent> v = ex.TakeEvents();
		CHECK(Count(v, ExchangeEvent::Kind::Liquidation) == 1 && Count(v, ExchangeEvent::Kind::Flat) == 0);
	}

	//liquidation price of long and short, isolated and cross: nothing just before, liquidated just after
	void LiquidationPrices()
	{
		for(MarginMode eMargin : {MarginMode::Isolated, MarginMode::Crossed})
			for(PositionSide eSide : {PositionSide::Long, PositionSide::Short})
			{
				CSimExchange ex(Rules(eMargin, PositionMode::OneWay), 1000);
				ex.SetMaintenanceTiers(Tiers());
				ex.Refresh(1000, 100);
				if(eSide == PositionSide::Long)
					ex.MarketBuy(500, 20);
				else
					ex.MarketSell(500, 20);
				const double dLiq = ex.GetPosition().dLiquidationPrice;
				CHECK(dLiq > 0);
				const double dSafe = eSide == PositionSide::Long ? dLiq * 1.0001 : dLiq * 0.9999;
				const double dGone = eSide == PositionSide::Long ? dLiq * 0.9999 : dLiq * 1.0001;
				ex.Refresh(2000, dSafe);
				CHECK(ex.AnyOpen());
				ex.Refresh(3000, dGone);
				CHECK(!ex.AnyOpen());
				//isolated: the margin is lost; cross: everything is lost
				if(eMargin == MarginMode::Crossed)
					NEAR(ex.GetAccount().dWallet, 0);
				else
					NEAR(ex.GetAccount().dWallet, 1000 - 500 * 20 * 0.0006 - 500);
				const std::vector<ExchangeEvent> v = ex.TakeEvents();
				CHECK(Count(v, ExchangeEvent::Kind::Flat) == 1);
				CHECK(!v.empty() && v.back().strReason == "liquidation");
			}
	}

	//hedge cross: long and short offset each other before the liquidation
	void HedgeCrossNetting()
	{
		CSimExchange ex(Rules(MarginMode::Crossed, PositionMode::Hedge), 1000);
		ex.SetMaintenanceTiers(Tiers());
		ex.Refresh(1000, 100);
		ex.MarketBuy(500, 20);										//long 100
		ex.MarketSell(300, 20);										//short 60
		NEAR(ex.GetAccount().dWallet, 1000 - 6 - 3.6);
		//balance + 100*(P-100) + 60*(100-P) = 0.004*100*P + 0.0006*100*P  ->  P = 3009.6 / 39.54
		NEAR(ex.GetPosition(PositionSide::Long).dLiquidationPrice, 3009.6 / 39.54);
		NEAR(ex.GetPosition(PositionSide::Short).dLiquidationPrice, 3009.6 / 39.54);
		ex.TakeEvents();

		ex.Refresh(2000, 76);
		//60 per side closed at 76 with the taker fee: a long of 40 is left, healthy
		CHECK(!ex.GetPosition(PositionSide::Short).bOpen);
		CHECK(ex.GetPosition(PositionSide::Long).bOpen);
		NEAR(ex.GetPosition(PositionSide::Long).dQty, 40);
		NEAR(ex.GetAccount().dWallet, 1000 - 6 - 3.6 - 60 * 76 * 0.0006 * 2);
		CHECK(ex.GetStats().nPartialLiquidations == 2);
		CHECK(ex.GetStats().nLiquidations == 0);
		CHECK(ex.TakeLiquidated() == false);
		CHECK(Count(ex.TakeEvents(), ExchangeEvent::Kind::PartialLiquidation) == 2);
	}

	//cross beyond the first tier: it steps down one tier instead of liquidating everything
	void CrossTierReduction()
	{
		CSimExchange ex(Rules(MarginMode::Crossed, PositionMode::OneWay), 30000);
		ex.SetMaintenanceTiers(Tiers());
		ex.Refresh(1000, 100);
		ex.MarketBuy(20000, 20);									//4000 contracts, 400,000 USDT
		NEAR(ex.GetPosition().dQty, 4000);
		ex.TakeEvents();

		ex.Refresh(2000, 93);
		//372,000 USDT in the second tier: reduced to 200,000 / 93 = 2150.537 (step 0.001 rounded up)
		const double dReduce = std::ceil((4000 - 200000.0 / 93) / 0.001 - 1e-9) * 0.001;
		CHECK(ex.AnyOpen());
		NEAR(ex.GetPosition().dQty, 4000 - dReduce);
		NEAR(ex.GetAccount().dWallet, 30000 - 240 + (93 - 100) * dReduce - dReduce * 93 * 0.0006);
		CHECK(ex.GetStats().nPartialLiquidations == 1 && ex.GetStats().nLiquidations == 0);
	}

	//reduction of a part: PnL realized, margin released in proportion, average unchanged
	void PartialReduce()
	{
		CSimExchange ex(Rules(MarginMode::Isolated, PositionMode::OneWay), 10000);
		ex.Refresh(1000, 100);
		ex.MarketBuy(100, 10);
		ex.Refresh(2000, 110);
		const double dWallet = ex.GetAccount().dWallet;
		const OrderResult r = ex.Reduce(PositionSide::Long, 4);
		CHECK(r.bOk);
		const Position& p = ex.GetPosition();
		NEAR(p.dQty, 6);
		NEAR(p.dAvgPrice, 100);
		NEAR(p.dMargin, 60);
		NEAR(p.dRealizedPnL, 40);
		NEAR(ex.GetAccount().dWallet, dWallet + 40 - 4 * 110 * 0.0006);
		CHECK(ex.Reduce(PositionSide::Short, 1).bOk == false);		//no short
		CHECK(ex.Reduce(PositionSide::Long, 100).bOk);				//more than the position: closes it
		CHECK(!ex.AnyOpen());
	}

	//stop loss: triggers when the price falls to the trigger, closes at market
	void StopLoss()
	{
		CSimExchange ex(Rules(MarginMode::Isolated, PositionMode::OneWay), 10000);
		ex.Refresh(1000, 100);
		ex.MarketBuy(100, 10);
		CHECK(!ex.StopClose(PositionSide::Long, 100, 0).bOk);		//trigger at the price: rejected
		const OrderResult stop = ex.StopClose(PositionSide::Long, 95, 0);
		CHECK(stop.bOk && stop.eType == OrderType::Stop);
		const OrderResult tp = ex.StopClose(PositionSide::Long, 110, 4);	//partial take profit above
		CHECK(tp.bOk);
		ex.TakeEvents();

		ex.Refresh(2000, 96);
		CHECK(ex.AnyOpen() && ex.GetOpenOrders().size() == 2);
		ex.Refresh(3000, 111);
		NEAR(ex.GetPosition().dQty, 6);
		ex.Refresh(4000, 94.9);
		CHECK(!ex.AnyOpen());
		CHECK(ex.GetOpenOrders().empty());
		const std::vector<Fill> vFills = ex.TakeFills();
		CHECK(vFills.size() == 2 && vFills.back().eType == OrderType::Stop);
		const std::vector<ExchangeEvent> v = ex.TakeEvents();
		CHECK(!v.empty() && v.back().eKind == ExchangeEvent::Kind::Flat && v.back().strReason == "stop");
	}

	//limits: a sell above the market opens the short, a close below the market closes it
	void LimitShort()
	{
		CSimExchange ex(Rules(MarginMode::Isolated, PositionMode::OneWay), 10000);
		ex.SetLimitRules(false, 0);
		ex.Refresh(1000, 100);
		CHECK(!ex.LimitSell(99, 100, 10).bOk);						//below the market: rejected
		const OrderResult r = ex.LimitSell(105, 100, 10);
		CHECK(r.bOk);
		NEAR(r.dQty, 9.523);										//100*10/105 = 9.5238 at step 0.001
		ex.Refresh(2000, 104);
		CHECK(!ex.AnyOpen());
		ex.Refresh(3000, 105.5);
		CHECK(ex.GetPosition(PositionSide::Short).bOpen);
		NEAR(ex.GetPosition().dAvgPrice, 105);
		CHECK(!ex.LimitClose(106).bOk);								//close of a short above the market
		CHECK(ex.LimitClose(95).bOk);
		CHECK(ex.LimitReduce(PositionSide::Short, 98, 2).bOk);
		ex.Refresh(4000, 97);
		NEAR(ex.GetPosition().dQty, 9.523 - 2);
		ex.Refresh(5000, 94);
		CHECK(!ex.AnyOpen());
		const std::vector<ExchangeEvent> v = ex.TakeEvents();
		CHECK(!v.empty() && v.back().strReason == "limit");
		NEAR(ex.GetAccount().dWallet, 10000 - 9.523 * 105 * 0.0002 + (105 - 98) * 2 - 2 * 98 * 0.0002
		                                + (105 - 95) * 7.523 - 7.523 * 95 * 0.0002);
	}

	//hedge: a sell with the long open opens the short, it does not reduce the long
	void HedgeKeepsBoth()
	{
		CSimExchange ex(Rules(MarginMode::Crossed, PositionMode::Hedge), 10000);
		ex.Refresh(1000, 100);
		ex.MarketBuy(100, 10);
		ex.MarketSell(50, 10);
		NEAR(ex.GetPosition(PositionSide::Long).dQty, 10);
		NEAR(ex.GetPosition(PositionSide::Short).dQty, 5);
		ex.ClosePosition(PositionSide::Long);
		CHECK(ex.AnyOpen());
		const std::vector<ExchangeEvent> v = ex.TakeEvents();
		CHECK(Count(v, ExchangeEvent::Kind::Flat) == 0);
		ex.Close();
		CHECK(!ex.AnyOpen());
		CHECK(Count(ex.TakeEvents(), ExchangeEvent::Kind::Flat) == 1);
	}

	//state saved and reloaded: positions, orders (stops too) and totals of the cycle
	void StateRoundTrip()
	{
		CSimExchange ex(Rules(MarginMode::Isolated, PositionMode::Hedge), 10000);
		ex.Refresh(1000, 100);
		ex.MarketBuy(100, 10);
		ex.MarketSell(200, 5);
		ex.Reduce(PositionSide::Short, 3);
		ex.StopClose(PositionSide::Short, 120, 0);
		ex.LimitBuy(90, 100, 10);

		SimState state;
		state.strDataFile = "x.csv";
		state.strStrategy = "test";
		state.exchange    = ex.GetSnapshot();
		const std::string strFile = (std::filesystem::temp_directory_path() / "sxe_exchange_test_state.txt").string();
		std::string strError;
		CHECK(CSimStateFile::Save(strFile, state, strError));
		SimState loaded;
		CHECK(CSimStateFile::Load(strFile, loaded, strError));
		std::filesystem::remove(strFile);

		CSimExchange copy(Rules(MarginMode::Isolated, PositionMode::Hedge), 0);
		copy.Restore(loaded.exchange);
		for(PositionSide eSide : {PositionSide::Long, PositionSide::Short})
		{
			const Position& a = ex.GetPosition(eSide);
			const Position& b = copy.GetPosition(eSide);
			CHECK(a.bOpen == b.bOpen && a.eSide == b.eSide);
			NEAR(b.dQty, a.dQty);
			NEAR(b.dAvgPrice, a.dAvgPrice);
			NEAR(b.dMargin, a.dMargin);
			NEAR(b.dRealizedPnL, a.dRealizedPnL);
		}
		NEAR(copy.GetAccount().dWallet, ex.GetAccount().dWallet);
		NEAR(copy.CycleFees(), ex.CycleFees());
		CHECK(copy.GetOpenOrders().size() == 2);
		if(copy.GetOpenOrders().size() == 2)
		{
			const LimitOrder& o = copy.GetOpenOrders()[0];
			CHECK(o.eType == OrderType::Stop && o.eSide == OrderSide::Close && o.ePositionSide == PositionSide::Short && o.bAbove && o.bAll);
		}
	}
}

int main()
{
	ShortOpenClose();
	ShortFunding();
	OneWayFlip();
	OneWayFlipRejected();
	HedgeIsolated();
	LiquidationPrices();
	HedgeCrossNetting();
	CrossTierReduction();
	PartialReduce();
	StopLoss();
	LimitShort();
	HedgeKeepsBoth();
	StateRoundTrip();

	std::printf("%d checks, %d failed\n", nChecks, nFailed);
	return nFailed == 0 ? 0 : 1;
}
