#include "NavScript.h"

#include <cstring>
#include <limits>

namespace benchmark::nav {
namespace {

bool isSpace(const char c) { return c == ' ' || c == '\t' || c == '\r'; }
bool isIdChar(const char c) {
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
}
bool isNameChar(const char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

bool parseNumber(const char* begin, const char* end, const uint32_t maximum, uint32_t& value) {
  if (begin == end) return false;
  uint64_t result = 0;
  for (const char* p = begin; p < end; ++p) {
    if (*p < '0' || *p > '9') return false;
    result = result * 10 + static_cast<uint64_t>(*p - '0');
    if (result > maximum) return false;
  }
  value = static_cast<uint32_t>(result);
  return true;
}

bool keyFromName(const char* begin, const size_t length, Key& key) {
  struct Entry {
    const char* name;
    Key key;
  };
  static constexpr Entry ENTRIES[] = {{"Back", Key::Back},   {"Confirm", Key::Confirm},
                                      {"Left", Key::Left},   {"Right", Key::Right},
                                      {"Up", Key::Up},       {"Down", Key::Down},
                                      {"PageForward", Key::PageForward}, {"PageBack", Key::PageBack}};
  for (const auto& entry : ENTRIES) {
    if (strlen(entry.name) == length && strncmp(entry.name, begin, length) == 0) {
      key = entry.key;
      return true;
    }
  }
  return false;
}

// `Name[@holdMs][*count]`
ParseCode parsePress(const char* begin, const char* end, const bool allowCount, Press& press, uint32_t& count) {
  const char* nameEnd = begin;
  while (nameEnd < end && *nameEnd != '@' && *nameEnd != '*') ++nameEnd;
  if (!keyFromName(begin, static_cast<size_t>(nameEnd - begin), press.key)) return ParseCode::UnknownKey;
  press.holdMs = 60;
  count = 1;
  const char* p = nameEnd;
  while (p < end) {
    const char marker = *p++;
    const char* numberEnd = p;
    while (numberEnd < end && *numberEnd != '@' && *numberEnd != '*') ++numberEnd;
    uint32_t value = 0;
    if (marker == '@') {
      if (!parseNumber(p, numberEnd, 5000, value) || value == 0) return ParseCode::BadNumber;
      press.holdMs = static_cast<uint16_t>(value);
    } else {
      if (!allowCount || !parseNumber(p, numberEnd, Step::LEAD_MAX, value) || value == 0) return ParseCode::BadNumber;
      count = value;
    }
    p = numberEnd;
  }
  return ParseCode::Ok;
}

ParseCode copyToken(const char* begin, const char* end, char* out, const size_t capacity, const bool nameChars) {
  const size_t length = static_cast<size_t>(end - begin);
  if (length == 0) return ParseCode::BadNumber;
  if (length >= capacity) return ParseCode::TokenTooLong;
  for (size_t i = 0; i < length; ++i) {
    if (!(nameChars ? isNameChar(begin[i]) : isIdChar(begin[i]))) return ParseCode::BadStepId;
    out[i] = begin[i];
  }
  out[length] = '\0';
  return ParseCode::Ok;
}

const char* lineEnd(const char* text) {
  while (*text && *text != '\n') ++text;
  return text;
}

bool isArgChar(const char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' ||
         c == '/' || c == '-';
}

// `verb:arg`. Each verb validates its own argument so a typo never reaches the device as a silent no-op.
ParseCode parseAction(const char* begin, const char* end, Step& step) {
  if (step.action != ActionVerb::None) return ParseCode::BadAction;
  const char* colon = begin;
  while (colon < end && *colon != ':') ++colon;
  if (colon == end) return ParseCode::BadAction;
  const size_t verbLength = static_cast<size_t>(colon - begin);
  const char* argBegin = colon + 1;
  const size_t argLength = static_cast<size_t>(end - argBegin);
  if (argLength == 0 || argLength >= Step::ARG_CHARS)
    return argLength == 0 ? ParseCode::BadAction : ParseCode::TokenTooLong;
  for (const char* p = argBegin; p < end; ++p)
    if (!isArgChar(*p)) return ParseCode::BadAction;
  const auto verbIs = [&](const char* name) { return strlen(name) == verbLength && strncmp(name, begin, verbLength) == 0; };
  ActionVerb verb = ActionVerb::None;
  if (verbIs("open")) {
    // An absolute card path with no parent references.
    if (*argBegin != '/') return ParseCode::BadAction;
    for (const char* p = argBegin; p + 1 < end; ++p)
      if (p[0] == '.' && p[1] == '.') return ParseCode::BadAction;
    verb = ActionVerb::Open;
  } else if (verbIs("menu")) {
    if (!(argLength == 4 && !strncmp(argBegin, "list", 4)) && !(argLength == 7 && !strncmp(argBegin, "toolbar", 7)))
      return ParseCode::BadAction;
    verb = ActionVerb::MenuStyle;
  } else if (verbIs("dict")) {
    for (const char* p = argBegin; p < end; ++p)
      if (*p == '/') return ParseCode::BadAction;  // a folder name under /dictionaries, not a path
    verb = ActionVerb::Dictionary;
  } else if (verbIs("idle")) {
    verb = ActionVerb::Idle;  // a label for the log reader; the step's quiet/norefresh window is the measurement
  } else if (verbIs("goto")) {
    // Opens a top-level screen by a Replace that clears the whole activity stack, so it does not depend on the screen,
    // tab or cursor the run starts from (or, for Library, on the Home layout): "library", or "home" (Home with the
    // Settings row selected, exactly what Back from the Settings screen leaves).
    const bool library = argLength == 7 && !strncmp(argBegin, "library", 7);
    const bool home = argLength == 4 && !strncmp(argBegin, "home", 4);
    if (!library && !home) return ParseCode::BadAction;
    verb = ActionVerb::Goto;
  } else {
    return ParseCode::BadAction;
  }
  step.action = verb;
  memcpy(step.arg, argBegin, argLength);
  step.arg[argLength] = '\0';
  return ParseCode::Ok;
}

}  // namespace

const char* parseCodeName(const ParseCode code) {
  switch (code) {
    case ParseCode::Ok: return "ok";
    case ParseCode::BadStepId: return "bad_step_id";
    case ParseCode::DuplicateId: return "duplicate_id";
    case ParseCode::UnknownField: return "unknown_field";
    case ParseCode::UnknownKey: return "unknown_key";
    case ParseCode::BadNumber: return "bad_number";
    case ParseCode::MissingGuard: return "missing_guard";
    case ParseCode::TooManyLead: return "too_many_lead";
    case ParseCode::TokenTooLong: return "token_too_long";
    case ParseCode::NoAction: return "no_action";
    case ParseCode::NoSteps: return "no_steps";
    case ParseCode::TooManySteps: return "too_many_steps";
    case ParseCode::BadAction: return "bad_action";
  }
  return "unknown";
}

ParseCode parseLine(const char* begin, const char* end, Step& step, bool& blank) {
  step = Step{};
  blank = false;
  const char* p = begin;
  while (p < end && isSpace(*p)) ++p;
  if (p == end || *p == '#') {
    blank = true;
    return ParseCode::Ok;
  }
  const char* tokenEnd = p;
  while (tokenEnd < end && !isSpace(*tokenEnd)) ++tokenEnd;
  ParseCode code = copyToken(p, tokenEnd, step.id, sizeof(step.id), false);
  if (code == ParseCode::BadNumber) code = ParseCode::BadStepId;
  if (code != ParseCode::Ok) return code;
  step.idHash = id(step.id);

  bool haveGuard = false;
  p = tokenEnd;
  while (true) {
    while (p < end && isSpace(*p)) ++p;
    if (p == end || *p == '#') break;
    tokenEnd = p;
    while (tokenEnd < end && !isSpace(*tokenEnd)) ++tokenEnd;
    const char* equals = p;
    while (equals < tokenEnd && *equals != '=') ++equals;
    const size_t nameLength = static_cast<size_t>(equals - p);
    const char* value = equals < tokenEnd ? equals + 1 : tokenEnd;
    const auto is = [&](const char* name) { return strlen(name) == nameLength && strncmp(name, p, nameLength) == 0; };
    char buffer[Step::TOKEN_MAX];
    uint32_t number = 0;
    if (equals == tokenEnd) {
      if (is("setup")) step.flags |= FlagSetup;
      else if (is("soft")) step.flags |= FlagSoft;
      else if (is("external")) step.flags |= FlagExternal;
      else if (is("optional")) step.flags |= FlagOptional;
      else if (is("blind")) step.flags |= FlagBlind;
      else if (is("sync")) step.flags |= FlagSync;
      else return ParseCode::UnknownField;
    } else if (is("guard")) {
      code = copyToken(value, tokenEnd, buffer, sizeof(buffer), true);
      if (code != ParseCode::Ok) return code == ParseCode::BadNumber ? ParseCode::MissingGuard : code;
      haveGuard = true;
      step.anyGuard = strcmp(buffer, "any") == 0;
      step.guardId = id(buffer);
    } else if (is("expect")) {
      code = copyToken(value, tokenEnd, buffer, sizeof(buffer), true);
      if (code != ParseCode::Ok) return code;
      step.hasExpect = true;
      step.expectId = id(buffer);
    } else if (is("needs")) {
      code = copyToken(value, tokenEnd, buffer, sizeof(buffer), true);
      if (code != ParseCode::Ok) return code;
      step.hasNeeds = true;
      step.needsId = id(buffer);
    } else if (is("key")) {
      uint32_t count = 1;
      code = parsePress(value, tokenEnd, false, step.key, count);
      if (code != ParseCode::Ok) return code;
      step.hasKey = true;
    } else if (is("lead")) {
      const char* part = value;
      while (part < tokenEnd) {
        const char* partEnd = part;
        while (partEnd < tokenEnd && *partEnd != ',') ++partEnd;
        Press press;
        uint32_t count = 1;
        code = parsePress(part, partEnd, true, press, count);
        if (code != ParseCode::Ok) return code;
        if (step.leadCount + count > Step::LEAD_MAX) return ParseCode::TooManyLead;
        for (uint32_t i = 0; i < count; ++i) step.lead[step.leadCount++] = press;
        part = partEnd < tokenEnd ? partEnd + 1 : partEnd;
      }
    } else if (is("x")) {
      if (!parseNumber(value, tokenEnd, 255, number) || number == 0) return ParseCode::BadNumber;
      step.reps = static_cast<uint8_t>(number);
    } else if (is("quiet")) {
      if (!parseNumber(value, tokenEnd, 60000, number) || number == 0) return ParseCode::BadNumber;
      step.quietMs = static_cast<uint16_t>(number);
    } else if (is("norefresh")) {
      if (!parseNumber(value, tokenEnd, 60000, number) || number == 0) return ParseCode::BadNumber;
      step.noRefreshMs = static_cast<uint16_t>(number);
    } else if (is("timeout")) {
      if (!parseNumber(value, tokenEnd, 3600000, number) || number == 0) return ParseCode::BadNumber;
      step.timeoutMs = number;
    } else if (is("do")) {
      code = parseAction(value, tokenEnd, step);
      if (code != ParseCode::Ok) return code;
    } else {
      return ParseCode::UnknownField;
    }
    p = tokenEnd;
  }
  if (!haveGuard) return ParseCode::MissingGuard;
  const bool external = (step.flags & FlagExternal) != 0;
  if (!external && !step.hasKey && step.leadCount == 0 && step.action == ActionVerb::None) return ParseCode::NoAction;
  if (external && (step.hasKey || step.leadCount || step.action != ActionVerb::None)) return ParseCode::NoAction;
  if (step.hasKey && step.action != ActionVerb::None) return ParseCode::BadAction;
  // A real Reader sync attempt is one key press, never a repetition or a software action.
  if ((step.flags & FlagSync) && (!step.hasKey || step.reps != 1)) return ParseCode::BadAction;
  return ParseCode::Ok;
}

ParseError validate(const char* script, size_t& stepCount) {
  stepCount = 0;
  ParseError error;
  if (!script) {
    error.code = ParseCode::NoSteps;
    return error;
  }
  // Duplicate detection without allocation: compare against every earlier line. Scripts are short.
  uint32_t lineNumber = 0;
  for (const char* line = script; *line; ) {
    const char* end = lineEnd(line);
    ++lineNumber;
    Step step;
    bool blank = false;
    const ParseCode code = parseLine(line, end, step, blank);
    if (code != ParseCode::Ok) {
      error.code = code;
      error.line = lineNumber;
      return error;
    }
    if (!blank) {
      if (stepCount >= MAX_STEPS) {
        error.code = ParseCode::TooManySteps;
        error.line = lineNumber;
        return error;
      }
      for (const char* earlier = script; earlier < line;) {
        const char* earlierEnd = lineEnd(earlier);
        const char* first = earlier;
        while (first < earlierEnd && isSpace(*first)) ++first;
        const size_t idLength = strlen(step.id);
        // Only a line that starts with the same id token is worth parsing in full.
        const bool sameToken = first + idLength <= earlierEnd && strncmp(first, step.id, idLength) == 0 &&
                               (first + idLength == earlierEnd || isSpace(first[idLength]));
        Step other;
        bool otherBlank = false;
        if (sameToken && parseLine(earlier, earlierEnd, other, otherBlank) == ParseCode::Ok && !otherBlank &&
            strcmp(other.id, step.id) == 0 && !(other.hasNeeds && step.hasNeeds && other.needsId != step.needsId)) {
          error.code = ParseCode::DuplicateId;
          error.line = lineNumber;
          return error;
        }
        earlier = *earlierEnd ? earlierEnd + 1 : earlierEnd;
      }
      ++stepCount;
    }
    line = *end ? end + 1 : end;
  }
  if (stepCount == 0) error.code = ParseCode::NoSteps;
  return error;
}

void RefreshProbe::arm(const uint64_t atUs) {
  current = RefreshSnapshot{};
  armedAt = atUs;
  asyncPending = false;
  asyncMode = 0;
}

void RefreshProbe::count(const RefreshCall call, const uint32_t mode, const uint64_t startUs, const uint64_t endUs) {
  // A refresh that began before the press cannot have been caused by it.
  if (startUs < armedAt) return;
  if (current.count == 0) {
    current.firstStartUs = startUs;
    current.firstCall = call;
    current.firstMode = static_cast<uint8_t>(mode);
  }
  ++current.count;
  current.lastEndUs = endUs;
  current.lastCall = call;
  current.lastMode = static_cast<uint8_t>(mode);
}

void RefreshProbe::onDisplayCall(const uint8_t call, const uint32_t mode, const uint64_t startUs,
                                 const uint64_t endUs) {
  switch (static_cast<DisplayCall>(call)) {
    case DisplayCall::Buffer: count(RefreshCall::Buffer, mode, startUs, endUs); break;
    case DisplayCall::Refresh: count(RefreshCall::Refresh, mode, startUs, endUs); break;
    case DisplayCall::AsyncSubmit:
      // Submission alone is not visible; its completion is the refresh. Remember when it began.
      asyncPending = startUs >= armedAt;
      asyncMode = static_cast<uint8_t>(mode);
      asyncStartUs = startUs;
      break;
    case DisplayCall::AsyncWait:
      // A wait with nothing submitted since arming is not a refresh.
      if (asyncPending) count(RefreshCall::AsyncWait, asyncMode, asyncStartUs, endUs);
      asyncPending = false;
      break;
    case DisplayCall::GrayBase: count(RefreshCall::GrayBase, mode, startUs, endUs); break;
    case DisplayCall::Gray: count(RefreshCall::Gray, mode, startUs, endUs); break;
    case DisplayCall::ConfiguredGrayBase: count(RefreshCall::ConfiguredGrayBase, mode, startUs, endUs); break;
    case DisplayCall::Precondition:
    case DisplayCall::Sleep: break;
  }
}

bool Sequencer::begin(const char* text, const uint64_t nowUs, const FeatureCheck check, void* context) {
  *this = Sequencer{};
  script = text;
  cursor = text;
  features = check;
  featureContext = context;
  startUs = nowUs;
  size_t count = 0;
  const ParseError error = validate(text, count);
  result.error = error;
  result.scriptId = text ? id(text) : 0;
  if (error.code != ParseCode::Ok) {
    result.outcome = Outcome::BadScript;
    state = State::Done;
    return false;
  }
  state = State::LoadStep;
  return true;
}

bool Sequencer::loadStep() {
  while (*cursor) {
    const char* end = lineEnd(cursor);
    bool blank = false;
    const ParseCode code = parseLine(cursor, end, step, blank);
    cursor = *end ? end + 1 : end;
    if (code != ParseCode::Ok) return false;  // validate() already accepted the text; unreachable
    if (blank) continue;
    ++stepIndex;
    return true;
  }
  return false;
}

bool Sequencer::settled(const Observation& observation, const Step& current, const uint64_t sinceUs) const {
  if (observation.busy) return false;
  if ((current.flags & FlagSync) && observation.syncPending) return false;  // the real attempt has not ended
  const RefreshSnapshot& refresh = observation.refresh;
  if (refresh.count > 0) return observation.nowUs >= refresh.lastEndUs + static_cast<uint64_t>(current.quietMs) * 1000;
  return observation.nowUs >= sinceUs + static_cast<uint64_t>(current.noRefreshMs) * 1000;
}

bool Sequencer::finish(const Observation& observation, const Outcome outcome, Command& out) {
  result.outcome = outcome;
  result.elapsedMs = static_cast<uint32_t>((observation.nowUs - startUs) / 1000);
  state = State::Done;
  out = Command{};
  out.kind = Command::Kind::Finish;
  out.summary = result;
  return true;
}

bool Sequencer::skip(const Observation& observation, const StepStatus status, Command& out) {
  StepRecord record;
  record.stepIndex = stepIndex;
  record.idHash = step.idHash;
  record.status = status;
  record.injectUs = record.settledUs = observation.nowUs;
  record.heapMinEver = observation.heapMinEver;
  record.heapMinSampled = observation.heapFree;
  if (status == StepStatus::External) ++result.external;
  else ++result.skipped;
  out = Command{};
  out.kind = Command::Kind::Record;
  out.record = record;
  state = State::LoadStep;
  return true;
}

bool Sequencer::poll(const Observation& observation, Command& out) {
  out = Command{};
  if (state == State::Done) return false;
  if (cancelled) return finish(observation, abortOutcome, out);
  while (true) {
    switch (state) {
      case State::LoadStep: {
        if (!loadStep()) return finish(observation, Outcome::Complete, out);
        ++result.steps;
        leadDone = 0;
        repDone = 0;
        heapMinSampled = std::numeric_limits<uint32_t>::max();
        if (step.flags & FlagExternal) return skip(observation, StepStatus::External, out);
        if (step.hasNeeds && !(features && features(step.needsId, featureContext))) {
          return skip(observation, StepStatus::Skipped, out);
        }
        if (!step.anyGuard && observation.activityId != step.guardId) {
          if (step.flags & FlagOptional) return skip(observation, StepStatus::Unsupported, out);
          // A supported step whose screen is not where it must be: fail, never skip or press blind.
          StepRecord record;
          record.stepIndex = stepIndex;
          record.idHash = step.idHash;
          record.status = StepStatus::Diverged;
          record.injectUs = record.settledUs = observation.nowUs;
          record.heapMinEver = observation.heapMinEver;
          record.heapMinSampled = observation.heapFree;
          ++result.failed;
          state = State::Finishing;
          pendingOutcome = Outcome::Diverged;
          out.kind = Command::Kind::Record;
          out.record = record;
          return true;
        }
        state = State::Lead;
        continue;
      }
      case State::Lead: {
        if (leadDone >= step.leadCount) {
          state = State::Measure;
          continue;
        }
        injectUs = observation.nowUs;
        out.kind = Command::Kind::Inject;
        out.press = step.lead[leadDone];
        out.armAtUs = injectUs;
        state = State::AwaitLead;
        return true;
      }
      case State::Measure: {
        if ((!step.hasKey && step.action == ActionVerb::None) || repDone >= step.reps) {
          state = State::LoadStep;
          continue;
        }
        injectUs = observation.nowUs;
        heapMinSampled = observation.heapFree;
        out.armAtUs = injectUs;
        out.stepFlags = step.flags;
        if (step.action != ActionVerb::None) {
          out.kind = Command::Kind::Act;
          out.action = step.action;
          memcpy(out.arg, step.arg, sizeof(out.arg));
        } else {
          out.kind = Command::Kind::Inject;
          out.press = step.key;
        }
        state = State::AwaitMeasure;
        return true;
      }
      case State::AwaitLead:
      case State::AwaitMeasure: {
        const bool measured = state == State::AwaitMeasure;
        if (observation.heapFree < heapMinSampled) heapMinSampled = observation.heapFree;
        // `expect` governs the step's last press: the measured key, or the final lead of a lead-only step.
        const bool lastPress = measured || (!step.hasKey && step.action == ActionVerb::None && leadDone + 1 == step.leadCount);
        const bool expectMet = !(lastPress && step.hasExpect) || observation.activityId == step.expectId;
        const bool done = expectMet && settled(observation, step, injectUs);
        const bool timedOut = observation.nowUs >= injectUs + static_cast<uint64_t>(step.timeoutMs) * 1000;
        if (!done && !timedOut) return false;

        StepRecord record;
        record.stepIndex = stepIndex;
        record.rep = measured ? repDone : 0;
        record.idHash = step.idHash;
        record.injectUs = injectUs;
        const RefreshSnapshot& refresh = observation.refresh;
        record.refreshCount = refresh.count;
        if (refresh.count) {
          record.firstSubmitUs = refresh.firstStartUs;
          record.settledUs = refresh.lastEndUs;
          record.firstCall = refresh.firstCall;
          record.firstMode = refresh.firstMode;
          record.lastCall = refresh.lastCall;
          record.lastMode = refresh.lastMode;
        } else {
          record.settledUs = observation.nowUs;
        }
        if (record.settledUs < injectUs) record.settledUs = injectUs;
        record.heapMinEver = observation.heapMinEver;
        record.heapMinSampled = heapMinSampled;
        Outcome fatal = Outcome::Complete;
        bool failed = false;
        if (done) {
          record.status = refresh.count ? StepStatus::Ok : StepStatus::NoRefresh;
          if (measured && (step.flags & FlagBlind)) record.status = StepStatus::Unverified;
        } else if (!expectMet) {
          record.status = StepStatus::Diverged;
          failed = true;
          if (!(step.flags & FlagSoft)) fatal = Outcome::Diverged;
        } else {
          record.status = StepStatus::Timeout;
          failed = true;
          fatal = Outcome::Timeout;
        }
        const bool emit = failed || (measured && !(step.flags & FlagSetup));  // lead/setup rows only to report failure
        if (measured) {
          ++result.measured;
          if (record.status == StepStatus::Ok) ++result.ok;
          else if (record.status == StepStatus::NoRefresh) ++result.noRefresh;
          else if (record.status == StepStatus::Unverified) ++result.unverified;
          else if (failed) ++result.failed;
        } else if (failed) {
          ++result.failed;
        }
        if (measured) ++repDone;
        else ++leadDone;
        state = fatal == Outcome::Complete ? (measured ? State::Measure : State::Lead) : State::Finishing;
        pendingOutcome = fatal;
        if (emit) {
          out.kind = Command::Kind::Record;
          out.stepFlags = step.flags;
          out.record = record;
          return true;
        }
        continue;
      }
      case State::Finishing: return finish(observation, pendingOutcome, out);
      case State::Idle:
      case State::Done: return false;
    }
  }
}

Event encodeStep(const StepRecord& record) {
  Event event;
  event.type = EventType::NavStep;
  event.startUs = record.injectUs;
  event.endUs = record.settledUs < record.injectUs ? record.injectUs : record.settledUs;
  const uint64_t firstDelta =
      record.refreshCount && record.firstSubmitUs > record.injectUs ? record.firstSubmitUs - record.injectUs : 0;
  event.data[0] = record.stepIndex;
  event.data[1] = record.rep;
  event.data[2] = record.idHash;
  event.data[3] = firstDelta > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(firstDelta);
  event.data[4] = static_cast<uint32_t>(record.firstCall) | (static_cast<uint32_t>(record.firstMode) << 8) |
                  (static_cast<uint32_t>(record.lastCall) << 16) | (static_cast<uint32_t>(record.lastMode) << 24);
  event.data[5] = (record.refreshCount > 0xFFFF ? 0xFFFFu : record.refreshCount) |
                  (static_cast<uint32_t>(record.status) << 16);
  event.data[6] = record.heapMinEver;
  event.data[7] = record.heapMinSampled;
  return event;
}

Event encodeResult(const Summary& summary, const uint64_t nowUs) {
  Event event;
  event.type = EventType::NavResult;
  event.startUs = event.endUs = nowUs;
  const auto clamp16 = [](const uint32_t value) { return value > 0xFFFFu ? 0xFFFFu : value; };
  event.data[0] = static_cast<uint32_t>(summary.outcome);
  event.data[1] = summary.steps;
  event.data[2] = summary.measured;
  event.data[3] = clamp16(summary.ok) | (clamp16(summary.noRefresh) << 16);
  if (summary.outcome == Outcome::BadScript) {
    event.data[4] = summary.error.line;
    event.data[5] = static_cast<uint32_t>(summary.error.code);
  } else {
    event.data[4] = (summary.skipped & 0xFFFFu) | ((summary.external & 0xFFFFu) << 16);
    event.data[5] = clamp16(summary.failed) | (clamp16(summary.unverified) << 16);
  }
  event.data[6] = summary.elapsedMs;
  event.data[7] = summary.scriptId;
  return event;
}

Event encodeInput(const bool pressed, const bool released, const bool touch, const bool injected,
                  const uint32_t activityId, const uint64_t nowUs) {
  Event event;
  event.type = EventType::NavInput;
  event.startUs = event.endUs = nowUs;
  event.data[0] = pressed ? 1 : 0;
  event.data[1] = released ? 1 : 0;
  event.data[2] = touch ? 1 : 0;
  event.data[3] = injected ? 1 : 0;
  event.data[4] = activityId;
  return event;
}

}  // namespace benchmark::nav
