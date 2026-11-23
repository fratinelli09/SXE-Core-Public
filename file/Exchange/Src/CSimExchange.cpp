#include "CSimExchange.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>

#include "TimeUtils.h"

namespace
{
	const long long FUNDING_PERIOD = 8 * 3600;

	bool ToDouble(const std::string& s, double& d)
	{
		const char* pBegin = s.data();
		const char* pEnd   = s.data() + s.size();
		while(pBegin < pEnd && *pBegin == ' ')
			++pBegin;
		const std::from_chars_result r = std::from_chars(pBegin, pEnd, d);
		return r.ec == std::errc() && r.ptr != pBegin;
	}

	const char* SideName(PositionSide eSide)
	{
		return eSide == PositionSide::Long ? "long" : "short";
	}
}

CSimExchange::CSimExchange(const ExchangeRules& rules_, double dInitialCapital):
	rules(rules_),
	account(),
	vPositions(),
	stats(),
	bLiquidation(true),
	dFundingRate(0),
	dFundingMultiplier(1),
	nFundingSlot(-1),
	bLiquidated(false),
	nTime(0),
	dPrice(0),
	bLimitThrough(true),
	dLimitTick(0),
	nNextOrderId(1),
	nOrdersVersion(0),
	dCycleFees(0),
	dCycleFunding(0),
	nFillOrderId(0)
{
	vPositions[1].eSide = PositionSide::Short;
	account.dWallet = dInitialCapital;
	_Update();
}

void CSimExchange::SetSimRules(bool bLiquidation_, double dFundingRate_, double dFundingMultiplier_)
{
	bLiquidation       = bLiquidation_;
	dFundingRate       = dFundingRate_;
	dFundingMultiplier = dFundingMultiplier_;
}

void CSimExchange::SetLimitRules(bool bThrough, double dTickStep)
{
	bLimitThrough = bThrough;
	dLimitTick    = dTickStep;
}

void CSimExchange::SetMaintenanceTiers(const std::vector<std::pair<double, double>>& vNewTiers)
{
	vTiers = vNewTiers;
	_Update();
}

const Position& CSimExchange::GetPosition() const
{
	//one-way: the only position, whatever its side; hedge: the long
	if(!_Hedge() && !vPositions[0].bOpen && vPositions[1].bOpen)
		return vPositions[1];
	return vPositions[0];
}

double CSimExchange::MaintenanceMargin(double dNotional) const
{
	if(vTiers.empty())
		return dNotional * rules.dMaintenanceMargin;

	double dMargin = 0, dLow = 0;
	for(const auto& tier : vTiers)
	{
		if(dNotional <= tier.first)
			return dMargin + (dNotional - dLow) * tier.second;
		dMargin += (tier.first - dLow) * tier.second;
		dLow = tier.first;
	}
	return dMargin + (dNotional - dLow) * vTiers.back().second;
}

double CSimExchange::_Collateral(const Position& p) const
{
	return _Isolated() ? p.dMargin : account.dWallet;
}

double CSimExchange::_PnL(const Position& p, double dAt) const
{
	return p.eSide == PositionSide::Long ? (dAt - p.dAvgPrice) * p.dQty : (p.dAvgPrice - dAt) * p.dQty;
}

double CSimExchange::_LiquidationPrice(const Position& p) const
{
	//price P at which collateral + PnL(P) = maintenance margin(qty*P) + fee*qty*P.
	//In tier i the margin is (margin of the previous bands) + rate_i * (qty*P - start_i):
	//it is solved tier by tier, keeping the solution that falls in its own tier.
	//Long: collateral + qty*(P - avg); short: collateral + qty*(avg - P)
	const double dQty = p.dQty;
	if(dQty <= 0)
		return 0;

	const bool bLong = p.eSide == PositionSide::Long;
	if(!_Isolated() && _Hedge() && vPositions[0].bOpen && vPositions[1].bOpen)
		return _CrossHedgeLiquidationPrice();

	if(bLong)
	{
		const double dFixed = dQty * p.dAvgPrice - _Collateral(p);
		if(vTiers.empty())
		{
			const double dDen = dQty * (1.0 - rules.dMaintenanceMargin - rules.dFeeTaker);
			return (dDen > 0 && dFixed > 0) ? dFixed / dDen : 0;
		}

		double dCum = 0, dLow = 0;
		for(size_t i = 0; i <= vTiers.size(); i++)
		{
			const bool   bLast = (i == vTiers.size());
			const double dRate = bLast ? vTiers.back().second : vTiers[i].second;
			const double dDen  = dQty * (1.0 - dRate - rules.dFeeTaker);
			if(dDen <= 0)
				return 0;

			const double dPrice = (dFixed + dCum - dRate * dLow) / dDen;
			if(dPrice <= 0)
				return 0;
			if(bLast || dQty * dPrice <= vTiers[i].first)
				return dPrice;

			dCum += (vTiers[i].first - dLow) * dRate;
			dLow  = vTiers[i].first;
		}
		return 0;
	}

	//short: collateral + qty*avg - cum + rate*start = qty*P*(1 + rate + fee)
	const double dFixed = dQty * p.dAvgPrice + _Collateral(p);
	if(vTiers.empty())
	{
		const double dDen = dQty * (1.0 + rules.dMaintenanceMargin + rules.dFeeTaker);
		return (dDen > 0 && dFixed > 0) ? dFixed / dDen : 0;
	}

	double dCum = 0, dLow = 0;
	for(size_t i = 0; i <= vTiers.size(); i++)
	{
		const bool   bLast = (i == vTiers.size());
		const double dRate = bLast ? vTiers.back().second : vTiers[i].second;
		const double dDen  = dQty * (1.0 + dRate + rules.dFeeTaker);
		const double dPrice = (dFixed - dCum + dRate * dLow) / dDen;
		if(dPrice <= 0)
			return 0;
		if(bLast || dQty * dPrice <= vTiers[i].first)
			return dPrice;

		dCum += (vTiers[i].first - dLow) * dRate;
		dLow  = vTiers[i].first;
	}
	return 0;
}

