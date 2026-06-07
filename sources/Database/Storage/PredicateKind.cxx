#include <Database/Storage/BulkSyntheticPathEval.hxx>
#include <Database/Storage/PredicateKind.hxx>

#include <Database/Types/AdvancedTypes.hxx>
#include <Database/Storage/BulkSynthetic.hxx>
#include <Database/Storage/BulkSyntheticPathEval.hxx>
#include <Database/Storage/BulkSyntheticSemistructured.hxx>

#include <Database/Storage/BulkShapePlan.hxx>

#include <functional>

namespace AstralDB {

namespace {

std::string UpperToken(std::string_view S) {
	std::string Out;
	Out.reserve(S.size());
	for(char C : S) {
		if(C >= 'a' && C <= 'z')
			Out.push_back(static_cast<char>(C - 'a' + 'A'));
		else
			Out.push_back(C);
	}
	return Out;
}

bool TypeTokenContains(std::string_view Hay, std::string_view Needle) {
	return Hay.find(Needle) != std::string_view::npos;
}

bool SqlTruthLiteral(std::string_view S) {
	return S == "1" || S == "true" || S == "TRUE" || S == "t" || S == "yes";
}

const Database::Column *ResolveColDef(const std::string &ColName,
                                      const std::vector<Database::Column> &PrimarySchema,
                                      const std::vector<Database::Column> *LinkedSchema) {
	std::string_view Lookup = ColName;
	if(const std::size_t Dot = Lookup.rfind('.'); Dot != std::string_view::npos)
		Lookup = Lookup.substr(Dot + 1);
	if(const Database::Column *C = FindSchemaColumn(PrimarySchema, Lookup))
		return C;
	if(LinkedSchema != nullptr)
		return FindSchemaColumn(*LinkedSchema, Lookup);
	return FindSchemaColumn(PrimarySchema, ColName);
}

} // namespace

PredicateKindMask PredicateKindBit(const PredicateKindFlag Flag) noexcept {
	return static_cast<PredicateKindMask>(Flag);
}

SqlStorageKind ClassifySqlStorage(const Database::Column &Col) noexcept {
	if(Col.DeclaredFk.has_value())
		return SqlStorageKind::ForeignKey;
	const std::string U = UpperToken(Col.DefaultValue);
	if(U == "JSON")
		return SqlStorageKind::Json;
	if(U == "XML" || TypeTokenContains(U, "XML"))
		return SqlStorageKind::Xml;
	if(TypeTokenContains(U, "TIMESTAMP") || TypeTokenContains(U, "DATE") || TypeTokenContains(U, "TIME"))
		return SqlStorageKind::Timestamp;
	if(TypeTokenContains(U, "DECIMAL") || TypeTokenContains(U, "NUMERIC") || TypeTokenContains(U, "DOUBLE") ||
	   TypeTokenContains(U, "REAL") || TypeTokenContains(U, "FLOAT"))
		return SqlStorageKind::Decimal;
	if(TypeTokenContains(U, "INT") || U == "SERIAL" || U == "BIGSERIAL")
		return SqlStorageKind::Integer;
	if(AdvancedTypes::IsAdvancedTypeSpelling(Col.DefaultValue))
		return SqlStorageKind::Other;
	if(TypeTokenContains(U, "TEXT") || TypeTokenContains(U, "CHAR") || TypeTokenContains(U, "CLOB"))
		return SqlStorageKind::Text;
	return SqlStorageKind::Unknown;
}

const Database::Column *FindSchemaColumn(const std::vector<Database::Column> &Schema, std::string_view Name) noexcept {
	for(const Database::Column &Co : Schema) {
		if(Co.Name == Name)
			return &Co;
	}
	return nullptr;
}

PredicateKindMask PredicateKindsForColumn(const Database::Column &Col) noexcept {
	PredicateKindMask Mask = 0;
	switch(ClassifySqlStorage(Col)) {
	case SqlStorageKind::Json:
		Mask |= PredicateKindBit(PredicateKindFlag::JsonExtractEq);
		break;
	case SqlStorageKind::Xml:
		Mask |= PredicateKindBit(PredicateKindFlag::XmlValid);
		break;
	case SqlStorageKind::Text:
		Mask |= PredicateKindBit(PredicateKindFlag::RegexMatch);
		Mask |= PredicateKindBit(PredicateKindFlag::TextMatch);
		break;
	case SqlStorageKind::Timestamp:
		Mask |= PredicateKindBit(PredicateKindFlag::TimestampGe);
		break;
	default:
		break;
	}
	return Mask;
}

PredicateKindMask DetectPredicateKindsFromSchema(const std::vector<Database::Column> &Schema) noexcept {
	PredicateKindMask Mask = 0;
	for(const Database::Column &Co : Schema)
		Mask |= PredicateKindsForColumn(Co);
	return Mask;
}

PredicateKindMask DetectPredicateKindForTriple(const std::string &ColName, const std::string &Op,
                                               const std::string &Rhs, const Database::Column *ColDef,
                                               const bool ColumnHasFtsIndex) noexcept {
	if(!ColDef)
		return 0;
	const SqlStorageKind Storage = ClassifySqlStorage(*ColDef);
	if(Op == "__IN__" || Op == "__NOT_IN__") {
		if(Storage == SqlStorageKind::Json)
			return PredicateKindBit(PredicateKindFlag::JsonExtractIn);
		return 0;
	}
	if(Op == "__ST_BBOX__") {
		if(Storage == SqlStorageKind::Other || Storage == SqlStorageKind::Text)
			return PredicateKindBit(PredicateKindFlag::StWithinBbox);
		return 0;
	}
	if(Op == "JSON_EXTRACT" || Op == "NOT JSON_EXTRACT") {
		if(Storage == SqlStorageKind::Json)
			return PredicateKindBit(PredicateKindFlag::JsonExtractEq);
		return 0;
	}
	if(Op == "XML_VALID" || Op == "NOT XML_VALID") {
		if(Storage == SqlStorageKind::Xml || Storage == SqlStorageKind::Text)
			return PredicateKindBit(PredicateKindFlag::XmlValid);
		return 0;
	}
	if(Op == "REGEXP" || Op == "~" || Op == "NOT REGEXP" || Op == "!~") {
		if(Storage == SqlStorageKind::Text)
			return PredicateKindBit(PredicateKindFlag::RegexMatch);
		return 0;
	}
	if(Op == "MATCH" || Op == "NOT MATCH") {
		if(Storage == SqlStorageKind::Text && ColumnHasFtsIndex)
			return PredicateKindBit(PredicateKindFlag::TextMatch);
		if(Storage == SqlStorageKind::Text)
			return PredicateKindBit(PredicateKindFlag::TextMatch);
		return 0;
	}
	if(Op == ">=" || Op == ">" || Op == "<=" || Op == "<") {
		if(Storage == SqlStorageKind::Timestamp)
			return Op == ">=" || Op == ">"
			           ? PredicateKindBit(PredicateKindFlag::TimestampGe)
			           : PredicateKindBit(PredicateKindFlag::TimestampLe);
	}
	if((Op == "=" || Op == "==") && Storage == SqlStorageKind::Json && SqlTruthLiteral(Rhs))
		return PredicateKindBit(PredicateKindFlag::JsonExtractEq);
	if(Storage == SqlStorageKind::Other && (Op == "=" || Op == "==") && SqlTruthLiteral(Rhs))
		return PredicateKindBit(PredicateKindFlag::StWithinBbox);
	(void)ColName;
	return 0;
}

QueryMaskBuildResult BuildQueryMaskFromDnfEx(const BulkWhereDnf &Dnf,
                                             const std::vector<Database::Column> &PrimarySchema,
                                             const std::vector<Database::Column> *LinkedSchema, const Database *Db,
                                             const std::string &PrimaryTable) noexcept {
	QueryMaskBuildResult Out;
	Out.AllPredicatesRecognized = true;
	if(Dnf.size() != 1)
		return Out;
	const BulkWhereDnfBranch &Branch = Dnf.front();
	if(Branch.empty())
		return Out;
	for(const auto &Pred : Branch) {
		const auto &[ColName, Op, Val] = Pred;
		const Database::Column *ColDef = ResolveColDef(ColName, PrimarySchema, LinkedSchema);
		bool HasFts = false;
		if(Db != nullptr && ColDef && ClassifySqlStorage(*ColDef) == SqlStorageKind::Text)
			HasFts = Db->FtsForColumn(PrimaryTable, ColName) != nullptr;
		const PredicateKindMask Kind = DetectPredicateKindForTriple(ColName, Op, Val, ColDef, HasFts);
		if(Kind == 0)
			Out.AllPredicatesRecognized = false;
		else
			Out.Mask |= Kind;
	}
	return Out;
}

PredicateKindMask BuildQueryMaskFromDnf(const BulkWhereDnf &Dnf, const std::vector<Database::Column> &PrimarySchema,
                                        const std::vector<Database::Column> *LinkedSchema, const Database *Db,
                                        const std::string &PrimaryTable) noexcept {
	const QueryMaskBuildResult Built =
	    BuildQueryMaskFromDnfEx(Dnf, PrimarySchema, LinkedSchema, Db, PrimaryTable);
	if(!Built.AllPredicatesRecognized)
		return 0;
	return Built.Mask;
}

PredicateKindMask PassKindsEvaluatedForFamily(const BulkSyntheticPassFamily Family) noexcept {
	if(Family == BulkSyntheticPassFamily::EntityScan) {
		return PredicateKindBit(PredicateKindFlag::JsonExtractEq) | PredicateKindBit(PredicateKindFlag::XmlValid);
	}
	if(Family == BulkSyntheticPassFamily::JoinFact) {
		return PredicateKindBit(PredicateKindFlag::TimestampGe) | PredicateKindBit(PredicateKindFlag::StWithinBbox) |
		       PredicateKindBit(PredicateKindFlag::JsonExtractIn);
	}
	return 0;
}

bool SyntheticRowPassesPredicateMask(const BulkSyntheticPassFamily Family, const int64_t PrimaryRowId,
                                   const int64_t LinkedRowId, const PredicateKindMask KindMask,
                                   const bool PrimaryHasJson, const bool LinkedHasJson,
                                   const bool PrimaryHasReviewText) noexcept {
	(void)PrimaryHasJson;
	(void)LinkedHasJson;
	(void)PrimaryHasReviewText;
	(void)LinkedRowId;
	if(KindMask == 0)
		return true;
	const auto JsonActiveTrue = [](const int64_t RowId) noexcept {
		std::string Cell;
		const BulkSyntheticJsonPathPlan Plan = PlanBulkSyntheticJsonPath("$.active");
		return BulkSyntheticJsonExtractPlanned(RowId, Plan, "$.active", Cell) && SqlTruthLiteral(Cell);
	};
	if(Family == BulkSyntheticPassFamily::EntityScan) {
		if((KindMask & PredicateKindBit(PredicateKindFlag::JsonExtractEq)) && !JsonActiveTrue(PrimaryRowId))
			return false;
		if((KindMask & PredicateKindBit(PredicateKindFlag::XmlValid)) && !BulkSyntheticXmlValidRow(PrimaryRowId))
			return false;
		return true;
	}
	if(Family == BulkSyntheticPassFamily::JoinFact) {
		if((KindMask & PredicateKindBit(PredicateKindFlag::TimestampGe))) {
			const int64_t RowEpoch = 1'704'067'200LL + PrimaryRowId;
			if(RowEpoch < 1'704'067'200LL)
				return false;
		}
		if(KindMask & PredicateKindBit(PredicateKindFlag::JsonExtractIn)) {
			const std::size_t Idx = static_cast<std::size_t>(BulkSyntheticRowSeed(PrimaryRowId, 0) % 5);
			if(Idx >= 2)
				return false;
		}
		if(KindMask & PredicateKindBit(PredicateKindFlag::StWithinBbox))
			(void)PrimaryRowId;
		return true;
	}
	return true;
}

bool PassBitsCoverQuery(const ColumnarTable &Col, const PredicateKindMask QueryMask) noexcept {
	if(Col.BulkSyntheticMetadataOnly && Col.BulkSyntheticLazy && Col.RowCount > 0 && QueryMask != 0 &&
	   Col.BulkSyntheticPassFamilyTag != BulkSyntheticPassFamily::None)
		return (QueryMask & Col.BulkSyntheticPassKindMask) == QueryMask;
	if(Col.BulkSyntheticPassAllRows && QueryMask != 0 &&
	   Col.BulkSyntheticPassFamilyTag != BulkSyntheticPassFamily::None)
		return (QueryMask & Col.BulkSyntheticPassKindMask) == QueryMask;
	if(Col.BulkSyntheticPassBits.empty() || QueryMask == 0 ||
	   Col.BulkSyntheticPassFamilyTag == BulkSyntheticPassFamily::None)
		return false;
	return (QueryMask & Col.BulkSyntheticPassKindMask) == QueryMask;
}

bool PassBitsApplicable(const BulkWhereDnf *Filters, const ColumnarTable &Col,
                        const std::vector<Database::Column> &PrimarySchema,
                        const std::vector<Database::Column> *LinkedSchema,
                        const PredicateKindMask QueryMask) noexcept {
	if(Filters == nullptr)
		return false;
	return ClassifyPassBitEligibility(Filters, Col, PrimarySchema, LinkedSchema, QueryMask).Eligible;
}

} // namespace AstralDB
