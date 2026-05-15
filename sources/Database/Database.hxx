#pragma once

#include <Database/WriteAheadLog.hxx>
#include <IO/Limits.hxx>
#include <IO/Spinlock.hxx>
#include <mutex>
#include <utility>
#include <IO/Task.hxx>
#include <IO/Logger.hxx>
#include <IO/AuditLog.hxx>
#include <DS/EncryptedString.hxx>
#include <Database/User.hxx>
#include <Database/IndexManagement.hxx>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <filesystem>
#include <future>
#include <functional>
#include <atomic>
#include <thread>
#include <optional>

namespace AstralDB {
#if defined(__GNUC__)
#define PREFETCH(Address) __builtin_prefetch(Address)
#else
#define PREFETCH(Address)
#endif

struct ForeignKey {
    std::string ColumnName;
    std::string ReferencedTable;
    std::string ReferencedColumn;
};

class Database {
public:
	struct Column {
		std::string Name;
		bool IsPrimaryKey = false;
		bool IsUnique = false;
		bool IsNotNull = false;
		/** Declared SQL type (INT, TEXT, NUMERIC(10,2), DATE, etc.). */
		std::string DefaultValue;
		/** Optional literal for row default on INSERT (parsed DEFAULT clause); not yet fully enforced. */
		std::optional<std::string> InsertDefaultLiteral;
		/** CHECK (...) source text when present (diagnostics). */
		std::optional<std::string> CheckConstraintSql;
		/** Precompiled WHERE-style DNF pack; enforced on INSERT/UPDATE when present. */
		std::optional<std::string> CheckConstraintDnfPacked;
		/** Populated when column ends with REFERENCES ref_table(ref_col); applied after CREATE. */
		std::optional<ForeignKey> DeclaredFk;
	};

	using Schema = std::vector<Column>;
    using Item = std::unordered_map<std::string, std::string>;
    using Table = std::vector<Item>;
    using TablesMap = std::unordered_map<std::string, Table>;

private:
    std::optional<User> CurrentUser_;
    std::vector<User> Users_;

    mutable SharedMutex DbMutex_;
	/** Bounding std::async fan-out against host thread exhaustion under bursty workloads. */
	mutable std::atomic<std::size_t> OutstandingAsyncJobs_{0};
	WriteAheadLog Wal_;
    std::map<std::string, Schema> TableSchemas_;
    Logger* Logger_ = nullptr;
    std::unordered_map<std::string, std::unordered_map<std::string, IndexManagement<std::string, size_t>>> Indexes_;
    std::unordered_map<std::string, std::vector<ForeignKey>> ForeignKeys_;
	/** Table-level CHECK: (snippet, packed DNF) in declaration order (see CREATE TABLE VM path). */
	std::unordered_map<std::string, std::vector<std::pair<std::string, std::string>>> TableCheckConstraints_;
	/** Persisted CREATE VIEW SELECT bodies (replay + compile-time lookup). Disjoint from physical table keys. */
	std::unordered_map<std::string, std::string> ViewDefinitionSql_;

    std::atomic<bool> Dirty_;
	std::atomic<bool> WalSuspended_{false};
    std::atomic<bool> StopFlushWorker_;
    std::thread FlushWorkerThread_;
	/** When true, destructor must not SyncToFile (on-disk file was replaced or will be reopened). */
	bool SkipExitSyncOnDestroy_ = false;

    std::unordered_map<std::string, std::unordered_map<std::string, Permissions>> Acls_;
	std::unordered_map<std::string, std::unordered_map<std::string, Permissions>> RoleAcls_;
	std::unordered_map<std::string, std::vector<std::string>> UserRoles_;
	std::unordered_set<std::string> Roles_;
	mutable AuditLog AuditLog_;

    void FlushWorker() noexcept;
	void JoinFlushWorkerBestEffort() noexcept;
	bool LoadSnapshotFromDiskPathSynchronously(std::filesystem::path Path);

