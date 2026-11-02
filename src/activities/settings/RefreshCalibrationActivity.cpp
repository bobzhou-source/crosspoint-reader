#include "RefreshCalibrationActivity.h"

#include <CrossPointSettings.h>
#include <EpdFontFamily.h>
#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <RefreshCalibrationText.h>
#include <RefreshPolicyConfig.h>
#include <SdCardFontSystem.h>
#include <esp_random.h>

#include <algorithm>
#include <cstdio>

#include "RefreshPolicyStore.h"
#include "activities/reader/ReaderUtils.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace rc = refresh_calibration;
namespace {
constexpr char kDir[] = "/.crosspoint/refresh-calibration";
constexpr char kCurrent[] = "/.crosspoint/refresh-calibration/current";

// Bundled public-domain text. Pages never come from the user's library.
constexpr char kLatin[] =
    "It is a truth universally acknowledged, that a single man in possession of a good fortune must be in want of a "
    "wife. However little known the feelings or views of such a man may be on his first entering a neighbourhood, "
    "this truth is so well fixed in the minds of the surrounding families, that he is considered as the rightful "
    "property of some one or other of their daughters. Call me Ishmael. Some years ago, having little or no money "
    "in my purse, and nothing particular to interest me on shore, I thought I would sail about a little and see "
    "the watery part of the world. It is a way I have of driving off the spleen and regulating the circulation. "
    "Whenever I find myself growing grim about the mouth; whenever it is a damp, drizzly November in my soul; "
    "whenever I find myself involuntarily pausing before coffin warehouses, and bringing up the rear of every "
    "funeral I meet; and especially whenever my hypos get such an upper hand of me, that it requires a strong "
    "moral principle to prevent me from deliberately stepping into the street, and methodically knocking people's "
    "hats off, then, I account it high time to get to sea as soon as I can. I went to the woods because I wished "
    "to live deliberately, to front only the essential facts of life, and see if I could not learn what it had to "
    "teach, and not, when I came to die, discover that I had not lived. ";

constexpr char kCjk[] =
    "晋太元中武陵人捕鱼为业缘溪行忘路之远近忽逢桃花林夹岸数百步中无杂树芳草鲜美落英缤纷渔人甚异之复前行欲穷其林林尽"
    "水源便得一山山有小口仿佛若有光便舍船从口入初极狭才通人复行数十步豁然开朗土地平旷屋舍俨然有良田美池桑竹之属阡"
    "陌交通鸡犬相闻其中往来种作男女衣着悉如外人黄发垂髫并怡然自乐见渔人乃大惊问所从来具答之便要还家设酒杀鸡作食村"
    "中闻有此人咸来问讯自云先世避秦时乱率妻子邑人来此绝境不复出焉遂与外人间隔问今是何世乃不知有汉无论魏晋此人一一"
    "为具言所闻皆叹惋余人各复延至其家皆出酒食停数日辞去此中人语云不足为外人道也既出得其船便扶向路处处志之及郡下诣"
    "太守说如此太守即遣人随其往寻向所志遂迷不复得路南阳刘子骥高尚士也闻之欣然规往未果寻病终后遂无问津者庆历四年春"
    "滕子京谪守巴陵郡越明年政通人和百废具兴乃重修岳阳楼增其旧制刻唐贤今人诗赋于其上属予作文以记之予观夫巴陵胜状在"
    "洞庭一湖衔远山吞长江浩浩汤汤横无际涯朝晖夕阴气象万千此则岳阳楼之大观也前人之述备矣然则北通巫峡南极潇湘迁客骚"
    "人多会于此览物之情得无异乎若夫淫雨霏霏连月不开阴风怒号浊浪排空日星隐曜山岳潜形商旅不行樯倾楫摧薄暮冥冥虎啸猿"
    "啼登斯楼也则有去国怀乡忧谗畏讥满目萧然感极而悲者矣至若春和景明波澜不惊上下天光一碧万顷沙鸥翔集锦鳞游泳岸芷汀"
    "兰郁郁青青而或长烟一空皓月千里浮光跃金静影沉璧渔歌互答此乐何极登斯楼也则有心旷神怡宠辱偕忘把酒临风其喜洋洋者"
    "矣嗟夫予尝求古仁人之心或异二者之为何哉不以物喜不以己悲居庙堂之高则忧其民处江湖之远则忧其君是进亦忧退亦忧然则"
    "何时而乐耶其必曰先天下之忧而忧后天下之乐而乐乎噫微斯人吾谁与归";

constexpr char kMenuItems[][24] = {"Continue reading", "Library",      "Recent books", "Settings",     "Wi-Fi networks",
                                   "Sleep",            "Reading stats", "About",        "Check updates", "Benchmark"};

// Probe geometry in the portrait logical frame (480x800 on the X4 Pro). The
// Mac analyzer hard-codes the same numbers; change both together.
constexpr int kFiducial = 56;
constexpr int kFiducialInset = 8;
constexpr int kBodyTop = 88;
constexpr int kBodyBottom = 696;
constexpr int kMargin = 24;
constexpr int kCodeX = 120;
constexpr int kCodeY = 48;
constexpr int kCodeCell = 24;
constexpr int kCodeCells = 10;
constexpr int kBandCount = 3;

uint8_t imageLevel(int x, int y, uint32_t seed) {
  const int cell = 48;
  const int shade = (x / cell + y / cell + static_cast<int>(seed % 3)) % 3;
  const int cx = 120 + static_cast<int>(seed % 90);
  const int cy = 80;
  const int dx = x - cx;
  const int dy = y - cy;
  if (dx * dx + dy * dy < 60 * 60) return 3;
  if ((x + y + static_cast<int>(seed)) % 17 == 0) return 0;
  return static_cast<uint8_t>(shade + 1);
}

constexpr uint8_t kBayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

StrId ratingLabel(uint8_t rating) {
  switch (rating) {
    case 0: return StrId::STR_REFRESH_RATING_NONE;
    case 1: return StrId::STR_REFRESH_RATING_FINE;
    case 2: return StrId::STR_REFRESH_RATING_BAD;
  }
  return StrId::STR_REFRESH_RATING_NONE;
}
}  // namespace

