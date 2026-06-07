#include <Database/Types/XmlCell.hxx>

#include <DS/XML.hxx>

namespace AstralDB {
namespace XmlCell {
namespace {

std::vector<std::string> SplitPath(std::string_view Path) {
	std::vector<std::string> Parts;
	std::string Cur;
	for(size_t I = 0; I < Path.size(); ++I) {
		const char C = Path[I];
		if(C == '/' || C == '.') {
			if(!Cur.empty())
				Parts.push_back(Cur);
			Cur.clear();
			continue;
		}
		Cur.push_back(C);
	}
	if(!Cur.empty())
		Parts.push_back(Cur);
	return Parts;
}

const DS::XmlNode *FindChildElement(const DS::XmlNode &Node, std::string_view Tag) {
	for(const auto &Ch : Node.Children) {
		if(Ch && Ch->NodeKind == DS::XmlNode::Kind::Element && Ch->Tag == Tag)
			return Ch.get();
	}
	return nullptr;
}

std::string CollectText(const DS::XmlNode &Node) {
	std::string Out;
	for(const auto &Ch : Node.Children) {
		if(!Ch)
			continue;
		if(Ch->NodeKind == DS::XmlNode::Kind::Text)
			Out.append(Ch->Text);
		else if(Ch->NodeKind == DS::XmlNode::Kind::Element)
			Out.append(CollectText(*Ch));
	}
	return Out;
}

const DS::XmlNode *NavigateElements(const DS::XmlNode &Root, const std::vector<std::string> &Parts, size_t End) {
	const DS::XmlNode *Cur = &Root;
	for(size_t I = 0; I < End; ++I) {
		const auto &P = Parts[I];
		if(P.empty() || P[0] == '@')
			return nullptr;
		Cur = FindChildElement(*Cur, P);
		if(!Cur)
			return nullptr;
	}
	return Cur;
}

} // namespace

std::shared_ptr<DS::XmlNode> ParseCellXml(std::string_view Cell) {
	if(Cell.empty())
		return nullptr;
	try {
		return DS::ParseXML(Cell);
	} catch(...) {
		return nullptr;
	}
}

std::optional<std::string> ExtractPath(const DS::XmlNode &Root, std::string_view Path) {
	const auto Parts = SplitPath(Path);
	if(Parts.empty())
		return CollectText(Root);
	const auto &Last = Parts.back();
	if(!Last.empty() && Last[0] == '@') {
		const DS::XmlNode *Cur = Parts.size() == 1 ? &Root : NavigateElements(Root, Parts, Parts.size() - 1);
		if(!Cur)
			return std::nullopt;
		const std::string Key = Last.substr(1);
		auto It = Cur->Attributes.find(Key);
		if(It == Cur->Attributes.end())
			return std::nullopt;
		return It->second;
	}
	const DS::XmlNode *Cur = NavigateElements(Root, Parts, Parts.size());
	if(!Cur)
		return std::nullopt;
	return CollectText(*Cur);
}

std::string SerializeCell(const DS::XmlNode &Root) { return DS::SerializeXML(Root); }

bool IsValidXml(std::string_view Cell) { return ParseCellXml(Cell) != nullptr; }

} // namespace XmlCell
} // namespace AstralDB
