#include "CSimStateFile.h"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>
#include <system_error>
#include <vector>

#include "Settings.h"

#define STATE_RUN			"RUN"
#define STATE_ACCOUNT		"ACCOUNT"
#define STATE_POSITION		"POSITION"
#define STATE_SHORT			"POSITION_SHORT"
#define STATE_STATS			"STATS"
#define STATE_CYCLE			"CYCLE"
#define STATE_ORDERS		"ORDERS"

namespace
{
	class CStateReader
	{
	public:
		explicit CStateReader(const std::string& strFile): ini(strFile, false), bOk(true) {}

		bool Valid() const { return ini.IsValid(); }
		bool Has(const char* pszSection, const char* pszKey) const { return ini.Has(pszSection, pszKey); }
		bool Ok() const { return bOk; }
		const std::string& Missing() const { return strMissing; }

		void Get(const char* pszSection, const char* pszKey, double& d)
		{
			if(!ini.Get(pszSection, pszKey, d))
				_Fail(pszSection, pszKey);
		}
		void Get(const char* pszSection, const char* pszKey, long long& n)
		{
			std::string s;
			if(!ini.Get(pszSection, pszKey, s))
			{
				_Fail(pszSection, pszKey);
				return;
			}
			try { n = std::stoll(s); }
			catch(...) { _Fail(pszSection, pszKey); }
		}
		void Get(const char* pszSection, const char* pszKey, int& n)
		{
			long long v = 0;
			Get(pszSection, pszKey, v);
			n = static_cast<int>(v);
		}
		void Get(const char* pszSection, const char* pszKey, bool& b)
		{
			long long v = 0;
			Get(pszSection, pszKey, v);
			b = v != 0;
		}
		void Get(const char* pszSection, const char* pszKey, std::string& s)
		{
			if(!ini.Get(pszSection, pszKey, s))
				_Fail(pszSection, pszKey);
		}

	private:
		void _Fail(const char* pszSection, const char* pszKey)
		{
			bOk = false;
			strMissing += std::string(strMissing.empty() ? "" : ", ") + "[" + pszSection + "] " + pszKey;
		}

		CReader     ini;
		bool        bOk;
		std::string strMissing;
	};
}

