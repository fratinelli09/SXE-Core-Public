#include "CCStrategy.h"

namespace
{
	sxe_account ToC(const Account& a)
	{
		sxe_account c{};
		c.wallet    = a.dWallet;
		c.equity    = a.dEquity;
		c.available = a.dAvailable;
		return c;
	}

	sxe_position ToC(const Position& p)
	{
		sxe_position c{};
		c.open              = p.bOpen ? 1 : 0;
		c.side              = p.eSide == PositionSide::Short ? SXE_SIDE_SHORT : SXE_SIDE_LONG;
		c.qty               = p.dQty;
		c.avg_price         = p.dAvgPrice;
		c.margin            = p.dMargin;
		c.leverage          = p.dLeverage;
		c.unrealized_pnl    = p.dUnrealizedPnL;
		c.liquidation_price = p.dLiquidationPrice;
		c.fees_paid         = p.dFeesPaid;
		c.funding_paid      = p.dFundingPaid;
		c.realized_pnl      = p.dRealizedPnL;
		return c;
	}

	sxe_bar ToC(const Candle& k)
	{
		sxe_bar c{};
		c.time   = k.nTime;
		c.open   = k.dOpen;
		c.high   = k.dHigh;
		c.low    = k.dLow;
		c.close  = k.dClose;
		c.volume = k.dVolume;
		return c;
	}

	int ToC(OrderType e)
	{
		return e == OrderType::Limit ? SXE_TYPE_LIMIT : e == OrderType::Stop ? SXE_TYPE_STOP : SXE_TYPE_MARKET;
	}

	int ToC(OrderSide e)
	{
		return e == OrderSide::Close ? SXE_ORDER_CLOSE : e == OrderSide::Sell ? SXE_ORDER_SELL : SXE_ORDER_BUY;
	}

	int ToC(PositionSide e)
	{
		return e == PositionSide::Short ? SXE_SIDE_SHORT : SXE_SIDE_LONG;
	}

	PositionSide SideOf(int nSide)
	{
		return nSide == SXE_SIDE_SHORT ? PositionSide::Short : PositionSide::Long;
	}

	sxe_rules ToC(const ExchangeRules& r)
	{
		sxe_rules c{};
		c.margin_mode        = r.eMarginMode == MarginMode::Isolated ? SXE_MARGIN_ISOLATED : SXE_MARGIN_CROSSED;
		c.position_mode      = r.ePositionMode == PositionMode::Hedge ? SXE_POSITIONS_HEDGE : SXE_POSITIONS_ONE_WAY;
		c.fee_taker          = r.dFeeTaker;
		c.fee_maker          = r.dFeeMaker;
		c.slippage           = r.dSlippage;
		c.maintenance_margin = r.dMaintenanceMargin;
		c.qty_step           = r.dQtyStep;
		c.min_qty            = r.dMinQty;
		c.min_notional       = r.dMinNotional;
		return c;
	}

	const char* Text(const char* psz)
	{
		return psz ? psz : "";
	}
}

CCStrategy::CCStrategy(const Functions& functions):
	fn(functions),
	host{},
	pStrategy(nullptr),
	pLog(nullptr),
	pCtx(nullptr),
	step{}
{
	host.size          = static_cast<int>(sizeof(sxe_host));
	host.engine        = this;
	host.market_buy    = &CCStrategy::_MarketBuy;
	host.market_sell   = &CCStrategy::_MarketSell;
	host.close_all     = &CCStrategy::_CloseAll;
	host.close_position = &CCStrategy::_ClosePosition;
	host.reduce        = &CCStrategy::_Reduce;
	host.limit_buy     = &CCStrategy::_LimitBuy;
	host.limit_sell    = &CCStrategy::_LimitSell;
	host.limit_close   = &CCStrategy::_LimitClose;
	host.limit_reduce  = &CCStrategy::_LimitReduce;
	host.stop_close    = &CCStrategy::_StopClose;
	host.move          = &CCStrategy::_Move;
	host.cancel        = &CCStrategy::_Cancel;
	host.cancel_all    = &CCStrategy::_CancelAll;
	host.log           = &CCStrategy::_Log;
	host.log_once      = &CCStrategy::_LogOnce;
	host.clear_once    = &CCStrategy::_ClearOnce;
}

CCStrategy::~CCStrategy()
{
	if(pStrategy && fn.pfnDestroy)
		fn.pfnDestroy(pStrategy);
	pStrategy = nullptr;
}

bool CCStrategy::Create()
{
	pStrategy = fn.pfnCreate(&host);
	if(!pStrategy)
	{
		strLastError = SXE_STRATEGY_SYMBOL_CREATE " did not create the strategy";
		return false;
	}
	return true;
}

void CCStrategy::_TakeError(const std::string& strDefault)
{
	const char* pszError = fn.pfnError(pStrategy);
	strLastError = pszError && *pszError ? pszError : strDefault;
}

StrategyInfo CCStrategy::GetInfo() const
{
	sxe_info info{};
	fn.pfnInfo(pStrategy, &info);
	strName        = Text(info.name);
	strVersion     = Text(info.version);
	strDescription = Text(info.description);
	return StrategyInfo{strName, strVersion, strDescription};
}