RefreshCalibrationActivity::RefreshCalibrationActivity(GfxRenderer& renderer, MappedInputManager& input)
    : Activity("RefreshCalibration", renderer, input),
      driver(false, AdaptiveRefreshPolicy::Config{20000, 20000, 500, 0, 0, 0, 0}),
      observer(false, refresh_policy_config::active().config),
      meter(false, AdaptiveRefreshPolicy::Config{20000, 20000, 500, 0, 0, 0, 0}) {}

void RefreshCalibrationActivity::onEnter() {
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  refresh_policy_store::load();
  observer = AdaptiveRefreshPolicy(false, refresh_policy_config::active().config);
  fontId = SETTINGS.sdFontFamilyName[0] ? sdFontSystem.resolveFontId(SETTINGS.sdFontFamilyName, SETTINGS.fontPointSize)
                                        : SETTINGS.getReaderFontId();
  const auto found = renderer.getFontMap().find(fontId);
  if (fontId == 0 || found == renderer.getFontMap().end()) {
    fontId = 0;
    message = StrId::STR_REFRESH_FONT_UNAVAILABLE;
    stage = Stage::Error;
    requestUpdate();
    return;
  }
  skipMask = found->second.hasCodepoint(0x4E0D) ? 0 : rc::classBit(rc::ContentClass::DenseCjk);
  inspectSession();
  requestUpdate();
}

void RefreshCalibrationActivity::onExit() {
  stage = Stage::Intro;
  ReaderUtils::applyMenuOrientation(renderer);
}

bool RefreshCalibrationActivity::forcedInversion(bool& inverted) const {
  if (stage != Stage::Running && stage != Stage::Rating) return false;
  inverted = rung.polarity == rc::Polarity::Night;
  return true;
}

