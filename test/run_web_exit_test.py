#!/usr/bin/env python3
"""Compile the production web activity loop and simulate input timing."""
import os
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
source = (root / "src/activities/network/CrossPointWebServerActivity.cpp").read_text(encoding="utf-8")
body = source[source.index("void CrossPointWebServerActivity::loop() {"):source.index("void CrossPointWebServerActivity::render(")]
cpp = r'''#include <cassert>
#include <functional>
#include <memory>
#define LOG_DBG(...) ((void)0)
#define LOG_INF(...) ((void)0)
unsigned now=100, requestMs=1;
unsigned long millis(){return now;}
void yield(){}
void resetTaskWatchdogIfSubscribed(){}
using wl_status_t=int;
constexpr int WL_CONNECTED=1;
struct {int status(){return WL_CONNECTED;}int RSSI(){return -50;}} WiFi;
struct Dns {void processNextRequest(){}} dns;
Dns* dnsServer=&dns;
struct MappedInputManager {enum class Button {Back};};
struct Input {
 bool edge=false; int updates=0; int pressOnUpdate=-1;
 bool wasPressed(MappedInputManager::Button){return edge;}
 void update(){edge=(++updates==pressOnUpdate);}
};
struct Server {int requests=0;std::function<void()> duringRequest;bool isRunning(){return true;}void handleClient(){++requests;now+=requestMs;if(duringRequest)duringRequest();}};
enum class WebServerActivityState {SERVER_RUNNING,SHUTTING_DOWN};
struct CrossPointWebServerActivity {
 WebServerActivityState state=WebServerActivityState::SERVER_RUNNING;
 bool isApMode=true,exited=false,leaveRequested=false;
 Input mappedInput;
 std::unique_ptr<Server> webServer=std::make_unique<Server>();
 unsigned long lastHandleClientTime=0;
 void onGoHome(){exited=true;}void requestUpdate(){}void loop();
};
''' + body + r'''
int main(){
 {CrossPointWebServerActivity a;a.mappedInput.edge=true;a.loop();assert(a.exited&&a.webServer->requests==0&&a.mappedInput.updates==0);}
 {CrossPointWebServerActivity a;a.mappedInput.pressOnUpdate=1;a.loop();assert(a.exited&&a.webServer->requests==1);}
 {CrossPointWebServerActivity a;a.loop();assert(!a.exited&&a.webServer->requests==20);}
 {CrossPointWebServerActivity a;requestMs=100;a.mappedInput.pressOnUpdate=1;a.loop();assert(a.exited&&a.webServer->requests==1);requestMs=1;}
 {CrossPointWebServerActivity a;a.isApMode=false;a.mappedInput.edge=true;a.loop();assert(a.exited&&a.webServer->requests==0);}
 {CrossPointWebServerActivity a;a.webServer->duringRequest=[&]{a.leaveRequested=true;};a.loop();assert(a.exited&&a.webServer->requests==1&&a.mappedInput.updates==0);}
 {CrossPointWebServerActivity a;a.leaveRequested=true;a.loop();assert(a.exited&&a.webServer->requests==0);}
}
'''
with tempfile.TemporaryDirectory(prefix="yomuka-web-exit-") as directory:
    path=Path(directory); (path / "test.cpp").write_text(cpp, encoding="utf-8")
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror", str(path / "test.cpp"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
print("PASS: entry edge preserved, in-batch press, bounded batch, slow request, STA exit and upload cancel latch")
