#pragma once
// Shared helpers for remc1-tests.

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace remc1test {

// 64-bit FNV-1a: cheap, stable, good enough to detect regressions (not cryptographic).
inline uint64_t Fnv1a64(const void* data, size_t size, uint64_t h = 0xcbf29ce484222325ull)
{
	const uint8_t* p = static_cast<const uint8_t*>(data);
	for (size_t i = 0; i < size; i++) {
		h ^= p[i];
		h *= 0x100000001b3ull;
	}
	return h;
}

std::string HashHex(uint64_t h);

// Game data folder from REMC1_DATA_DIR ("" if unset/invalid). Tier-2/3 tests GTEST_SKIP without it.
std::string DataDir();

std::vector<uint8_t> ReadFile(const std::filesystem::path& p);

// Golden files: "key value" lines in tests/goldens/<name>.
// With REMC1_UPDATE_GOLDENS=1 the observed values are written back instead of compared.
class Goldens {
public:
	explicit Goldens(const std::string& name);
	~Goldens();
	// Returns true if the value matches the golden (or goldens are being updated).
	bool Check(const std::string& key, const std::string& value, std::string* expected = nullptr);
	bool Updating() const { return updating_; }
	size_t Size() const { return values_.size(); }

private:
	std::filesystem::path path_;
	std::map<std::string, std::string> values_;
	bool updating_ = false;
	bool dirty_ = false;
};

} // namespace remc1test
