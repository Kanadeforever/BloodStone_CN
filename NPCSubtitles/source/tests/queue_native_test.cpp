// 离线队列行为验证：不需要启动游戏，直接调用发行包内的实际队列代码。
// 每一组断言对应一次用户在游戏中可能遇到的“同时多人说话”的情况。
#include "../src/subtitle_queue.h"
#include <cassert>
#include <cwchar>

int main(){
 using namespace subtitle_queue;
 // 三条紧邻触发的对白必须同时存在，且各自按进入屏幕的时刻计时。
 Queue q={};
 assert(Push(q,101,L"第一句",1000,1500)==PushResult::Visible);
 assert(Push(q,102,L"第二句",1100,4000)==PushResult::Visible);
 assert(Push(q,103,L"第三句",1200,4000)==PushResult::Visible);
 assert(q.visible_count==3&&q.visible[0].hash==101&&q.visible[1].hash==102&&q.visible[2].hash==103);
 // 第四句不得抢占或覆盖任何一行；同一个哈希在 800 毫秒内重复只记录一次。
 assert(Push(q,104,L"第四句",1300,1800)==PushResult::Waiting);
 assert(Push(q,104,L"第四句",1400,1800)==PushResult::Duplicate);
 assert(q.visible_count==3&&q.waiting_count==1);
 // 第一条到期后按 FIFO 显示第四条；第二、三条原有期限不应被重置。
 Advance(q,2500);
 assert(q.visible_count==3&&q.waiting_count==0);
 assert(q.visible[0].hash==102&&q.visible[0].started_ms==1100);
 assert(q.visible[1].hash==103&&q.visible[1].started_ms==1200);
 assert(q.visible[2].hash==104&&q.visible[2].started_ms==2500);
 Advance(q,4300);
 assert(q.visible_count==2&&q.visible[0].hash==102&&q.visible[1].hash==103);
 Advance(q,5300);
 assert(q.visible_count==0);
 // 请求行数大于三行时，每条独立消息至少占一行；单句最多占三行。
 unsigned needed[3]={3,2,3},budget[3]={};
 AllocateLineBudget(3,needed,budget);
 assert(budget[0]==1&&budget[1]==1&&budget[2]==1);
 AllocateLineBudget(2,needed,budget);
 assert(budget[0]==2&&budget[1]==1&&budget[2]==0);
 needed[0]=1;needed[1]=3;
 AllocateLineBudget(2,needed,budget);
 assert(budget[0]==1&&budget[1]==2&&budget[2]==0);
 needed[0]=3;
 AllocateLineBudget(1,needed,budget);
 assert(budget[0]==3&&budget[1]==0&&budget[2]==0);
 // 等待队列的容量固定：队列满时拒绝新增，但绝不覆盖之前的内容。
 Queue crowded={};
 for(unsigned i=1;i<=3;i++)assert(Push(crowded,i,L"屏幕对白",i*100,10000)==PushResult::Visible);
 for(unsigned i=0;i<kWaiting;i++)
  assert(Push(crowded,100+i,L"等待对白",500+i*10,1000)==PushResult::Waiting);
 assert(Push(crowded,999,L"额外对白",1000,1000)==PushResult::Full);
 assert(crowded.visible[0].hash==1&&crowded.waiting[crowded.waiting_head].hash==100);
 Advance(crowded,11000);
 assert(crowded.visible_count==3&&crowded.visible[0].hash==100&&crowded.visible[1].hash==101&&crowded.visible[2].hash==102);
 // 过场开始时，即使同时存在三条可见字幕和多条等待字幕也必须完全清空。
 Queue scene={};
 for(unsigned i=0;i<3;i++)assert(Push(scene,50+i,L"过场前对白",1000+i*100,6000)==PushResult::Visible);
 assert(Push(scene,99,L"等待中的旧对白",1500,6000)==PushResult::Waiting);
 Clear(scene);
 assert(scene.visible_count==0&&scene.waiting_count==0&&scene.waiting_head==0);
 assert(!scene.has_last&&scene.visible[0].text[0]==0&&scene.waiting[0].text[0]==0);
 Advance(scene,2500);
 assert(scene.visible_count==0&&scene.waiting_count==0);
 // 过场结束后，字幕可以重新使用原有槽位；同一哈希也不应被旧去重记录阻止。
 assert(Push(scene,50,L"过场后的新对白",3000,1200)==PushResult::Visible);
 assert(scene.visible_count==1&&scene.visible[0].hash==50);
 // GetTickCount 接近 32 位回绕时，跨过 0 仍能正确比较短期有效期。
 Queue rollover={};
 assert(Push(rollover,88,L"跨回绕",0xFFFFFF00u,1000)==PushResult::Visible);
 Advance(rollover,0x00000020u);
 assert(rollover.visible_count==1);
 Advance(rollover,0x000002E9u);
 assert(rollover.visible_count==0);
 // 文本容量必须保留终止符，防止游戏字形 API 扫描出数组边界。
 wchar_t long_text[500]={};
 for(unsigned i=0;i<499;i++)long_text[i]=L'中';
 Item trimmed=MakeItem(7,long_text,1000);
 assert(trimmed.text[kMaxText-1]==0&&trimmed.text[kMaxText-2]==L'中');
 return 0;
}
