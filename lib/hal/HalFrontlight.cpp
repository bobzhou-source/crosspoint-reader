#include "HalFrontlight.h"

#include <HalStorage.h>
#include <Logging.h>


namespace {
constexpr const char* kCurvePath = "/.crosspoint/frontlight.json";
constexpr size_t kCurveFileMax = 2048;
}  // namespace

HalFrontlight HalFrontlight::instance;

void HalFrontlight::loadCurve() {
  if (!Storage.exists(kCurvePath)) return;
  const String body = Storage.readFile(kCurvePath);
  if (body.isEmpty() || body.length() > kCurveFileMax) {
    LOG_INF("LIGHT", "Frontlight curve file ignored (empty or over %u bytes)", static_cast<unsigned>(kCurveFileMax));
    return;
  }
  const frontlight::Curve curve = frontlight::parseCurveJson(body.c_str(), body.length());
  if (!curve.fromFile) {
    LOG_INF("LIGHT", "Frontlight curve file is not an object; compiled curve kept");
    return;
  }
  manager.setCurve(curve);
  LOG_INF("LIGHT", "Frontlight curve file applied join=%u efficacy_x100=%u", curve.joinPercent,
          static_cast<unsigned>(curve.warmEfficacy * 100.0f + 0.5f));
}

void HalFrontlight::begin(const uint8_t brightness, const uint8_t warmth, const bool on) {
  if (!manager.present()) return;

  if (BoardConfig::isX4Pro()) loadCurve();
  manager.begin();
  lastBrightness = brightness > 100 ? 100 : brightness;
  manager.setColorTemperature(warmth > 100 ? 100 : warmth);
  lit = on;
  manager.setBrightness(lit ? lastBrightness : 0);
  LOG_INF("LIGHT", "Frontlight up: %u%% warm=%u%% %s", lastBrightness, manager.colorTemperature(), lit ? "on" : "off");
}

void HalFrontlight::setBrightness(const uint8_t percent) {
  lastBrightness = percent > 100 ? 100 : percent;
  if (lit) manager.setBrightness(lastBrightness);
}

void HalFrontlight::setWarmth(const uint8_t warmPercent) {
  manager.setColorTemperature(warmPercent > 100 ? 100 : warmPercent);
}

void HalFrontlight::setOn(const bool on) {
  if (on == lit) return;
  lit = on;
  manager.setBrightness(lit ? lastBrightness : 0);
}