//cross hedge with long and short open: balance + long PnL + short PnL = maintenance of the larger
//side + fee. f(P) = A + B*P in every tier; the zero is searched on the losing side
double CSimExchange::_CrossHedgeLiquidationPrice() const
{
	const Position& l = vPositions[0];
	const Position& s = vPositions[1];
	const double dBig = std::max(l.dQty, s.dQty);
	if(dBig <= 0)
		return 0;

	const double dNet  = l.dQty - s.dQty;
	double dBest = 0;
	auto consider = [&](double dRate, double dCum, double dLow, double dHigh)
	{
		const double dA = account.dWallet - l.dQty * l.dAvgPrice + s.dQty * s.dAvgPrice - dCum + dRate * dLow;
		const double dB = dNet - (dRate + rules.dFeeTaker) * dBig;
		if(dB == 0)
			return;
		const double dP = -dA / dB;
		const double dNotional = dBig * dP;
		if(dP <= 0 || dNotional < dLow || (dHigh > 0 && dNotional > dHigh))
			return;
		//net long: liquidated going down (below the price), net short or even: going up
		const bool bAdverse = dNet > 0 ? dP <= dPrice : dP >= dPrice;
		if(!bAdverse)
			return;
		if(dBest <= 0 || std::fabs(dP - dPrice) < std::fabs(dBest - dPrice))
			dBest = dP;
	};

	if(vTiers.empty())
	{
		consider(rules.dMaintenanceMargin, 0, 0, 0);
		return dBest;
	}
	double dCum = 0, dLow = 0;
	for(size_t i = 0; i <= vTiers.size(); i++)
	{
		const bool   bLast = (i == vTiers.size());
		const double dRate = bLast ? vTiers.back().second : vTiers[i].second;
		consider(dRate, dCum, dLow, bLast ? 0 : vTiers[i].first);
		if(bLast)
			break;
		dCum += (vTiers[i].first - dLow) * dRate;
		dLow  = vTiers[i].first;
	}
	return dBest;
}

bool CSimExchange::LoadFunding(const std::string& strFile, std::string& strError)
{
	vFunding.clear();
	if(strFile.empty())
		return true;

	std::ifstream csv(strFile);
	if(!csv)
	{
		strError = "Funding history cannot be read: " + strFile;
		return false;
	}

	std::string strLine;
	while(std::getline(csv, strLine))
	{
		const size_t nSep = strLine.find_first_of(",;");
		if(nSep == std::string::npos)
			continue;

		const long long nTimeRow = TimeUtils::Parse(strLine.substr(0, nSep));
		double dRate = 0;
		std::string strRate = strLine.substr(nSep + 1);
		if(!strRate.empty() && strRate.back() == '\r')
			strRate.pop_back();
		if(nTimeRow <= 0 || !ToDouble(strRate, dRate))
			continue;

		vFunding.emplace_back(nTimeRow / FUNDING_PERIOD, dRate);
	}
	std::sort(vFunding.begin(), vFunding.end());

	if(vFunding.empty())
	{
		strError = "No valid funding rate in " + strFile;
		return false;
	}
	return true;
}

bool CSimExchange::SetOrderLog(const std::string& strFile, bool bAppend)
{
	strOrderLog = strFile;

	std::ofstream orderLog(strOrderLog, bAppend ? std::ios::app : std::ios::trunc);
	return static_cast<bool>(orderLog);
}

double CSimExchange::_FundingRate(long long nSlot) const
{
	const auto it = std::lower_bound(vFunding.begin(), vFunding.end(), std::make_pair(nSlot, -1e300));
	if(it != vFunding.end() && it->first == nSlot)
		return it->second * dFundingMultiplier;
	return dFundingRate * dFundingMultiplier;
}

void CSimExchange::ApplyFunding(long long nCandleTime, double dMark)
{
	const long long nSlot = nCandleTime / FUNDING_PERIOD;

	//first slot seen, no position or slot already counted: only the slot is recorded
	if(nFundingSlot < 0 || !AnyOpen() || nSlot <= nFundingSlot)
	{
		if(nSlot > nFundingSlot)
			nFundingSlot = nSlot;
		return;
	}

	for(Position& p : vPositions)
	{
		if(!p.bOpen || p.dQty <= 0)
			continue;

		//every slot crossed (more than one if hours are missing in the data); positive rate:
		//the long pays, the short receives
		double dPaid = 0;
		for(long long s = nFundingSlot + 1; s <= nSlot; s++)
			dPaid += p.dQty * dMark * _FundingRate(s);
		if(p.eSide == PositionSide::Short)
			dPaid = -dPaid;

		account.dWallet -= dPaid;
		p.dFundingPaid  += dPaid;
		stats.dFundingPaid += dPaid;
		dCycleFunding   += dPaid;
		//isolated: the funding goes through the margin of the position
		if(_Isolated())
			p.dMargin = std::max(0.0, p.dMargin - dPaid);
	}
	nFundingSlot = nSlot;

	_Update();
}

bool CSimExchange::Refresh(long long nNewTime, double dNewPrice)
{
	nTime  = nNewTime;
	dPrice = dNewPrice;

	//first the orders reached, then the liquidation with the positions updated
	_CheckOrders();

	if(bLiquidation)
		_CheckLiquidation();

	_Update();
	return true;
}

bool CSimExchange::TakeLiquidated()
{
	const bool b = bLiquidated;
	bLiquidated = false;
	return b;
}

bool CSimExchange::_Unsafe(const Position& p) const
{
	const double dEquity = _Collateral(p) + (p.eSide == PositionSide::Long ? p.dQty * (dPrice - p.dAvgPrice)
	                                                                          : p.dQty * (p.dAvgPrice - dPrice));
	const double dMaint  = MaintenanceMargin(p.dQty * dPrice) + p.dQty * dPrice * rules.dFeeTaker;
	return dEquity <= dMaint;
}

bool CSimExchange::_CrossHedgeUnsafe() const
{
	const Position& l = vPositions[0];
	const Position& s = vPositions[1];
	const double dBig    = std::max(l.dQty, s.dQty);
	const double dEquity = account.dWallet + l.dQty * (dPrice - l.dAvgPrice) + s.dQty * (s.dAvgPrice - dPrice);
	const double dMaint  = MaintenanceMargin(dBig * dPrice) + dBig * dPrice * rules.dFeeTaker;
	return dEquity <= dMaint;
}

void CSimExchange::_CheckLiquidation()
{
	if(!AnyOpen())
		return;

	if(_Isolated())
	{
		for(Position& p : vPositions)
			if(p.bOpen && p.dQty > 0 && _Unsafe(p))
				_Liquidate(p.eSide);
		return;
	}

	const bool bBoth = vPositions[0].bOpen && vPositions[1].bOpen;
	if(bBoth ? _CrossHedgeUnsafe() : _Unsafe(_OpenPosition()))
		_LiquidateCross();
}

