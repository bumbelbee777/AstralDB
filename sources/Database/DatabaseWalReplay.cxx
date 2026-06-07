#include <Database/Database.hxx>
#include <Database/PtBridge/PtBridge.hxx>
#include <Database/Graph/GraphStorage.hxx>
#include <Database/Embedding/EmbeddingStorage.hxx>
#include <Database/Catalog/Triggers.hxx>
#include <SQL/Procedures/BytecodeProcedures.hxx>
#include <SQL/Bytecode/Bytecode.hxx>
#include <SQL/Procedures/BytecodeTriggers.hxx>
#include <IO/Error.hxx>
#include <algorithm>

namespace AstralDB {

namespace {
[[noreturn]] void FailStorage(std::string Message) {
	throw std::runtime_error(Err::Prefixed("storage", std::move(Message)));
}
} // namespace

void Database::ReplayWalDefineView(const std::string &ViewName, std::string SqlBody) {
	std::scoped_lock Guard(DbMutex_);
	if(ViewName.empty())
		FailStorage("WAL replay CREATE VIEW: empty name.");
	if(TableSchemas_.find(ViewName) != TableSchemas_.end())
		FailStorage("WAL replay CREATE VIEW: name \"" + ViewName + "\" collides with existing table.");
	ViewDefinitionSql_[ViewName] = std::move(SqlBody);
}

void Database::ReplayWalDropView(const std::string &ViewName) {
	std::scoped_lock Guard(DbMutex_);
	ViewDefinitionSql_.erase(ViewName);
}

void Database::ReplayWalDefineProcedure(const std::string &ProcedureName, std::string SqlBody,
                                        std::string ControlFlowBlob) {
	{
		std::scoped_lock Guard(DbMutex_);
		if(ProcedureName.empty())
			FailStorage("WAL replay CREATE PROCEDURE: empty name.");
		ProcedureDefinitionSql_[ProcedureName] = SqlBody;
	}
	PtBridge::ReplayWalCacheProcedure(*this, ProcedureName, std::move(SqlBody), std::move(ControlFlowBlob));
}

void Database::ReplayWalDefineTrigger(const std::string &TriggerName, std::string SpecJson) {
	std::vector<StoredTriggerEntry> Entries;
	if(!DeserializeTriggerRegistry(SpecJson, Entries) || Entries.empty())
		FailStorage("WAL replay CREATE TRIGGER: corrupt spec for \"" + TriggerName + "\".");
	StoredTriggerEntry Spec = std::move(Entries.front());
	Spec.Name = TriggerName;
	{
		std::scoped_lock Guard(DbMutex_);
		TriggerDefinitions_[TriggerName] = Spec;
	}
	TriggerCatalog Catalog;
	const auto CatPath = DefaultTriggerCatalogPath(DbPath_);
	Catalog.CatalogPath = CatPath;
	Catalog = LoadTriggerCatalog(CatPath);
	SQL::UnregisterTrigger(Catalog, TriggerName);
	SQL::CacheTriggerFromSql(Catalog, DbPath_, Spec, Logger_, SQL::OptimizationLevel::Advanced, this, true, true);
	SaveTriggerCatalog(Catalog);
}

void Database::ReplayWalDropTrigger(const std::string &TriggerName) {
	{
		std::scoped_lock Guard(DbMutex_);
		TriggerDefinitions_.erase(TriggerName);
	}
	TriggerCatalog Catalog;
	const auto CatPath = DefaultTriggerCatalogPath(DbPath_);
	Catalog = LoadTriggerCatalog(CatPath);
	if(Catalog.CatalogPath.empty())
		Catalog.CatalogPath = CatPath;
	SQL::UnregisterTrigger(Catalog, TriggerName);
	SaveTriggerCatalog(Catalog);
}

void Database::ReplayWalSetTriggerEnabled(const std::string &TriggerName, bool Enabled) {
	{
		std::scoped_lock Guard(DbMutex_);
		auto It = TriggerDefinitions_.find(TriggerName);
		if(It != TriggerDefinitions_.end())
			It->second.Enabled = Enabled;
	}
	TriggerCatalog Catalog;
	const auto CatPath = DefaultTriggerCatalogPath(DbPath_);
	Catalog = LoadTriggerCatalog(CatPath);
	if(Catalog.CatalogPath.empty())
		Catalog.CatalogPath = CatPath;
	for(StoredTriggerEntry &E : Catalog.Triggers) {
		if(E.Name == TriggerName) {
			E.Enabled = Enabled;
			break;
		}
	}
	SaveTriggerCatalog(Catalog);
}

void Database::ReplayWalDropProcedure(const std::string &ProcedureName) {
	{
		std::scoped_lock Guard(DbMutex_);
		ProcedureDefinitionSql_.erase(ProcedureName);
	}
	SQL::ProcedureCatalog Catalog;
	const auto CatPath = SQL::DefaultProcedureCatalogPath(DbPath_);
	Catalog = SQL::LoadProcedureCatalog(CatPath);
	if(Catalog.CatalogPath.empty())
		Catalog.CatalogPath = CatPath;
	SQL::UnregisterProcedure(Catalog, ProcedureName);
	SQL::SaveProcedureCatalog(Catalog);
}

void Database::ReplayWalAddUser(std::string Name, std::string EncryptedBlob, std::array<uint8_t, 32> KeyMaterial) {
	std::scoped_lock Guard(DbMutex_);
	EncryptedString Es(std::move(EncryptedBlob), KeyMaterial);
	User Nu(std::move(Name), std::move(Es));
	for(auto &Existing : Users_) {
		if(Existing.Name == Nu.Name) {
			Existing = std::move(Nu);
			return;
		}
	}
	Users_.push_back(std::move(Nu));
}

void Database::ReplayWalRemoveUser(const std::string &Name) {
	std::scoped_lock Guard(DbMutex_);
	const auto It = std::find_if(Users_.begin(), Users_.end(), [&](const User &U) { return U.Name == Name; });
	if(It == Users_.end())
		return;
	if(CurrentUser_.has_value() && CurrentUser_->Name == Name)
		CurrentUser_.reset();
	Users_.erase(It);
}

void Database::ReplayWalGrantAcl(const std::string &UserName, const std::string &Table, int Bits) {
	std::scoped_lock Guard(DbMutex_);
	Acls_[UserName][Table] =
	    static_cast<Permissions>(static_cast<int>(Acls_[UserName][Table]) | Bits);
}

void Database::ReplayWalRevokeAcl(const std::string &UserName, const std::string &Table, int Bits) {
	std::scoped_lock Guard(DbMutex_);
	Acls_[UserName][Table] =
	    static_cast<Permissions>(static_cast<int>(Acls_[UserName][Table]) & ~Bits);
}

void Database::ReplayWalCreateRole(const std::string &RoleName) {
	std::scoped_lock Guard(DbMutex_);
	Roles_.insert(RoleName);
}

void Database::ReplayWalDropRole(const std::string &RoleName) {
	std::scoped_lock Guard(DbMutex_);
	Roles_.erase(RoleName);
	RoleAcls_.erase(RoleName);
	for(auto &[Un, Roles] : UserRoles_) {
		Roles.erase(std::remove(Roles.begin(), Roles.end(), RoleName), Roles.end());
	}
}

void Database::ReplayWalGrantRoleMembership(const std::string &RoleName, const std::string &UserName) {
	std::scoped_lock Guard(DbMutex_);
	Roles_.insert(RoleName);
	auto &Vec = UserRoles_[UserName];
	if(std::find(Vec.begin(), Vec.end(), RoleName) == Vec.end())
		Vec.push_back(RoleName);
}

void Database::ReplayWalRevokeRoleMembership(const std::string &RoleName, const std::string &UserName) {
	std::scoped_lock Guard(DbMutex_);
	auto &Vec = UserRoles_[UserName];
	Vec.erase(std::remove(Vec.begin(), Vec.end(), RoleName), Vec.end());
}

void Database::ReplayWalGrantRoleAcl(const std::string &RoleName, const std::string &Table, int Bits) {
	std::scoped_lock Guard(DbMutex_);
	Roles_.insert(RoleName);
	RoleAcls_[RoleName][Table] =
	    static_cast<Permissions>(static_cast<int>(RoleAcls_[RoleName][Table]) | Bits);
}

void Database::ReplayWalRevokeRoleAcl(const std::string &RoleName, const std::string &Table, int Bits) {
	std::scoped_lock Guard(DbMutex_);
	RoleAcls_[RoleName][Table] =
	    static_cast<Permissions>(static_cast<int>(RoleAcls_[RoleName][Table]) & ~Bits);
}

void Database::ReplayWalFineGrant(const std::string &UserName, RowColPermission Rule) {
	std::scoped_lock Guard(DbMutex_);
	for(User &U : Users_) {
		if(U.Name != UserName)
			continue;
		U.FineGrainedPermissions.push_back(std::move(Rule));
		return;
	}
}

void Database::ReplayWalFineRevoke(const std::string &UserName, RowColPermission Rule) {
	std::scoped_lock Guard(DbMutex_);
	for(User &U : Users_) {
		if(U.Name != UserName)
			continue;
		auto &Vec = U.FineGrainedPermissions;
		Vec.erase(std::remove_if(Vec.begin(), Vec.end(),
		                         [&](const RowColPermission &R) {
			                         return R.Table == Rule.Table && R.RowId == Rule.RowId &&
			                                R.Column == Rule.Column &&
			                                static_cast<int>(R.Perms) == static_cast<int>(Rule.Perms);
		                         }),
		          Vec.end());
		return;
	}
}

void Database::ReplayWalAddForeignKey(const std::string &TableName, ForeignKey Key) {
	std::scoped_lock Guard(DbMutex_);
	auto &Vec = ForeignKeys_[TableName];
	for(const ForeignKey &E : Vec) {
		if(E.ColumnName == Key.ColumnName && E.ReferencedTable == Key.ReferencedTable &&
		   E.ReferencedColumn == Key.ReferencedColumn && E.OnDelete == Key.OnDelete)
			return;
	}
	Vec.push_back(std::move(Key));
}

void Database::ReplayWalSetTableStorage(const std::string &TableName, StorageLayout Policy) {
	std::scoped_lock Guard(DbMutex_);
	auto It = Tables_.find(TableName);
	if(It == Tables_.end())
		return;
	It->second.SetDeclaredPolicy(Policy);
}

void Database::ReplayWalCreateSequence(const std::string &Name, int64_t Start, int64_t Increment) {
	std::scoped_lock Guard(DbMutex_);
	SequenceState St;
	St.Start = Start;
	St.Increment = Increment;
	St.Current = Start;
	Sequences_[Name] = St;
}

void Database::ReplayWalDropSequence(const std::string &Name) {
	std::scoped_lock Guard(DbMutex_);
	Sequences_.erase(Name);
}

void Database::ReplayWalCreateType(const std::string &TypeName, ObjectTypeSchema Fields) {
	std::scoped_lock Guard(DbMutex_);
	ObjectTypes_[TypeName] = std::move(Fields);
}

void Database::ReplayWalDropType(const std::string &TypeName) {
	std::scoped_lock Guard(DbMutex_);
	ObjectTypes_.erase(TypeName);
	for(auto It = TypedTableBindings_.begin(); It != TypedTableBindings_.end();) {
		if(It->second == TypeName)
			It = TypedTableBindings_.erase(It);
		else
			++It;
	}
}

void Database::ReplayWalBindTypedTable(const std::string &TableName, const std::string &TypeName) {
	std::scoped_lock Guard(DbMutex_);
	TypedTableBindings_[TableName] = TypeName;
}

void Database::ReplayWalEmbeddingRegister(EmbeddingCatalogEntry Entry) {
	std::scoped_lock Guard(DbMutex_);
	ReplayWalEmbeddingRegisterAssumeLocked(*this, std::move(Entry));
}

void Database::ReplayWalEmbeddingDrop(const std::string &Name) {
	std::scoped_lock Guard(DbMutex_);
	ReplayWalEmbeddingDropAssumeLocked(*this, Name);
}

void Database::ReplayWalGraphRegister(GraphSpec Spec) {
	std::scoped_lock Guard(DbMutex_);
	ReplayWalGraphRegisterAssumeLocked(*this, std::move(Spec));
}

void Database::ReplayWalGraphDrop(const std::string &Name) {
	std::scoped_lock Guard(DbMutex_);
	ReplayWalGraphDropAssumeLocked(*this, Name);
}

void Database::ReplayWalGraphProjection(const GraphProjectionRequest &Req) {
	std::scoped_lock Guard(DbMutex_);
	ReplayWalGraphProjectionAssumeLocked(*this, Req);
}
} // namespace AstralDB
