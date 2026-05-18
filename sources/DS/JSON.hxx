#pragma once

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace AstralDB {
namespace DS {

struct JSON;
using JSONObject = std::unordered_map<std::string, JSON>;
using JSONArray = std::vector<JSON>;

struct JSON {
    std::variant<std::monostate, std::string, double, bool, JSONArray, JSONObject> Value;

    JSON() : Value(std::monostate{}) {}
    JSON(std::nullptr_t) : Value(std::monostate{}) {}
    JSON(const std::string& Value) : Value(Value) {}
    JSON(std::string&& Value) : Value(std::move(Value)) {}
    JSON(const char* Value) : Value(std::string(Value)) {}
    JSON(int Value) : Value(static_cast<double>(Value)) {}
    JSON(double Value) : Value(Value) {}
    JSON(bool Value) : Value(Value) {}
    JSON(const JSONArray& Value) : Value(Value) {}
    JSON(JSONArray&& Value) : Value(std::move(Value)) {}
    JSON(const JSONObject& Value) : Value(Value) {}
    JSON(JSONObject&& Value) : Value(std::move(Value)) {}

    bool IsNull() const { return std::holds_alternative<std::monostate>(Value); }
    bool IsString() const { return std::holds_alternative<std::string>(Value); }
    bool IsNumber() const { return std::holds_alternative<double>(Value); }
    bool IsBool() const { return std::holds_alternative<bool>(Value); }
    bool IsArray() const { return std::holds_alternative<JSONArray>(Value); }
    bool IsObject() const { return std::holds_alternative<JSONObject>(Value); }

    const std::string& AsString() const { return std::get<std::string>(Value); }
    double AsNumber() const { return std::get<double>(Value); }
    bool AsBool() const { return std::get<bool>(Value); }
    const JSONArray& AsArray() const { return std::get<JSONArray>(Value); }
    const JSONObject& AsObject() const { return std::get<JSONObject>(Value); }

    JSON& operator[](const std::string& Key) {
        if (!IsObject()) Value = JSONObject{};
        return std::get<JSONObject>(Value)[Key];
    }

    const JSON& operator[](const std::string& Key) const {
        static const JSON NullValue;
        if (!IsObject()) return NullValue;
        const auto& Obj = std::get<JSONObject>(Value);
        auto It = Obj.find(Key);
        return It != Obj.end() ? It->second : NullValue;
    }

    JSON& operator[](size_t Index) {
        if (!IsArray()) Value = JSONArray{};
        auto& Arr = std::get<JSONArray>(Value);
        if (Index >= Arr.size()) Arr.resize(Index + 1);
        return Arr[Index];
    }

    const JSON& operator[](size_t Index) const {
        static const JSON NullValue;
        if (!IsArray()) return NullValue;
        const auto& Arr = std::get<JSONArray>(Value);
        return Index < Arr.size() ? Arr[Index] : NullValue;
    }
};

struct JSONDecodeError : std::runtime_error {
	using std::runtime_error::runtime_error;
};

namespace JsonCodecDetail {

inline void SkipWs(std::string_view S, size_t &I) {
	while(I < S.size() && std::isspace(static_cast<unsigned char>(S[I])))
		++I;
}

inline std::string UnescapeJSONString(std::string_view Raw) {
	std::string Out;
	Out.reserve(Raw.size());
	for(size_t J = 0; J < Raw.size(); ++J) {
		if(Raw[J] != '\\' || J + 1 >= Raw.size()) {
			Out.push_back(Raw[J]);
			continue;
		}
		const char Esc = Raw[J + 1];
		switch(Esc) {
		case '\"':
			Out.push_back('"');
			++J;
			break;
		case '\\':
			Out.push_back('\\');
			++J;
			break;
		case '/':
			Out.push_back('/');
			++J;
			break;
		case 'b':
			Out.push_back('\b');
			++J;
			break;
		case 'f':
			Out.push_back('\f');
			++J;
			break;
		case 'n':
			Out.push_back('\n');
			++J;
			break;
		case 'r':
			Out.push_back('\r');
			++J;
			break;
		case 't':
			Out.push_back('\t');
			++J;
			break;
		case 'u': {
			if(J + 5 >= Raw.size())
				throw JSONDecodeError("Bad \\u escape (truncated)");
			unsigned CP = 0;
			for(int K = 0; K < 4; ++K) {
				const char Hd = Raw[J + 2 + static_cast<size_t>(K)];
				unsigned V = 16;
				if(Hd >= '0' && Hd <= '9')
					V = static_cast<unsigned>(Hd - '0');
				else if(Hd >= 'a' && Hd <= 'f')
					V = 10 + static_cast<unsigned>(Hd - 'a');
				else if(Hd >= 'A' && Hd <= 'F')
					V = 10 + static_cast<unsigned>(Hd - 'A');
				else
					throw JSONDecodeError("Bad \\u hex digit");
				CP = CP * 16 + V;
			}
			if(CP < 128)
				Out.push_back(static_cast<char>(CP));
			else if(CP < 0x800) {
				Out.push_back(static_cast<char>(0xC0 | ((CP >> 6) & 0x1F)));
				Out.push_back(static_cast<char>(0x80 | (CP & 0x3F)));
			} else {
				Out.push_back(static_cast<char>(0xE0 | ((CP >> 12) & 0x0F)));
				Out.push_back(static_cast<char>(0x80 | ((CP >> 6) & 0x3F)));
				Out.push_back(static_cast<char>(0x80 | (CP & 0x3F)));
			}
			J += 5;
			break;
		}
		default:
			throw JSONDecodeError(std::string("Bad escape '\\") + Esc + "'");
		}
	}
	return Out;
}

inline JSON ParseAtom(std::string_view S, size_t &I);

inline JSON ParseStringLit(std::string_view S, size_t &I) {
	if(I >= S.size() || S[I] != '"')
		throw JSONDecodeError("Expected '\"' for JSON string");
	++I;
	const size_t Start = I;
	while(I < S.size() && S[I] != '\"') {
		if(S[I] == '\\')
			++I;
		++I;
	}
	if(I >= S.size())
		throw JSONDecodeError("Unterminated JSON string");
	const std::string Decoded =
	    UnescapeJSONString(std::string_view(S.data() + Start, I - Start));
	++I; // closing quote
	return JSON(Decoded);
}

inline JSON ParseArray(std::string_view S, size_t &I) {
	if(I >= S.size() || S[I] != '[')
		throw JSONDecodeError("Expected '['");
	++I;
	SkipWs(S, I);
	JSONArray A;
	if(I < S.size() && S[I] == ']') {
		++I;
		return JSON(std::move(A));
	}
	for(;;) {
		A.push_back(ParseAtom(S, I));
		SkipWs(S, I);
		if(I < S.size() && S[I] == ',') {
			++I;
			continue;
		}
		if(I >= S.size() || S[I] != ']')
			throw JSONDecodeError("Expected ',' or ']' in JSON array");
		++I;
		return JSON(std::move(A));
	}
}

inline JSON ParseObject(std::string_view S, size_t &I) {
	if(I >= S.size() || S[I] != '{')
		throw JSONDecodeError("Expected '{'");
	++I;
	SkipWs(S, I);
	JSONObject O;
	if(I < S.size() && S[I] == '}') {
		++I;
		return JSON(std::move(O));
	}
	for(;;) {
		SkipWs(S, I);
		if(I >= S.size() || S[I] != '\"')
			throw JSONDecodeError("Expected string key for JSON object");
		JSON Kj = ParseStringLit(S, I);
		const std::string Key = std::get<std::string>(Kj.Value);
		SkipWs(S, I);
		if(I >= S.size() || S[I] != ':')
			throw JSONDecodeError("Expected ':' after JSON object key");
		++I;
		SkipWs(S, I);
		O.emplace(std::move(Key), ParseAtom(S, I));
		SkipWs(S, I);
		if(I < S.size() && S[I] == ',') {
			++I;
			continue;
		}
		if(I >= S.size() || S[I] != '}')
			throw JSONDecodeError("Expected ',' or '}' in JSON object");
		++I;
		return JSON(std::move(O));
	}
}

inline JSON ParseNumber(std::string_view S, size_t &I) {
	const size_t Start = I;
	if(I < S.size() && S[I] == '-')
		++I;
	if(I >= S.size())
		throw JSONDecodeError("Truncated JSON number");
	if(S[I] == '0') {
		++I;
		if(I < S.size() && std::isdigit(static_cast<unsigned char>(S[I])))
			throw JSONDecodeError("Invalid leading zero in JSON number");
	} else if(std::isdigit(static_cast<unsigned char>(S[I]))) {
		while(I < S.size() && std::isdigit(static_cast<unsigned char>(S[I])))
			++I;
	} else
		throw JSONDecodeError("Invalid JSON number");
	if(I < S.size() && S[I] == '.') {
		++I;
		if(I >= S.size() || !std::isdigit(static_cast<unsigned char>(S[I])))
			throw JSONDecodeError("Invalid JSON fraction");
		while(I < S.size() && std::isdigit(static_cast<unsigned char>(S[I])))
			++I;
	}
	if(I < S.size() && (S[I] == 'e' || S[I] == 'E')) {
		++I;
		if(I < S.size() && (S[I] == '+' || S[I] == '-'))
			++I;
		if(I >= S.size() || !std::isdigit(static_cast<unsigned char>(S[I])))
			throw JSONDecodeError("Invalid JSON exponent");
		while(I < S.size() && std::isdigit(static_cast<unsigned char>(S[I])))
			++I;
	}
	const std::string_view NumSlice(S.data() + Start, I - Start);
	try {
		return JSON(std::stod(std::string(NumSlice)));
	} catch(...) {
		throw JSONDecodeError("Invalid JSON numeric value");
	}
}

inline JSON ParseAtom(std::string_view S, size_t &I) {
	SkipWs(S, I);
	if(I >= S.size())
		throw JSONDecodeError("Unexpected end of JSON input");
	char C = S[I];
	if(C == '\"')
		return ParseStringLit(S, I);
	if(C == '[')
		return ParseArray(S, I);
	if(C == '{')
		return ParseObject(S, I);
	if(C == '-' || std::isdigit(static_cast<unsigned char>(C)))
		return ParseNumber(S, I);
	if(S.substr(I).starts_with("true")) {
		I += 4;
		return JSON(true);
	}
	if(S.substr(I).starts_with("false")) {
		I += 5;
		return JSON(false);
	}
	if(S.substr(I).starts_with("null")) {
		I += 4;
		return JSON(nullptr);
	}
	throw JSONDecodeError("Invalid JSON token");
}

} // namespace JsonCodecDetail

inline JSON DecodeJSONStrict(std::string_view S) {
	size_t I = 0;
	JsonCodecDetail::SkipWs(S, I);
	JSON Root = JsonCodecDetail::ParseAtom(S, I);
	JsonCodecDetail::SkipWs(S, I);
	if(I != S.size())
		throw JSONDecodeError("Trailing characters after JSON value");
	return Root;
}

inline std::optional<JSON> TryDecodeJSON(std::string_view S) noexcept {
	try {
		return DecodeJSONStrict(S);
	} catch(...) {
		return std::nullopt;
	}
}

inline std::string JsonEscape(std::string_view S) {
	std::string O;
	O.reserve(S.size() + 8);
	for(unsigned char UC : S) {
		const char Ch = static_cast<char>(UC);
		switch(Ch) {
		case '\"':
			O += "\\\"";
			break;
		case '\\':
			O += "\\\\";
			break;
		case '\b':
			O += "\\b";
			break;
		case '\f':
			O += "\\f";
			break;
		case '\n':
			O += "\\n";
			break;
		case '\r':
			O += "\\r";
			break;
		case '\t':
			O += "\\t";
			break;
		default:
			if(UC < 0x20) {
				char Buf[8];
				std::snprintf(Buf, sizeof(Buf), "\\u%04x", UC);
				O += Buf;
			} else
				O.push_back(Ch);
			break;
		}
	}
	return O;
}

inline void SerializeJSONToStream(std::ostringstream &Os, const JSON &J);

inline void SerializeJSONToStream(std::ostringstream &Os, const JSONObject &O) {
	std::vector<const std::string *> Keys;
	Keys.reserve(O.size());
	for(const auto &[K, _] : O)
		Keys.push_back(&K);
	std::sort(Keys.begin(), Keys.end(),
	          [](const std::string *A, const std::string *B) { return *A < *B; });
	Os << '{';
	for(size_t I = 0; I < Keys.size(); ++I) {
		if(I)
			Os << ',';
		Os << '"' << JsonEscape(*Keys[I]) << "\":";
		SerializeJSONToStream(Os, O.at(*Keys[I]));
	}
	Os << '}';
}

inline void SerializeJSONToStream(std::ostringstream &Os, const JSONArray &A) {
	Os << '[';
	for(size_t I = 0; I < A.size(); ++I) {
		if(I)
			Os << ',';
		SerializeJSONToStream(Os, A[I]);
	}
	Os << ']';
}

inline void SerializeJSONToStream(std::ostringstream &Os, const JSON &J) {
	std::visit(
	    [&Os](auto &&Alt) {
		    using V = std::decay_t<decltype(Alt)>;
		    if constexpr(std::is_same_v<V, std::monostate>)
			    Os << "null";
		    else if constexpr(std::is_same_v<V, std::string>)
			    Os << '"' << JsonEscape(Alt) << '"';
		    else if constexpr(std::is_same_v<V, double>) {
			    std::ostringstream Tmp;
			    Tmp.setf(std::ios::fmtflags(0), std::ios::floatfield);
			    Tmp << Alt;
			    Os << Tmp.str();
		    } else if constexpr(std::is_same_v<V, bool>)
			    Os << (Alt ? "true" : "false");
		    else if constexpr(std::is_same_v<V, JSONArray>)
			    SerializeJSONToStream(Os, Alt);
		    else if constexpr(std::is_same_v<V, JSONObject>)
			    SerializeJSONToStream(Os, Alt);
		    else
			    Os << "null";
	    },
	    J.Value);
}

inline std::string SerializeJSON(const JSON &J) {
	std::ostringstream Os;
	SerializeJSONToStream(Os, J);
	return Os.str();
}

} // namespace DS
} // namespace AstralDB