//isolated: the margin of the position is lost, the rest of the account is not touched
void CSimExchange::_Liquidate(PositionSide eSide)
{
	Position& p = _Pos(eSide);
	const Position before = p;

	account.dWallet = std::max(0.0, account.dWallet - p.dMargin);
	p = Position();
	p.eSide = eSide;
	stats.nLiquidations++;
	bLiquidated = true;

	OrderResult r;
	r.bOk        = true;
	r.dFillPrice = dPrice;
	r.dQty       = before.dQty;
	_LogOrder("LIQUIDATION", r, 0, eSide);

	//at the liquidation the exchange cancels the orders of the position (in one-way all of them)
	for(size_t i = vOrders.size(); i-- > 0; )
		if(!_Hedge() || vOrders[i].ePositionSide == eSide)
			_Remove(i, "liquidation");

	ExchangeEvent e;
	e.eKind          = ExchangeEvent::Kind::Liquidation;
	e.nTime          = nTime;
	e.eSide          = eSide;
	e.dPrice         = dPrice;
	e.dQty           = before.dQty;
	e.dWalletAfter   = account.dWallet;
	e.positionBefore = before;
	e.dPnL           = -before.dMargin;
	e.dAvgPrice      = before.dAvgPrice;
	vEvents.push_back(e);

	if(!AnyOpen())
		_OnFlat(before, dPrice, 0, "liquidation");
	_Update();
}

//cross: orders cancelled, long/short offset (hedge), reduction by one tier at a time,
//checking again; if that is not enough everything is liquidated and the balance goes to zero
void CSimExchange::_LiquidateCross()
{
	while(!vOrders.empty())
		_Remove(vOrders.size() - 1, "liquidation");
	_Update();

	auto unsafe = [this]()
	{
		if(vPositions[0].bOpen && vPositions[1].bOpen)
			return _CrossHedgeUnsafe();
		return AnyOpen() && _Unsafe(_OpenPosition());
	};
	//forced reduction at market (at the current price, taker fee): it does not count as a close
	auto forced = [this](PositionSide eSide, double dQty)
	{
		Position& p = _Pos(eSide);
		dQty = std::min(dQty, p.dQty);
		const Position before = p;
		const double dPnL = _PnL(p, dPrice) * (dQty / p.dQty);
		const double dFee = dQty * dPrice * rules.dFeeTaker;
		account.dWallet    += dPnL - dFee;
		stats.dFeesPaid    += dFee;
		stats.dRealizedPnL += dPnL;
		dCycleFees         += dFee;
		stats.nPartialLiquidations++;
		if(dQty >= p.dQty - 1e-12)
		{
			p = Position();
			p.eSide = eSide;
		}
		else
		{
			p.dMargin      -= p.dMargin * (dQty / p.dQty);
			p.dQty         -= dQty;
			p.dRealizedPnL += dPnL;
			p.dFeesPaid    += dFee;
		}

		OrderResult r;
		r.bOk        = true;
		r.dFillPrice = dPrice;
		r.dQty       = dQty;
		r.dFee       = dFee;
		r.dFeeRate   = rules.dFeeTaker;
		_LogOrder("PARTIAL_LIQUIDATION", r, 0, eSide);

		ExchangeEvent e;
		e.eKind          = ExchangeEvent::Kind::PartialLiquidation;
		e.nTime          = nTime;
		e.eSide          = eSide;
		e.dPrice         = dPrice;
		e.dQty           = dQty;
		e.dWalletAfter   = account.dWallet;
		e.positionBefore = before;
		e.dFee           = dFee;
		e.dPnL           = dPnL;
		e.dPositionQty   = p.bOpen ? p.dQty : 0;
		e.dAvgPrice      = before.dAvgPrice;
		vEvents.push_back(e);
		_Update();
		return before;
	};

	//hedge: long and short of the same symbol offset each other
	if(vPositions[0].bOpen && vPositions[1].bOpen && unsafe())
	{
		const double dNet = std::min(vPositions[0].dQty, vPositions[1].dQty);
		const Position longBefore  = forced(PositionSide::Long, dNet);
		const Position shortBefore = forced(PositionSide::Short, dNet);
		if(!AnyOpen())
		{
			bLiquidated = true;
			_OnFlat(shortBefore, dPrice, 0, "liquidation");
			(void)longBefore;
			return;
		}
	}

	//tiers: from the highest it steps down one at a time until the account is healthy again
	while(unsafe() && !vTiers.empty())
	{
		const Position& p = _OpenPosition();
		const double dNotional = p.dQty * dPrice;
		size_t nTier = 0;
		while(nTier < vTiers.size() && dNotional > vTiers[nTier].first)
			++nTier;
		if(nTier == 0)
			break;		//first tier: full liquidation
		double dTarget = vTiers[nTier - 1].first / dPrice;
		double dReduce = p.dQty - dTarget;
		if(rules.dQtyStep > 0)
			dReduce = std::ceil(dReduce / rules.dQtyStep - 1e-9) * rules.dQtyStep;
		if(dReduce <= 0)
			break;
		const Position before = forced(p.eSide, dReduce);
		if(!AnyOpen())
		{
			bLiquidated = true;
			_OnFlat(before, dPrice, 0, "liquidation");
			return;
		}
	}
	if(!unsafe())
		return;

	//full liquidation: the whole balance is lost (the loss goes on the last position liquidated)
	const double dWalletLost = account.dWallet;
	account.dWallet = 0.0;
	Position last;
	for(Position& p : vPositions)
	{
		if(!p.bOpen)
			continue;
		const Position before = p;
		const PositionSide eSide = p.eSide;
		p = Position();
		p.eSide = eSide;
		stats.nLiquidations++;

		OrderResult r;
		r.bOk        = true;
		r.dFillPrice = dPrice;
		r.dQty       = before.dQty;
		_LogOrder("LIQUIDATION", r, 0, eSide);

		ExchangeEvent e;
		e.eKind          = ExchangeEvent::Kind::Liquidation;
		e.nTime          = nTime;
		e.eSide          = eSide;
		e.dPrice         = dPrice;
		e.dQty           = before.dQty;
		e.dWalletAfter   = account.dWallet;
		e.positionBefore = before;
		e.dAvgPrice      = before.dAvgPrice;
		vEvents.push_back(e);
		last = before;
	}
	for(size_t k = vEvents.size(); k-- > 0; )
		if(vEvents[k].eKind == ExchangeEvent::Kind::Liquidation)
		{
			vEvents[k].dPnL = -dWalletLost;
			break;
		}
	bLiquidated = true;
	_OnFlat(last, dPrice, 0, "liquidation");
	_Update();
}

void CSimExchange::_Update()
{
	double dUnrealized = 0, dMargins = 0, dCrossValue = 0;
	for(Position& p : vPositions)
	{
		if(!p.bOpen)
			continue;
		p.dUnrealizedPnL = (p.eSide == PositionSide::Long) ? (dPrice - p.dAvgPrice) * p.dQty
		                                                   : (p.dAvgPrice - dPrice) * p.dQty;
		dUnrealized += p.dUnrealizedPnL;
		dMargins    += p.dMargin;
		dCrossValue += p.dQty * dPrice / p.dLeverage;
	}
	for(Position& p : vPositions)
		if(p.bOpen)
			p.dLiquidationPrice = _LiquidationPrice(p);

	account.dEquity = account.dWallet + dUnrealized;

	if(!AnyOpen())
		account.dAvailable = account.dWallet;
	else if(_Isolated())
		account.dAvailable = account.dWallet - dMargins;
	else
		account.dAvailable = account.dEquity - dCrossValue;
	account.dAvailable -= _Reserved();

	if(account.dAvailable < 0)
		account.dAvailable = 0;
}

