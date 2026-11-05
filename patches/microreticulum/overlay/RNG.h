#pragma once

#include "solar_os_reticulum_random.h"

// microReticulum seeds and services rweather's software RNG from Reticulum.cpp.
// SolarOS draws every random byte from its own generator
// (microReticulum/Cryptography/Random.h). begin() runs when the Reticulum
// instance is made and seeds it from a true random source; SolarOS reseeds
// it at each later start, and the generator reseeds itself as it runs.
struct SolarOsRng {
	void begin(const char*) { (void)solar_os_reticulum_random_reseed(); }
	void loop() {}
};

inline SolarOsRng RNG;
