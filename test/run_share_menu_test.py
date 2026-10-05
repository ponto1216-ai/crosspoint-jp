"""Check actual chooser/candidate loops: three network routes, fourth sharing route, and back navigation."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--compiler', default='g++')
parser.add_argument('--zig', action='store_true')
args = parser.parse_args()
menu_source = (root / 'src/activities/network/NetworkModeSelectionActivity.cpp').read_text(encoding='utf-8')
menu_header = (root / 'src/activities/network/NetworkModeSelectionActivity.h').read_text(encoding='utf-8')
candidate_source = (root / 'src/activities/home/ShareCandidatesActivity.cpp').read_text(encoding='utf-8')
menu_loop = menu_source[menu_source.index('void NetworkModeSelectionActivity::loop()'):
                        menu_source.index('void NetworkModeSelectionActivity::render(')]
candidate_loop = candidate_source[candidate_source.index('void ShareCandidatesActivity::loop()'):
                                  candidate_source.index('std::string ShareCandidatesActivity::unitSummary(')]
mode_enum = menu_header[menu_header.index('enum class NetworkMode'):menu_header.index(';') + 1]
menu_count = re.search(r'constexpr int MENU_ITEM_COUNT = \d+;', menu_source).group(0)
cpp = r'''
#include <algorithm>
#include <cassert>
#include <functional>
#include <memory>
#include <vector>
#define LOG_INF(...) ((void)0)
struct MappedInputManager {
 enum class Button {Back,Confirm,Left,Right,None};
 Button pressed=Button::None,released=Button::None,held=Button::None;
 bool wasPressed(Button b){return pressed==b;}
 bool wasReleased(Button b){return released==b;}
 bool isPressed(Button b){return held==b;}
};
using B=MappedInputManager::Button;
struct RenderLock{};
struct HalPowerManager {struct Lock{};};
struct ActivityResult{};
struct Renderer {int getLineHeight(int){return 21;}};
struct UiLayout {bool landscape=false; static UiLayout from(Renderer&){return {};}};
struct UITheme {static int getNumberOfItemsPerPage(Renderer&,bool,bool,bool,bool,int){return 5;}};
constexpr int UI_10_FONT_ID=1;
unsigned long millis(){return 100;}
struct ButtonNavigator {
 MappedInputManager& input;
 static int nextIndex(int i,int n){return (i+1)%n;}
 static int previousIndex(int i,int n){return (i+n-1)%n;}
 static int nextPageIndex(int i,int n,int p){return std::min(i+p,n-1);}
 static int previousPageIndex(int i,int,int p){return std::max(0,i-p);}
 template<class F> void onNext(F f){if(input.wasPressed(B::Right))f();}
 template<class F> void onPrevious(F f){if(input.wasPressed(B::Left))f();}
 template<class F> void onNextRelease(F f){if(input.wasReleased(B::Right))f();}
 template<class F> void onPreviousRelease(F f){if(input.wasReleased(B::Left))f();}
 template<class F> void onNextContinuous(F){}
 template<class F> void onPreviousContinuous(F){}
};
struct Scanner {
 bool complete=true; unsigned steps=0;
 std::vector<int> values={1,2};
 bool done()const{return complete;}
 bool step(){++steps;return complete;}
 const std::vector<int>& books()const{return values;}
};
struct ShareCandidatesActivity {
 Renderer& renderer; MappedInputManager& mappedInput; ButtonNavigator navigator;
 Scanner scanner; size_t selected=0;
 bool detail=false,ignoreOpeningConfirm=true,returnToMenu;
 bool wentHome=false,finished=false; unsigned updates=0;
 unsigned long startedAt=0,lastProgressRender=0;
 ShareCandidatesActivity(Renderer& r,MappedInputManager& i,bool parent=false)
 :renderer(r),mappedInput(i),navigator{i},returnToMenu(parent){}
 void requestUpdate(){++updates;} void onGoHome(){wentHome=true;} void finish(){finished=true;}
 void loop();
};
''' + mode_enum + '\n' + menu_count + r'''
struct NetworkModeSelectionActivity {
 Renderer& renderer; MappedInputManager& mappedInput; ButtonNavigator buttonNavigator;
 int selectedIndex=0,modeSeen=-1; bool cancelled=false;
 std::unique_ptr<ShareCandidatesActivity> child;
 NetworkModeSelectionActivity(Renderer& r,MappedInputManager& i):renderer(r),mappedInput(i),buttonNavigator{i}{}
 void requestUpdate(){} void onCancel(){cancelled=true;}
 void onModeSelected(NetworkMode mode){modeSeen=static_cast<int>(mode);}
 void startActivityForResult(std::unique_ptr<ShareCandidatesActivity> value,std::function<void(const ActivityResult&)>) {
   child=std::move(value);
 }
 void loop();
};
''' + menu_loop + candidate_loop + r'''
int main(){
 Renderer renderer; MappedInputManager input;
 for(int choice=0;choice<3;++choice){
  NetworkModeSelectionActivity menu(renderer,input); menu.selectedIndex=choice;
  input.pressed=B::Confirm; menu.loop();
  assert(menu.modeSeen==choice && !menu.child);
 }
 NetworkModeSelectionActivity menu(renderer,input); menu.selectedIndex=3;
 input.pressed=B::Confirm; input.held=B::Confirm; menu.loop();
 assert(menu.modeSeen==-1 && menu.child && menu.child->returnToMenu);
 auto& child=*menu.child;
 child.loop(); assert(!child.detail);
 input.pressed=B::None; input.held=B::None; input.released=B::Confirm;
 child.loop(); assert(!child.detail); // release of the chooser's opening press
 input.released=B::None; child.loop();
 input.released=B::Confirm; child.loop(); assert(child.detail);
 input.released=B::Back; child.loop(); assert(!child.detail && !child.finished);
 child.loop(); assert(child.finished && !child.wentHome);
 assert(menu.selectedIndex==3); // parent chooser selection remains on sharing
 ShareCandidatesActivity direct(renderer,input); direct.loop(); assert(direct.wentHome && !direct.finished);
 ShareCandidatesActivity searching(renderer,input,true); searching.scanner.complete=false;
 searching.loop(); assert(searching.finished && searching.scanner.steps==0);
 NetworkModeSelectionActivity nav(renderer,input);
 input.released=B::None; input.pressed=B::Right;
 for(int index=1;index<=4;++index){nav.loop();assert(nav.selectedIndex==index%4);}
 input.pressed=B::Back; nav.loop(); assert(nav.cancelled);
}
'''
with tempfile.TemporaryDirectory(prefix='yomuka-share-menu-') as directory:
    path = Path(directory)
    source = path / 'test.cpp'
    source.write_text(cpp, encoding='utf-8')
    binary = path / 'test.exe'
    command = [args.compiler] + (['c++'] if args.zig else [])
    subprocess.run(command + ['-std=c++20', '-Wall', '-Wextra', '-Werror', '-Wno-unused-variable',
                             str(source), '-o', str(binary)], check=True, timeout=180)
    subprocess.run([str(binary)], check=True, timeout=30)
print('PASS: original network choices, sharing child, opening release, back navigation and four-item wrap')