double CSimExchange::_RoundQty(double dQty) const
{
	if(rules.dQtyStep > 0)
		dQty = std::floor(dQty / rules.dQtyStep + 1e-9) * rules.dQtyStep;
	return dQty;
}

double CSimExchange::_FillPrice(bool bBuy) const
{
	return bBuy ? dPrice * (1.0 + rules.dSlippage) : dPrice * (1.0 - rules.dSlippage);
}

OrderResult CSimExchange::MarketBuy(double dUsdt, double dLeverage)
{
	const OrderResult r = _MarketOpen(PositionSide::Long, dUsdt, dLeverage);
	if(r.bOk)
		_LogOrder("BUY", r, 0, PositionSide::Long);
	return r;
}

OrderResult CSimExchange::MarketSell(double dUsdt, double dLeverage)
{
	const OrderResult r = _MarketOpen(PositionSide::Short, dUsdt, dLeverage);
	if(r.bOk)
		_LogOrder("SELL", r, 0, PositionSide::Short);
	return r;
}

//market order opening in the direction eSide (one-way: first reduces the opposite position)
OrderResult CSimExchange::_MarketOpen(PositionSide eSide, double dUsdt, double dLeverage)
{
	if(dPrice <= 0)
		return _Reject("price not available yet");
	if(!(dUsdt > 0))
		return _Reject("invalid amount");
	if(!(dLeverage >= 1))
		return _Reject("invalid leverage");

	const bool bBuy = eSide == PositionSide::Long;
	double dQty = _RoundQty(dUsdt * dLeverage / dPrice);

	const double dFill = _FillPrice(bBuy);
	double dNotional = dQty * dFill;

	if(dQty <= 0 || dQty + 1e-12 < rules.dMinQty)
		return _Reject("quantity below the minimum");
	if(dNotional < rules.dMinNotional)
		return _Reject("order value below the minimum");

	const double dWalletBefore = account.dWallet;

	//one-way with the opposite position open: reduces, closes or reverses
	if(!_Hedge() && _Pos(_Other(eSide)).bOpen)
		return _Execute(eSide, dQty, dFill, dLeverage, rules.dFeeTaker, OrderType::Market, "strategy", dWalletBefore);

	double dFee    = dNotional * rules.dFeeTaker;
	double dNeeded = dNotional / dLeverage + dFee;

	//isolated: if it is short by a little (fees and funding paid in the cycle) the order uses all the available margin
	if(_Isolated() && dNeeded > account.dAvailable + 1e-9 && account.dAvailable > 0)
	{
		const double dMarginMax = account.dAvailable / (1.0 + dLeverage * rules.dFeeTaker * (1.0 + rules.dSlippage));
		double dQtyMax = _RoundQty(dMarginMax * dLeverage / dFill);

		if(dQtyMax <= 0 || dQtyMax + 1e-12 < rules.dMinQty || dQtyMax * dFill < rules.dMinNotional)
			return _Reject("insufficient available margin");

		dQty      = dQtyMax;
		dNotional = dQtyMax * dFill;
		dFee      = dNotional * rules.dFeeTaker;
		dNeeded   = dNotional / dLeverage + dFee;
	}

	if(dNeeded > account.dAvailable + 1e-9)
		return _Reject("insufficient available margin");

	return _ApplyOpen(eSide, dQty, dFill, dLeverage, rules.dFeeTaker, OrderType::Market);
}

//one-way: an order in the direction eSide with the opposite position open. It reduces it; if it is
//larger it closes it and opens the rest in eSide. If the margin is not enough for the rest, nothing
OrderResult CSimExchange::_Execute(PositionSide eSide, double dQty, double dFill, double dLeverage, double dFeeRate,
                                   OrderType eType, const char* pszReason, double dWalletBefore)
{
	const PositionSide eOther = _Other(eSide);
	const Position& opposite = _Pos(eOther);
	if(dQty < opposite.dQty - 1e-12)
		return _ApplyReduce(eOther, dQty, dFill, dFeeRate, eType, pszReason);

	const Saved saved = _Save();
	const double dOppositeQty = opposite.dQty;
	OrderResult closed = _ApplyClose(eOther, dFill, dFeeRate, eType, pszReason);
	const double dRest = _RoundQty(dQty - dOppositeQty);
	if(dRest <= 0 || dRest + 1e-12 < rules.dMinQty)
	{
		_CancelCloseOrders(eOther, "position closed");
		return closed;
	}

	_Update();
	const double dNeeded = dRest * dFill / dLeverage + dRest * dFill * dFeeRate;
	if(dNeeded > account.dAvailable + 1e-9)
	{
		_Restore(saved);
		return _Reject("insufficient available margin to reverse the position", eType);
	}
	(void)dWalletBefore;
	_CancelCloseOrders(eOther, "position closed");
	OrderResult opened = _ApplyOpen(eSide, dRest, dFill, dLeverage, dFeeRate, eType);
	opened.dQty += closed.dQty;
	opened.dFee += closed.dFee;
	return opened;
}

//opening or increase executed (market or limit): average price, margin, fee
OrderResult CSimExchange::_ApplyOpen(PositionSide eSide, double dQty, double dFill, double dLeverage, double dFeeRate, OrderType eType)
{
	Position& position = _Pos(eSide);
	const double dNotional = dQty * dFill;
	const double dFee      = dNotional * dFeeRate;
	const double dWalletBefore = account.dWallet;

	//first opening with the account without positions: a cycle starts
	if(!AnyOpen())
	{
		dCycleFees    = 0;
		dCycleFunding = 0;
	}

	const double dOldQty = position.bOpen ? position.dQty : 0.0;
	const double dTotQty = dOldQty + dQty;
	position.dAvgPrice  = (position.dAvgPrice * dOldQty + dFill * dQty) / dTotQty;
	position.dQty       = dTotQty;
	position.dMargin   += dNotional / dLeverage;
	position.dLeverage  = dLeverage;
	position.dFeesPaid += dFee;
	position.bOpen      = true;
	position.eSide      = eSide;

	account.dWallet -= dFee;
	stats.dFeesPaid += dFee;
	stats.nOrders++;
	dCycleFees += dFee;

	_Update();

	ExchangeEvent e;
	e.eKind         = ExchangeEvent::Kind::Open;
	e.nTime         = nTime;
	e.eSide         = eSide;
	e.dPrice        = dFill;
	e.dQty          = dQty;
	e.dWalletBefore = dWalletBefore;
	e.dWalletAfter  = account.dWallet;
	e.dFee          = dFee;
	e.eType         = eType;
	e.nOrderId      = nFillOrderId;
	e.dPositionQty  = position.dQty;
	e.dAvgPrice     = position.dAvgPrice;
	vEvents.push_back(e);

	OrderResult r;
	r.bOk        = true;
	r.dFillPrice = dFill;
	r.dQty       = dQty;
	r.dFee       = dFee;
	r.eType      = eType;
	r.dFeeRate   = dFeeRate;
	return r;
}

