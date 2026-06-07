#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace AstralDB {
namespace SQL {

/** RW→RX bump allocator for hand-rolled JIT kernels (W^X-safe). */
class JitExecPage {
public:
	static constexpr std::size_t PageSize = 4096;

	~JitExecPage();
	JitExecPage() = default;
	JitExecPage(const JitExecPage &) = delete;
	JitExecPage &operator=(const JitExecPage &) = delete;

	/** Copy \p Code into a fresh executable page; returns entry or nullptr. */
	void *Publish(const std::uint8_t *Code, std::size_t Size);

	/** Allocate and publish; ownership stays in \p Out until \c InvalidateAll. */
	static void *PublishOwned(std::vector<std::uint8_t> &Code, std::unique_ptr<JitExecPage> &Out);

private:
	struct Region {
		void *Base = nullptr;
		std::size_t Mapped = 0;
		std::size_t Used = 0;
	};

	void *BumpInCurrent(const std::uint8_t *Code, std::size_t Size);
	bool AppendRegion(std::size_t MinMapped);

	std::vector<Region> Regions_;
};

} // namespace SQL
} // namespace AstralDB