void RefreshCalibrationActivity::inspectSession() {
  resumable = false;
  damagedLog = false;
  char id[sizeof(sessionId)]{};
  const size_t n = Storage.readFileToBuffer(kCurrent, id, sizeof(id));
  if (n == 0 || n >= sizeof(id)) return;
  id[n] = '\0';
  snprintf(logPath, sizeof(logPath), "%s/%s.jsonl", kDir, id);
  HalFile file;
  if (!Storage.openFileForRead("RCAL", logPath, file)) return;
  auto buffer = makeUniqueNoThrow<char[]>(file.size() + 1);
  if (!buffer) return;
  const size_t size = file.size();
  if (file.read(buffer.get(), size) != static_cast<int>(size)) return;
  buffer[size] = '\0';
  rc::Replay replay;
  const char* cursor = buffer.get();
  while (*cursor) {
    const char* end = strchr(cursor, '\n');
    const size_t length = end ? static_cast<size_t>(end - cursor) : strlen(cursor);
    const auto status = replay.feed(cursor, length);
    if (status != rc::Replay::Status::Ok) {
      damagedLog = true;
      return;
    }
    if (!end) break;
    cursor = end + 1;
  }
  if (!replay.started() || replay.ladder().done() || replay.ladder().skipMask() != skipMask) return;
  ladder = replay.ladder();
  snprintf(sessionId, sizeof(sessionId), "%s", id);
  resumeRated = ladder.rated();
  resumable = true;
}

bool RefreshCalibrationActivity::startSession() {
  if (!Storage.ensureDirectoryExists(kDir)) return false;
  snprintf(sessionId, sizeof(sessionId), "rc-%08lx", static_cast<unsigned long>(esp_random()));
  snprintf(logPath, sizeof(logPath), "%s/%s.jsonl", kDir, sessionId);
  rc::SessionInfo info;
  info.session = sessionId;
  info.firmware = CROSSPOINT_VERSION;
  info.fontName = SETTINGS.sdFontFamilyName[0] ? SETTINGS.sdFontFamilyName : "built-in";
  info.fontId = static_cast<uint32_t>(fontId);
  info.pointSize = SETTINGS.fontPointSize;
  info.antialias = SETTINGS.textAntiAliasing != 0;
  info.skipMask = skipMask;
  info.pixelCount = static_cast<uint32_t>(renderer.getScreenWidth()) * renderer.getScreenHeight();
  const auto& state = refresh_policy_config::active();
  info.policySource = refresh_policy_config::sourceName(state.source);
  info.config = state.config;
  const size_t n = rc::formatSessionRow(row, sizeof(row), info);
  if (!n || !appendRow(row, n)) return false;
  return Storage.writeFile(kCurrent, String(sessionId));
}

bool RefreshCalibrationActivity::appendRow(const char* text, const size_t length) {
  auto file = Storage.open(logPath, O_WRONLY | O_CREAT | O_APPEND);
  if (!file || file.write(text, length) != length || !file.sync()) {
    LOG_ERR("RCAL", "Could not append calibration log row");
    return false;
  }
  return true;
}

void RefreshCalibrationActivity::fail(const StrId reason) {
  message = reason;
  stage = Stage::Error;
  requestUpdate();
}

void RefreshCalibrationActivity::startRung() {
  rung = ladder.next();
  measurement = {};
  frame = 0;
  inkSum = 0;
  driver.reset(false);
  observer.reset(false);
  meter.reset(false);
  rungStartedMs = millis();
  stage = Stage::Running;
  frameDone.store(false);
  requestUpdate();
}

void RefreshCalibrationActivity::rate(const uint8_t rating) {
  const size_t n = rc::formatRungRow(row, sizeof(row), rung, measurement, rating, millis() - rungStartedMs);
  if (!n || !appendRow(row, n) || !ladder.rate(rung, rating)) {
    fail(StrId::STR_REFRESH_LOG_FAILED);
    return;
  }
  if (ladder.done()) {
    const size_t done = rc::formatCompleteRow(row, sizeof(row), ladder);
    if (done) appendRow(row, done);
    Storage.remove(kCurrent);
    stage = Stage::Done;
    message = StrId::STR_REFRESH_DONE;
  } else {
    startRung();
  }
  requestUpdate();
}