OrderResult CSimExchange::Close()
{
	if(!AnyOpen())
		return _Reject("no open position");

	OrderResult total;
	for(PositionSide eSide : {PositionSide::Long, PositionSide::Short})
	{
		if(!_Pos(eSide).bOpen)
			continue;
		const OrderResult r = ClosePosition(eSide);
		if(!total.bOk)
			total = r;
		else
		{
			total.dQty += r.dQty;
			total.dFee += r.dFee;
		}
	}
	return total;
}

OrderResult CSimExchange::ClosePosition(PositionSide eSide)
{
	if(!_Pos(eSide).bOpen)
		return _Reject(std::string("no open ") + SideName(eSide) + " position");

	const OrderResult r = _ApplyClose(eSide, _FillPrice(eSide == PositionSide::Short), rules.dFeeTaker, OrderType::Market, "strategy");
	_LogOrder("CLOSE", r, 0, eSide);
	_CancelCloseOrders(eSide, "position closed");
	return r;
}

OrderResult CSimExchange::Reduce(PositionSide eSide, double dQty)
{
	const Position& p = _Pos(eSide);
	if(!p.bOpen)
		return _Reject(std::string("no open ") + SideName(eSide) + " position");
	if(!(dQty > 0))
		return _Reject("invalid quantity");

	//the whole position or a part rounded to the step, not below the minimum
	if(dQty >= p.dQty - 1e-12)
		return ClosePosition(eSide);
	dQty = _RoundQty(dQty);
	if(dQty <= 0 || dQty + 1e-12 < rules.dMinQty)
		return _Reject("quantity below the minimum");

	const OrderResult r = _ApplyReduce(eSide, dQty, _FillPrice(eSide == PositionSide::Short), rules.dFeeTaker, OrderType::Market, "strategy");
	_LogOrder("REDUCE", r, 0, eSide);
	return r;
}

//reduction of a part: PnL realized on the closed part, margin released in proportion
OrderResult CSimExchange::_ApplyReduce(PositionSide eSide, double dQty, double dFill, double dFeeRate, OrderType eType, const char* pszReason)
{
	Position& position = _Pos(eSide);
	if(dQty >= position.dQty - 1e-12)
	{
		const OrderResult r = _ApplyClose(eSide, dFill, dFeeRate, eType, pszReason);
		_CancelCloseOrders(eSide, "position closed");
		return r;
	}

	const double dPnL = (eSide == PositionSide::Long) ? (dFill - position.dAvgPrice) * dQty : (position.dAvgPrice - dFill) * dQty;
	const double dFee = dQty * dFill * dFeeRate;

	account.dWallet    += dPnL - dFee;
	stats.dFeesPaid    += dFee;
	stats.dRealizedPnL += dPnL;
	stats.nOrders++;
	dCycleFees         += dFee;

	position.dMargin      -= position.dMargin * (dQty / position.dQty);
	position.dQty         -= dQty;
	position.dRealizedPnL += dPnL;
	position.dFeesPaid    += dFee;
	_Update();

	ExchangeEvent e;
	e.eKind          = ExchangeEvent::Kind::Reduce;
	e.nTime          = nTime;
	e.eSide          = eSide;
	e.dPrice         = dFill;
	e.dQty           = dQty;
	e.dWalletAfter   = account.dWallet;
	e.dFee           = dFee;
	e.dPnL           = dPnL;
	e.eType          = eType;
	e.nOrderId       = nFillOrderId;
	e.dPositionQty   = position.dQty;
	e.dAvgPrice      = position.dAvgPrice;
	e.positionBefore = position;
	e.strReason      = pszReason;
	vEvents.push_back(e);

	OrderResult r;
	r.bOk        = true;
	r.dFillPrice = dFill;
	r.dQty       = dQty;
	r.dFee       = dFee;
	r.eType      = eType;
	r.dFeeRate   = dFeeRate;
	return r;
}

//complete close executed (market, limit or stop)
OrderResult CSimExchange::_ApplyClose(PositionSide eSide, double dFill, double dFeeRate, OrderType eType, const char* pszReason)
{
	Position& position = _Pos(eSide);
	const Position closed = position;
	const double dQty  = position.dQty;
	const double dPnL  = (eSide == PositionSide::Long) ? (dFill - position.dAvgPrice) * dQty : (position.dAvgPrice - dFill) * dQty;
	const double dFee  = dQty * dFill * dFeeRate;

	account.dWallet += dPnL - dFee;
	stats.dFeesPaid    += dFee;
	stats.dRealizedPnL += dPnL;
	stats.nOrders++;
	stats.nClosed++;
	const double dTotalPnL = position.dRealizedPnL + dPnL;
	if(dTotalPnL - dFee - position.dFeesPaid - position.dFundingPaid > 0)
		stats.nWins++;

	position = Position();
	position.eSide = eSide;
	_Update();

	ExchangeEvent e;
	e.eKind          = ExchangeEvent::Kind::Close;
	e.nTime          = nTime;
	e.eSide          = eSide;
	e.dPrice         = dFill;
	e.dQty           = dQty;
	e.dWalletAfter   = account.dWallet;
	e.dFee           = dFee;
	e.dPnL           = dPnL;
	e.eType          = eType;
	e.nOrderId       = nFillOrderId;
	e.dAvgPrice      = closed.dAvgPrice;
	e.positionBefore = closed;
	e.strReason      = pszReason;
	vEvents.push_back(e);

	if(!AnyOpen())
		_OnFlat(closed, dFill, dFee, pszReason);
	else
		dCycleFees += dFee;

	OrderResult r;
	r.bOk        = true;
	r.dFillPrice = dFill;
	r.dQty       = dQty;
	r.dFee       = dFee;
	r.eType      = eType;
	r.dFeeRate   = dFeeRate;
	return r;
}

