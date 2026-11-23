#pragma once
#include <chrono>
#include <functional>
#include <memory>
#include <string>

#include "CCsvFeed.h"
#include "CCycleArchive.h"
#include "CSimExchange.h"
#include "CSimStateFile.h"
#include "CStrategyLoader.h"
#include "Settings.h"
#include "ThreadBase.h"

//=============================
// Engine: a single thread, it drives feed, exchange and strategy
//
// Setup():  loads the strategy (CStrategyLoader), LoadConfig(), prepares the feed and
//           the exchange of the chosen mode, resumes the saved state if asked,
//           Init() of the strategy
// run():    one step (one tick):
//             new candle -> funding
//             price -> exchange (liquidation)
//             Run() of the strategy
//             cycle closed (by the strategy or by a liquidation) -> archive in Backup
//             state saved (at every order and every save_every candles)
//           At the end of the data, Stop, Error, account exhausted or Ctrl+C: Term() and summary
//
// Simulation only for now.
//
// Several engines in the same process: each with its own Logger, its own folders
// (CSettings::SetOutputFolders), shared candles (EngineOptions::pCandles) and its loop
// in the thread chosen by whoever uses them (run_here()). At the end of the run the
// numbers of the summary are in GetResult().
//=============================

//result of a run, complete when the simulation has ended
struct SimResult
{
	bool        bFinished           = false;	//simulation ended (also because of an error)
	bool        bError              = false;
	std::string strReason;						//why it ended
	long long   nFirstTime          = 0;		//first and last candle of the period
	long long   nLastTime           = 0;
	double      dInitialCapital     = 0;
	double      dFinalEquity        = 0;
	double      dReturn             = 0;		//% of the initial capital
	double      dMaxDrawdown        = 0;		//worst fall of the equity from its peak (%)
	long long   nCycles             = 0;		//positions closed by the strategy (liquidations are in nLiquidations)
	long long   nWins               = 0;
	long long   nLiquidations       = 0;
	long long   nOrders             = 0;
	long long   nRejected           = 0;
	double      dFees               = 0;
	double      dFunding            = 0;
	double      dWorstCycleDrawdown = 0;		//worst max_drawdown of a cycle (%), the last one included if open
	double      dMaxLeverage        = 0;		//highest max_leverage of a cycle
	bool        bOpenAtEnd          = false;	//position still open at the end of the run
	long long   nUnderwater         = 0;		//cycles with equity below zero: only with liquidation = 0, unrealistic
	long long   nSteps              = 0;
	double      dSeconds            = 0;
};

//progress of the run, every EngineOptions::nProgressEvery steps
struct ProgressInfo
{
	long long nTime     = 0;
	long long nStep     = 0;
	long long nCycle    = 0;	//current cycle, or the next one if the position is closed
	double    dPrice    = 0;
	double    dEquity   = 0;
	double    dDrawdown = 0;	//from the peak of the equity (%), <= 0
};

//a fill (C interface: sxe_engine_on_fill)
struct FillInfo
{
	long long   nTime        = 0;
	long long   nOrderId     = 0;		//0 = market order
	PositionSide eSide       = PositionSide::Long;
	const char* pszAction    = "";		//"open", "reduce", "close", "liquidation", "partial_liquidation"
	OrderType   eType        = OrderType::Market;
	double      dPrice       = 0;
	double      dQty         = 0;
	double      dFee         = 0;
	double      dPnL         = 0;		//gross realized PnL; liquidation: margin lost (negative)
	double      dPositionQty = 0;		//after the fill
	double      dAvgPrice    = 0;		//average price after (close: of the closed position)
	double      dWallet      = 0;		//balance after
	double      dFunding     = 0;		//close, liquidation: funding paid by the position
	std::string strReason;				//reduce, close: "strategy", "limit", "stop", "liquidation"
};

//state at every closed candle (C interface: sxe_engine_on_candle)
struct CandleInfo
{
	long long nTime       = 0;		//open time of the closed candle
	double    dOpen       = 0;
	double    dHigh       = 0;
	double    dLow        = 0;
	double    dClose      = 0;
	double    dEquity     = 0;		//at the first price after the close, after Run() of the strategy
	double    dWallet     = 0;
	double    dDrawdown   = 0;		//from the peak of the equity (%), <= 0
	double    dLongQty    = 0;
	double    dLongAvg    = 0;
	double    dShortQty   = 0;
	double    dShortAvg   = 0;
};