int RefreshCalibrationActivity::readRating() const {
  using Button = MappedInputManager::Button;
  if (mappedInput.wasReleased(Button::Left) || mappedInput.wasReleased(Button::Up)) return 0;
  if (mappedInput.wasReleased(Button::Confirm)) return 1;
  if (mappedInput.wasReleased(Button::Right) || mappedInput.wasReleased(Button::Down)) return 2;
  int x = 0;
  int y = 0;
  if (!mappedInput.wasScreenTapped(x, y)) return -1;
  if (y < kBodyBottom) return 3;
  const int third = std::max(1, renderer.getScreenWidth() / 3);
  if (x < third) return 0;
  if (x < 2 * third) return 1;
  return 2;
}

void RefreshCalibrationActivity::loop() {
  using Button = MappedInputManager::Button;
  if (stage == Stage::Intro || stage == Stage::Done || stage == Stage::Error) {
    if (mappedInput.wasReleased(Button::Back)) {
      Activity::finish();
      return;
    }
    if (stage == Stage::Intro && resumable &&
        (mappedInput.wasReleased(Button::Left) || mappedInput.wasReleased(Button::Up))) {
      resumable = false;
      ladder = rc::Ladder(skipMask);
      requestUpdate();
      return;
    }
    int tapX = 0;
    int tapY = 0;
    const bool bodyTap = mappedInput.wasScreenTapped(tapX, tapY);
    if (stage == Stage::Intro && (mappedInput.wasReleased(Button::Confirm) || bodyTap)) {
      if (!resumable && !startSession()) {
        fail(StrId::STR_REFRESH_LOG_FAILED);
        return;
      }
      startRung();
    }
    return;
  }
  if (stage == Stage::Running && frameDone.exchange(false)) {
    if (mappedInput.wasReleased(Button::Back)) {
      stage = Stage::Intro;
      resumable = ladder.rated() > 0;
      resumeRated = ladder.rated();
      requestUpdate();
      return;
    }
    ++frame;
    if (frame > rung.fastCount()) stage = Stage::Rating;
    requestUpdate();
    return;
  }
  if (stage == Stage::Rating) {
    const int rating = readRating();
    if (rating >= 0 && rating <= 2) {
      rate(static_cast<uint8_t>(rating));
    } else if (rating == 3 || mappedInput.wasReleased(Button::Back)) {
      stage = Stage::Intro;
      resumable = true;
      resumeRated = ladder.rated();
      requestUpdate();
    }
  }
}

void RefreshCalibrationActivity::render(RenderLock&&) {
  if (stage == Stage::Running) {
    renderFrame();
    frameDone.store(true);
  } else if (stage == Stage::Rating) {
    drawProbe();
  } else {
    renderMenu();
  }
}

void RefreshCalibrationActivity::renderMenu() {
  renderer.clearScreen();
  const auto& theme = GUI;
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  theme.drawHeader(renderer, Rect{0, 0, width, metrics.headerHeight}, tr(STR_REFRESH_CALIBRATION));
  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int statusTop = height - metrics.buttonHintsHeight - 3 * lineHeight;
  UITheme::drawCenteredWrappedText(renderer, Rect{12, metrics.headerHeight + 8, width - 24, statusTop - metrics.headerHeight - 16},
                                   UI_12_FONT_ID, I18N.get(message), 14, true, EpdFontFamily::REGULAR,
                                   UITheme::TextVerticalAlignment::TOP);
  const auto& config = refresh_policy_config::active();
  snprintf(line, sizeof(line), "policy %s: day %lu / night %lu", refresh_policy_config::sourceName(config.source),
           static_cast<unsigned long>(config.config.clearThresholdPermille),
           static_cast<unsigned long>(config.config.nightClearThresholdPermille));
  renderer.drawText(UI_12_FONT_ID, 12, statusTop, line);
  if (stage == Stage::Done) {
    renderer.drawText(SMALL_FONT_ID, 12, statusTop + lineHeight, logPath);
  } else if (resumable) {
    snprintf(line, sizeof(line), "%u rated, resume", static_cast<unsigned>(resumeRated));
    renderer.drawText(UI_12_FONT_ID, 12, statusTop + lineHeight, line);
  } else if (damagedLog) {
    renderer.drawText(UI_12_FONT_ID, 12, statusTop + lineHeight, tr(STR_REFRESH_LOG_DAMAGED));
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), stage == Stage::Intro ? tr(STR_REFRESH_START) : "",
                                            resumable ? tr(STR_REFRESH_NEW) : "", "");
  theme.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