//no position is left: the cycle is over. The position given carries the fees and funding of
//the whole cycle (without the fee of the last close)
void CSimExchange::_OnFlat(const Position& closed, double dExitPrice, double dCloseFee, const char* pszReason)
{
	ExchangeEvent e;
	e.eKind          = ExchangeEvent::Kind::Flat;
	e.nTime          = nTime;
	e.eSide          = closed.eSide;
	e.dPrice         = dExitPrice;
	e.dWalletAfter   = account.dWallet;
	e.dCloseFee      = dCloseFee;
	e.positionBefore = closed;
	e.positionBefore.dFeesPaid    = dCycleFees;
	e.positionBefore.dFundingPaid = dCycleFunding;
	e.strReason      = pszReason;
	vEvents.push_back(e);

	dCycleFees    = 0;
	dCycleFunding = 0;
}

CSimExchange::Saved CSimExchange::_Save() const
{
	Saved s;
	s.account       = account;
	s.vPositions[0] = vPositions[0];
	s.vPositions[1] = vPositions[1];
	s.stats         = stats;
	s.dCycleFees    = dCycleFees;
	s.dCycleFunding = dCycleFunding;
	s.nEvents       = vEvents.size();
	return s;
}

void CSimExchange::_Restore(const Saved& s)
{
	account       = s.account;
	vPositions[0] = s.vPositions[0];
	vPositions[1] = s.vPositions[1];
	stats         = s.stats;
	dCycleFees    = s.dCycleFees;
	dCycleFunding = s.dCycleFunding;
	vEvents.resize(s.nEvents);
	_Update();
}

CSimExchange::Snapshot CSimExchange::GetSnapshot() const
{
	Snapshot s;
	s.nTime         = nTime;
	s.dPrice        = dPrice;
	s.dWallet       = account.dWallet;
	s.position      = vPositions[0];
	s.shortPosition = vPositions[1];
	s.nFundingSlot  = nFundingSlot;
	s.stats         = stats;
	s.vOrders       = vOrders;
	s.nNextOrderId  = nNextOrderId;
	s.dCycleFees    = dCycleFees;
	s.dCycleFunding = dCycleFunding;
	return s;
}

void CSimExchange::Restore(const Snapshot& s)
{
	nTime           = s.nTime;
	dPrice          = s.dPrice;
	account.dWallet = s.dWallet;
	vPositions[0]   = s.position;
	vPositions[1]   = s.shortPosition;
	//states saved in one-way: the position can be short
	if(vPositions[0].bOpen && vPositions[0].eSide == PositionSide::Short)
		std::swap(vPositions[0], vPositions[1]);
	vPositions[0].eSide = PositionSide::Long;
	vPositions[1].eSide = PositionSide::Short;
	nFundingSlot    = s.nFundingSlot;
	stats           = s.stats;
	vOrders         = s.vOrders;
	nNextOrderId    = s.nNextOrderId;
	dCycleFees      = s.dCycleFees;
	dCycleFunding   = s.dCycleFunding;
	nOrdersVersion++;
	_Update();
}

OrderResult CSimExchange::_Reject(const std::string& strError, OrderType eType)
{
	stats.nRejected++;

	OrderResult r;
	r.bOk      = false;
	r.strError = strError;
	r.eType    = eType;
	_LogOrder("REJECT", r);
	return r;
}

void CSimExchange::_LogOrder(const char* pszType, const OrderResult& r, double dLimitPrice, PositionSide eSide)
{
	if(strOrderLog.empty())
		return;

	std::error_code ec;
	const bool bNew = !std::filesystem::exists(strOrderLog, ec) || std::filesystem::file_size(strOrderLog, ec) == 0;

	std::ofstream orderLog(strOrderLog, std::ios::app);
	orderLog.imbue(std::locale::classic());
	orderLog << std::fixed;
	//one row per order: type (market/limit/stop), id, limit or trigger price, fee rate, side
	if(bNew)
		orderLog << "time;type;price;fill_price;qty;fee;wallet;order;id;limit_price;fee_rate;side\n";

	orderLog << TimeUtils::Format(nTime) << ";" << pszType << ";" << std::setprecision(2) << dPrice << ";";
	if(r.bOk)
		orderLog << r.dFillPrice << ";" << std::setprecision(6) << r.dQty << ";" << std::setprecision(4) << r.dFee;
	else
		orderLog << r.strError << ";;";
	orderLog << ";" << std::setprecision(2) << account.dWallet << ";"
	         << (r.eType == OrderType::Limit ? "limit" : r.eType == OrderType::Stop ? "stop" : "market") << ";";
	if(r.nOrderId > 0)
		orderLog << r.nOrderId;
	orderLog << ";";
	if(dLimitPrice > 0)
		orderLog << dLimitPrice;
	orderLog << ";";
	if(r.bOk && r.dFeeRate > 0)
		orderLog << std::setprecision(6) << r.dFeeRate;
	orderLog << ";" << (r.bOk ? SideName(eSide) : "") << "\n";
}

//margin and fee reserved by the open opening orders
double CSimExchange::_Reserved() const
{
	double dReserved = 0;
	for(const LimitOrder& order : vOrders)
		dReserved += order.dReserved;
	return dReserved;
}

double CSimExchange::_OpenQty(double dLimit, double dUsdt, double dLeverage) const
{
	return _RoundQty(dUsdt * dLeverage / dLimit);
}

//"" if the order can be placed; a limit that would be filled at once is rejected (as post-only)
std::string CSimExchange::_CheckLimit(const LimitOrder& order) const
{
	const double dLimit = order.dPrice;
	if(!(dLimit > 0))
		return order.eType == OrderType::Stop ? "invalid trigger price" : "invalid limit price";
	if(dPrice <= 0)
		return "price not available yet";

	if(order.eType == OrderType::Stop)
	{
		if(dLimit == dPrice)
			return "trigger equal to the market price: it would trigger at once";
		return std::string();
	}

	if(order.eSide == OrderSide::Buy || order.eSide == OrderSide::Sell)
	{
		if(order.eSide == OrderSide::Buy && dLimit >= dPrice)
			return "buy limit at or above the market price: it would be filled at once as a taker order";
		if(order.eSide == OrderSide::Sell && dLimit <= dPrice)
			return "sell limit at or below the market price: it would be filled at once as a taker order";
		if(order.dQty <= 0 || order.dQty + 1e-12 < rules.dMinQty)
			return "quantity below the minimum";
		if(order.dQty * dLimit < rules.dMinNotional)
			return "order value below the minimum";
		return std::string();
	}

	//close: of a long above the market, of a short below
	if(order.ePositionSide == PositionSide::Long && dLimit <= dPrice)
		return "close limit at or below the market price: it would be filled at once as a taker order";
	if(order.ePositionSide == PositionSide::Short && dLimit >= dPrice)
		return "close limit of a short at or above the market price: it would be filled at once as a taker order";
	return std::string();
}

OrderResult CSimExchange::LimitBuy(double dLimit, double dUsdt, double dLeverage)
{
	return _PlaceOpenLimit(PositionSide::Long, dLimit, dUsdt, dLeverage);
}

