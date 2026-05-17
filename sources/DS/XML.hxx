#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace AstralDB {
namespace DS {

/** Minimal DOM node for future SQL/XML (\c XMLQUERY / \c XMLTABLE) work. */
struct XmlNode {
	enum class Kind { Element, Text };

	Kind NodeKind = Kind::Element;
	std::string Tag;
	std::unordered_map<std::string, std::string> Attributes;
	std::vector<std::shared_ptr<XmlNode>> Children;
	std::string Text;

	static std::shared_ptr<XmlNode> Element(std::string Tag);
	static std::shared_ptr<XmlNode> TextNode(std::string Text);
};

} // namespace DS
} // namespace AstralDB
