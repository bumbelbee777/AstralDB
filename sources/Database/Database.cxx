#include <Database/Database.hxx>
#include <IO/Task.hxx>
#include <DS/LZ4.hxx>
#include <optional>
#include <sstream>
#include <fstream>
#include <stdexcept>
#include <algorithm>
#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <Database/IndexManagement.hxx>

namespace AstralDB {
void Database::FlushWorker() noexcept {
	using namespace std::chrono_literals;
	while(!StopFlushWorker_.load(std::memory_order_acquire)) {
		if(Dirty_.load(std::memory_order_acquire)) {
			std::this_thread::sleep_for(50ms);  // Batch delay
			{
				SpinlockGuard Guard(Lock_);
				if(Dirty_.load(std::memory_order_acquire)) {
					try {
						SyncToFile();
					} catch (...) {
						// In production, log the error
					}
					Dirty_.store(false, std::memory_order_release);
				}
			}
		} else {
			std::this_thread::sleep_for(10ms);
		}
	}
}

Database::Database(const std::filesystem::path &DbPath, Logger* Logger)
	: Owner_("Admin0", "admin", Permissions::All), CurrentUser_(std::nullopt), DbPath_(DbPath), Logger_(Logger), Dirty_(false), StopFlushWorker_(false) {
	FlushWorkerThread_ = std::thread([this]() { this->FlushWorker(); });
	if(Logger_) Logger_->Info("Database initialized at " + DbPath.string());
}

Database::~Database() {
	StopFlushWorker_.store(true, std::memory_order_release);
	if (FlushWorkerThread_.joinable())
		FlushWorkerThread_.join();
	if(Logger_) Logger_->Info("Database destroyed");
}

std::string Database::CompressData(const std::string &Data) {
	return DS::LZ4Compress(Data.data());
}

std::string Database::DecompressData(const std::string &CompressedData) {
	return DS::LZ4Decompress(CompressedData);
}

std::string Database::EncryptData(const std::string &Data) {
	EncryptedString Encrypted(Data);
	return std::string(Encrypted.Encrypted());
}

std::string Database::DecryptData(const std::string &EncryptedData) {
	EncryptedString Decrypted(EncryptedData, true);
	return std::string(Decrypted.Encrypted());
}

void Database::SyncToFile() {
	std::ostringstream OutputStream;
	OutputStream << TableSchemas_.size() << "\n";
	for(const auto &SchemaPair : TableSchemas_) {
		OutputStream << SchemaPair.first << "\n";
		OutputStream << SchemaPair.second.size() << "\n";
		for(const auto &Column : SchemaPair.second) {
			OutputStream << Column.Name << " " 
					 << Column.IsPrimaryKey << " " 
					 << Column.IsUnique << " " 
					 << Column.IsNotNull << " " 
					 << Column.DefaultValue << "\n";
		}
	}
	OutputStream << Tables_.size() << "\n";
	for(const auto &TablePair : Tables_) {
		OutputStream << TablePair.first << "\n";  // Table name
		OutputStream << TablePair.second.size() << "\n";  // Row count
		for(const auto &Row : TablePair.second) {
			OutputStream << Row.size() << "\n";  // Column count per row
			for (const auto &Column : Row) {
				OutputStream << Column.first << "\n";  // Column name
				OutputStream << Column.second << "\n";  // Column value
			}
		}
	}
	std::string RawData = OutputStream.str();
	std::string CompressedData = CompressData(RawData);
	std::string EncryptedData = EncryptData(CompressedData);
	std::ofstream File(DbPath_, std::ios::binary);
	if(!File) {
		if(Logger_) Logger_->Error("Failed to open file for writing in SyncToFile");
		throw std::runtime_error("Failed to open file for writing.");
	}
	File.write(EncryptedData.data(), EncryptedData.size());
	if(Logger_) Logger_->Info("Database synced to file");
}

std::future<void> Database::CreateTable(const std::string &TableName, const Schema &Columns) {
	return RunAsync([this, TableName, Columns]() {
		{
			SpinlockGuard Guard(Lock_);
			if (TableSchemas_.find(TableName) != TableSchemas_.end())
				throw std::runtime_error("Table already exists");
			TableSchemas_[TableName] = Columns;
			Tables_[TableName] = Table();
		}
		Dirty_.store(true, std::memory_order_release);
	});
}

std::future<void> Database::DropTable(const std::string &TableName) {
	return RunAsync([this, TableName]() {
		{
			SpinlockGuard Guard(Lock_);
			TableSchemas_.erase(TableName);
			Tables_.erase(TableName);
			Indexes_.erase(TableName);
			ForeignKeys_.erase(TableName);
		}
		Dirty_.store(true, std::memory_order_release);
	});
}

std::future<void> Database::Insert(const std::string &TableName, const Item &Row) {
	return RunAsync([this, TableName, Row]() {
		{
			SpinlockGuard Guard(Lock_);
			if(Tables_.find(TableName) == Tables_.end())
				throw std::runtime_error("Table does not exist");
			auto &TableRef = Tables_[TableName];
			if(!TableRef.empty())
				PREFETCH(TableRef.data());
			TableRef.push_back(Row);
			for (const auto& [ColumnName, Value] : Row) {
				if (Indexes_[TableName].find(ColumnName) != Indexes_[TableName].end()) {
					auto& Index = Indexes_[TableName][ColumnName];
					std::get<BPlusTree<std::string, size_t>>(Index.Index()).Insert(Value, TableRef.size() - 1);
				}
			}
		}
		Dirty_.store(true, std::memory_order_release);
	});
}

std::future<void> Database::Delete(const std::string &TableName, const std::function<bool(const Item&)> &Condition) {
	return RunAsync([this, TableName, Condition]() {
		{
			SpinlockGuard Guard(Lock_);
			auto &TableRef = Tables_.at(TableName);
			for (size_t i = 0; i < TableRef.size(); ++i) {
				if (Condition(TableRef[i])) {
					for (const auto& [ColumnName, Value] : TableRef[i]) {
						if (Indexes_[TableName].find(ColumnName) != Indexes_[TableName].end()) {
							auto& Index = Indexes_[TableName][ColumnName];
							std::get<BPlusTree<std::string, size_t>>(Index.Index()).Remove(Value);
						}
					}
				}
			}
			TableRef.erase(std::remove_if(TableRef.begin(), TableRef.end(), Condition), TableRef.end());
		}
		Dirty_.store(true, std::memory_order_release);
	});
}

std::future<void> Database::Update(const std::string &TableName, 
								   const std::function<bool(const Item&)> &Condition, 
								   const Item &NewValues) {
	return RunAsync([this, TableName, Condition, NewValues]() {
		bool Modified = false;
		{
			SpinlockGuard Guard(Lock_);
			auto TableIt = Tables_.find(TableName);
			if(TableIt == Tables_.end())
				throw std::runtime_error("Table not found");
			auto &TableRef = TableIt->second;
			for (size_t i = 0; i < TableRef.size(); ++i) {
				auto& Row = TableRef[i];
				if (Condition(Row)) {
					for (const auto& [ColumnName, NewValue] : NewValues) {
						if (Indexes_[TableName].find(ColumnName) != Indexes_[TableName].end()) {
							auto& Index = Indexes_[TableName][ColumnName];
							std::get<BPlusTree<std::string, size_t>>(Index.Index()).Remove(Row[ColumnName]);
							std::get<BPlusTree<std::string, size_t>>(Index.Index()).Insert(NewValue, i);
						}
						Row[ColumnName] = NewValue;
					}
					Modified = true;
				}
			}
		}
		if(Modified) Dirty_.store(true, std::memory_order_release);
	});
}

std::future<Database::Table> Database::Select(const std::string &TableName, const std::function<bool(const Item&)> &Condition) const {
	return RunAsync([this, TableName, Condition]() -> Table {
		Table Result;
		{
			SpinlockGuard Guard(Lock_);
			auto TableIt = Tables_.find(TableName);
			if(TableIt == Tables_.end())
				throw std::runtime_error("Table does not exist.");
			const auto &TableRef = TableIt->second;
			if(!TableRef.empty()) PREFETCH(TableRef.data());
			if(Indexes_.find(TableName) != Indexes_.end()) {
				for (const auto &ColumnIndexes : Indexes_.at(TableName)) {
					// Instead of iterating ColumnIndexes.second, get the BPlusTree and iterate its keys
					const auto& indexVariant = ColumnIndexes.second.Index();
					if (std::holds_alternative<BPlusTree<std::string, size_t>>(indexVariant)) {
						const auto& bptree = std::get<BPlusTree<std::string, size_t>>(indexVariant);
						for (const auto& key : bptree.GetAllKeys()) {
							size_t RowIndex;
							if (bptree.Search(key, RowIndex) && RowIndex < TableRef.size()) {
								const auto &Row = TableRef[RowIndex];
								if (Condition(Row))
									Result.push_back(Row);
							}
						}
					}
				}
			} else {
				for(const auto &Row : TableRef)
					if(Condition(Row)) Result.push_back(Row);
			}
		}
		return Result;
	});
}

std::future<bool> Database::ValidateRow(const std::string &TableName, const Item &Row) const {
	return RunAsync([this, TableName, Row]() -> bool {
		bool Valid = true;
		{
			SpinlockGuard Guard(Lock_);
			auto SchemaIt = TableSchemas_.find(TableName);
			if(SchemaIt == TableSchemas_.end())
				return false;
			const Schema &Columns = SchemaIt->second;
			for(const auto &Column : Columns) {
				if((Column.IsPrimaryKey || Column.IsNotNull) && Row.find(Column.Name) == Row.end()) {
					Valid = false;
					break;
				}
				if(Column.IsUnique && Indexes_.find(TableName) != Indexes_.end()) {
					auto ItCol = Indexes_.at(TableName).find(Column.Name);
					if (ItCol != Indexes_.at(TableName).end() && Row.find(Column.Name) != Row.end()) {
						if (std::get<BPlusTree<std::string, size_t>>(ItCol->second.Index()).Contains(Row.at(Column.Name))) {
							Valid = false;
							break;
						}
					}
				}
			}
		}
		return Valid;
	});
}

std::future<bool> Database::LoadFromFile(std::filesystem::path &Path) {
	return RunAsync([this, &Path]() -> bool {
		std::ifstream File(Path, std::ios::binary);
		if(!File) return false;
		std::string EncryptedData((std::istreambuf_iterator<char>(File)),
								  std::istreambuf_iterator<char>());
		std::string CompressedData = DecryptData(EncryptedData);
		std::string RawData = DecompressData(CompressedData);
		std::istringstream Input(RawData);
		size_t SchemaCount;
		Input >> SchemaCount;
		{
			SpinlockGuard Guard(Lock_);
			TableSchemas_.clear();
			Tables_.clear();
			for (size_t i = 0; i < SchemaCount; ++i) {
				std::string TableName;
				Input >> TableName;
				size_t ColumnCount;
				Input >> ColumnCount;
				Schema NewSchema;
				for(size_t j = 0; j < ColumnCount; ++j) {
					Column NewColumn;
					Input >> NewColumn.Name >> NewColumn.IsPrimaryKey 
						  >> NewColumn.IsUnique >> NewColumn.IsNotNull 
						  >> NewColumn.DefaultValue;
					NewSchema.push_back(NewColumn);
				}
				TableSchemas_[TableName] = NewSchema;
			}
			size_t TableCount;
			Input >> TableCount;
			for (size_t i = 0; i < TableCount; ++i) {
				std::string TableName;
				Input >> TableName;
				size_t RowCount;
				Input >> RowCount;
				Table NewTable;
				for(size_t j = 0; j < RowCount; ++j) {
					size_t ItemCount;
					Input >> ItemCount;
					Item NewRow;
					for(size_t k = 0; k < ItemCount; ++k) {
						std::string Key, Value;
						Input >> Key >> Value;
						NewRow[Key] = Value;
					}
					NewTable.push_back(NewRow);
				}
				Tables_[TableName] = NewTable;
			}
		}
		return true;
	});
}

std::future<Database::Table> Database::JoinTables(const std::string &LeftTable, const std::string &RightTable,
								  const std::function<bool(const Item&, const Item&)> &JoinCondition) const {
	return RunAsync([this, LeftTable, RightTable, JoinCondition]() -> Table {
		Table Result;
		{
			SpinlockGuard Guard(Lock_);
			auto LeftIt = Tables_.find(LeftTable);
			auto RightIt = Tables_.find(RightTable);
			if(LeftIt == Tables_.end() || RightIt == Tables_.end())
				throw std::runtime_error("One or both tables do not exist.");
			const auto &LeftData = LeftIt->second;
			const auto &RightData = RightIt->second;
			if(!LeftData.empty()) PREFETCH(LeftData.data());
			if(!RightData.empty()) PREFETCH(RightData.data());
			for(const auto &LeftRow : LeftData) {
				for(const auto &RightRow : RightData) {
					if(JoinCondition(LeftRow, RightRow)) {
						Item JoinedRow = RightRow;
						JoinedRow.insert(LeftRow.begin(), LeftRow.end());
						Result.push_back(JoinedRow);
					}
				}
			}
		}
		return Result;
	});
}

std::future<void> Database::AddForeignKey(const std::string &TableName, const ForeignKey &Key) {
	return RunAsync([this, TableName, Key]() {
		SpinlockGuard Guard(Lock_);
		ForeignKeys_[TableName].push_back(Key);
	});
}

bool Database::HasPermission(const User &user, Permissions Perms, const std::string &Table) const {
	SpinlockGuard Guard(Lock_);
	auto userIt = Acls_.find(user.Name);
	if (userIt == Acls_.end()) return false;
	// Table-specific permissions
	if (!Table.empty()) {
		auto tableIt = userIt->second.find(Table);
		if (tableIt != userIt->second.end()) {
			return (static_cast<int>(tableIt->second) & static_cast<int>(Perms)) == static_cast<int>(Perms);
		}
	}
	// Global permissions
	auto globalIt = userIt->second.find("");
	if (globalIt != userIt->second.end()) {
		return (static_cast<int>(globalIt->second) & static_cast<int>(Perms)) == static_cast<int>(Perms);
	}
	return false;
}

std::future<void> Database::GrantPermission(const std::string &Username, Permissions Perms, const std::string &Table) {
	return RunAsync([this, Username, Perms, Table]() {
		SpinlockGuard Guard(Lock_);
		Acls_[Username][Table] = static_cast<Permissions>(static_cast<int>(Acls_[Username][Table]) | static_cast<int>(Perms));
		Dirty_.store(true, std::memory_order_release);
	});
}

std::future<void> Database::RevokePermission(const std::string &Username, Permissions Perms, const std::string &Table) {
	return RunAsync([this, Username, Perms, Table]() {
		SpinlockGuard Guard(Lock_);
		Acls_[Username][Table] = static_cast<Permissions>(static_cast<int>(Acls_[Username][Table]) & ~static_cast<int>(Perms));
		Dirty_.store(true, std::memory_order_release);
	});
}

std::future<Permissions> Database::UserPermissions(const std::string &Username, const std::string &Table) const {
	return RunAsync([this, Username, Table]() -> Permissions {
		SpinlockGuard Guard(Lock_);
		auto UserIt = Acls_.find(Username);
		if (UserIt == Acls_.end()) return static_cast<Permissions>(0);
		if (!Table.empty()) {
			auto tableIt = UserIt->second.find(Table);
			if (tableIt != UserIt->second.end()) return tableIt->second;
		}
		auto globalIt = UserIt->second.find("");
		if (globalIt != UserIt->second.end()) return globalIt->second;
		return static_cast<Permissions>(0);
	});
}

bool Database::AuthenticateUser(const std::string& Username, const std::string& Password) {
	SpinlockGuard Guard(Lock_);
	for (auto& User : Users_) {
		if (User.Name == Username && User.VerifyPassword(Password)) {
			CurrentUser_.emplace(std::move(User));
			return true;
		}
	}
	return false;
}

void Database::Logout() {
	SpinlockGuard Guard(Lock_);
	CurrentUser_.reset();
}

bool Database::IsAuthenticated() const {
	SpinlockGuard Guard(Lock_);
	return CurrentUser_.has_value() && !CurrentUser_->Name.empty();
}

const std::optional<User>& Database::CurrentUser() const {
	return CurrentUser_;
}

// Helper: get or create index for a table/column
IndexManagement<std::string, size_t>& Database::GetOrCreateIndex(const std::string& table, const std::string& column) {
	if (Indexes_[table].find(column) == Indexes_[table].end()) {
		Indexes_[table].emplace(std::piecewise_construct,
			std::forward_as_tuple(column),
			std::forward_as_tuple());
	}
	return Indexes_[table][column];
}

std::future<void> Database::AddIndex(const std::string &TableName, const std::string &ColumnName) {
	return RunAsync([this, TableName, ColumnName]() {
		SpinlockGuard Guard(Lock_);
		auto& table = Tables_[TableName];
		auto& index = GetOrCreateIndex(TableName, ColumnName);
		for (size_t i = 0; i < table.size(); ++i) {
			const auto& row = table[i];
			auto it = row.find(ColumnName);
			if (it != row.end()) {
				// For now, always use BPlusTree
				std::get<BPlusTree<std::string, size_t>>(index.Index()).Insert(it->second, i);
			}
		}
	});
}

std::future<void> Database::RemoveIndex(const std::string &TableName, const std::string &ColumnName) {
	return RunAsync([this, TableName, ColumnName]() {
		SpinlockGuard Guard(Lock_);
		Indexes_[TableName].erase(ColumnName);
	});
}
}