OrderResult CSimExchange::LimitSell(double dLimit, double dUsdt, double dLeverage)
{
	return _PlaceOpenLimit(PositionSide::Short, dLimit, dUsdt, dLeverage);
}

OrderResult CSimExchange::_PlaceOpenLimit(PositionSide eSide, double dLimit, double dUsdt, double dLeverage)
{
	if(!(dUsdt > 0))
		return _Reject("invalid amount", OrderType::Limit);
	if(!(dLeverage >= 1))
		return _Reject("invalid leverage", OrderType::Limit);

	LimitOrder order;
	order.eSide         = eSide == PositionSide::Long ? OrderSide::Buy : OrderSide::Sell;
	order.ePositionSide = eSide;
	order.dPrice        = dLimit;
	order.dUsdt         = dUsdt;
	order.dLeverage     = dLeverage;
	order.dQty          = (dLimit > 0) ? _OpenQty(dLimit, dUsdt, dLeverage) : 0;
	const std::string strError = _CheckLimit(order);
	if(!strError.empty())
		return _Reject(strError, OrderType::Limit);

	order.dReserved = order.dQty * dLimit / dLeverage + order.dQty * dLimit * rules.dFeeMaker;
	if(order.dReserved > account.dAvailable + 1e-9)
		return _Reject("insufficient available margin", OrderType::Limit);

	order.nId   = nNextOrderId++;
	order.nTime = nTime;
	vOrders.push_back(order);
	nOrdersVersion++;
	stats.nLimitOrders++;
	_Update();

	OrderResult r;
	r.bOk      = true;
	r.dQty     = order.dQty;
	r.eType    = OrderType::Limit;
	r.dFeeRate = rules.dFeeMaker;
	r.nOrderId = order.nId;
	_LogOrder(eSide == PositionSide::Long ? "LIMIT_BUY" : "LIMIT_SELL", r, dLimit, eSide);
	return r;
}

OrderResult CSimExchange::LimitClose(double dLimit)
{
	if(!AnyOpen())
		return _Reject("no open position", OrderType::Limit);
	const PositionSide eSide = GetPosition().bOpen ? GetPosition().eSide : PositionSide::Short;
	if(std::any_of(vOrders.begin(), vOrders.end(), [eSide](const LimitOrder& o)
	               { return o.eSide == OrderSide::Close && o.eType == OrderType::Limit && o.bAll && o.ePositionSide == eSide; }))
		return _Reject("a close limit is already open: move it or cancel it", OrderType::Limit);

	return _PlaceClose(eSide, OrderType::Limit, dLimit, 0);
}

OrderResult CSimExchange::LimitReduce(PositionSide eSide, double dLimit, double dQty)
{
	if(!(dQty > 0))
		return _Reject("invalid quantity", OrderType::Limit);
	return _PlaceClose(eSide, OrderType::Limit, dLimit, dQty);
}

OrderResult CSimExchange::StopClose(PositionSide eSide, double dTrigger, double dQty)
{
	if(dQty < 0)
		return _Reject("invalid quantity", OrderType::Stop);
	return _PlaceClose(eSide, OrderType::Stop, dTrigger, dQty);
}

//close (reduce-only) of a position: limit or stop; dQty 0 = the whole position at the fill
OrderResult CSimExchange::_PlaceClose(PositionSide eSide, OrderType eType, double dLimit, double dQty)
{
	const Position& p = _Pos(eSide);
	if(!p.bOpen)
		return _Reject(std::string("no open ") + SideName(eSide) + " position", eType);

	LimitOrder order;
	order.eSide         = OrderSide::Close;
	order.ePositionSide = eSide;
	order.eType         = eType;
	order.dPrice        = dLimit;
	order.bAll          = !(dQty > 0) || dQty >= p.dQty - 1e-12;
	order.dQty          = order.bAll ? p.dQty : _RoundQty(dQty);
	if(!order.bAll && (order.dQty <= 0 || order.dQty + 1e-12 < rules.dMinQty))
		return _Reject("quantity below the minimum", eType);
	order.bAbove        = dLimit > dPrice;
	const std::string strError = _CheckLimit(order);
	if(!strError.empty())
		return _Reject(strError, eType);

	order.nId   = nNextOrderId++;
	order.nTime = nTime;
	vOrders.push_back(order);
	nOrdersVersion++;
	if(eType == OrderType::Stop)
		stats.nStopOrders++;
	else
		stats.nLimitOrders++;

	OrderResult r;
	r.bOk      = true;
	r.dQty     = order.dQty;
	r.eType    = eType;
	r.dFeeRate = eType == OrderType::Stop ? rules.dFeeTaker : rules.dFeeMaker;
	r.nOrderId = order.nId;
	_LogOrder(eType == OrderType::Stop ? "STOP" : order.bAll ? "LIMIT_CLOSE" : "LIMIT_REDUCE", r, dLimit, eSide);
	return r;
}

OrderResult CSimExchange::Move(long long nOrderId, double dLimit)
{
	const auto it = std::find_if(vOrders.begin(), vOrders.end(), [&](const LimitOrder& o){ return o.nId == nOrderId; });
	if(it == vOrders.end())
		return _Reject("order " + std::to_string(nOrderId) + " not open", OrderType::Limit);

	LimitOrder moved = *it;
	moved.dPrice = dLimit;
	if(moved.eSide != OrderSide::Close)
	{
		moved.dQty      = (dLimit > 0) ? _OpenQty(dLimit, moved.dUsdt, moved.dLeverage) : 0;
		moved.dReserved = moved.dQty * dLimit / moved.dLeverage + moved.dQty * dLimit * rules.dFeeMaker;
	}
	if(moved.eType == OrderType::Stop)
		moved.bAbove = dLimit > dPrice;
	const std::string strError = _CheckLimit(moved);
	if(!strError.empty())
		return _Reject(strError, moved.eType);
	if(moved.dReserved - it->dReserved > account.dAvailable + 1e-9)
		return _Reject("insufficient available margin", moved.eType);

	*it = moved;
	nOrdersVersion++;
	_Update();

	OrderResult r;
	r.bOk      = true;
	r.dQty     = moved.eSide != OrderSide::Close ? moved.dQty : moved.bAll ? _Pos(moved.ePositionSide).dQty : moved.dQty;
	r.eType    = moved.eType;
	r.dFeeRate = moved.eType == OrderType::Stop ? rules.dFeeTaker : rules.dFeeMaker;
	r.nOrderId = nOrderId;
	_LogOrder("MOVE", r, dLimit, moved.ePositionSide);
	return r;
}