//how the engine runs, besides EngineConfig.ini
struct EngineOptions
{
	bool bArchive   = true;		//b<N> folders, result.txt and total_result.txt
	bool bStateFile = true;		//state_file (resume)
	bool bOrderLog  = true;		//order_log
	//candles already read with CCsvFeed::Read() or given by the caller; nullptr = read here
	std::shared_ptr<const CandleSeries> pCandles;

	//callbacks (C interface), called in the thread of the engine; empty = none
	long long nProgressEvery = 0;									//steps between two fnProgress, 0 = never
	std::function<void(const ProgressInfo&)>            fnProgress;
	std::function<void(const CCycleArchive::CycleRow&)> fnCycle;		//every closed cycle
	std::function<void(const FillInfo&)>                fnFill;		//every fill
	std::function<void(const CandleInfo&)>              fnCandle;		//every closed candle
};

class CEngine : public ThreadBase
{
public:
	//log of the engine and of the strategy: the one of the process, or one per engine
	explicit CEngine(Logger& logger = Logger::Get());
	~CEngine() override;

	bool Setup(const CSettings& settings, const EngineOptions& options = EngineOptions());

	void run() override;

	//0 = ended normally, 1 = error
	int ExitCode() const { return nExitCode; }

	const SimResult& GetResult() const { return result; }

	//stop requested from outside (Ctrl+C): the engine closes at the next step
	void RequestStop() { bStopRequested = true; }

protected:
	Logger& _log() override { return logger; }

private:
	//IOrders of the strategy: executes on the exchange and updates the context at once
	class COrderRouter : public IOrders
	{
	public:
		explicit COrderRouter(CEngine& engine): engine(engine) {}
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
	private:
		OrderResult _After(const OrderResult& r);	//events of the exchange and context updated
		CEngine& engine;
	};

	bool _Resume(const CSettings& settings);
	void _SaveState();
	void _ProcessEvents();				//openings, ends of cycles and liquidations reported by the exchange
	void _Finish(bool bError, const std::string& strReason);
	void _ArchiveCycle();
	void _ArchiveOpenCycle();
	void _UpdateContext();
	void _PrintStatus(bool bForce);

private:
	Logger&                       logger;
	CStrategyLoader               loader;
	CStrategyBase*                pStrategy;
	std::unique_ptr<CCsvFeed>     pFeed;
	std::unique_ptr<CSimExchange> pExchange;
	CCycleArchive                 archive;
	COrderRouter                  orderRouter;
	StepContext                   ctx;
	MarketStep                    step;

	bool              bConsole;
	double            dInitialCapital;
	double            dMinNotional;
	std::string       strStrategyName;
	std::string       strDataFile;
	std::string       strStateFile;
	int               nSaveEvery;
	std::atomic<bool> bStopRequested;
	bool              bStrategyStarted;
	bool              bArchiveOpenCycle;	//simulation from the start: at the end the open cycle goes to the backup too
	bool              bSaveNow;				//order or liquidation in the step: state to save at once
	bool              bStateFile;
	long long         nProgressEvery;
	std::function<void(const ProgressInfo&)> fnProgress;
	std::function<void(const FillInfo&)>     fnFill;
	std::function<void(const CandleInfo&)>   fnCandle;
	bool              bResumed;				//simulation state reloaded from state_file
	bool              bLiquidation;			//[SIMULATION] liquidation
	bool              bUnderwater;			//current cycle with equity below zero (only with liquidation off)
	long long         nUnderwater;			//cycles that went below zero at least once
	long long         nOrdersVersion;		//version of the open orders already copied into ctx
	int               nExitCode;
	SimResult         result;

	//statistics of the run
	long long         nStartStep;			//step this run started from (resume)
	double            dPeakEquity;
	double            dMaxDrawdown;			//worst fall of the equity from its peak (%)
	std::chrono::steady_clock::time_point tStart;

	//close executed during the step: the cycle is archived when Run() returns,
	//so the log lines written by the strategy in that step end up in its backup
	struct PendingClose
	{
		bool        bPending = false;
		double      dExitPrice = 0;
		double      dFee = 0;
		double      dWalletAfter = 0;
		Position    positionBefore;
		std::string strReason;
	};
	PendingClose      pendingClose;
};
