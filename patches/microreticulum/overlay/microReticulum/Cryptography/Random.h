#pragma once

#include "../Bytes.h"

#include "solar_os_reticulum_random.h"

#include <stdexcept>
#include <stdint.h>

// Every random byte Reticulum uses comes from SolarOS's CTR-DRBG, which is
// seeded from a true random source even with Wi-Fi and Bluetooth off (see
// services/solar_os_reticulum_random.h). It throws rather than hand out
// bytes it could not make.

namespace RNS { namespace Cryptography {

	inline void fill_random(uint8_t* out, size_t length) {
		if (length > 0 && solar_os_reticulum_random(out, length) != ESP_OK) {
			throw std::runtime_error("random generator unavailable");
		}
	}

	inline const Bytes random(size_t length) {
		Bytes rand;
		if (length > 0) {
			fill_random(rand.writable(length), length);
		}
		return rand;
	}

	inline uint32_t randomnum() {
		uint32_t value = 0;
		fill_random((uint8_t*)&value, sizeof(value));
		return value;
	}

	inline uint32_t randomnum(uint32_t max) {
		return max == 0 ? 0 : randomnum() % max;
	}

	inline float random() {
		return (float)(randomnum() / (float)0xffffffff);
	}

} }