	void AppendWalAfterCreate(const std::string &TableName, const Schema &Columns);
	void AppendWalAfterInsert(const std::string &TableName, const Item &Row);
	void AppendWalAfterDrop(const std::string &TableName);
	void AppendWalAfterDefineView(const std::string &ViewName, const std::string &SqlBody);
	void AppendWalAfterDropView(const std::string &ViewName);
	void AppendWalAfterAddUser(const User &User);
	void AppendWalAfterRemoveUser(const std::string &Name);
	void AppendWalAfterGrantAcl(const std::string &UserName, const std::string &Table, int Bits);
	void AppendWalAfterRevokeAcl(const std::string &UserName, const std::string &Table, int Bits);
	void AppendWalAfterCreateRole(const std::string &RoleName);
	void AppendWalAfterDropRole(const std::string &RoleName);
	void AppendWalAfterGrantRoleMembership(const std::string &RoleName, const std::string &UserName);
	void AppendWalAfterRevokeRoleMembership(const std::string &RoleName, const std::string &UserName);
	void AppendWalAfterGrantRoleAcl(const std::string &RoleName, const std::string &Table, int Bits);
	void AppendWalAfterRevokeRoleAcl(const std::string &RoleName, const std::string &Table, int Bits);
	void AppendWalAfterFineGrant(const std::string &UserName, const RowColPermission &Rule);
	void AppendWalAfterFineRevoke(const std::string &UserName, const RowColPermission &Rule);

	void RejectRowIfChecksFailAssumeLocked(const std::string &TableName, const Item &Row) const;
	void AuditRecordAssumeLocked(const std::string &Event, const std::string &Detail, const std::string &Outcome) const;

	bool AclEnforcementActiveAssumeLocked() const;
	Permissions EffectivePermissionsAssumeLocked(const User &User, const std::string &Table) const;
	std::string RowKeyAssumeLocked(const std::string &Table, const Item &Row) const;
	bool RowAllowsAssumeLocked(Permissions Perms, const std::string &Table, const Item &Row) const;
	bool ColumnAllowsAssumeLocked(Permissions Perms, const std::string &Table, const Item &Row,
	                              const std::string &Column) const;
	Item MaskRowForSelectAssumeLocked(const std::string &Table, const Item &Row) const;
	bool IsRoleAssumeLocked(const std::string &Name) const;
	void RequireSessionDdlAssumeLocked() const;
	void RequireSessionGrantAdminAssumeLocked() const;
	void RequireSessionTablePermissionAssumeLocked(Permissions Perms, const std::string &Table) const;
	void RequireSessionRowPermissionAssumeLocked(Permissions Perms, const std::string &Table, const Item &Row) const;
	void RequireSessionInsertAssumeLocked(const std::string &Table, const Item &Row) const;
	void RequireSessionUpdateAssumeLocked(const std::string &Table, const Item &ExistingRow,
	                                      const Item &NewValues) const;
	void EnsureBootstrapAdminAclAssumeLocked();

    std::string CompressData(const std::string &Data);
    std::string DecompressData(const std::string &CompressedData);
    std::string EncryptData(const std::string &Data);
    std::string DecryptData(const std::string &EncryptedData);

    /** Serialize current state; caller must hold \c DbMutex_ exclusively (see \c SyncToFile()). */
    void SyncToFileUnlocked();

public:
    TablesMap Tables_;
    std::filesystem::path DbPath_;

    explicit Database(const std::filesystem::path &DbPath, Logger* Logger = nullptr);
    ~Database();
    Database(Database&&) noexcept = delete;
    Database& operator=(Database&&) noexcept = delete;
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    void SyncToFile();
	/** Like \c SyncToFile() then copy \c DbPath_ to \a SnapshotPath with \c DbMutex_ held throughout so the
	 *  background flush worker cannot truncate the DB file mid-copy (BEGIN / SAVEPOINT snapshots). */
	void SyncToFileAndCopyMainDbFileTo(const std::filesystem::path &SnapshotPath);
	/** Persist any buffered WAL lines to the .wal file (COMMIT / checkpoint / shutdown). */
	void FlushWalToDisk();