bool CSimExchange::Cancel(long long nOrderId)
{
	for(size_t i = 0; i < vOrders.size(); i++)
		if(vOrders[i].nId == nOrderId)
		{
			_Remove(i, "cancelled by the strategy");
			_Update();
			return true;
		}
	return false;
}

void CSimExchange::CancelAll()
{
	while(!vOrders.empty())
		_Remove(vOrders.size() - 1, "cancelled by the strategy");
	_Update();
}

void CSimExchange::_Remove(size_t nIndex, const std::string& strReason)
{
	const LimitOrder order = vOrders[nIndex];
	vOrders.erase(vOrders.begin() + static_cast<long>(nIndex));
	nOrdersVersion++;
	stats.nCancelled++;

	OrderResult r;
	r.eType    = order.eType;
	r.nOrderId = order.nId;
	r.strError = strReason;
	_LogOrder("CANCEL", r, order.dPrice, order.ePositionSide);
}

void CSimExchange::_CancelCloseOrders(PositionSide eSide, const std::string& strReason)
{
	for(size_t i = vOrders.size(); i-- > 0; )
		if(vOrders[i].eSide == OrderSide::Close && vOrders[i].ePositionSide == eSide)
			_Remove(i, strReason);
}

//limit: through = the price must go through the limit by dLimitTick (orders queued at the same
//price could be filled first); touch = reaching it is enough. Stop: reaching it is enough
bool CSimExchange::_Triggered(const LimitOrder& order) const
{
	if(order.eType == OrderType::Stop)
		return order.bAbove ? dPrice >= order.dPrice : dPrice <= order.dPrice;

	//opening a long or closing a short buys; opening a short or closing a long sells
	const bool bBuying = order.eSide == OrderSide::Buy || (order.eSide == OrderSide::Close && order.ePositionSide == PositionSide::Short);
	if(bBuying)
		return bLimitThrough ? (dLimitTick > 0 ? dPrice <= order.dPrice * (1.0 - dLimitTick) : dPrice < order.dPrice)
		                     : dPrice <= order.dPrice;
	return bLimitThrough ? (dLimitTick > 0 ? dPrice >= order.dPrice * (1.0 + dLimitTick) : dPrice > order.dPrice)
	                     : dPrice >= order.dPrice;
}

//orders reached by the current price: limits at their price with the maker fee, stops at
//market with the taker fee
void CSimExchange::_CheckOrders()
{
	for(size_t i = 0; i < vOrders.size(); )
	{
		const LimitOrder order = vOrders[i];
		if(!_Triggered(order) || (order.eSide == OrderSide::Close && !_Pos(order.ePositionSide).bOpen))
		{
			++i;
			continue;
		}
		vOrders.erase(vOrders.begin() + static_cast<long>(i));
		nOrdersVersion++;
		const double dWalletBefore = account.dWallet;
		nFillOrderId = order.nId;

		OrderResult r;
		if(order.eSide == OrderSide::Buy || order.eSide == OrderSide::Sell)
		{
			_Update();
			const bool bFlip = !_Hedge() && _Pos(_Other(order.ePositionSide)).bOpen;
			const double dNeeded = order.dQty * order.dPrice / order.dLeverage + order.dQty * order.dPrice * rules.dFeeMaker;
			if(!bFlip && dNeeded > account.dAvailable + 1e-9)
			{
				stats.nCancelled++;
				OrderResult rejected;
				rejected.eType    = OrderType::Limit;
				rejected.nOrderId = order.nId;
				rejected.strError = "insufficient margin at fill: order cancelled";
				_LogOrder("CANCEL", rejected, order.dPrice, order.ePositionSide);
				nFillOrderId = 0;
				continue;
			}
			if(bFlip)
			{
				const long long nRejected = stats.nRejected;
				r = _Execute(order.ePositionSide, order.dQty, order.dPrice, order.dLeverage, rules.dFeeMaker, OrderType::Limit,
				             "limit", dWalletBefore);
				if(!r.bOk)
				{
					stats.nRejected = nRejected;
					stats.nCancelled++;
					OrderResult rejected;
					rejected.eType    = OrderType::Limit;
					rejected.nOrderId = order.nId;
					rejected.strError = "insufficient margin at fill: order cancelled";
					_LogOrder("CANCEL", rejected, order.dPrice, order.ePositionSide);
					nFillOrderId = 0;
					continue;
				}
			}
			else
			{
				r = _ApplyOpen(order.ePositionSide, order.dQty, order.dPrice, order.dLeverage, rules.dFeeMaker, OrderType::Limit);
			}
			r.nOrderId = order.nId;
			_LogOrder(order.eSide == OrderSide::Buy ? "FILL_BUY" : "FILL_SELL", r, order.dPrice, order.ePositionSide);
		}
		else
		{
			const Position& p = _Pos(order.ePositionSide);
			const double dQty = order.bAll ? p.dQty : std::min(order.dQty, p.dQty);
			if(order.eType == OrderType::Stop)
			{
				r = _ApplyReduce(order.ePositionSide, dQty, _FillPrice(order.ePositionSide == PositionSide::Short),
				                 rules.dFeeTaker, OrderType::Stop, "stop");
				r.nOrderId = order.nId;
				_LogOrder("FILL_STOP", r, order.dPrice, order.ePositionSide);
			}
			else
			{
				r = _ApplyReduce(order.ePositionSide, dQty, order.dPrice, rules.dFeeMaker, OrderType::Limit, "limit");
				r.nOrderId = order.nId;
				_LogOrder("FILL_CLOSE", r, order.dPrice, order.ePositionSide);
			}
			if(!_Pos(order.ePositionSide).bOpen)
				_CancelCloseOrders(order.ePositionSide, "position closed");
		}
		if(order.eType == OrderType::Stop)
			stats.nStopFills++;
		else
		{
			stats.nLimitFills++;
			stats.dMakerFees += r.dFee;
		}

		Fill fill;
		fill.nOrderId = order.nId;
		fill.eSide    = order.eSide;
		fill.eType    = order.eType;
		fill.nTime    = nTime;
		fill.dPrice   = r.dFillPrice;
		fill.dQty     = r.dQty;
		fill.dFee     = r.dFee;
		fill.dFeeRate = r.dFeeRate;
		fill.dWalletBefore = dWalletBefore;
		fill.ePositionSide = order.ePositionSide;
		vFills.push_back(fill);
		nFillOrderId = 0;
		i = 0;		//a fill changes margin and positions: check again from the start
	}
	_Update();
}

std::vector<Fill> CSimExchange::TakeFills()
{
	std::vector<Fill> vTaken;
	vTaken.swap(vFills);
	return vTaken;
}

std::vector<ExchangeEvent> CSimExchange::TakeEvents()
{
	std::vector<ExchangeEvent> vTaken;
	vTaken.swap(vEvents);
	return vTaken;
}
