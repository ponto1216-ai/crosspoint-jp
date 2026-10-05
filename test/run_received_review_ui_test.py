"""Execute the actual received-review input loop with deterministic buttons and scan boundary."""
import argparse,subprocess,tempfile
from pathlib import Path
root=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser();p.add_argument('--compiler',default='g++');p.add_argument('--zig',action='store_true');args=p.parse_args()
s=(root/'src/activities/home/ReceivedReviewActivity.cpp').read_text(encoding='utf-8')
loop=s[s.index('void ReceivedReviewActivity::loop()'):s.index('void ReceivedReviewActivity::render(')]
cpp=r'''
#include <algorithm>
#include <functional>
#include <iostream>
#include <vector>
#include <string>
#define CHECK(x) do {if(!(x)){std::cerr<<__LINE__<<": "<<#x<<"\n";return 1;}}while(0)
struct MappedInputManager {
 enum class Button {Back,Confirm,Left,Right,None};
 Button held=Button::None,pressed=Button::None,released=Button::None;
 bool isPressed(Button b){return held==b;} bool wasPressed(Button b){return pressed==b;} bool wasReleased(Button b){return released==b;} void update(){}
};
using B=MappedInputManager::Button;
struct RenderLock{~RenderLock(){}};struct HalPowerManager{struct Lock{~Lock(){}};};unsigned long millis(){return 2000;}
struct Scanner {
 bool complete=false,cancelled=false;int steps=0;std::vector<int> entries{0,1,2};
 bool done(){return complete;} const auto& books(){return entries;}
 template<class F>void step(F f){++steps;cancelled=f();if(cancelled)complete=true;}
};
struct Navigator {
 MappedInputManager& input;
 template<class F>void onNextRelease(F f){if(input.wasReleased(B::Right))f();}
 template<class F>void onPreviousRelease(F f){if(input.wasReleased(B::Left))f();}
};
struct ButtonNavigator {
 static size_t nextIndex(size_t i,size_t n){return (i+1)%n;}
 static size_t previousIndex(size_t i,size_t n){return (i+n-1)%n;}
};
struct ReceivedReviewActivity {
 MappedInputManager& mappedInput;Scanner scanner;Navigator navigator;
 size_t cursor=0,detailCursor=0;unsigned long lastUpdate=0;
 bool detail=false,ignoreConfirm=true,ignoreBack=true,finished=false;
 explicit ReceivedReviewActivity(MappedInputManager& i):mappedInput(i),navigator{i}{}
 void finish(){finished=true;}void requestUpdate(){}
 std::vector<std::string> details(int){return {"title","id","position","notice"};}
 void loop();
};
'''+loop+r'''
int main(){
 MappedInputManager i;ReceivedReviewActivity opening(i);
 i.held=B::Back;opening.loop();CHECK(!opening.scanner.cancelled && !opening.finished);
 i.held=B::None;i.released=B::Back;opening.loop();CHECK(!opening.finished && !opening.scanner.cancelled);
 i.released=B::None;opening.loop();i.pressed=B::Back;i.held=B::Back;opening.loop();CHECK(opening.scanner.cancelled);
 i.pressed=B::None;i.held=B::None;i.released=B::Back;opening.loop();CHECK(opening.finished);
 i.released=B::None;ReceivedReviewActivity list(i);list.scanner.complete=true;list.loop();
 i.released=B::Right;list.loop();CHECK(list.cursor==1);i.released=B::Confirm;list.loop();CHECK(list.detail);
 i.released=B::Right;list.loop();CHECK(list.detailCursor==1);
 i.released=B::None;i.pressed=B::Back;list.loop();CHECK(list.detail && !list.finished);
 i.pressed=B::None;i.released=B::Back;list.loop();CHECK(!list.detail && !list.finished);
 i.released=B::None;list.loop();i.released=B::Back;list.loop();CHECK(list.finished);
 MappedInputManager j;ReceivedReviewActivity empty(j);empty.scanner.complete=true;empty.scanner.entries.clear();empty.loop();j.released=B::Confirm;empty.loop();CHECK(!empty.detail);
 std::cout<<"PASS: opening Back release ignored, scanning cancel, list navigation, detail, one Back returns once, empty inbox\n";
}
'''
with tempfile.TemporaryDirectory(prefix='yomuka-review-ui-') as d:
 path=Path(d);source=path/'test.cpp';source.write_text(cpp);exe=path/'test.exe'
 subprocess.run([args.compiler]+(['c++'] if args.zig else [])+['-std=c++20','-O2','-Wall','-Wextra','-Werror',str(source),'-o',str(exe)],check=True,timeout=180)
 subprocess.run([str(exe)],check=True,timeout=30)