void RefreshCalibrationActivity::renderFrame() {
  layoutRecorded = false;
  renderer.clearScreen();
  // Same two passes as the readers: the scan pass records the page's codepoints so an SD-card
  // font loads them in one batch instead of glyph by glyph, and the scope resets the per-page
  // glyph caches afterwards.
  auto scope = renderer.getFontCacheManager()->createPrewarmScope();
  drawContent(-1);
  scope.endScanAndPrewarm();
  drawContent(-1);
  displayContentFrame(frame == 0);
  commitPageAdvance();
}

void RefreshCalibrationActivity::commitPageAdvance() {
  if (rung.kind != rc::RungKind::Probe || rung.cls == rc::ContentClass::Menu) return;
  textOffset[rung.cls == rc::ContentClass::DenseCjk ? 1 : 0] = pageEnd;
}

void RefreshCalibrationActivity::displayContentFrame(const bool clear) {
  const bool textPage = rung.kind == rc::RungKind::Probe && rung.cls != rc::ContentClass::GrayImage &&
                        rung.cls != rc::ContentClass::Menu;
  const bool grayscale = textPage && ReaderUtils::textAntiAliasingVisible(renderer);
  const bool image = rung.kind == rc::RungKind::Probe && rung.cls == rc::ContentClass::GrayImage &&
                     renderer.grayscaleCapabilities().supported();
  AdaptiveRefreshPolicy::Inputs inputs;
  inputs.frame = renderer.measureFrameChange();
  inputs.hasImage = rung.kind == rc::RungKind::Probe && rung.cls == rc::ContentClass::GrayImage;
  inputs.visibleGrayscale = grayscale || image;
  inputs.externalClearPending = renderer.hasPendingClearingRefresh();
  inputs.invertedOutput = display.isInverted();
  account(inputs, clear);
  const auto mode = clear ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH;
  if (grayscale) {
    if (renderer.grayscaleCapabilities().base == HalDisplay::GrayscaleBase::Combined) {
      renderer.displayGrayscaleBase(mode);
    } else {
      renderer.displayBuffer(mode);
    }
    ReaderUtils::renderAntiAliased(renderer, [this]() { drawContent(-1); });
  } else if (image) {
    if (renderer.grayscaleCapabilities().base == HalDisplay::GrayscaleBase::Combined) {
      renderer.displayGrayscaleBase(mode);
    } else if (clear) {
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
      renderer.preconditionGrayscale();
    } else {
      renderer.displayGrayscaleBase(HalDisplay::FAST_REFRESH);
    }
    for (const auto pass : {GfxRenderer::GRAYSCALE_LSB, GfxRenderer::GRAYSCALE_MSB}) {
      renderer.clearScreen(0x00);
      renderer.setRenderMode(pass);
      drawContent(pass == GfxRenderer::GRAYSCALE_LSB ? 0 : 1);
      if (pass == GfxRenderer::GRAYSCALE_LSB) renderer.copyGrayscaleLsbBuffers();
      else renderer.copyGrayscaleMsbBuffers();
    }
    renderer.displayGrayBuffer();
    renderer.setRenderMode(GfxRenderer::BW);
    renderer.clearScreen();
    drawContent(-1);
    renderer.cleanupGrayscaleWithFrameBuffer();
  } else {
    renderer.displayBuffer(mode);
  }
  const auto booked = clear ? AdaptiveRefreshPolicy::Refresh::Clear : AdaptiveRefreshPolicy::Refresh::Fast;
  driver.commit(booked);
  observer.commit(booked);
  meter.commit(booked);
  if (frame > 0) {
    ++measurement.frames;
    inkSum += inputs.frame.inkPixels;
    measurement.meanInkPixels = static_cast<uint32_t>(inkSum / measurement.frames);
  }
  if (frame == rung.fastCount()) {
    measurement.pixelCount = inputs.frame.pixelCount;
    measurement.changedPixels = meter.debtPixels();
    measurement.weightedDebt = observer.debtPixels();
    measurement.grayscale = grayscale;
    measurement.image = inputs.hasImage;
  }
}

