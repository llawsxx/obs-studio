#pragma once

#include <cstdint>
#include <limits>

namespace dvc {

inline uint64_t mul_div(uint64_t value, uint64_t multiplier, uint64_t divisor)
{
	const uint64_t whole = value / divisor;
	const uint64_t remainder = value % divisor;
	if (remainder <= std::numeric_limits<uint64_t>::max() / multiplier) {
		return whole * multiplier + remainder * multiplier / divisor;
	}
	uint64_t fraction = 0;
	uint64_t accumulator = 0;
	for (int bit = 63; bit >= 0; --bit) {
		accumulator = accumulator * 2 + ((multiplier >> bit) & 1 ? remainder : 0);
		fraction = fraction * 2 + accumulator / divisor;
		accumulator %= divisor;
	}
	return whole * multiplier + fraction;
}

struct Rate {
	uint32_t numerator = 60;
	uint32_t denominator = 1;
	bool fixed_interval = false;

	bool valid() const
	{
		return numerator && denominator && numerator <= 1000000 && denominator <= 1000000 &&
		       numerator >= denominator && uint64_t(numerator) <= uint64_t(denominator) * 1000;
	}

	uint64_t offset(uint64_t index) const
	{
		const uint64_t period = uint64_t(1000000000) * denominator;
		return fixed_interval ? index * (period / numerator) : mul_div(index, period, numerator);
	}

	uint64_t index_at(uint64_t elapsed) const
	{
		return fixed_interval ? elapsed / offset(1)
				      : mul_div(elapsed, numerator, uint64_t(1000000000) * denominator);
	}

	uint64_t after(uint64_t elapsed) const
	{
		uint64_t index = index_at(elapsed) + 1;
		while (offset(index) <= elapsed) {
			++index;
		}
		return index;
	}

	uint64_t window() const
	{
		return fixed_interval ? offset(1) * 3 / 5 : mul_div(600000000, denominator, numerator);
	}
};

inline bool eligible(uint64_t sequence, uint64_t consumed, uint64_t timestamp, uint64_t now, Rate rate)
{
	const uint64_t difference = timestamp > now ? timestamp - now : now - timestamp;
	return sequence > consumed && difference <= rate.window();
}

}