	/** Internal: bounded async worker accounting; do not call from application code. */
	void AcquireAsyncBudget() const;
	void ReleaseAsyncBudget() const;

    std::future<void> CreateTable(const std::string &TableName, const Schema &Columns);
    std::future<void> AddColumn(const std::string &TableName, const Column &NewColumn);
    std::future<void> DropColumn(const std::string &TableName, const std::string &ColumnName);
    std::future<void> DropTable(const std::string &TableName);
    std::future<void> Insert(const std::string &TableName, const Item &Row);
    std::future<void> Delete(const std::string &TableName, const std::function<bool(const Item&)> &Condition);
    std::future<void> Update(const std::string &TableName,
                             const std::function<bool(const Item&)> &Condition,
                             const Item &NewValues);
    std::future<Table> Select(const std::string &TableName, const std::function<bool(const Item&)> &Condition) const;
    std::future<bool> ValidateRow(const std::string &TableName, const Item &Row) const;
	/** Stored compiled table CHECK list; bytecode VM invokes after CREATE. Serializes via DbMutex_. */
	void ReplaceTableLevelCheckConstraints(const std::string &TableName,
	                                       std::vector<std::pair<std::string, std::string>> Checks);
    std::future<bool> LoadFromFile(std::filesystem::path &Path);

	std::optional<Schema> TableSchemaSnapshot(const std::string &TableName) const;

	/** Snapshot from \c TableSchemas_ without locking. Only call while \c DbMutex_ is already held exclusively
	 *  (same thread as DELETE/UPDATE matchers) or from other contexts where concurrent schema mutation cannot occur. */
	std::optional<Schema> TableSchemaAssumeDbMutexHeld(const std::string &TableName) const;

    std::future<Table> JoinTables(const std::string &LeftTable, const std::string &RightTable,
                                  const std::function<bool(const Item&, const Item&)> &JoinCondition) const;
    std::future<void> AddForeignKey(const std::string &TableName, const ForeignKey &Key);

    std::future<void> AddUser(const User &User);
    std::future<void> RemoveUser(const User &User);
    std::future<void> SetCurrentUser(const User &User);

    std::future<void> AddIndex(const std::string &TableName, const std::string &ColumnName);
    std::future<void> RemoveIndex(const std::string &TableName, const std::string &ColumnName);

    // Authentication
    bool AuthenticateUser(const std::string& Username, const std::string& Password);
    void Logout();
    bool IsAuthenticated() const;
    const std::optional<User>& CurrentUser() const;

    // Permissions & ACLs
    bool HasPermission(const User &User, Permissions Perms, const std::string &Table = "") const;
    std::future<void> GrantPermission(const std::string &UserName, Permissions Perms, const std::string &Table = "");
    std::future<void> RevokePermission(const std::string &UserName, Permissions Perms, const std::string &Table = "");
    std::future<Permissions> UserPermissions(const std::string &UserName, const std::string &Table = "") const;
	/** Attach a row/column-scoped grant to a catalog user (stored on the user record). */
	std::future<void> GrantRowPermission(const std::string &UserName, RowColPermission Rule);
	std::future<void> RevokeRowPermission(const std::string &UserName, RowColPermission Rule);

	std::future<void> CreateRole(const std::string &RoleName);
	std::future<void> DropRole(const std::string &RoleName);
	std::future<void> GrantRoleToUser(const std::string &RoleName, const std::string &UserName);
	std::future<void> RevokeRoleFromUser(const std::string &RoleName, const std::string &UserName);
	std::future<void> GrantRolePermission(const std::string &RoleName, Permissions Perms, const std::string &Table = "");
	std::future<void> RevokeRolePermission(const std::string &RoleName, Permissions Perms, const std::string &Table = "");

	void SetAuditLogPath(std::optional<std::filesystem::path> Path);

    IndexManagement<std::string, size_t>& GetOrCreateIndex(const std::string& table, const std::string& column);