bool CCStrategy::LoadConfig(const std::string& strConfigFile)
{
	if(fn.pfnLoadConfig(pStrategy, strConfigFile.c_str()) == 0)
	{
		_TakeError("invalid configuration");
		return false;
	}
	return true;
}

bool CCStrategy::Init(const InitContext& ctx)
{
	pLog         = &ctx.log;
	strSymbol    = ctx.strSymbol;
	strLogDir    = ctx.strLogDir;
	strBackupDir = ctx.strBackupDir;

	sxe_init init{};
	init.mode           = ctx.eMode == Mode::Real ? SXE_MODE_REAL : SXE_MODE_SIMULATED;
	init.resume         = ctx.bResume ? 1 : 0;
	init.symbol         = strSymbol.c_str();
	init.start_time     = ctx.nStartTime;
	init.log_dir        = strLogDir.c_str();
	init.backup_dir     = strBackupDir.c_str();
	init.account        = ToC(ctx.account);
	init.position       = ToC(ctx.position);
	init.long_position  = ToC(ctx.longPosition);
	init.short_position = ToC(ctx.shortPosition);
	init.rules          = ToC(ctx.rules);

	if(fn.pfnInit(pStrategy, &init) == 0)
	{
		_TakeError("initialization failed");
		return false;
	}
	return true;
}

void CCStrategy::_Refresh()
{
	const StepContext& ctx = *pCtx;
	step.account        = ToC(ctx.account);
	step.position       = ToC(ctx.position);
	step.long_position  = ToC(ctx.longPosition);
	step.short_position = ToC(ctx.shortPosition);

	vOpenOrders.clear();
	vOpenOrders.reserve(ctx.vOpenOrders.size());
	for(const LimitOrder& o : ctx.vOpenOrders)
	{
		sxe_open_order c{};
		c.id            = o.nId;
		c.side          = ToC(o.eSide);
		c.type          = ToC(o.eType);
		c.position_side = ToC(o.ePositionSide);
		c.all           = o.bAll ? 1 : 0;
		c.above         = o.bAbove ? 1 : 0;
		c.price         = o.dPrice;
		c.usdt          = o.dUsdt;
		c.leverage      = o.dLeverage;
		c.qty           = o.dQty;
		c.reserved      = o.dReserved;
		c.time          = o.nTime;
		vOpenOrders.push_back(c);
	}
	step.open_orders      = vOpenOrders.empty() ? nullptr : vOpenOrders.data();
	step.open_order_count = static_cast<int64_t>(vOpenOrders.size());
}

RunResult CCStrategy::Run(StepContext& ctx)
{
	pCtx = &ctx;
	pLog = &ctx.log;

	step.time          = ctx.nTime;
	step.step          = ctx.nStep;
	step.cycle         = ctx.nCycle;
	step.price         = ctx.dPrice;
	step.last_candle   = ToC(ctx.lastCandle);
	step.candle_closed = ctx.bCandleClosed ? 1 : 0;
	step.liquidated    = ctx.bLiquidated ? 1 : 0;

	vExecutions.clear();
	vExecutions.reserve(ctx.vFills.size());
	for(const Fill& f : ctx.vFills)
	{
		sxe_execution c{};
		c.order_id      = f.nOrderId;
		c.side          = ToC(f.eSide);
		c.type          = ToC(f.eType);
		c.position_side = ToC(f.ePositionSide);
		c.time          = f.nTime;
		c.price         = f.dPrice;
		c.qty           = f.dQty;
		c.fee           = f.dFee;
		c.fee_rate      = f.dFeeRate;
		c.wallet_before = f.dWalletBefore;
		vExecutions.push_back(c);
	}
	step.executions      = vExecutions.empty() ? nullptr : vExecutions.data();
	step.execution_count = static_cast<int64_t>(vExecutions.size());
	_Refresh();

	const int nResult = fn.pfnStep(pStrategy, &step);
	pCtx = nullptr;

	if(nResult == SXE_STEP_CONTINUE)
		return RunResult::Continue;
	if(nResult == SXE_STEP_STOP)
		return RunResult::Stop;
	_TakeError("strategy error (code " + std::to_string(nResult) + ")");
	return RunResult::Error;
}

void CCStrategy::Term()
{
	fn.pfnTerm(pStrategy);
}

int CCStrategy::_Result(const OrderResult& r, sxe_order_result* pOut)
{
	strOrderError = r.strError;
	pOut->ok         = r.bOk ? 1 : 0;
	pOut->type       = ToC(r.eType);
	pOut->order_id   = r.nOrderId;
	pOut->fill_price = r.dFillPrice;
	pOut->qty        = r.dQty;
	pOut->fee        = r.dFee;
	pOut->fee_rate   = r.dFeeRate;
	pOut->error      = strOrderError.c_str();
	_Refresh();
	return pOut->ok;
}

