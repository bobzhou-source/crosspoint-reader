#pragma once

#include <LocalsendServer.h>

#include <memory>

#include "activities/Activity.h"

class LocalsendActivity final : public Activity {
 public:
  explicit LocalsendActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Localsend", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool skipLoopDelay() override { return server && server->isRunning(); }
  bool preventAutoSleep() override { return server && server->isRunning(); }

 private:
  enum class State : uint8_t { Connecting, Listening, Failed, MemoryError };
  State state = State::Connecting;
  std::unique_ptr<LocalsendServer> server;
  LocalsendServer::Status displayStatus;
  char connectedSSID[33] = {};
  char connectedIP[48] = {};
  uint32_t lastProgressRender = 0;
  bool wifiStarted = false;

  void startReceiver();
  void refreshStatus(bool force = false);
};