    bool ExportToCSV(std::filesystem::path Destination);
    bool ExportToJSON(std::filesystem::path Destination);
    bool ExportToTSV(std::filesystem::path Destination);
    /** Full-database bundle (markers + CSV semantics for csv/tsv). Format: json | csv | tsv */
    bool ExportBundle(std::filesystem::path Destination, std::string_view Format);
    bool ImportFromCSV(const std::string& TableName, std::filesystem::path Source);
    bool ImportFromJSON(const std::string& TableName, std::filesystem::path Source);
    bool ImportFromTSV(const std::string& TableName, std::filesystem::path Source);
    /** Replace all loaded tables/schemas with bundle contents. Format: json | csv | tsv */
    bool ImportBundle(std::filesystem::path Source, std::string_view Format);

    static bool ConvertTabularFiles(std::filesystem::path SourcePath, std::filesystem::path DestPath,
                                     std::string_view SourceFormat, std::string_view DestFormat);

    void SetLogger(Logger* Logger) { Logger_ = Logger; }
    Logger* GetLogger() const { return Logger_; }

	/** On destruction, do not persist in-memory tables to disk (caller replaced the backing file). */
	void SetSkipExitSyncOnDestroy(bool Skip) noexcept { SkipExitSyncOnDestroy_ = Skip; }

	/** Drain async workers and halt the flush thread before overwriting the backing DB file (paired with ClearDirty…). */
	void QuiesceBackgroundIOForFilesystemRollback() noexcept;

	/** Clears Dirty_ under DbMutex_. Call after QuiesceBackgroundIOForFilesystemRollback() before copy/replace on disk. */
	void ClearDirtyForFilesystemRollback();

	/** Run \a fn under \c DbMutex_ exclusive lock. SQL VM uses this for synchronous \c Tables_ access; never call
	 *  \c Insert().get() / \c CreateTable().get() etc. from inside \a fn (async workers need the same mutex). */
	template<class Fn>
	void WithExclusiveBytecodeLock(Fn &&fn) {
		std::scoped_lock<SharedMutex> lock(DbMutex_);
		std::forward<Fn>(fn)();
	}

	/** Snapshot copy of table + schema (no WAL records). Dest is replaced if it already existed. */
	void CloneTable(const std::string &Dest, const std::string &Src);

	/** COPY of view SQL bodies (thread-safe); used when compiling against a primed database. */
	std::unordered_map<std::string, std::string> ViewDefinitionsSnapshot() const;
	bool HasViewDefinition(const std::string &ViewName) const;
	/** Register a view (no physical table). Fails if a table with the same name exists. */
	void DefineView(const std::string &ViewName, std::string SqlBody);
	void DropViewDefinition(const std::string &ViewName, bool IfExists);
	/** Idempotent WAL replay helpers (no new WAL rows; overwrite view SQL if name already mapped). */
    void ReplayWalDefineView(const std::string &ViewName, std::string SqlBody);
    void ReplayWalDropView(const std::string &ViewName);
	void ReplayWalAddUser(std::string Name, std::string EncryptedBlob, std::array<uint8_t, 32> KeyMaterial);
	void ReplayWalRemoveUser(const std::string &Name);
	void ReplayWalGrantAcl(const std::string &UserName, const std::string &Table, int Bits);
	void ReplayWalRevokeAcl(const std::string &UserName, const std::string &Table, int Bits);
	void ReplayWalCreateRole(const std::string &RoleName);
	void ReplayWalDropRole(const std::string &RoleName);
	void ReplayWalGrantRoleMembership(const std::string &RoleName, const std::string &UserName);
	void ReplayWalRevokeRoleMembership(const std::string &RoleName, const std::string &UserName);
	void ReplayWalGrantRoleAcl(const std::string &RoleName, const std::string &Table, int Bits);
	void ReplayWalRevokeRoleAcl(const std::string &RoleName, const std::string &Table, int Bits);
	void ReplayWalFineGrant(const std::string &UserName, RowColPermission Rule);
	void ReplayWalFineRevoke(const std::string &UserName, RowColPermission Rule);
};
}
