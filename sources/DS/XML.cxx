#include <DS/XML.hxx>

namespace AstralDB {
namespace DS {

std::shared_ptr<XmlNode> XmlNode::Element(std::string Tag) {
	auto N = std::make_shared<XmlNode>();
	N->NodeKind = Kind::Element;
	N->Tag = std::move(Tag);
	return N;
}

std::shared_ptr<XmlNode> XmlNode::TextNode(std::string Text) {
	auto N = std::make_shared<XmlNode>();
	N->NodeKind = Kind::Text;
	N->Text = std::move(Text);
	return N;
}

} // namespace DS
} // namespace AstralDB
