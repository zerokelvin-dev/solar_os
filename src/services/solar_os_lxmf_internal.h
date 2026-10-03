#pragma once

#include <microReticulum.h>

#include "esp_err.h"

/*
 * Driven by the Reticulum worker thread, which owns every call here. The
 * public C surface only queues work for it.
 */
namespace solar_os {
namespace lxmf {

esp_err_t attach(const RNS::Identity &identity);
void detach();
void tick();

}  // namespace lxmf
}  // namespace solar_os
