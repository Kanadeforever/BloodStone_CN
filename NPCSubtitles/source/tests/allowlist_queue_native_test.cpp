// 本测试直接执行实际白名单与队列代码，不启动游戏，也不依赖 Windows API。
// 验证关键行为：字节级身份匹配、过场清队但保留白名单、环形等待队列顺序、过场结束清空。
#include "../src/cutscene_allowlist.h"
#include "../src/subtitle_queue.h"
#include <cassert>

static bool Kept(unsigned hash){ return cutscene_allowlist::IsHashAllowed(hash); }
static void CheckAllowlist(){
 assert(cutscene_allowlist::kCount==4);
 for(unsigned i=0;i<cutscene_allowlist::kCount;i++){
  const auto& e=cutscene_allowlist::kEntries[i];
  assert(Kept(e.hash));
  assert(cutscene_allowlist::IsExactEntryAllowed(e.hash,e.key));
  for(unsigned j=0;j<cutscene_allowlist::kCount;j++){
   if(i!=j)assert(!cutscene_allowlist::IsExactEntryAllowed(e.hash,cutscene_allowlist::kEntries[j].key));
  }
 }
 assert(!Kept(0xCAC03B4Fu));
 assert(!Kept(0xCCE93E4Fu));
 // 不再恢复水坝 Bond 过场台词或摩纳哥妮可过场台词。
 assert(!Kept(0xE3146C1Au));
 assert(!Kept(0x8B43BA09u));
 assert(!Kept(0x74028F57u));
 // 水坝拉克的两句剧情对白同样不再享有过场放行资格。
 assert(!Kept(0x0713CDE3u));
 assert(!Kept(0x34584E74u));
 assert(!cutscene_allowlist::IsExactEntryAllowed(0x0713CDE3u,"Dam_Works_2_01_RAK_11"));
 assert(!cutscene_allowlist::IsExactEntryAllowed(0x34584E74u,"Dam_Works_2_01_RAK_35"));
 assert(!cutscene_allowlist::IsExactEntryAllowed(0xCCE93E4Fu,"Monaco_Driving_01_NICOLE_CS_68"));
 assert(!cutscene_allowlist::IsExactEntryAllowed(0xCCE93E4Fu,nullptr));
}
static void CheckCutsceneTransition(){
 subtitle_queue::Queue q{};
 const unsigned a=cutscene_allowlist::kEntries[0].hash;
 const unsigned b=cutscene_allowlist::kEntries[1].hash;
 const unsigned c=cutscene_allowlist::kEntries[2].hash;
 assert(subtitle_queue::Push(q,0x111u,L"普通台词一",1000,2500)==subtitle_queue::PushResult::Visible);
 assert(subtitle_queue::Push(q,a,L"白名单甲",1001,2500)==subtitle_queue::PushResult::Visible);
 assert(subtitle_queue::Push(q,0x222u,L"普通台词二",1002,2500)==subtitle_queue::PushResult::Visible);
 assert(subtitle_queue::Push(q,b,L"白名单乙",1003,2500)==subtitle_queue::PushResult::Waiting);
 assert(subtitle_queue::Push(q,0x333u,L"普通台词三",1004,2500)==subtitle_queue::PushResult::Waiting);
 assert(subtitle_queue::Push(q,c,L"白名单丙",1005,2500)==subtitle_queue::PushResult::Waiting);
 // 过场开始：普通对白立即丢弃，而等待中的白名单按原先顺序保留。
 subtitle_queue::Retain(q,Kept);
 assert(q.visible_count==1 && q.visible[0].hash==a);
 assert(q.waiting_count==2);
 assert(q.waiting[q.waiting_head].hash==b);
 assert(q.waiting[(q.waiting_head+1)%subtitle_queue::kWaiting].hash==c);
 subtitle_queue::Advance(q,1006);
 assert(q.visible_count==3);
 assert(q.visible[0].hash==a && q.visible[1].hash==b && q.visible[2].hash==c);
 // 进入下一场过场之前先清队：旧白名单不能出现在新场景里。
 subtitle_queue::Clear(q);
 assert(!q.visible_count && !q.waiting_count && !q.has_last);
 assert(subtitle_queue::Push(q,a,L"同一条可以再次出现",2000,2400)==subtitle_queue::PushResult::Visible);
}
static void CheckWrappedRing(){
 subtitle_queue::Queue q{};
 q.visible_count=3;
 for(unsigned i=0;i<3;i++)q.visible[i]=subtitle_queue::MakeItem(0x100+i,L"普通",1000);
 q.waiting_head=14;
 q.waiting_count=5;
 const unsigned expected[5]={0x001u,0x9CF9A69Bu,0x002u,0x8350582Au,0x4C746585u};
 for(unsigned i=0;i<5;i++)q.waiting[(q.waiting_head+i)%subtitle_queue::kWaiting]=subtitle_queue::MakeItem(expected[i],L"顺序",1000);
 subtitle_queue::Retain(q,Kept);
 assert(!q.visible_count && q.waiting_count==3);
 assert(q.waiting[q.waiting_head].hash==0x9CF9A69Bu);
 assert(q.waiting[(q.waiting_head+1)%subtitle_queue::kWaiting].hash==0x8350582Au);
 assert(q.waiting[(q.waiting_head+2)%subtitle_queue::kWaiting].hash==0x4C746585u);
 subtitle_queue::Advance(q,500);
 assert(q.visible_count==3 && !q.waiting_count);
 assert(q.visible[0].hash==0x9CF9A69Bu && q.visible[1].hash==0x8350582Au && q.visible[2].hash==0x4C746585u);
}
int main(){CheckAllowlist();CheckCutsceneTransition();CheckWrappedRing();return 0;}