int CCStrategy::_NoStep(sxe_order_result* pOut)
{
	static const char* pszError = "order outside sxe_strategy_step";
	*pOut = sxe_order_result{};
	pOut->error = pszError;
	return 0;
}

#define SXE_C_ORDER(pEngine, pOut, call)							\
	auto* pSelf = static_cast<CCStrategy*>(pEngine);				\
	if(!pSelf->pCtx)												\
		return _NoStep(pOut);										\
	return pSelf->_Result(pSelf->pCtx->orders.call, pOut)

int CCStrategy::_MarketBuy(void* pEngine, double dUsdt, double dLeverage, sxe_order_result* pOut)
{
	SXE_C_ORDER(pEngine, pOut, MarketBuy(dUsdt, dLeverage));
}

int CCStrategy::_MarketSell(void* pEngine, double dUsdt, double dLeverage, sxe_order_result* pOut)
{
	SXE_C_ORDER(pEngine, pOut, MarketSell(dUsdt, dLeverage));
}

int CCStrategy::_CloseAll(void* pEngine, sxe_order_result* pOut)
{
	SXE_C_ORDER(pEngine, pOut, Close());
}

int CCStrategy::_ClosePosition(void* pEngine, int nSide, sxe_order_result* pOut)
{
	SXE_C_ORDER(pEngine, pOut, ClosePosition(SideOf(nSide)));
}

int CCStrategy::_Reduce(void* pEngine, int nSide, double dQty, sxe_order_result* pOut)
{
	SXE_C_ORDER(pEngine, pOut, Reduce(SideOf(nSide), dQty));
}

int CCStrategy::_LimitBuy(void* pEngine, double dPrice, double dUsdt, double dLeverage, sxe_order_result* pOut)
{
	SXE_C_ORDER(pEngine, pOut, LimitBuy(dPrice, dUsdt, dLeverage));
}

int CCStrategy::_LimitSell(void* pEngine, double dPrice, double dUsdt, double dLeverage, sxe_order_result* pOut)
{
	SXE_C_ORDER(pEngine, pOut, LimitSell(dPrice, dUsdt, dLeverage));
}

int CCStrategy::_LimitClose(void* pEngine, double dPrice, sxe_order_result* pOut)
{
	SXE_C_ORDER(pEngine, pOut, LimitClose(dPrice));
}

int CCStrategy::_LimitReduce(void* pEngine, int nSide, double dPrice, double dQty, sxe_order_result* pOut)
{
	SXE_C_ORDER(pEngine, pOut, LimitReduce(SideOf(nSide), dPrice, dQty));
}

int CCStrategy::_StopClose(void* pEngine, int nSide, double dTrigger, double dQty, sxe_order_result* pOut)
{
	SXE_C_ORDER(pEngine, pOut, StopClose(SideOf(nSide), dTrigger, dQty));
}

int CCStrategy::_Move(void* pEngine, int64_t nOrderId, double dPrice, sxe_order_result* pOut)
{
	SXE_C_ORDER(pEngine, pOut, Move(static_cast<long long>(nOrderId), dPrice));
}

#undef SXE_C_ORDER

int CCStrategy::_Cancel(void* pEngine, int64_t nOrderId)
{
	auto* pSelf = static_cast<CCStrategy*>(pEngine);
	if(!pSelf->pCtx)
		return 0;
	const bool bOk = pSelf->pCtx->orders.Cancel(static_cast<long long>(nOrderId));
	pSelf->_Refresh();
	return bOk ? 1 : 0;
}

void CCStrategy::_CancelAll(void* pEngine)
{
	auto* pSelf = static_cast<CCStrategy*>(pEngine);
	if(!pSelf->pCtx)
		return;
	pSelf->pCtx->orders.CancelAll();
	pSelf->_Refresh();
}

void CCStrategy::_Log(void* pEngine, int nLevel, const char* pszMessage)
{
	auto* pSelf = static_cast<CCStrategy*>(pEngine);
	Logger& log = pSelf->pLog ? *pSelf->pLog : Logger::Get();
	const std::string strMessage = Text(pszMessage);
	if(nLevel == SXE_LEVEL_ERROR)
		log.Error(strMessage);
	else if(nLevel == SXE_LEVEL_WARNING)
		log.Warning(strMessage);
	else
		log.Info(strMessage);
}

void CCStrategy::_LogOnce(void* pEngine, int nLevel, const char* pszMessage)
{
	auto* pSelf = static_cast<CCStrategy*>(pEngine);
	Logger& log = pSelf->pLog ? *pSelf->pLog : Logger::Get();
	const std::string strMessage = Text(pszMessage);
	if(nLevel == SXE_LEVEL_ERROR)
		log.ErrorOnce(strMessage);
	else if(nLevel == SXE_LEVEL_WARNING)
		log.WarningOnce(strMessage);
	else
		log.Info(strMessage);
}

void CCStrategy::_ClearOnce(void* pEngine)
{
	auto* pSelf = static_cast<CCStrategy*>(pEngine);
	Logger& log = pSelf->pLog ? *pSelf->pLog : Logger::Get();
	log.ClearOnce();
}
