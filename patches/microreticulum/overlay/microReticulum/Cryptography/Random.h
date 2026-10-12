#pragma once

#include "../Bytes.h"

#include <esp_random.h>

#include <stdint.h>

namespace RNS { namespace Cryptography {

	inline const Bytes random(size_t length) {
		Bytes rand;
		esp_fill_random(rand.writable(length), length);
		return rand;
	}

	inline uint32_t randomnum() {
		return esp_random();
	}

	inline uint32_t randomnum(uint32_t max) {
		return randomnum() % max;
	}

	inline float random() {
		return (float)(randomnum() / (float)0xffffffff);
	}

} }
