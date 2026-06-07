#pragma once

#include <IO/Limits.hxx>
#include <Database/Database.hxx>
#include <Database/Storage/ColumnarStorage.hxx>
#include <SQL/Bytecode/Bytecode.hxx>
#include <SQL/Profiler/SqlSessionConfig.hxx>
#include <SQL/Shape/ShapeTelemetryTypes.hxx>
#include <cstdint>
#include <vector>
#include <iostream>
#include <stdexcept>
#include <memory>
#include <string>
#include <IO/Logger.hxx>
#include <unordered_map>
#include <filesystem>
#include <vector>
#include <optional>

namespace AstralDB {
namespace SQL {

class VmDebugSession;

struct VmStackSlot {
	uint64_t Word = 0;
	/** When true, \c Word is an owning \c std::string* allocated with \c new. */
	bool OwnsCppStringHeap = false;
};

class BytecodeInterpreter {
	uintptr_t Ic;
	uintptr_t Sp;
	uintptr_t Bp;
	uint32_t Flags;
	std::vector<uint64_t> Registers_;
	std::vector<VmStackSlot> StackSlots_;

	std::vector<std::unique_ptr<Database>> Databases_;
	std::filesystem::path DatabasePath_{"astral.db"};
	Logger *Logger_ = nullptr;
	std::unordered_map<std::string, std::string> Savepoints_;
	std::unordered_map<std::string, Database::DatabaseWorkingSnapshot> MemSavepoints_;
	/** When executing deduplicated bytecode, immediate strings are resolved via this pool for PUSH_POOL. */
	const std::vector<std::string> *StringOperandPool_ = nullptr;
	/** Monotonic interpreter steps for this execution slice (RESET clears). */
	std::size_t StepsExecuted_ = 0;

public:
	struct TimeSqlStats {
		std::uint64_t RowsScanned = 0;
		std::uint64_t ResultRows = 0;
		/** Minimum scanned rows fast paths must touch (0 = no check). */
		std::uint64_t MinRowsScannedExpected = 0;
		/** Minimum result rows expected when data should match predicates. */
		std::uint64_t MinResultRowsExpected = 0;
		std::uint32_t FastPathFlags = 0;
		ShapeTelemetry ShapeStats{};
		bool IntegrityFailed = false;
		std::string IntegrityMessage;
	};

private:
	TimeSqlStats LastTimeSqlStats_{};

public:
	VmDebugSession *DebugSession_ = nullptr;
	std::optional<StorageLayout> SessionStorageHint_;
	/** Extra SELECT columns pushed by the most recent \c COLUMNS_EXPAND in this query. */
	size_t ColumnsExpandExtra_ = 0;
	int64_t SessionIsolation_ = 0;
	SqlSessionConfig SessionConfig_{SqlSessionConfig::FromEnvironment()};
	std::unordered_map<std::string, std::string> TableComments_;
	std::unordered_map<std::string, std::unordered_map<std::string, std::string>> ColumnComments_;

	struct ProcTryFrame {
		std::string Savepoint;
		std::size_t EndIc = 0;
		std::vector<std::pair<std::size_t, std::string>> Handlers;
	};
	std::vector<ProcTryFrame> ProcTryStack_;

	bool DispatchProcedureException(const std::runtime_error &Err);
	void VmSavepoint(const std::string &Name);
	void VmRollbackToSavepoint(const std::string &Name);
	void VmReleaseSavepoint(const std::string &Name);

	void CleanupStack();

public:
	void PushScalarWord(uint64_t v);
	void PushOwningStringHeap(std::string *p);
	uint64_t PopScalarWord(const char *ctx);
	std::string PopOwnedStringMoved(const char *ctx);
	void PopDiscardTopSlot();

	BytecodeInterpreter(Logger *Logger = nullptr) : Ic(0), Sp(0), Bp(0), Flags(0), Logger_(Logger) {
		Registers_.resize(16, 0);
	}

	~BytecodeInterpreter() { CleanupStack(); }

	void SetLogger(Logger *Logger) { Logger_ = Logger; }
	Logger *GetLogger() const { return Logger_; }

	void SetDebugSession(VmDebugSession *Session) { DebugSession_ = Session; }
	VmDebugSession *DebugSession() const { return DebugSession_; }

