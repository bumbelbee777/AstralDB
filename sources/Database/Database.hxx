#pragma once

#include <IO/Spinlock.hxx>
#include <IO/Task.hxx>
#include <IO/Logger.hxx>
#include <DS/EncryptedString.hxx>
#include <Database/User.hxx>
#include <Database/IndexManagement.hxx>
#include <string>
#include <unordered_map>
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
    struct Column {
        std::string Name;
        bool IsPrimaryKey = false;
        bool IsUnique = false;
        bool IsNotNull = false;
        std::string DefaultValue;
    };

    User Owner_;
    std::optional<User> CurrentUser_;
    std::vector<User> Users_;

    using Schema = std::vector<Column>;
    using Item = std::unordered_map<std::string, std::string>;
    using Table = std::vector<Item>;
    using TablesMap = std::unordered_map<std::string, Table>;

    mutable Spinlock Lock_;
    std::unordered_map<std::string, Schema> TableSchemas_;
    TablesMap Tables_;
    std::filesystem::path DbPath_;
    Logger* Logger_ = nullptr;
    std::unordered_map<std::string, std::unordered_map<std::string, IndexManagement<std::string, size_t>>> Indexes_;
    std::unordered_map<std::string, std::vector<ForeignKey>> ForeignKeys_;

    std::atomic<bool> Dirty_;
    std::atomic<bool> StopFlushWorker_;
    std::thread FlushWorkerThread_;

    std::unordered_map<std::string, std::unordered_map<std::string, Permissions>> Acls_;

    void FlushWorker() noexcept;
    void SyncToFile();
    std::string CompressData(const std::string &Data);
    std::string DecompressData(const std::string &CompressedData);
    std::string EncryptData(const std::string &Data);
    std::string DecryptData(const std::string &EncryptedData);
public:
    explicit Database(const std::filesystem::path &DbPath, Logger* Logger = nullptr);
    ~Database();
    Database(Database&&) noexcept = delete;
    Database& operator=(Database&&) noexcept = delete;
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    std::future<void> CreateTable(const std::string &TableName, const Schema &Columns);
    std::future<void> DropTable(const std::string &TableName);
    std::future<void> Insert(const std::string &TableName, const Item &Row);
    std::future<void> Delete(const std::string &TableName, const std::function<bool(const Item&)> &Condition);
    std::future<void> Update(const std::string &TableName,
                             const std::function<bool(const Item&)> &Condition,
                             const Item &NewValues);
    std::future<Table> Select(const std::string &TableName, const std::function<bool(const Item&)> &Condition) const;
    std::future<bool> ValidateRow(const std::string &TableName, const Item &Row) const;
    std::future<bool> LoadFromFile(std::filesystem::path &Path);

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

    const TablesMap &Tables() const;
    std::filesystem::path GetDbPath() const;

    IndexManagement<std::string, size_t>& GetOrCreateIndex(const std::string& table, const std::string& column);

    bool ExportToCSV(std::filesystem::path Destination);
    bool ExportToJSON(std::filesystem::path Destination);

    void SetLogger(Logger* Logger) { Logger_ = Logger; }
    Logger* GetLogger() const { return Logger_; }
};
}