void RefreshCalibrationActivity::account(const AdaptiveRefreshPolicy::Inputs& inputs, const bool clear) {
  if (clear) driver.requestClear();
  (void)driver.decide(inputs);
  const auto watched = observer.decide(inputs);
  if (frame > 0 && measurement.policyClearAt == 0 && watched.refresh == AdaptiveRefreshPolicy::Refresh::Clear) {
    measurement.policyClearAt = frame;
  }
  (void)meter.decide(inputs);
}

void RefreshCalibrationActivity::drawContent(const int plane) {
  const int width = renderer.getScreenWidth() - 2 * kMargin;
  const int height = kBodyBottom - kBodyTop;
  if (rung.kind == rc::RungKind::Reference) {
    renderer.fillRect(kMargin, kBodyTop, width, height, false);
    return;
  }
  if (rung.cls == rc::ContentClass::Menu) {
    if (plane < 0) drawMenuPage(frame);
    return;
  }
  if (rung.cls == rc::ContentClass::GrayImage) {
    const int imageBottom = kBodyTop + height * 2 / 3;
    if (plane < 0 && !layoutRecorded) {
      imageSeed = static_cast<uint32_t>(frame) * 17 + 3;
      pageOffset = textOffset[0];
      layoutRecorded = true;
    }
    const bool gray = renderer.grayscaleCapabilities().supported();
    const uint8_t pass = !gray ? 3 : plane < 0 ? 0 : plane == 0 ? 1 : 2;
    if (!renderer.isFontCacheScanning()) drawImage(kBodyTop, imageBottom, pass);
    if (plane < 0) drawTextPage(TextKind::Latin, pageOffset, imageBottom + 8, kBodyBottom, width, 6, true);
    return;
  }
  const auto kind = rung.cls == rc::ContentClass::DenseCjk ? TextKind::Cjk : TextKind::Latin;
  const int lines = rung.cls == rc::ContentClass::Sparse ? 4 : 200;
  if (!layoutRecorded) {
    pageOffset = textOffset[static_cast<int>(kind)];
    layoutRecorded = true;
  }
  if (plane < 0) drawTextPage(kind, pageOffset, kBodyTop, kBodyBottom, width, lines, true);
}

void RefreshCalibrationActivity::drawTextPage(const TextKind kind, uint32_t offset, const int top, const int bottom,
                                             const int maxWidth, const int maxLines, const bool record) {
  const bool cjk = kind == TextKind::Cjk;
  const char* corpus = cjk ? kCjk : kLatin;
  const size_t corpusLength = cjk ? sizeof(kCjk) - 1 : sizeof(kLatin) - 1;
  const auto width = [](const void* context, const char* text) {
    const auto* self = static_cast<const RefreshCalibrationActivity*>(context);
    return self->renderer.getTextWidth(self->fontId, text);
  };
  const int lineHeight = renderer.getLineHeight(fontId);
  for (int y = top, lines = 0; y + lineHeight <= bottom && lines < maxLines; y += lineHeight, ++lines) {
    offset = rc::nextFixtureLine(corpus, corpusLength, offset, cjk, maxWidth, width, this, line, sizeof(line));
    renderer.drawText(fontId, kMargin, y, line);
  }
  if (record) pageEnd = offset;
}

