#pragma once
#include <functional>
#include <string>
#include <vector>

#include "Types.h"

//=============================
// Archive of the cycles (Target/Backup)
//
// Cycle = from the first opening of a position until none is left open
// (in hedge a cycle can have long and short together; in one-way reversing the position
// closes a cycle and opens another one).
// At the close:
//   - creates <backup>/b<N> and writes result.txt in it (summary of the cycle)
//   - moves into b<N> every file of <log> except those to keep (simulation state)
//   - adds a row to <backup>/total_result.txt
// Numbering goes on from the highest b<N> already there: a backup is never overwritten.
// With bWrite = false (runs that write no files) it writes and moves nothing: it only keeps
// the totals of the cycles in memory (GetTotals).
// Every closed cycle is also given to the callback (SetCycleCallback), with or without files.
//=============================
class CCycleArchive
{
public:
	struct Snapshot
	{
		bool      bOpen          = false;
		long long nOpenTime      = 0;
		double    dWalletStart   = 0;
		double    dFirstPrice    = 0;
		int       nBuys          = 0;		//openings and increases
		double    dMinEquityPerc = 0;
		double    dMaxLeverage   = 0;
		bool      bLong          = false;	//the cycle had a long
		bool      bShort         = false;	//... a short
	};

	struct Totals
	{
		long long nCycles        = 0;
		double    dWorstDrawdown = 0;
		double    dMaxLeverage   = 0;
		double    dHours         = 0;
	};

	struct CycleRow
	{
		long long   nCycle        = 0;
		long long   nOpenTime     = 0;
		long long   nCloseTime    = 0;
		double      dCapitalStart = 0;
		double      dCapitalEnd   = 0;
		double      dGrowth       = 0;
		double      dDrawdown     = 0;
		double      dMaxLeverage  = 0;
		int         nBuys         = 0;	//openings and increases
		double      dEntryPrice   = 0;
		double      dAvgPrice     = 0;
		double      dExitPrice    = 0;
		double      dFees         = 0;
		double      dFunding      = 0;
		std::string strClosedBy;		//why it closed (strReason of Close)
		std::string strSide;			//"long", "short" or "long+short"
	};

public:
	CCycleArchive();

	bool Setup(const std::string& strLogDir, const std::string& strBackupDir,
	           const std::vector<std::string>& vKeep, std::string& strError, bool bWrite = true);

	long long CurrentCycle() const { return nCycle; }
	bool IsOpen() const { return bOpen; }

	void WriteNote(const std::string& strNote);

	//opening or increase executed: without an open cycle it opens one
	void OnOpen(long long nTime, double dFillPrice, double dWalletBefore, PositionSide eSide);
	//at every step with the cycle open: maximum drawdown and leverage of the cycle; dNotional = value
	//of the open positions (quantity * average price, long and short)
	void OnStep(const Account& account, double dNotional);

	std::string Close(long long nTime, double dExitPrice, const Position& positionBefore,
	                  double dCloseFee, double dWalletAfter, const std::string& strReason, std::string& strError);

	bool MoveLogFiles(std::string& strError);

	Snapshot GetSnapshot() const;
	void Restore(const Snapshot& snapshot);

	const Totals& GetTotals() const { return totals; }

	void SetCycleCallback(std::function<void(const CycleRow&)> fnCallback) { fnCycle = std::move(fnCallback); }

private:
	std::string strLogDir;
	std::string strBackupDir;
	std::string strTotalFile;
	std::string strCycleDir;
	std::vector<std::string> vKeep;
	bool      bWrite;
	Totals    totals;
	std::function<void(const CycleRow&)> fnCycle;

	long long nCycle;
	bool      bOpen;
	long long nOpenTime;
	double    dWalletStart;
	double    dFirstPrice;
	int       nBuys;
	double    dMinEquityPerc;		//worst equity of the cycle against the initial capital (%)
	double    dMaxLeverage;			//value of the positions / initial capital
	bool      bLong;
	bool      bShort;

	std::string _Side() const;
};