bool CSimStateFile::Save(const std::string& strFile, const SimState& s, std::string& strError)
{
	std::ostringstream out;
	out.imbue(std::locale::classic());
	out << std::setprecision(17);

	const Position& p = s.exchange.position;
	const Position& q = s.exchange.shortPosition;
	const CSimExchange::Stats& st = s.exchange.stats;
	const CCycleArchive::Snapshot& c = s.cycle;

	out << "# SXE simulation state: written by the engine, do not edit\n"
	    << "[" STATE_RUN "]\n"
	    << "data_file = "     << s.strDataFile  << "\n"
	    << "strategy = "      << s.strStrategy  << "\n"
	    << "candle_time = "   << s.nCandleTime  << "\n"
	    << "next_tick = "     << s.nNextTick    << "\n"
	    << "step = "          << s.nStep        << "\n"
	    << "peak_equity = "   << s.dPeakEquity  << "\n"
	    << "max_drawdown = "  << s.dMaxDrawdown << "\n"
	    << "\n[" STATE_ACCOUNT "]\n"
	    << "time = "          << s.exchange.nTime        << "\n"
	    << "price = "         << s.exchange.dPrice       << "\n"
	    << "wallet = "        << s.exchange.dWallet      << "\n"
	    << "funding_slot = "  << s.exchange.nFundingSlot << "\n"
	    << "\n[" STATE_POSITION "]\n"
	    << "open = "          << (p.bOpen ? 1 : 0)  << "\n"
	    << "side = "          << (p.eSide == PositionSide::Short ? "short" : "long") << "\n"
	    << "qty = "           << p.dQty             << "\n"
	    << "avg_price = "     << p.dAvgPrice        << "\n"
	    << "margin = "        << p.dMargin          << "\n"
	    << "leverage = "      << p.dLeverage        << "\n"
	    << "fees_paid = "     << p.dFeesPaid        << "\n"
	    << "funding_paid = "  << p.dFundingPaid     << "\n"
	    << "realized_pnl = "  << p.dRealizedPnL     << "\n"
	    << "\n[" STATE_SHORT "]\n"
	    << "open = "          << (q.bOpen ? 1 : 0)  << "\n"
	    << "qty = "           << q.dQty             << "\n"
	    << "avg_price = "     << q.dAvgPrice        << "\n"
	    << "margin = "        << q.dMargin          << "\n"
	    << "leverage = "      << q.dLeverage        << "\n"
	    << "fees_paid = "     << q.dFeesPaid        << "\n"
	    << "funding_paid = "  << q.dFundingPaid     << "\n"
	    << "realized_pnl = "  << q.dRealizedPnL     << "\n"
	    << "\n[" STATE_STATS "]\n"
	    << "orders = "        << st.nOrders         << "\n"
	    << "rejected = "      << st.nRejected       << "\n"
	    << "closed = "        << st.nClosed         << "\n"
	    << "wins = "          << st.nWins           << "\n"
	    << "liquidations = "  << st.nLiquidations   << "\n"
	    << "fees_paid = "     << st.dFeesPaid       << "\n"
	    << "funding_paid = "  << st.dFundingPaid    << "\n"
	    << "realized_pnl = "  << st.dRealizedPnL    << "\n"
	    << "limit_orders = "  << st.nLimitOrders    << "\n"
	    << "limit_fills = "   << st.nLimitFills     << "\n"
	    << "cancelled = "     << st.nCancelled      << "\n"
	    << "maker_fees = "    << st.dMakerFees      << "\n"
	    << "partial_liquidations = " << st.nPartialLiquidations << "\n"
	    << "stop_orders = "   << st.nStopOrders     << "\n"
	    << "stop_fills = "    << st.nStopFills      << "\n"
	    << "cycle_fees = "    << s.exchange.dCycleFees    << "\n"
	    << "cycle_funding = " << s.exchange.dCycleFunding << "\n"
	    << "\n[" STATE_CYCLE "]\n"
	    << "open = "          << (c.bOpen ? 1 : 0)  << "\n"
	    << "open_time = "     << c.nOpenTime        << "\n"
	    << "wallet_start = "  << c.dWalletStart     << "\n"
	    << "first_price = "   << c.dFirstPrice      << "\n"
	    << "buys = "          << c.nBuys            << "\n"
	    << "min_equity = "    << c.dMinEquityPerc   << "\n"
	    << "max_leverage = "  << c.dMaxLeverage     << "\n"
	    << "long = "          << (c.bLong ? 1 : 0)  << "\n"
	    << "short = "         << (c.bShort ? 1 : 0) << "\n"
	    << "\n[" STATE_ORDERS "]\n"
	    << "next_id = "       << s.exchange.nNextOrderId    << "\n"
	    << "count = "         << s.exchange.vOrders.size()  << "\n";
	for(size_t i = 0; i < s.exchange.vOrders.size(); i++)
	{
		const LimitOrder& o = s.exchange.vOrders[i];
		out << "order." << (i + 1) << " = " << o.nId << ";"
		    << (o.eSide == OrderSide::Buy ? "buy" : o.eSide == OrderSide::Sell ? "sell" : "close") << ";"
		    << o.dPrice << ";" << o.dUsdt << ";" << o.dLeverage << ";" << o.dQty << ";" << o.dReserved << ";" << o.nTime << ";"
		    << (o.ePositionSide == PositionSide::Long ? "long" : "short") << ";"
		    << (o.eType == OrderType::Stop ? "stop" : "limit") << ";" << (o.bAll ? 1 : 0) << ";" << (o.bAbove ? 1 : 0) << "\n";
	}

	const std::string strTmp = strFile + ".tmp";
	{
		std::ofstream file(strTmp, std::ios::trunc);
		file << out.str();
		file.flush();
		if(!file)
		{
			strError = "Cannot write " + strTmp + ": simulation state not saved";
			return false;
		}
	}

	std::error_code ec;
	std::filesystem::rename(strTmp, strFile, ec);
	if(ec)
	{
		strError = "Cannot update " + strFile + " (" + ec.message() + "): simulation state not saved";
		std::filesystem::remove(strTmp, ec);
		return false;
	}
	return true;
}