	void ClearSessionStorageHint() { SessionStorageHint_.reset(); }
	const std::optional<StorageLayout> &SessionStorageHint() const { return SessionStorageHint_; }

	SqlSessionConfig &SessionConfig() { return SessionConfig_; }
	const SqlSessionConfig &SessionConfig() const { return SessionConfig_; }

	std::size_t StepsExecuted() const { return StepsExecuted_; }

	void ResetTimeSqlStats() { LastTimeSqlStats_ = {}; }
	const TimeSqlStats &LastTimeSqlStats() const { return LastTimeSqlStats_; }
	TimeSqlStats &MutableTimeSqlStats() { return LastTimeSqlStats_; }

	void DatabasePath(std::filesystem::path Path) { DatabasePath_ = std::move(Path); }
	const std::filesystem::path &DatabasePath() const { return DatabasePath_; }

	void Execute(const Bytecode &Code);

	/** Execute bytecode with optional immutable string pool (from BuildCompiledBytecode / Dedup). */
	void Execute(const Bytecode &Code, const std::vector<std::string> *StringPool);

	void Execute(const CompiledBytecode &Compiled);

	/** Run a nested program without resetting session state; resumes at the next outer instruction. */
	void RunNestedBytecode(const Bytecode &Code, const std::vector<std::string> *StringPool = nullptr);

	bool Step(const Bytecode &Code);
	bool StepBulk(const Bytecode &Code, const Instruction &Inst);

	/** Clear VM stack/registers only (retains open database session). */
	void ResetVmState();

	/** Alias for \ref ResetVmState. */
	void Reset() { ResetVmState(); }

	/** Drop database handles and optional new path before another \c Execute of a full script. */
	void ResetExecutionSession(std::optional<std::filesystem::path> NewDatabasePath = std::nullopt,
	                          bool WipeOnDisk = true);

	uintptr_t CurrentInstruction() const { return Ic; }
	void SeekInstruction(std::size_t Ip) { Ic = Ip; }

	uintptr_t StackBase() const { return Bp; }

	uintptr_t StackTop() const { return Sp; }

	std::vector<uint64_t> Registers() const { return Registers_; }

	Database *BorrowedPrimary_ = nullptr;
	Database *PrimaryDatabase() {
		if(BorrowedPrimary_)
			return BorrowedPrimary_;
		return Databases_.empty() ? nullptr : Databases_[0].get();
	}

	const Database *PrimaryDatabase() const {
		if(BorrowedPrimary_)
			return BorrowedPrimary_;
		return Databases_.empty() ? nullptr : Databases_[0].get();
	}

	/** Open the primary Database instance at DatabasePath without executing bytecode (catalog used at compile-time). */
	void EnsurePrimaryDatabaseOpened();
	/** Use an already-open session database (trigger/procedure nested execution under DbMutex). */
	void SetBorrowedPrimaryDatabase(Database *Db) { BorrowedPrimary_ = Db; }

	/** DML/DDL target; uses \c BorrowedPrimary_ when set (nested trigger/procedure bytecode). */
	Database *MutatingDatabase() {
		if(BorrowedPrimary_)
			return BorrowedPrimary_;
		if(Databases_.empty())
			Databases_.push_back(std::make_unique<Database>(DatabasePath_, Logger_));
		return Databases_[0].get();
	}

	/** Recreate primary database connection from DbPath disk state (used after WAL/snapshot restores). */
	void ReloadPrimaryDatabaseFromDisk();
	/** Close the live connection, overwrite the main DB file from \a SnapshotPath, drop WAL, reopen. */
	void RestorePrimaryDatabaseFromSnapshotFile(const std::filesystem::path &SnapshotPath);

	void DumpRegs() const {
		std::cout << "Registers:\n";
		std::cout << "IC: " << Ic << "\n";
		std::cout << "SP: " << Sp << "\n";
		std::cout << "BP: " << Bp << "\n";
		std::cout << "Flags:" << Flags << "\n";
		for(size_t i = 0; i < Registers_.size(); ++i)
			std::cout << "R" << i << ": " << Registers_[i] << "\n";
	}
};
} // namespace SQL
} // namespace AstralDB

