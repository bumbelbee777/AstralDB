#include "User.hxx"
#include <DS/Blake3.hxx>
#include <DS/XChaCha20.hxx>
#include <cstdint>
#include <random>
// #include <chrono> // Removed: not used

namespace AstralDB {
// Static members
std::vector<uint8_t> User::DeviceSalt_;
std::vector<uint8_t> User::InstanceSalt_;
std::vector<uint8_t> User::SessionSalt_;
std::array<uint8_t, 32> User::XChaChaKey_ = {0};
std::array<uint8_t, 24> User::XChaChaNonce_ = {0};

void User::InitSalts() {
	if(DeviceSalt_.empty()) DeviceSalt_.resize(32, 0xA1);
	if(InstanceSalt_.empty()) InstanceSalt_.resize(32, 0xB2);
	if(SessionSalt_.empty()) RegenerateSessionSalt();
	// Key/nonce can be derived from salts for demo
	for(size_t i=0; i<32; ++i) XChaChaKey_[i] = (DeviceSalt_[i%DeviceSalt_.size()] ^ InstanceSalt_[i%InstanceSalt_.size()]);
	for(size_t i=0; i<24; ++i) XChaChaNonce_[i] = (SessionSalt_[i%SessionSalt_.size()] ^ 0xC3);
}

void User::SetDeviceSalt(const std::vector<uint8_t>& Salt) {
	DeviceSalt_ = Salt;
	InitSalts();
}
void User::SetInstanceSalt(const std::vector<uint8_t>& Salt) {
	InstanceSalt_ = Salt;
	InitSalts();
}

void User::SetSessionSalt(const std::vector<uint8_t>& Salt) {
	SessionSalt_ = Salt;
	InitSalts();
}

void User::RegenerateSessionSalt() {
	SessionSalt_.resize(32);
	std::random_device rd;
	std::mt19937 gen(rd());
	std::uniform_int_distribution<int> dis(0, 255);
	for(auto& b : SessionSalt_) b = dis(gen);
	InitSalts();
}

std::vector<uint8_t> User::GetCombinedSalt() {
	std::vector<uint8_t> combined;
	combined.insert(combined.end(), DeviceSalt_.begin(), DeviceSalt_.end());
	combined.insert(combined.end(), InstanceSalt_.begin(), InstanceSalt_.end());
	combined.insert(combined.end(), SessionSalt_.begin(), SessionSalt_.end());
	return combined;
}

std::vector<uint8_t> User::SaltAndHashPassword(const std::string& Password) const {
	InitSalts();
	std::vector<uint8_t> Input(Password.begin(), Password.end());
	auto Hash = Blake3::Hash(Input);
	auto Salt = GetCombinedSalt();
	std::vector<uint8_t> Salted(Hash.begin(), Hash.end());
	Salted.insert(Salted.end(), Salt.begin(), Salt.end());
	auto arr = Blake3::Hash(Salted);
	return std::vector<uint8_t>(arr.begin(), arr.end());
}

User::User(const std::string& Password)
	: Name("Admin0"), Password(EncryptedString()) {
	auto hashed = SaltAndHashPassword(Password);
	std::vector<uint8_t> encrypted;
	XChaCha20 cipher(XChaChaKey_, XChaChaNonce_);
	cipher.Encrypt(hashed, encrypted);
	this->Password = EncryptedString(std::string_view(reinterpret_cast<const char*>(encrypted.data()), encrypted.size()));
}

User::User(const std::string& Name, const std::string& Password, enum Permissions)
	: Name(Name), Password(EncryptedString()) {
	auto Hashed = SaltAndHashPassword(Password);
	std::vector<uint8_t> Encrypted;
	XChaCha20 Cipher(XChaChaKey_, XChaChaNonce_);
	Cipher.Encrypt(Hashed, Encrypted);
	this->Password = EncryptedString(std::string_view(reinterpret_cast<const char*>(Encrypted.data()), Encrypted.size()));
}

bool User::VerifyPassword(const std::string& Input) const {
	auto Hashed = SaltAndHashPassword(Input);
	std::vector<uint8_t> Encrypted;
	XChaCha20 Cipher(XChaChaKey_, XChaChaNonce_);
	Cipher.Encrypt(Hashed, Encrypted);
	return Password.Decrypted() == std::string(Encrypted.begin(), Encrypted.end());
}
}