#pragma once

#include <cctype>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
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

	static std::shared_ptr<XmlNode> Element(std::string Tag) {
		auto N = std::make_shared<XmlNode>();
		N->NodeKind = Kind::Element;
		N->Tag = std::move(Tag);
		return N;
	}

	static std::shared_ptr<XmlNode> TextNode(std::string Text) {
		auto N = std::make_shared<XmlNode>();
		N->NodeKind = Kind::Text;
		N->Text = std::move(Text);
		return N;
	}
};

struct XMLDecodeError : std::runtime_error {
	using std::runtime_error::runtime_error;
};

namespace XmlCodecDetail {

inline void SkipWs(std::string_view S, size_t &I) {
	while(I < S.size() && std::isspace(static_cast<unsigned char>(S[I])))
		++I;
}

inline std::string ReadName(std::string_view S, size_t &I) {
	std::string Out;
	while(I < S.size()) {
		const unsigned char C = static_cast<unsigned char>(S[I]);
		if(!std::isalnum(C) && C != '_' && C != '-' && C != ':')
			break;
		Out.push_back(static_cast<char>(C));
		++I;
	}
	return Out;
}

inline std::string UnescapeXml(std::string_view Raw) {
	std::string Out;
	Out.reserve(Raw.size());
	for(size_t J = 0; J < Raw.size(); ++J) {
		if(Raw[J] != '&' || J + 1 >= Raw.size()) {
			Out.push_back(Raw[J]);
			continue;
		}
		const size_t Semi = Raw.find(';', J);
		if(Semi == std::string_view::npos) {
			Out.push_back(Raw[J]);
			continue;
		}
		const std::string_view Ent = Raw.substr(J, Semi - J + 1);
		if(Ent == "&lt;")
			Out.push_back('<');
		else if(Ent == "&gt;")
			Out.push_back('>');
		else if(Ent == "&amp;")
			Out.push_back('&');
		else if(Ent == "&quot;")
			Out.push_back('"');
		else if(Ent == "&apos;")
			Out.push_back('\'');
		else
			Out.append(Ent.begin(), Ent.end());
		J = Semi;
	}
	return Out;
}

inline std::shared_ptr<XmlNode> ParseElement(std::string_view S, size_t &I);

inline std::shared_ptr<XmlNode> ParseElement(std::string_view S, size_t &I) {
	SkipWs(S, I);
	if(I >= S.size() || S[I] != '<')
		throw XMLDecodeError("Expected '<' at element start");
	++I;
	if(I < S.size() && S[I] == '/')
		throw XMLDecodeError("Unexpected close tag");
	if(I < S.size() && S[I] == '?') {
		const size_t End = S.find("?>", I);
		if(End == std::string_view::npos)
			throw XMLDecodeError("Unclosed processing instruction");
		I = End + 2;
		return nullptr;
	}
	const std::string Tag = ReadName(S, I);
	if(Tag.empty())
		throw XMLDecodeError("Empty element tag");
	auto Node = XmlNode::Element(Tag);
	SkipWs(S, I);
	while(I < S.size() && S[I] != '>' && S[I] != '/') {
		const std::string Attr = ReadName(S, I);
		SkipWs(S, I);
		if(I >= S.size() || S[I] != '=')
			throw XMLDecodeError("Expected '=' after attribute name");
		++I;
		SkipWs(S, I);
		if(I >= S.size() || (S[I] != '"' && S[I] != '\''))
			throw XMLDecodeError("Expected quoted attribute value");
		const char Quote = S[I++];
		const size_t VStart = I;
		while(I < S.size() && S[I] != Quote)
			++I;
		if(I >= S.size())
			throw XMLDecodeError("Unclosed attribute value");
		Node->Attributes[Attr] = UnescapeXml(S.substr(VStart, I - VStart));
		++I;
		SkipWs(S, I);
	}
	bool SelfClose = false;
	if(I < S.size() && S[I] == '/') {
		SelfClose = true;
		++I;
	}
	if(I >= S.size() || S[I] != '>')
		throw XMLDecodeError("Expected '>' closing start tag");
	++I;
	if(SelfClose)
		return Node;
	for(;;) {
		SkipWs(S, I);
		if(I >= S.size())
			throw XMLDecodeError("Unexpected end of document in element");
		if(S[I] == '<') {
			if(I + 1 < S.size() && S[I + 1] == '/') {
				I += 2;
				const std::string Close = ReadName(S, I);
				SkipWs(S, I);
				if(I >= S.size() || S[I] != '>')
					throw XMLDecodeError("Expected '>' on close tag");
				++I;
				if(Close != Tag)
					throw XMLDecodeError("Mismatched close tag");
				return Node;
			}
			if(auto Child = ParseElement(S, I))
				Node->Children.push_back(std::move(Child));
			continue;
		}
		const size_t TStart = I;
		while(I < S.size() && S[I] != '<')
			++I;
		const std::string Text = UnescapeXml(S.substr(TStart, I - TStart));
		if(!Text.empty())
			Node->Children.push_back(XmlNode::TextNode(Text));
	}
}

} // namespace XmlCodecDetail

inline std::shared_ptr<XmlNode> ParseXML(std::string_view S) {
	size_t I = 0;
	XmlCodecDetail::SkipWs(S, I);
	std::shared_ptr<XmlNode> Root;
	while(I < S.size()) {
		if(S[I] != '<')
			throw XMLDecodeError("Leading non-markup content outside root element");
		auto Node = XmlCodecDetail::ParseElement(S, I);
		if(Node) {
			if(Root)
				throw XMLDecodeError("Multiple root elements");
			Root = std::move(Node);
		}
		XmlCodecDetail::SkipWs(S, I);
	}
	if(!Root)
		throw XMLDecodeError("Empty XML document");
	return Root;
}

inline void SerializeNode(const XmlNode &N, std::string &Out) {
	if(N.NodeKind == XmlNode::Kind::Text) {
		for(char C : N.Text) {
			if(C == '<')
				Out.append("&lt;");
			else if(C == '>')
				Out.append("&gt;");
			else if(C == '&')
				Out.append("&amp;");
			else
				Out.push_back(C);
		}
		return;
	}
	Out.push_back('<');
	Out.append(N.Tag);
	for(const auto &[K, V] : N.Attributes) {
		Out.push_back(' ');
		Out.append(K);
		Out.append("=\"");
		for(char C : V) {
			if(C == '"')
				Out.append("&quot;");
			else if(C == '&')
				Out.append("&amp;");
			else if(C == '<')
				Out.append("&lt;");
			else
				Out.push_back(C);
		}
		Out.push_back('"');
	}
	if(N.Children.empty()) {
		Out.append("/>");
		return;
	}
	Out.push_back('>');
	for(const auto &Ch : N.Children) {
		if(Ch)
			SerializeNode(*Ch, Out);
	}
	Out.push_back('<');
	Out.push_back('/');
	Out.append(N.Tag);
	Out.push_back('>');
}

inline std::string SerializeXML(const XmlNode &Root) {
	std::string Out;
	Out.reserve(128);
	SerializeNode(Root, Out);
	return Out;
}

} // namespace DS
} // namespace AstralDB