void RefreshCalibrationActivity::drawImage(const int top, const int bottom, const uint8_t pass) {
  const int width = renderer.getScreenWidth() - 2 * kMargin;
  for (int y = top; y < bottom; ++y) {
    for (int x = kMargin; x < kMargin + width; ++x) {
      const uint8_t level = imageLevel(x - kMargin, y - top, imageSeed);
      bool black = false;
      if (pass == 3) {
        black = level * 4 > kBayer[(y - top) & 3][(x - kMargin) & 3];
      } else if (pass == 0) {
        black = level >= 1;
      } else if (pass == 1) {
        black = level == 1;
      } else {
        black = level == 1 || level == 2;
      }
      if (black) renderer.drawPixel(x, y, true);
    }
  }
}

void RefreshCalibrationActivity::drawMenuPage(const uint16_t selection) {
  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  renderer.drawText(UI_12_FONT_ID, kMargin, kBodyTop, "Main menu", true, EpdFontFamily::BOLD);
  for (int i = 0; i < 10; ++i) {
    const int y = kBodyTop + 2 * lineHeight + i * (lineHeight + 6);
    const bool selected = i == selection % 10;
    if (selected) renderer.fillRect(kMargin, y - 2, renderer.getScreenWidth() - 2 * kMargin, lineHeight + 4, true);
    renderer.drawText(UI_12_FONT_ID, kMargin + 8, y, kMenuItems[i], !selected);
  }
}

void RefreshCalibrationActivity::drawProbe() {
  const bool night = rung.polarity == rc::Polarity::Night;
  const bool black = !night;  // drawn so the physical result is identical in both polarities
  renderer.clearScreen(night ? 0x00 : 0xFF);
  for (int corner = 0; corner < 4; ++corner) {
    const int x = corner & 1 ? renderer.getScreenWidth() - kFiducialInset - kFiducial : kFiducialInset;
    const int y = corner & 2 ? kBodyBottom + 4 : kFiducialInset;
    renderer.fillRect(x, y, kFiducial, kFiducial, black);
    renderer.fillRect(x + kFiducial / 4, y + kFiducial / 4, kFiducial / 2, kFiducial / 2, !black);
  }
  rung.formatId(line, sizeof(line));
  renderer.drawText(UI_12_FONT_ID, kFiducialInset + kFiducial + 8, kFiducialInset + 4, line, black, EpdFontFamily::BOLD);
  uint16_t code = static_cast<uint16_t>((1u << 9) | (rung.seq << 1));
  if (__builtin_popcount(code) & 1) code |= 1u;
  for (int i = 0; i < kCodeCells; ++i) {
    renderer.fillRect(kCodeX + i * kCodeCell, kCodeY, kCodeCell - 4, kCodeCell / 2, ((code >> (9 - i)) & 1) ? black : !black);
  }
  const int bandHeight = (kBodyBottom - kBodyTop) / kBandCount;
  renderer.fillRect(kMargin, kBodyTop, renderer.getScreenWidth() - 2 * kMargin, bandHeight, !black);
  for (int y = kBodyTop + bandHeight; y < kBodyTop + 2 * bandHeight; y += 2) {
    for (int x = kMargin; x < renderer.getScreenWidth() - kMargin; x += 2) renderer.drawPixel(x + (y & 2 ? 1 : 0), y, black);
  }
  renderer.fillRect(kMargin, kBodyTop + 2 * bandHeight, renderer.getScreenWidth() - 2 * kMargin, bandHeight, black);
  const auto labels = mappedInput.mapLabels(tr(STR_REFRESH_PAUSE), I18N.get(ratingLabel(1)), I18N.get(ratingLabel(0)),
                                            I18N.get(ratingLabel(2)));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  const int labelY = kBodyBottom + 8;
  const int rightX = renderer.getScreenWidth() - kMargin - kFiducial - 70;
  renderer.drawText(UI_10_FONT_ID, kMargin + kFiducial + 4, labelY, tr(STR_REFRESH_RATING_NONE), black);
  renderer.drawCenteredText(UI_10_FONT_ID, labelY, tr(STR_REFRESH_RATING_FINE), black);
  renderer.drawText(UI_10_FONT_ID, rightX, labelY, tr(STR_REFRESH_RATING_BAD), black);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
