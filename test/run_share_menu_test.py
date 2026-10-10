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
#include <cstdint>
#include <vector>
#include <string>
#include <iostream>
#include <variant>
#undef assert
#define assert(condition) do { if (!(condition)) { std::cerr << __LINE__ << ": " << #condition << "\n"; return 1; } } while (0)
#define LOG_INF(...) ((void)0)
struct MappedInputManager {
 enum class Button {Back,Confirm,Left,Right,None};
 Button pressed=Button::None,released=Button::None,held=Button::None;
 bool wasPressed(Button b){return pressed==b;}
 bool wasReleased(Button b){return released==b;}
 bool isPressed(Button b){return held==b;}
 unsigned heldMs=0;
 unsigned getHeldTime(){return heldMs;}
 void update(){}
};
using B=MappedInputManager::Button;
struct RenderLock{};
struct HalPowerManager {struct Lock{};};
struct ReceivedFilesResult { std::vector<std::string> files; };
struct ActivityResult { std::variant<std::monostate,ReceivedFilesResult> data; };
struct Renderer {int getLineHeight(int){return 21;}};
struct UiLayout {bool landscape=false; static UiLayout from(Renderer&){return {};}};
struct UITheme {static int getNumberOfItemsPerPage(Renderer&,bool,bool,bool,bool,int){return 5;}};
constexpr int UI_10_FONT_ID=1;
constexpr unsigned long ProgressRenderIntervalMs=3000;
unsigned long fakeNow=100;
unsigned long millis(){return fakeNow;}
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
struct Candidate {uint64_t bookId=0; bool deletionOnly()const{return false;}};
struct Scanner {
 bool complete=true, needsSearch=false, restarted=false; unsigned steps=0;
 size_t missingBooks=0;
 std::vector<Candidate> values={{1},{2}}, missing;
 std::vector<uint64_t> ignored;
 bool done()const{return complete;}
 bool step(){++steps;return complete;}
 bool canSearchMissing()const{return complete && needsSearch;}
 void restart(bool fullSearch){restarted=fullSearch;complete=false;needsSearch=false;}
 void setExclusions(const std::vector<uint64_t>& ids){ignored=ids;}
 bool isExcluded(uint64_t id)const{return std::find(ignored.begin(),ignored.end(),id)!=ignored.end();}
 const std::vector<Candidate>& books()const{return values;}
 const std::vector<Candidate>& unresolvedCandidates()const{return missing;}
};
int READING_HISTORY=0;
struct Batch {
 std::vector<int> entries; unsigned cursor=0;
 void start(const std::vector<uint8_t>& selection){entries.clear();cursor=0;for(auto value:selection)if(value)entries.push_back(value);}
 void cancel(){cursor=entries.size();}
 bool done(){return cursor==entries.size();}
 void step(const std::vector<Candidate>&,int,const std::function<bool()>& cancel){if(cancel())cursor=entries.size();else ++cursor;}
 const std::vector<int>& results(){return entries;}
};
constexpr int STR_DIRECT_ONE_BOOK=1;
constexpr int STR_SHARE_EXCLUSIONS_SAVE_FAILED=2;
const char* tr(int){return "one book";}
namespace yomuka::sync { using ShareCandidate = Candidate; }
struct ShareSearchExclusions {static bool save(const std::vector<uint64_t>&){return true;}};
struct DirectShareActivity {
 bool receiving; size_t count;
 DirectShareActivity(Renderer&,MappedInputManager&,bool receive,const std::vector<Candidate>& books={}):receiving(receive),count(books.size()){}
};
struct ReceivedReviewActivity { ReceivedReviewActivity(Renderer&,MappedInputManager&,std::vector<std::string> = {}){} };
struct ShareCandidatesActivity {
 Renderer& renderer; MappedInputManager& mappedInput; ButtonNavigator navigator;
 Scanner scanner; size_t selected=0;
 bool detail=false,ignoreOpeningConfirm=true,returnToMenu;
 bool consumeConfirmRelease=false,review=false,generating=false,results=false;
 bool managingMissing=false,fullSearchCompleted=false,exclusionsLoaded=true;
 std::vector<uint64_t> excludedIds;
 bool consumeBackRelease=false;
 std::string actionMessage;
 int directMode=-1; size_t directCount=0; bool reviewOpened=false; std::function<void(const ActivityResult&)> receivedCallback;
 template<class F> void startActivityForResult(std::unique_ptr<DirectShareActivity> child,F callback){directMode=child->receiving?1:0; directCount=child->count;receivedCallback=callback;}
 template<class F> void startActivityForResult(std::unique_ptr<ReceivedReviewActivity>,F){reviewOpened=true;}
 std::vector<uint8_t> selection={0,0}; Batch batch;
 bool wentHome=false,finished=false; unsigned updates=0;
 unsigned long startedAt=0,lastProgressRender=0;
 ShareCandidatesActivity(Renderer& r,MappedInputManager& i,bool parent=false)
 :renderer(r),mappedInput(i),navigator{i},returnToMenu(parent){}
 void requestUpdate(){++updates;} void onGoHome(){wentHome=true;} void finish(){finished=true;}
 std::vector<size_t> managedIndices() const;
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
 child.selected=2; input.released=B::Confirm; child.loop(); assert(!child.review); // no selected books
 child.selected=0;
 input.released=B::Confirm; child.loop(); assert(child.selection[0] && !child.detail);
 child.loop(); assert(!child.selection[0]); child.loop(); assert(child.selection[0]);
 input.released=B::None; input.held=B::Confirm; input.heldMs=700; child.loop(); assert(child.detail);
 input.held=B::None; input.released=B::Confirm; child.loop(); assert(child.selection[0]);
 input.released=B::Back; child.loop(); assert(!child.detail && !child.finished);
 child.selected=2; input.released=B::Confirm; child.loop(); assert(child.review && !child.generating);
 input.released=B::Back; child.loop(); assert(!child.review);
 child.selected=2; input.released=B::Confirm; child.loop(); child.loop(); assert(child.generating);
 input.released=B::Back; child.loop(); assert(child.results && !child.generating);
 child.loop(); assert(!child.results);
 child.selected=2; input.released=B::Confirm; child.loop(); child.loop(); assert(child.generating);
 input.released=B::None; input.held=B::Back; input.heldMs=0; child.loop(); assert(child.results && child.consumeBackRelease);
 child.loop(); assert(child.results && !child.finished);
 input.held=B::None; input.released=B::Back; child.loop(); assert(child.results && !child.consumeBackRelease);
 input.released=B::None; child.loop(); assert(child.results);
 input.released=B::Back; child.loop(); assert(!child.results);
 child.selected=3; input.released=B::Confirm; child.loop(); assert(child.directMode==0 && child.directCount==1);
 child.selection={1,1}; child.loop(); assert(child.directMode==0 && child.directCount==2);
 child.selected=4; child.loop(); assert(child.directMode==1);
 child.receivedCallback(ActivityResult{}); assert(!child.reviewOpened);
 child.receivedCallback(ActivityResult{ReceivedFilesResult{{"/YomukaSync/0000000000000001.json"}}}); assert(child.reviewOpened);
 child.selected=5;child.loop();assert(child.reviewOpened);
 child.selection={0,0}; child.selected=3; child.directMode=-1; child.loop(); assert(child.directMode==-1 && !child.actionMessage.empty());
 child.scanner.values.clear(); child.selection.clear(); child.selected=2; child.loop(); assert(child.directMode==1);
 child.scanner.values={{1},{2}}; child.selection={0,0}; child.scanner.needsSearch=true;
 child.selected=6; input.released=B::Confirm; child.loop();
 assert(child.scanner.restarted && !child.scanner.complete && child.selection.empty() && child.selected==0);
 input.released=B::Back;
 child.loop(); assert(child.finished && !child.wentHome);
 assert(menu.selectedIndex==3); // parent chooser selection remains on sharing
 ShareCandidatesActivity exclusions(renderer,input,true);
 exclusions.ignoreOpeningConfirm=false;
 exclusions.fullSearchCompleted=true;
 exclusions.managingMissing=true;
 exclusions.scanner.missing={{7}};
 exclusions.scanner.missingBooks=1;
 exclusions.selected=0;
 input.released=B::Confirm;
 exclusions.loop();
 assert(exclusions.scanner.isExcluded(7) && exclusions.excludedIds.size()==1);
 input.released=B::Back;
 exclusions.loop();
 assert(!exclusions.managingMissing && !exclusions.finished);
 exclusions.selected=6;
 input.released=B::Confirm;
 exclusions.loop();
 assert(exclusions.managingMissing);
 exclusions.selected=0;
 exclusions.loop();
 assert(!exclusions.scanner.isExcluded(7) && exclusions.excludedIds.empty());
 ShareCandidatesActivity cadence(renderer,input,true);
 cadence.scanner.complete=false;
 cadence.lastProgressRender=100;
 input.released=B::None;
 fakeNow=3099; cadence.loop(); assert(cadence.updates==0);
 fakeNow=3100; cadence.loop(); assert(cadence.updates==1);
 fakeNow=3200; cadence.loop(); assert(cadence.updates==1);
 input.released=B::Back;
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
print('PASS: sharing routes, back navigation, missing-book exclusion and 3-second progress cadence')
