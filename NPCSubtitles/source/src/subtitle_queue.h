#pragma once
// 多人对白调度器：把“当前可显示的三条对白”和“等待中的对白”分开存放。
// 所有函数只操作调用者传入的内存，不访问游戏地址，也不调用操作系统 API。
// 这样可以在普通电脑上独立测试队列顺序、过期时间及 32 位时钟回绕。
namespace subtitle_queue {
constexpr unsigned kMaxText = 384;
constexpr unsigned kVisible = 3;
constexpr unsigned kWaiting = 16;
constexpr unsigned kDuplicateMs = 800;

// 每条对白都有独立的哈希、正文、显示起点和持续时间。
// 等待中的对白不开始计时，只有真正进入显示槽位后才开始计时。
struct Item {
 unsigned hash = 0;
 unsigned started_ms = 0;
 unsigned duration_ms = 0;
 wchar_t text[kMaxText] = {};
};
struct Queue {
 Item visible[kVisible] = {};
 Item waiting[kWaiting] = {};
 unsigned visible_count = 0;
 unsigned waiting_count = 0;
 unsigned waiting_head = 0;
 unsigned last_hash = 0;
 unsigned last_seen_ms = 0;
 bool has_last = false;
};
enum class PushResult { Visible, Waiting, Duplicate, Full };
// 过场开始或场景对白接管时，立即移除所有可见与等待中的旧对白。
// 队列使用固定内存，清空的是逻辑内容；不重新分配内存，也不会留下排队消息。
inline void Clear(Queue&q){
 for(unsigned i=0;i<kVisible;i++)q.visible[i].text[0]=0;
 for(unsigned i=0;i<kWaiting;i++)q.waiting[i].text[0]=0;
 q.visible_count=0;
 q.waiting_count=0;
 q.waiting_head=0;
 q.last_hash=0;
 q.last_seen_ms=0;
 q.has_last=false;
}


// 保留符合条件的对白，清掉其他可见和等待项；用于过场切换时保护明确放行的台词。
// FIFO 等待队列采用环形数组，逐项从队头取出后在队尾重新放回。
// 每次循环都会先释放一个槽位，因此重新追加不会覆盖尚未处理的消息。
// 不重新分配内存，也不改变保留消息的原有显示时间与先后顺序。
template<class Keep>
inline void Retain(Queue&q,Keep keep){
 unsigned visible_write=0;
 for(unsigned i=0;i<q.visible_count;i++){
  if(keep(q.visible[i].hash)){
   if(visible_write!=i)q.visible[visible_write]=q.visible[i];
   visible_write++;
  }
 }
 for(unsigned i=visible_write;i<kVisible;i++)q.visible[i].text[0]=0;
 q.visible_count=visible_write;
 const unsigned original_waiting=q.waiting_count;
 for(unsigned i=0;i<original_waiting;i++){
  Item item=q.waiting[q.waiting_head];
  q.waiting[q.waiting_head].text[0]=0;
  q.waiting_head=(q.waiting_head+1)%kWaiting;
  q.waiting_count--;
  if(keep(item.hash)){
   const unsigned tail=(q.waiting_head+q.waiting_count)%kWaiting;
   q.waiting[tail]=item;
   q.waiting_count++;
  }
 }
 if(!keep(q.last_hash)){
  q.has_last=false;
  q.last_hash=0;
  q.last_seen_ms=0;
 }
}

// 用显式的有界复制，保证异常长的 LOC 文本不会溢出固定缓冲区。
inline Item MakeItem(unsigned hash,const wchar_t*text,unsigned duration_ms){
 Item item = {};
 item.hash=hash;
 item.duration_ms=duration_ms;
 for(unsigned i=0;text&&i+1<kMaxText&&text[i];i++)item.text[i]=text[i];
 return item;
}
// 使用有符号的时间差判断是否到期，避免 GetTickCount 在约 49.7 天后回绕造成错误。
inline bool Expired(unsigned now,unsigned deadline){
 return static_cast<int>(now-deadline)>=0;
}
// 每帧更新时先删除过期对白，再按先进先出顺序补上等待中的对白。
// 被补入的对白从本次更新时刻开始计时，不会在等待期间提前消失。
inline void Advance(Queue&q,unsigned now){
 unsigned write=0;
 for(unsigned i=0;i<q.visible_count;i++){
  Item&item=q.visible[i];
  if(!Expired(now,item.started_ms+item.duration_ms)){
   if(write!=i)q.visible[write]=item;
   write++;
  }
 }
 q.visible_count=write;
 while(q.visible_count<kVisible&&q.waiting_count){
  Item item=q.waiting[q.waiting_head];
  q.waiting_head=(q.waiting_head+1)%kWaiting;
  q.waiting_count--;
  item.started_ms=now;
  q.visible[q.visible_count++]=item;
 }
}
// 画面总共只有三行。先给每条正在显示的对白保留一行，
// 再把剩余行分给有换行需求的对白；因此后来的消息不会被长句挤出画面。
inline void AllocateLineBudget(unsigned active,const unsigned requested[kVisible],unsigned budget[kVisible]){
 for(unsigned i=0;i<kVisible;i++)budget[i]=(i<active)?1u:0u;
 if(active>kVisible)active=kVisible;
 unsigned spare=kVisible-active;
 while(spare){
  bool assigned=false;
  for(unsigned i=0;i<active;i++)if(budget[i]<requested[i]){
   budget[i]++;
   spare--;
   assigned=true;
   break;
  }
  if(!assigned)break;
 }
}
// 每条新对白占用自己的显示槽位；三个槽位都占满时进入等待队列。
// 固定容量用来限制极端连续喊话造成的内存开销；满时返回 Full，不覆盖已显示内容。
inline PushResult Push(Queue&q,unsigned hash,const wchar_t*text,unsigned now,unsigned duration_ms){
 Advance(q,now);
 if(q.has_last&&q.last_hash==hash&&now-q.last_seen_ms<kDuplicateMs)
  return PushResult::Duplicate;
 if(q.visible_count==kVisible&&q.waiting_count==kWaiting)
  return PushResult::Full;
 Item item=MakeItem(hash,text,duration_ms);
 q.has_last=true;
 q.last_hash=hash;
 q.last_seen_ms=now;
 if(q.visible_count<kVisible){
  item.started_ms=now;
  q.visible[q.visible_count++]=item;
  return PushResult::Visible;
 }
 unsigned tail=(q.waiting_head+q.waiting_count)%kWaiting;
 q.waiting[tail]=item;
 q.waiting_count++;
 return PushResult::Waiting;
}
} // namespace subtitle_queue