bool CSimStateFile::Load(const std::string& strFile, SimState& s, std::string& strError)
{
	CStateReader in(strFile);
	if(!in.Valid())
	{
		strError = "Simulation state cannot be read: " + strFile;
		return false;
	}

	Position& p = s.exchange.position;
	Position& q = s.exchange.shortPosition;
	CSimExchange::Stats& st = s.exchange.stats;
	CCycleArchive::Snapshot& c = s.cycle;

	in.Get(STATE_RUN, "data_file", s.strDataFile);
	in.Get(STATE_RUN, "strategy", s.strStrategy);
	in.Get(STATE_RUN, "candle_time", s.nCandleTime);
	in.Get(STATE_RUN, "next_tick", s.nNextTick);
	in.Get(STATE_RUN, "step", s.nStep);
	in.Get(STATE_RUN, "peak_equity", s.dPeakEquity);
	in.Get(STATE_RUN, "max_drawdown", s.dMaxDrawdown);

	in.Get(STATE_ACCOUNT, "time", s.exchange.nTime);
	in.Get(STATE_ACCOUNT, "price", s.exchange.dPrice);
	in.Get(STATE_ACCOUNT, "wallet", s.exchange.dWallet);
	in.Get(STATE_ACCOUNT, "funding_slot", s.exchange.nFundingSlot);

	in.Get(STATE_POSITION, "open", p.bOpen);
	in.Get(STATE_POSITION, "qty", p.dQty);
	in.Get(STATE_POSITION, "avg_price", p.dAvgPrice);
	in.Get(STATE_POSITION, "margin", p.dMargin);
	in.Get(STATE_POSITION, "leverage", p.dLeverage);
	in.Get(STATE_POSITION, "fees_paid", p.dFeesPaid);
	in.Get(STATE_POSITION, "funding_paid", p.dFundingPaid);
	if(in.Has(STATE_POSITION, "realized_pnl"))
		in.Get(STATE_POSITION, "realized_pnl", p.dRealizedPnL);
	//one-way: the position can be short (states saved before shorts existed: always long)
	if(in.Has(STATE_POSITION, "side"))
	{
		std::string strSide;
		in.Get(STATE_POSITION, "side", strSide);
		p.eSide = strSide == "short" ? PositionSide::Short : PositionSide::Long;
	}
	q.eSide = PositionSide::Short;
	if(in.Has(STATE_SHORT, "open"))
	{
		in.Get(STATE_SHORT, "open", q.bOpen);
		in.Get(STATE_SHORT, "qty", q.dQty);
		in.Get(STATE_SHORT, "avg_price", q.dAvgPrice);
		in.Get(STATE_SHORT, "margin", q.dMargin);
		in.Get(STATE_SHORT, "leverage", q.dLeverage);
		in.Get(STATE_SHORT, "fees_paid", q.dFeesPaid);
		in.Get(STATE_SHORT, "funding_paid", q.dFundingPaid);
		in.Get(STATE_SHORT, "realized_pnl", q.dRealizedPnL);
	}

	in.Get(STATE_STATS, "orders", st.nOrders);
	in.Get(STATE_STATS, "rejected", st.nRejected);
	in.Get(STATE_STATS, "closed", st.nClosed);
	in.Get(STATE_STATS, "wins", st.nWins);
	in.Get(STATE_STATS, "liquidations", st.nLiquidations);
	in.Get(STATE_STATS, "fees_paid", st.dFeesPaid);
	in.Get(STATE_STATS, "funding_paid", st.dFundingPaid);
	in.Get(STATE_STATS, "realized_pnl", st.dRealizedPnL);
	if(in.Has(STATE_STATS, "limit_orders"))
	{
		in.Get(STATE_STATS, "limit_orders", st.nLimitOrders);
		in.Get(STATE_STATS, "limit_fills", st.nLimitFills);
		in.Get(STATE_STATS, "cancelled", st.nCancelled);
		in.Get(STATE_STATS, "maker_fees", st.dMakerFees);
	}
	if(in.Has(STATE_STATS, "cycle_fees"))
	{
		in.Get(STATE_STATS, "partial_liquidations", st.nPartialLiquidations);
		in.Get(STATE_STATS, "stop_orders", st.nStopOrders);
		in.Get(STATE_STATS, "stop_fills", st.nStopFills);
		in.Get(STATE_STATS, "cycle_fees", s.exchange.dCycleFees);
		in.Get(STATE_STATS, "cycle_funding", s.exchange.dCycleFunding);
	}
	else
	{
		//states saved before shorts existed: the cycle is the position
		s.exchange.dCycleFees    = p.dFeesPaid;
		s.exchange.dCycleFunding = p.dFundingPaid;
	}

	in.Get(STATE_CYCLE, "open", c.bOpen);
	in.Get(STATE_CYCLE, "open_time", c.nOpenTime);
	in.Get(STATE_CYCLE, "wallet_start", c.dWalletStart);
	in.Get(STATE_CYCLE, "first_price", c.dFirstPrice);
	in.Get(STATE_CYCLE, "buys", c.nBuys);
	in.Get(STATE_CYCLE, "min_equity", c.dMinEquityPerc);
	in.Get(STATE_CYCLE, "max_leverage", c.dMaxLeverage);
	if(in.Has(STATE_CYCLE, "long"))
	{
		in.Get(STATE_CYCLE, "long", c.bLong);
		in.Get(STATE_CYCLE, "short", c.bShort);
	}
	else
		c.bLong = c.bOpen;

	s.exchange.vOrders.clear();
	if(in.Has(STATE_ORDERS, "count"))
	{
		long long nCount = 0;
		in.Get(STATE_ORDERS, "next_id", s.exchange.nNextOrderId);
		in.Get(STATE_ORDERS, "count", nCount);
		for(long long i = 1; i <= nCount && in.Ok(); i++)
		{
			const std::string strKey = "order." + std::to_string(i);
			std::string strOrder;
			in.Get(STATE_ORDERS, strKey.c_str(), strOrder);
			std::vector<std::string> vFields;
			std::istringstream fields(strOrder);
			for(std::string strField; std::getline(fields, strField, ';'); )
				vFields.push_back(strField);
			LimitOrder o;
			bool bValid = (vFields.size() == 8 || vFields.size() == 12) &&
			              (vFields[1] == "buy" || vFields[1] == "close" || vFields[1] == "sell");
			try
			{
				if(bValid)
				{
					o.nId       = std::stoll(vFields[0]);
					o.dPrice    = std::stod(vFields[2]);
					o.dUsdt     = std::stod(vFields[3]);
					o.dLeverage = std::stod(vFields[4]);
					o.dQty      = std::stod(vFields[5]);
					o.dReserved = std::stod(vFields[6]);
					o.nTime     = std::stoll(vFields[7]);
				}
			}
			catch(...) { bValid = false; }
			if(!bValid)
			{
				strError = "Invalid open order in " + strFile + ": [" STATE_ORDERS "] " + strKey + " = " + strOrder;
				return false;
			}
			o.eSide = vFields[1] == "buy" ? OrderSide::Buy : vFields[1] == "sell" ? OrderSide::Sell : OrderSide::Close;
			o.ePositionSide = o.eSide == OrderSide::Sell ? PositionSide::Short : PositionSide::Long;
			if(vFields.size() == 12)
			{
				o.ePositionSide = vFields[8] == "short" ? PositionSide::Short : PositionSide::Long;
				o.eType         = vFields[9] == "stop" ? OrderType::Stop : OrderType::Limit;
				o.bAll          = vFields[10] == "1";
				o.bAbove        = vFields[11] == "1";
			}
			s.exchange.vOrders.push_back(o);
		}
	}

	if(!in.Ok())
	{
		strError = "Incomplete simulation state in " + strFile + " (" + in.Missing() + ")";
		return false;
	}
	return true;
}
