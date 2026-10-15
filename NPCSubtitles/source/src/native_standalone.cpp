// 007 Blood Stone 独立 NPC 中文字幕插件。只调用已确认的原生 Dialogue/字形入口，
// 从 ASI 同目录解析原生中文 LOC，直接复用游戏贴图在 D3D9 上绘制字幕。
// 本源文件不依赖 C++ 标准库与 Windows SDK，保证可在本项目封存的 Clang/LLD 上离线构建。
#include "win32_minimal.h"
#include "subtitle_queue.h"

#ifndef NPC_DIAG_STAGE
#define NPC_DIAG_STAGE 4
#endif
static_assert(NPC_DIAG_STAGE>=1 && NPC_DIAG_STAGE<=4,"诊断阶段必须介于 1 与 4 之间");
extern "C" void* CDECL memset(void* p,int value,unsigned count){U8* b=(U8*)p;for(unsigned i=0;i<count;i++)b[i]=(U8)value;return p;}
extern "C" void* CDECL memcpy(void* d,const void* s,unsigned n){U8* a=(U8*)d;const U8* b=(const U8*)s;for(unsigned i=0;i<n;i++)a[i]=b[i];return d;}
// Win32/x86 编译器在大栈帧函数入口调用 _chkstk；该函数不仅要逐页探测，
// 还必须把 ESP 移到新栈帧。编译器在 CALL 后不会另外执行 SUB ESP, EAX。
// 输入：EAX 为所需的栈帧字节数；输出：ESP 已减少该字节数。
// EAX、ECX、EDX 的旧值均需要恢复，因为编译器可能在调用后继续使用它们。
// 最后将原返回地址和被暂存的寄存器搬到新栈顶，再依次弹出并返回。
extern "C" __declspec(naked) void _chkstk() { __asm {
    push eax
    push ecx
    push edx
    lea ecx, [esp+16]
probe_next_page:
    cmp eax, 4096
    jb probe_last_page
    sub ecx, 4096
    test byte ptr [ecx], 0
    sub eax, 4096
    jmp probe_next_page
probe_last_page:
    sub ecx, eax
    test byte ptr [ecx], 0
    lea edx, [ecx-16]
    test byte ptr [edx], 0
    mov eax, [esp]
    mov [edx], eax
    mov eax, [esp+4]
    mov [edx+4], eax
    mov eax, [esp+8]
    mov [edx+8], eax
    mov eax, [esp+12]
    mov [edx+12], eax
    mov esp, edx
    pop edx
    pop ecx
    pop eax
    ret
} }
extern "C" int _fltused = 0;
namespace {
constexpr U32 IMAGE_BASE=0x00400000u, IMAGE_SIZE=0x010CC000u;
constexpr U32 TIMESTAMP=0x4CA36144u;
constexpr U32 DIALOGUE_VA=0x0093A2A1u, LOOKUP_VA=0x00F12820u;
constexpr U32 FONT_SLOT_VA=0x011A02D8u, RESOURCE_VA=0x00E42C21u;
constexpr U32 MAX_RECORDS=4200u, MAX_TEXT=384u;
constexpr U32 FVF=0x004|0x040|0x100;
void* g_self=nullptr;U8* g_game=nullptr; wchar_t g_folder[1024]={};
Handle g_log=nullptr;U32 g_diagnostics_enabled=0;
U32 g_entries=0,g_files=0,g_accepted=0,g_events=0;
// 统计值只用于故障诊断，不参与游戏流程和字幕决策。
U32 g_diag_queue=0,g_diag_route=0,g_diag_duplicate=0,g_diag_draw=0;
U32 g_diag_waiting=0,g_diag_queue_full=0,g_diag_max_visible=0;
U32 g_diag_draw_fail=0,g_diag_missing_font=0,g_diag_missing_glyph=0;
U32 g_diag_frames=0,g_diag_resets=0,g_diag_last_report=0;
U32 g_diag_last_width=0,g_diag_last_height=0;
U32 g_diag_loc_skipped=0,g_diag_font_capture=0,g_diag_texture_capture=0;
U32 g_diag_dialogue_hook_retry=0,g_diag_font_hook_retry=0;
U32 g_diag_invalid_viewport=0,g_diag_font_metrics_logged=0;
U32 g_diag_native_event_probes=0,g_diag_route_unknown=0;
struct Entry{U32 hash;wchar_t text[MAX_TEXT];U8 allowed,ambiguous;};
Entry* g_entries_table=nullptr;
// 游戏原始对白事件 +0x1C 参与原生 UI 分流：0 表示交给游戏字幕，
// 1 表示不走该原生字幕提交分支。只使用来自实际事件的字段判断，
// 不按对白正文、角色名称或调用栈签名推测字幕归属。
constexpr U32 ROUTE_FLAG_OFFSET=0x1Cu;
// Bond.exe 的原生游戏事件；通过明文 EXE 里的发送点和注册名称双重确认。
constexpr U32 CUTSCENE_START_ID=0xA2193087u, CUTSCENE_END_ID=0xA2193088u;
constexpr U32 GAME_EVENT_SEND_VA=0x00E46E70u, GAME_EVENT_LOCK_VA=0x004E7EE0u;

struct Capture{void* font;U16 handle;U32 variant;float scale;U32 seen;};
Capture g_fonts[16]={};U32 g_font_count=0;
struct TextureSlot{U16 handle;void* texture;};TextureSlot g_textures[24]={};U32 g_texture_count=0;
struct Context{void* font;void* texture;U32 variant;float scale;U32 width,height;};
Context g_selected={};
void* g_orig_glyph=nullptr,*g_orig_resource=nullptr,*g_orig_dialogue=nullptr;
U8* g_dialogue_site=nullptr,*g_resource_site=nullptr;void**g_glyph_slot=nullptr;
I32 g_dialogue_disp=0,g_resource_disp=0;
void* g_prev_create9=nullptr,*g_prev_create_device=nullptr,*g_prev_end_scene=nullptr,*g_prev_reset=nullptr;
void** g_create_slot=nullptr,**g_end_slot=nullptr,**g_reset_slot=nullptr;
void* g_state_block=nullptr,*g_state_device=nullptr;
U32 g_started=0,g_seen_chinese=0,g_logged_missing_font=0;
U32 g_d3d_frame_seen=0,g_glyph_seen=0,g_texture_seen=0,g_draw_seen=0;
U32 g_event_hit_logs=0,g_event_miss_logs=0,g_event_filtered_logs=0,g_event_ambiguous_logs=0;
// 字幕 Hook 可能和 D3D9 帧回调位于不同线程。这里只锁住短暂的消息复制，
// 绘制和查询字形都在锁外运行，不会让渲染线程长时间等待。
subtitle_queue::Queue g_subtitles={};
volatile U32 g_subtitles_lock=0;
// 仅保存是否处于过场阶段。绘制线程只做一次原子读取，不轮询磁盘或游戏对象。
volatile U32 g_cutscene_active=0;
U32 g_cutscene_start_count=0,g_cutscene_end_count=0,g_cutscene_cleared=0;
U32 g_native_route_clear=0;

void LockSubtitles(){
 while(__atomic_exchange_n(&g_subtitles_lock,1u,__ATOMIC_ACQUIRE)!=0u)Sleep(0);
}
void UnlockSubtitles(){__atomic_store_n(&g_subtitles_lock,0u,__ATOMIC_RELEASE);}
// 切换画面前清理可见字幕和 FIFO 等待队列，防止过场开始后继续补出旧对白。
// 仅在事件到达时调用，因此不增加每帧绘制负担。
void ClearSubtitles(){LockSubtitles();subtitle_queue::Clear(g_subtitles);UnlockSubtitles();}


// 事件总线 Hook 采用一次性的 8 字节跳板，原有指令在可执行副本中回放。
// 跳板的地址必须先写好再安装 JMP，才能保证绝大多数事件完全沿用游戏原流程。
void Log(const char* key,U32 value,bool numeric);
extern "C" void* g_cutscene_bus_continue=nullptr;
void* g_cutscene_trampoline=nullptr;
U32 g_cutscene_hook_ready=0;
void OnCutsceneTransition(U32 type){
 if(type==CUTSCENE_START_ID){
  __atomic_store_n(&g_cutscene_active,1u,__ATOMIC_RELEASE);
  ClearSubtitles();
  g_cutscene_start_count++;g_cutscene_cleared++;
  Log("CUTSCENE_START_QUEUE_CLEARED",0,false);
 }else if(type==CUTSCENE_END_ID){
  __atomic_store_n(&g_cutscene_active,0u,__ATOMIC_RELEASE);
  g_cutscene_end_count++;
  Log("CUTSCENE_END_QUEUE_RESUMED",0,false);
 }
}
// 其他游戏事件只进行两个整数比较，然后通过原始跳板继续执行。
// 这里没有修改事件内容、调用约定、返回地址或游戏 UI 函数指针。
extern "C" __declspec(naked) void CutsceneBusThunk(){__asm{
 push eax
 mov eax,[esp+8]
 test eax,eax
 jz pass_event
 mov eax,[eax]
 cmp eax,0xA2193087
 je matched_event
 cmp eax,0xA2193088
 jne pass_event
matched_event:
 pushfd
 pushad
 push eax
 call OnCutsceneTransition
 add esp,4
 popad
 popfd
pass_event:
 pop eax
 jmp dword ptr [g_cutscene_bus_continue]
}}
// 硬规则：只在插件自身目录寻找 BSCNContents，不假定 scripts/plugins 位置。
U32 Len(const char* s){U32 n=0;while(s&&s[n])n++;return n;}
U32 WLen(const wchar_t* s){U32 n=0;while(s&&s[n])n++;return n;}
U8 Lower(U8 v){return v>='A'&&v<='Z'?U8(v+32):v;}
U8 Upper(U8 v){return v>='a'&&v<='z'?U8(v-32):v;}
void Cat(char* dst,const char* src,U32 cap){U32 j=Len(dst);for(U32 i=0;src&&src[i]&&j+1<cap;i++)dst[j++]=src[i];dst[j]=0;}
void CatNum(char*d,U32 n,U32 cap){char t[12]={};U32 p=0;do{t[p++]=char('0'+n%10);n/=10;}while(n&&p<11);char c[12]={};for(U32 j=0;j<p;j++)c[j]=t[p-j-1];Cat(d,c,cap);}
void CatWide(wchar_t* dst,const wchar_t* src,U32 cap){U32 j=WLen(dst);for(U32 i=0;src&&src[i]&&j+1<cap;i++)dst[j++]=src[i];dst[j]=0;}
// Windows 传入的是完整命令行而非 argv，本插件不依赖 CRT，必须独立拆分参数。
// 完整匹配 --log，排除 --logger、路径内的 --log 和引号中的多词参数。
// 反斜杠遇到引号时按 Windows 的双引号参数规则解释。
bool CommandLineHasLogFlag(){
 const wchar_t* p=GetCommandLineW();if(!p)return false;
 U32 argument=0;
 while(*p){
  while(*p==L' '||*p==L'\t'||*p==L'\r'||*p==L'\n')p++;
  if(!*p)break;
  bool quoted=false;wchar_t token[20]={};U32 length=0;
  while(*p){
   if(!quoted&&(*p==L' '||*p==L'\t'||*p==L'\r'||*p==L'\n'))break;
   U32 slashes=0;
   while(*p==L'\\'){slashes++;p++;}
   if(*p==L'"'){
    for(U32 k=0;k<slashes/2;k++)if(length+1<20)token[length++]=L'\\';
    if(slashes&1){if(length+1<20)token[length++]=L'"';p++;}
    else if(quoted&&p[1]==L'"'){if(length+1<20)token[length++]=L'"';p+=2;}
    else{quoted=!quoted;p++;}
    continue;
   }
   for(U32 k=0;k<slashes;k++)if(length+1<20)token[length++]=L'\\';
   if(!*p)break;
   if(length+1<20)token[length++]=*p;
   p++;
  }
  token[length]=0;
  if(argument>0&&length==5&&token[0]==L'-'&&token[1]==L'-'&&
     (token[2]==L'l'||token[2]==L'L')&&
     (token[3]==L'o'||token[3]==L'O')&&
     (token[4]==L'g'||token[4]==L'G'))return true;
  argument++;
 }
 return false;
}
void CatPad(char* dst,U32 num,U32 digits,U32 cap){
 char raw[16]={};U32 count=0;do{raw[count++]=char('0'+num%10);num/=10;}while(num&&count<15);
 while(count<digits&&count<15)raw[count++]='0';
 char result[16]={};for(U32 i=0;i<count;i++)result[i]=raw[count-i-1];Cat(dst,result,cap);
}
void CatHex(char* dst,U32 value,U32 cap){
 const char* digits="0123456789ABCDEF";char code[11]="0x00000000";
 for(U32 i=0;i<8;i++)code[2+i]=digits[(value>>(28-4*i))&15];Cat(dst,code,cap);
}
// Windows 的 wchar_t 是 UTF-16：转换为 UTF-8 写入日志，用户可直接用编辑器阅读。
// 控制字符、换行替换为空格，避免文件名和台词破坏一行一事件的格式。
void CatUtf8(char* dst,const wchar_t* value,U32 cap,U32 max_chars=180){
 if(!value)return;
 U32 used=Len(dst);U32 i=0;
 while(value[i]&&i<max_chars&&used+5<cap){
  U32 u=U16(value[i++]);if(u==L'\r'||u==L'\n'||u==L'\t')u=' ';
  if(u<32)u='?';
  if(u>=0xD800&&u<=0xDBFF&&value[i]>=0xDC00&&value[i]<=0xDFFF){
   u=0x10000+((u-0xD800)<<10)+(U16(value[i++])-0xDC00);
  }else if(u>=0xD800&&u<=0xDFFF)u='?';
  if(u<0x80){dst[used++]=char(u);}
  else if(u<0x800){dst[used++]=char(0xC0|(u>>6));dst[used++]=char(0x80|(u&63));}
  else if(u<0x10000){dst[used++]=char(0xE0|(u>>12));dst[used++]=char(0x80|((u>>6)&63));dst[used++]=char(0x80|(u&63));}
  else{dst[used++]=char(0xF0|(u>>18));dst[used++]=char(0x80|((u>>12)&63));dst[used++]=char(0x80|((u>>6)&63));dst[used++]=char(0x80|(u&63));}
 }
 dst[used]=0;
}
// 只有明确传入 --log 才调用系统日志接口，常规启动不创建、截断或追加任何日志。
// 每条使用一次同步 WriteFile，单行附带本地时间、线程 ID 和进程运行毫秒。
void Log(const char* key,U32 value=0,bool numeric=false){
 if(!g_diagnostics_enabled)return;
 char line[1200]={};SystemTime now={};GetLocalTime(&now);
 Cat(line,"[",sizeof line);CatPad(line,now.year,4,sizeof line);Cat(line,"-",sizeof line);
 CatPad(line,now.month,2,sizeof line);Cat(line,"-",sizeof line);CatPad(line,now.day,2,sizeof line);
 Cat(line," ",sizeof line);CatPad(line,now.hour,2,sizeof line);Cat(line,":",sizeof line);
 CatPad(line,now.minute,2,sizeof line);Cat(line,":",sizeof line);CatPad(line,now.second,2,sizeof line);
 Cat(line,".",sizeof line);CatPad(line,now.milliseconds,3,sizeof line);Cat(line,"] [tid=",sizeof line);
 CatNum(line,GetCurrentThreadId(),sizeof line);Cat(line,"] [ms=",sizeof line);CatNum(line,GetTickCount(),sizeof line);
 Cat(line,"] NPCSubtitles v0.1e-log6 ",sizeof line);Cat(line,key,sizeof line);
 if(numeric){Cat(line,"=",sizeof line);CatNum(line,value,sizeof line);}
 Cat(line,"\r\n",sizeof line);
 OutputDebugStringA(line);
 if(g_log&&g_log!=(Handle)-1){U32 written=0;WriteFile(g_log,line,Len(line),&written,nullptr);}
}
void LogPair(const char* key,U32 a,U32 b){
 if(!g_diagnostics_enabled)return;
 char t[230]={};Cat(t,key,230);Cat(t," first=",230);CatNum(t,a,230);
 Cat(t," second=",230);CatNum(t,b,230);Log(t);
}
void LogHash(const char* label,U32 hash,const wchar_t* preview=nullptr){
 if(!g_diagnostics_enabled)return;
 char line[850]={};Cat(line,label,sizeof line);Cat(line," hash=",sizeof line);
 CatNum(line,hash,sizeof line);Cat(line," (",sizeof line);CatHex(line,hash,sizeof line);Cat(line,")",sizeof line);
 if(preview){Cat(line," text=",sizeof line);CatUtf8(line,preview,sizeof line,160);}
 Log(line);
}
void LogPath(const char* label,const wchar_t* path){
 if(!g_diagnostics_enabled)return;
 char line[1000]={};Cat(line,label,sizeof line);Cat(line," path=",sizeof line);
 CatUtf8(line,path,sizeof line,260);Log(line);
}
void LogFailure(const char* label,U32 error){
 if(!g_diagnostics_enabled)return;
 char line[350]={};Cat(line,label,sizeof line);Cat(line," win32_error=",sizeof line);
 CatNum(line,error,sizeof line);Cat(line," (",sizeof line);CatHex(line,error,sizeof line);Cat(line,")",sizeof line);
 Log(line);
}
bool ShouldLogEvent(U32 count){return g_diagnostics_enabled&&(count<=512||count%128==0);}
void ReportStats(){
 if(!g_diagnostics_enabled)return;
 U32 now=GetTickCount();if(g_diag_last_report&&now-g_diag_last_report<30000)return;
 g_diag_last_report=now;
 LogPair("STATS dialogue_events/indexed",g_events,g_entries);
 LogPair("STATS hits/miss",g_event_hit_logs,g_event_miss_logs);
 LogPair("STATS filtered/ambiguous",g_event_filtered_logs,g_event_ambiguous_logs);
 LogPair("STATS queued/deduplicated",g_diag_queue,g_diag_duplicate);
 LogPair("STATS deferred/full",g_diag_waiting,g_diag_queue_full);
 Log("STATS max_simultaneous_dialogues",g_diag_max_visible,true);
 LogPair("STATS native_events_suppressed/draw_calls",g_diag_route,g_diag_draw);
 LogPair("STATS draw_fail/missing_glyph",g_diag_draw_fail,g_diag_missing_glyph);
 LogPair("STATS render_frames/resets",g_diag_frames,g_diag_resets);
 LogPair("STATS font_captures/texture_captures",g_diag_font_capture,g_diag_texture_capture);
 LogPair("STATS recognized_loc/skipped_loc",g_files,g_diag_loc_skipped);
 LogPair("STATS route_probe_events/unknown",g_diag_native_event_probes,g_diag_route_unknown);
 LogPair("STATS missing_font_wait/invalid_viewport",g_diag_missing_font,g_diag_invalid_viewport);
 LogPair("STATS cutscene_start/end",g_cutscene_start_count,g_cutscene_end_count);
 LogPair("STATS cutscene_clear/native_clear",g_cutscene_cleared,g_native_route_clear);
}
bool Path(wchar_t* out,const wchar_t* suffix){out[0]=0;CatWide(out,g_folder,1024);CatWide(out,L"\\",1024);CatWide(out,suffix,1024);return WLen(out)<1020;}
void* Method(void* obj,U32 slot){if(!obj)return nullptr;return (*(void***)obj)[slot];}
template<class Fn> Fn VFunc(void* obj,U32 slot){return reinterpret_cast<Fn>(Method(obj,slot));}
U32 U32At(const U8*p){return U32(p[0])|(U32(p[1])<<8)|(U32(p[2])<<16)|(U32(p[3])<<24);}
U16 U16At(const U8*p){return U16(p[0]|(U16(p[1])<<8));}
bool SameAscii(const char* a,const char*b){while(*a&&*b){if(Upper(*a)!=Upper(*b))return false;a++;b++;}return *a==*b;}
bool Contains(const char* hay,const char* needle){if(!needle||!*needle)return true;for(U32 i=0;hay[i];i++){U32 j=0;while(needle[j]&&Upper(hay[i+j])==Upper(needle[j]))j++;if(!needle[j])return true;}return false;}
// 仅允许已确认的系统名称过滤，剩余 NPC 事件由运行时语义路由排重。
bool SkipKey(const char* key){const char* blocked[]={"BOND","NICOLE","DANTE","MON_DIA_","_TUTORIAL_","_INFO_","_CS_"};for(U32 i=0;i<7;i++)if(Contains(key,blocked[i]))return true;return false;}
U32 PeRva(U32 static_va){return static_va-IMAGE_BASE;}
U8* Addr(U32 static_va){return g_game+PeRva(static_va);}
bool ReadMemory(const void*p,void*out,U32 bytes){U32 n=0;return p&&out&&ReadProcessMemory(GetCurrentProcess(),p,out,bytes,&n)&&n==bytes;}
bool Patch(void*ptr,const void*bytes,U32 size){U32 old=0;if(!VirtualProtect(ptr,size,0x40,&old))return false;memcpy(ptr,bytes,size);FlushInstructionCache(GetCurrentProcess(),ptr,size);U32 dummy=0;VirtualProtect(ptr,size,old,&dummy);return true;}
bool PatchPointer(void** ptr,void* target,void** previous){if(!ReadMemory(ptr,previous,4)||!*previous)return false;return Patch(ptr,&target,4);}
bool PatchCall(U8*ptr,void* target,void**old,I32*old_disp){U8 code[5]={};if(!ReadMemory(ptr,code,5)||code[0]!=0xE8)return false;*old_disp=(I32)U32At(code+1);*old=ptr+5+*old_disp;const I32 displacement=(I32)((U8*)target-(ptr+5));code[1]=U8(displacement);code[2]=U8(displacement>>8);code[3]=U8(displacement>>16);code[4]=U8(displacement>>24);return Patch(ptr,code,5);}
// 只在校验通过的明文 EXE 安装事件监听。检查函数入口与原始 CALL 目标，
// 不猜测其他游戏版本的指令；若不一致则保留现有 NPC 字幕功能。
// 原始 8 字节为 push esi; mov esi,ecx; call 0x004E7EE0，CALL 需要重新计算相对位移。
bool InstallCutsceneBus(){
 U8*site=Addr(GAME_EVENT_SEND_VA);U8 old[8]={};
 if(!ReadMemory(site,old,8)||old[0]!=0x56||old[1]!=0x8B||old[2]!=0xF1||old[3]!=0xE8)
  return false;
 if(site+8+(I32)U32At(old+4)!=Addr(GAME_EVENT_LOCK_VA))return false;
 U8*tramp=(U8*)VirtualAlloc(nullptr,32,0x3000,0x40);
 if(!tramp)return false;
 for(U32 i=0;i<8;i++)tramp[i]=old[i];
 I32 call_rel=(I32)(Addr(GAME_EVENT_LOCK_VA)-(tramp+8));
 for(U32 i=0;i<4;i++)tramp[4+i]=U8(U32(call_rel)>>(i*8));
 tramp[8]=0xE9;
 I32 ret_rel=(I32)((site+8)-(tramp+13));
 for(U32 i=0;i<4;i++)tramp[9+i]=U8(U32(ret_rel)>>(i*8));
 FlushInstructionCache(GetCurrentProcess(),tramp,13);
 g_cutscene_trampoline=tramp;
 g_cutscene_bus_continue=tramp;
 U8 jump[8]={0xE9,0,0,0,0,0x90,0x90,0x90};
 I32 jmp_rel=(I32)((U8*)&CutsceneBusThunk-(site+5));
 for(U32 i=0;i<4;i++)jump[1+i]=U8(U32(jmp_rel)>>(i*8));
 if(!Patch(site,jump,8)){
  g_cutscene_bus_continue=nullptr;g_cutscene_trampoline=nullptr;
  VirtualFree(tramp,0,0x8000);return false;
 }
 g_cutscene_hook_ready=1;Log("CUTSCENE_NATIVE_EVENTS_HOOK_READY");return true;
}
// 从游戏当前 PE 头核对已确认时间戳和大小，避免错误版本写入固定地址。
bool Validate(){if(!g_game||g_game[0]!='M'||g_game[1]!='Z')return false;U32 off=U32At(g_game+0x3C);if(off<64||off>0x2000)return false;U8* nt=g_game+off;if(nt[0]!='P'||nt[1]!='E'||nt[2]||nt[3])return false;return U32At(nt+8)==TIMESTAMP&&U32At(nt+24+0x38)==IMAGE_SIZE;}
// 自己读取 LOC：使用原生 OLTX 的 EN 语言块，不读取过期 BLI/BFA 数据。
// 保留关键区段的严格边界校验，错误条目只跳过，不传入渲染系统。
void AddEntry(U32 hash,const wchar_t* text,bool allowed){if(!g_entries_table)return;for(U32 i=0;i<g_entries;i++)if(g_entries_table[i].hash==hash){bool mismatch=false;for(U32 j=0;j<MAX_TEXT;j++){if(g_entries_table[i].text[j]!=text[j]){mismatch=true;break;}if(!text[j])break;}if(mismatch)g_entries_table[i].ambiguous=1;return;}
 if(g_entries>=MAX_RECORDS)return;Entry& e=g_entries_table[g_entries++];e.hash=hash;e.allowed=allowed?1:0;for(U32 i=0;i<MAX_TEXT;i++){e.text[i]=text[i];if(!text[i])break;}}
U32 FindText(const U8*file,U32 size,U32 lang,U32 langsize,U32 count,U32 hash){for(U32 i=0;i<count;i++){U32 off=lang+8+8*i;if(off+8>size)return 0;if(U32At(file+off)==hash){U32 rel=U32At(file+off+4);if(rel>=langsize)return 0;return lang+rel;}}return 0;}
// 找到 key 表时必须逐条确认 key 的相对偏移在语言块之前。
bool ValidKeys(const U8*data,U32 sz,U32 start,U32 langstart,U32*count){if(start<0xDC||start+8>langstart)return false;U32 n=U32At(data+start),step=U32At(data+start+4);if(!n||n>200000||step!=8||U64(start)+8+U64(n)*8>langstart)return false;for(U32 i=0;i<n;i++){U32 rel=U32At(data+start+8+i*8+4);if(U64(start)+rel>=langstart||rel<8+n*8)return false;}*count=n;return true;}
void ParseLoc(const wchar_t*path){Handle f=CreateFileW(path,0x80000000,1,nullptr,3,0x80,nullptr);
 if(f==(void*)-1){LogFailure("LOC_OPEN_FAILED",GetLastError());return;}U32 high=0,size=GetFileSize(f,&high);if(high||size<250||size>8*1024*1024){LogPair("LOC_REJECTED_FILE_SIZE high/low",high,size);CloseHandle(f);return;}U8*data=(U8*)VirtualAlloc(nullptr,size,0x3000,4);if(!data){LogFailure("LOC_ALLOCATE_FAILED",GetLastError());CloseHandle(f);return;}U32 read=0;bool ok=ReadFile(f,data,size,&read,nullptr)&&read==size;CloseHandle(f);if(!ok||data[0]!='O'||data[1]!='L'||data[2]!='T'||data[3]!='X'){Log("LOC_REJECTED_INVALID_OLTX_HEADER_OR_READ");VirtualFree(data,0,0x8000);return;}
 U32 lang=0,langsize=0,firstlang=size;for(U32 i=0;i<8;i++){U32 x=0x1C+24*i;if(x+24>size)break;U32 a=U32At(data+x+16),b=U32At(data+x+20);if(a&&b>=8&&U64(a)+b<=size){if(a<firstlang)firstlang=a;}if(Lower(data[x])=='e'&&Lower(data[x+1])=='n'){lang=a;langsize=b;}}
 if(!lang||langsize<8||U64(lang)+langsize>size||firstlang>=size){Log("LOC_REJECTED_EN_LANGUAGE_BLOCK");VirtualFree(data,0,0x8000);return;}
 U32 txtcount=U32At(data+lang),txtstep=U32At(data+lang+4);if(txtstep!=8||!txtcount||txtcount>200000||U64(txtcount)*8+8>langsize){Log("LOC_REJECTED_EN_TEXT_INDEX");VirtualFree(data,0,0x8000);return;}
 U32 hint=U32At(data+0x18),base=U32At(data+8),keys=0,kstart=0;
 U32 candidates[3]={hint,hint+base,0xDC};for(U32 i=0;i<3;i++)if(ValidKeys(data,size,candidates[i],firstlang,&keys)){kstart=candidates[i];break;}
 if(!kstart)for(U32 i=0xDC;i+8<firstlang;i+=4)if(ValidKeys(data,size,i,firstlang,&keys)){kstart=i;break;}
 if(!kstart){Log("LOC_REJECTED_KEY_TABLE");VirtualFree(data,0,0x8000);return;}g_files++;
 for(U32 i=0;i<keys;i++){U32 rec=kstart+8+i*8;U32 hash=U32At(data+rec),ko=U32At(data+rec+4)+kstart;U32 to=FindText(data,size,lang,langsize,txtcount,hash);if(!to||ko>=firstlang)continue;char key[160]={};U32 k=0;for(;k<159&&ko+k<firstlang&&data[ko+k]>=0x20&&data[ko+k]<=0x7e;k++)key[k]=char(data[ko+k]);if(!k||ko+k>=firstlang||data[ko+k]!=0)continue;
 wchar_t text[MAX_TEXT]={};U32 t=0;for(;t+1<MAX_TEXT&&to+t*2+1<lang+langsize;t++){U16 ch=U16At(data+to+t*2);if(!ch)break;text[t]=wchar_t(ch);}if(!t||to+t*2+1>=lang+langsize)continue;
 bool allowed=!SkipKey(key);AddEntry(hash,text,allowed);if(allowed)g_accepted++;
 }VirtualFree(data,0,0x8000);
}
// 每份 LOC 单独记录加载结果，帮助定位资源损坏、文件缺失和读取异常。
// 原来的解析函数保留原样；诊断包装层绝不更改成功/失败的判断。
void ParseLocDiagnostic(const wchar_t* file){
 U32 old_files=g_files,old_entries=g_entries;ParseLoc(file);
 if(!g_diagnostics_enabled)return;
 LogPath("LOC_FILE_SCANNED",file);
 if(g_files==old_files){g_diag_loc_skipped++;Log("LOC_FILE_SKIPPED invalid_or_unreadable");}
 else LogPair("LOC_FILE_ACCEPTED added_records/total",g_entries-old_entries,g_entries);
}
// 递归枚举游戏目前目录下的 script_dialogue.loc，只接受实际后缀匹配的文件。
void Walk(const wchar_t*root,U32 depth){if(depth>12)return;wchar_t search[1024]={};CatWide(search,root,1024);CatWide(search,L"\\*",1024);FindDataW data{};Handle h=FindFirstFileW(search,&data);if(h==(void*)-1)return;do{if(data.name[0]==L'.'&&(data.name[1]==0||(data.name[1]==L'.'&&data.name[2]==0)))continue;wchar_t path[1024]={};CatWide(path,root,1024);CatWide(path,L"\\",1024);CatWide(path,data.name,1024);if(WLen(path)>=1000)continue;
 if(data.attributes&0x10){if(!(data.attributes&0x400))Walk(path,depth+1);}else{const wchar_t* name=data.name;U32 n=WLen(name);const char* suffix="script_dialogue.loc";U32 sl=19;if(n>=sl){bool match=true;for(U32 k=0;k<sl;k++)if(Lower(U8(name[n-sl+k]))!=U8(suffix[k])){match=false;break;}if(match)ParseLocDiagnostic(path);}}
 }while(FindNextFileW(h,&data));FindClose(h);}
void LoadIndex(){wchar_t loc[1024]={};Path(loc,L"BSCNContents\\loc");U32 a=GetFileAttributesW(loc);if(a==kInvalidFile||!(a&0x10)){LogPath("LOC_DIRECTORY_MISSING",loc);LogFailure("LOC_DIRECTORY_INVALID",GetLastError());return;}LogPath("LOC_DIRECTORY_SCAN",loc);Walk(loc,0);LogPair("NPC_DIALOGUE_INDEX_READY files_and_records",g_files,g_entries);}
// 启用诊断时输出实际使用的原生事件字段和合法取值。
// 游戏字幕提交的分支由游戏自身执行，插件仅决定是否额外入队。
void LogNativeEventRoute(){
 Log("NATIVE_EVENT_ROUTE_OFFSET",ROUTE_FLAG_OFFSET,true);
 Log("NATIVE_EVENT_ROUTE_NATIVE_VALUE",0,true);
 Log("NATIVE_EVENT_ROUTE_NPC_VALUE",1,true);
}
struct Pushad{U32 edi,esi,ebp,esp,ebx,edx,ecx,eax;};
U32 g_route_hash=0,g_route_known=0,g_route_is_npc=0;
// 查表时只检验哈希本身是否存在，不在此处混入策略过滤。
// 否则 BOND/NICOLE 等被主动屏蔽的真实条目会被错误记录为索引缺失。
const Entry* FindEntry(U32 hash){
 if(!g_entries_table)return nullptr;
 for(U32 i=0;i<g_entries;i++)if(g_entries_table[i].hash==hash)return g_entries_table+i;
 return nullptr;
}
// 原生游戏查询对白时，EDI 是当前事件结构，ESI 是对白处理对象。
// 诊断记录额外读取字段以便排错；常规运行只读取必需的一字节分流标志。
void ProbeOriginalDialogueRoute(U32 hash,const Pushad* frame){
 if(!g_diagnostics_enabled||!frame)return;
 g_diag_native_event_probes++;
 // 先记录前 4096 个事件，此后按 64 个事件抽样；总量仍由 STATS 统计。
 if(g_diag_native_event_probes>4096&&(g_diag_native_event_probes%64)!=0)return;
 U8 raw[6]={};U8 state[2]={};U32 mirrored_hash=0;
 bool event_ok=frame->edi>=0x10000&&ReadMemory((const U8*)frame->edi+0x18,raw,6);
 bool object_ok=frame->esi>=0x10000&&
   ReadMemory((const U8*)frame->esi+0x79,state,2)&&
   ReadMemory((const U8*)frame->esi+0x84,&mirrored_hash,4);
 char msg[430]="ORIGINAL_DIALOGUE_EVENT hash=";
 CatHex(msg,hash,sizeof msg);Cat(msg," object=",sizeof msg);CatHex(msg,frame->esi,sizeof msg);
 Cat(msg," event_ptr=",sizeof msg);CatHex(msg,frame->edi,sizeof msg);
 Cat(msg," raw_read=",sizeof msg);CatNum(msg,event_ok?1:0,sizeof msg);
 if(event_ok){
  Cat(msg," event_18_1D=",sizeof msg);
  for(U32 i=0;i<6;i++){
   const char*d="0123456789ABCDEF";char bytes[4]={d[raw[i]>>4],d[raw[i]&15],char(i==5?0:' '),0};Cat(msg,bytes,sizeof msg);
  }
  Cat(msg," source_1C=",sizeof msg);CatNum(msg,raw[4],sizeof msg);
  Cat(msg," source_1D=",sizeof msg);CatNum(msg,raw[5],sizeof msg);
 }
 Cat(msg," state_read=",sizeof msg);CatNum(msg,object_ok?1:0,sizeof msg);
 if(object_ok){
  Cat(msg," state_79=",sizeof msg);CatNum(msg,state[0],sizeof msg);
  Cat(msg," state_7A=",sizeof msg);CatNum(msg,state[1],sizeof msg);
  Cat(msg," object_hash=",sizeof msg);CatHex(msg,mirrored_hash,sizeof msg);
  Cat(msg," expected_7A=",sizeof msg);CatNum(msg,event_ok?(raw[4]==0?1:0):2,sizeof msg);
 }
 Log(msg);
}
// 查询原文之前读取原始事件标志。此处与原生查询 CALL 位于同一执行点，
// 不修改游戏已有的原生 UI 字幕处理，也不安装任何第二处游戏 CALL Hook。
extern "C" void CDECL BeforeDialogue(U32 hash,const Pushad*frame){
 g_route_hash=hash;g_route_known=0;g_route_is_npc=0;
 if(!frame)return;
 U8 value=0xFF;
 if(frame->edi>=0x10000&&ReadMemory((const U8*)frame->edi+ROUTE_FLAG_OFFSET,&value,1)&&value<=1){
  g_route_known=1;
  g_route_is_npc=(value==1)?1u:0u;
 }
 ProbeOriginalDialogueRoute(hash,frame);
}
extern "C" void CDECL AfterDialogue(U32 hash,void* result){
 (void)result;
 g_events++;
 // 先读取本次事件的分类结果并清除，防止下一条事件继承旧状态。
 // 无法可靠读取的事件保持静默，避免给游戏原生剧情强行补出第二份字幕。
 const bool route_known=g_route_known&&g_route_hash==hash;
 const bool npc_event=route_known&&g_route_is_npc!=0;
 g_route_known=0;
 if(!route_known){
  g_diag_route_unknown++;
  if(ShouldLogEvent(g_diag_route_unknown))LogHash("NPC_EVENT_ROUTE_UNKNOWN",hash);
  return;
 }
 if(!npc_event){
  // 即便某些过场没有发送 CutSceneStart，原生剧情对白接管时也必须
  // 清掉此前可见和排队的环境对白，防止过场中残留。
  LockSubtitles();
  const bool pending=g_subtitles.visible_count||g_subtitles.waiting_count;
  if(pending)subtitle_queue::Clear(g_subtitles);
  UnlockSubtitles();
  if(pending){g_native_route_clear++;Log("NATIVE_DIALOGUE_QUEUE_CLEARED");}
  g_diag_route++;
  if(ShouldLogEvent(g_diag_route))LogHash("NPC_NATIVE_EVENT_SUPPRESSED",hash);
  return;
 }
 // 过场期间不能把 NPC 台词重新加入显示队列。
 if(__atomic_load_n(&g_cutscene_active,__ATOMIC_ACQUIRE))return;
 const Entry* entry=FindEntry(hash);
 if(!entry){if(ShouldLogEvent(++g_event_miss_logs))LogHash("NPC_DIALOGUE_INDEX_MISS",hash);return;}
 if(entry->ambiguous){if(ShouldLogEvent(++g_event_ambiguous_logs))LogHash("NPC_DIALOGUE_AMBIGUOUS_HASH",hash);return;}
 if(!entry->allowed){if(ShouldLogEvent(++g_event_filtered_logs))LogHash("NPC_DIALOGUE_FILTERED_HASH",hash);return;}
 if(ShouldLogEvent(++g_event_hit_logs))LogHash("NPC_DIALOGUE_INDEX_HIT",hash);
 // 新的对白不能直接覆盖正在显示的全局文本，必须保留各自的显示期限。
 // 三个槽位之外采用 FIFO 等待队列；语音过于密集时先保留已有对白。
 U32 now=GetTickCount();U32 length=WLen(entry->text);
 U32 duration=2400+length*85;if(duration>6500)duration=6500;
 LockSubtitles();
 // 二次检查必须位于锁内，防止过场刚开始时另一个线程仍插入旧对白。
 if(__atomic_load_n(&g_cutscene_active,__ATOMIC_ACQUIRE)){UnlockSubtitles();return;}
 const auto enqueue_result=subtitle_queue::Push(g_subtitles,hash,entry->text,now,duration);
 const U32 visible=g_subtitles.visible_count;
 const U32 waiting=g_subtitles.waiting_count;
 UnlockSubtitles();
 if(enqueue_result==subtitle_queue::PushResult::Duplicate){g_diag_duplicate++;return;}
 if(enqueue_result==subtitle_queue::PushResult::Full){
  g_diag_queue_full++;
  if(ShouldLogEvent(g_diag_queue_full))LogHash("NPC_SUBTITLE_QUEUE_FULL",hash,entry->text);
  return;
 }
 g_diag_queue++;
 if(visible>g_diag_max_visible)g_diag_max_visible=visible;
 if(enqueue_result==subtitle_queue::PushResult::Waiting){
  g_diag_waiting++;
  if(ShouldLogEvent(g_diag_waiting))LogPair("NPC_SUBTITLE_DEFERRED visible/waiting",visible,waiting);
 }
 if(ShouldLogEvent(g_diag_queue))LogHash("NPC_SUBTITLE_QUEUE_HASH",hash,entry->text);
}
extern "C" __declspec(naked) void DialogueThunk(){__asm{
 pushad
 mov eax,[esp+12]
 mov ecx,[eax+4]
 push esp
 push ecx
 call BeforeDialogue
 add esp,8
 popad
 push dword ptr [esp+4]
 call dword ptr [g_orig_dialogue]
 add esp,4
 push edx
 push ecx
 push eax
 mov eax,[esp]
 push eax
 push dword ptr [esp+20]
 call AfterDialogue
 add esp,8
 pop eax
 pop ecx
 pop edx
 ret
}}
// 捕获游戏自己的字体与贴图，不生成 BFA，也不加载第二份字库。
using GlyphFn=void* (FASTCALL*)(void*,void*,U16,U32,float);
using ResourceFn=void* (FASTCALL*)(void*,void*,U32);
extern "C" void* FASTCALL GlyphHook(void* font,void* edx,U16 cp,U32 variant,float scale){
 void* ptr=g_orig_glyph?((GlyphFn)g_orig_glyph)(font,edx,cp,variant,scale):nullptr;if(!ptr||cp<32)return ptr;Glyph glyph={};if(!ReadMemory(ptr,&glyph,sizeof glyph)||glyph.codepoint!=cp)return ptr;U16 handle=0;if(!ReadMemory((U8*)font+0x12C,&handle,2)||!handle)return ptr;
 U32 now=GetTickCount();for(U32 i=0;i<g_font_count;i++)if(g_fonts[i].font==font){g_fonts[i]={font,handle,variant,scale,now};return ptr;}
 if(g_font_count<16)g_fonts[g_font_count++]={font,handle,variant,scale,now};else g_fonts[now%16]={font,handle,variant,scale,now};
 g_diag_font_capture++;if(!g_glyph_seen++){Log("NATIVE_GLYPH_CAPTURED",cp,true);}
 return ptr;
}
// 将候选纹理视为未可信的原生指针。只有它的 COM 虚函数表可读，
// 且 AddRef/Release/GetLevelDesc 三个目标都可读，才允许调用 COM 方法。
// 该检查不会改变游戏内存，也不假定 D3D9 DLL 来自固定安装路径。
bool TextureInterfaceReadable(void* texture){
 if(!texture)return false;
 void** vtable=nullptr;
 if(!ReadMemory(texture,&vtable,4)||!vtable)return false;
 const U32 indices[3]={1,2,17};
 for(U32 index=0;index<3;index++){
  void* method=nullptr;
  if(!ReadMemory(vtable+indices[index],&method,4)||!method)return false;
  U8 instruction=0;
  if(!ReadMemory(method,&instruction,1))return false;
 }
 return true;
}
void ClearTextures(){for(U32 i=0;i<g_texture_count;i++){void*t=g_textures[i].texture;if(t)VFunc<U32(WINAPI*)(void*)>(t,2)(t);}g_texture_count=0;g_selected={};}
extern "C" void* FASTCALL ResourceHook(void*resolver,void*edx,U32 key){void* result=g_orig_resource?((ResourceFn)g_orig_resource)(resolver,edx,key):nullptr;void*texture=nullptr;
 if(!result||!ReadMemory((U8*)result+8,&texture,4)||!TextureInterfaceReadable(texture))return result;U16 handle=U16(key);
 for(U32 i=0;i<g_texture_count;i++)if(g_textures[i].handle==handle){if(g_textures[i].texture!=texture){VFunc<U32(WINAPI*)(void*)>(texture,1)(texture);void*old=g_textures[i].texture;if(old)VFunc<U32(WINAPI*)(void*)>(old,2)(old);g_textures[i].texture=texture;}return result;}
 if(g_texture_count<24){VFunc<U32(WINAPI*)(void*)>(texture,1)(texture);g_textures[g_texture_count++]={handle,texture};g_diag_texture_capture++;if(!g_texture_seen++)Log("NATIVE_TEXTURE_CAPTURED",handle,true);}return result;
}
Context Snapshot(){Context c={};U32 latest=0;for(U32 i=0;i<g_font_count;i++)for(U32 j=0;j<g_texture_count;j++)if(g_fonts[i].handle==g_textures[j].handle&&g_fonts[i].seen>=latest){c.font=g_fonts[i].font;c.texture=g_textures[j].texture;c.variant=g_fonts[i].variant;c.scale=g_fonts[i].scale;latest=g_fonts[i].seen;}
 if(!c.texture||!TextureInterfaceReadable(c.texture))return {};VFunc<U32(WINAPI*)(void*)>(c.texture,1)(c.texture);SurfaceDesc desc={};if(VFunc<HRESULT(WINAPI*)(void*,U32,SurfaceDesc*)>(c.texture,17)(c.texture,0,&desc)<0){VFunc<U32(WINAPI*)(void*)>(c.texture,2)(c.texture);return {};}
 c.width=desc.width;c.height=desc.height;
 if(g_diagnostics_enabled&&!g_diag_font_metrics_logged){
  g_diag_font_metrics_logged=1;LogPair("FONT_TEXTURE_SIZE width/height",c.width,c.height);
 }
 if(!c.width||!c.height){VFunc<U32(WINAPI*)(void*)>(c.texture,2)(c.texture);return {};}
 return c;
}
void FreeContext(Context*c){if(c->texture)VFunc<U32(WINAPI*)(void*)>(c->texture,2)(c->texture);*c={};}
// 纹理存储的是游戏自己的字符图块。字形白色部分有颜色，黑底也可能是全不透明。
// 普通 SRC_ALPHA 混合会把图块的黑色像素画成黑色矩形，因此不能直接沿用。
// 改为白色亮度遮罩混合：输出 = 字形亮度 + 背景*(1-字形亮度)。
// 如此黑底完全不改变画面，灰色抗锯齿自然过渡，不需要另画黑底字幕框。
struct Metric{Glyph glyph;U16 ch;float w,h,advance;};
// 三条对白最多各有 MAX_TEXT 个字符，合并为一次 D3D9 绘制以减少状态切换。
constexpr U32 MAX_VERTICES=3u*MAX_TEXT*6u;
Metric g_metrics[3][MAX_TEXT]={};
U32 g_metric_counts[3]={};
unsigned g_requested_lines[3]={};
Vertex g_shadow[MAX_VERTICES]={},g_main[MAX_VERTICES]={};
void Quad(Vertex*out,U32*count,float x,float y,float w,float h,float u0,float v0,float u1,float v1,U32 color){
 if(*count+6>MAX_VERTICES)return;
 Vertex a={x-.5f,y-.5f,0,1,color,u0,v0},b={x+w-.5f,y-.5f,0,1,color,u1,v0};
 Vertex c={x-.5f,y+h-.5f,0,1,color,u0,v1},d={x+w-.5f,y+h-.5f,0,1,color,u1,v1};
 out[(*count)++]=a;out[(*count)++]=b;out[(*count)++]=c;
 out[(*count)++]=c;out[(*count)++]=b;out[(*count)++]=d;
}
void Configure(void*dev,void*texture){
 // 只修改 StateBlock 保护范围内的 D3D9 状态，画完会由调用方恢复游戏原状态。
 VFunc<HRESULT(WINAPI*)(void*,void*)>(dev,92)(dev,nullptr);
 VFunc<HRESULT(WINAPI*)(void*,void*)>(dev,107)(dev,nullptr);
 VFunc<HRESULT(WINAPI*)(void*,void*)>(dev,87)(dev,nullptr);
 VFunc<HRESULT(WINAPI*)(void*,U32)>(dev,89)(dev,FVF);
 VFunc<HRESULT(WINAPI*)(void*,U32,void*)>(dev,65)(dev,0,texture);
 using Render=HRESULT(WINAPI*)(void*,U32,U32);
 Render r=VFunc<Render>(dev,57);
 r(dev,7,0);r(dev,14,0);r(dev,22,1);r(dev,137,0);
 r(dev,27,1);r(dev,15,0);
 using Stage=HRESULT(WINAPI*)(void*,U32,U32,U32);
 Stage st=VFunc<Stage>(dev,67);
 // RGB 必须由纹理亮度与顶点色相乘；alpha 不用于决定覆盖，防止实心贴图带出黑底。
 st(dev,0,1,4);st(dev,0,2,2);st(dev,0,3,0);
 st(dev,0,4,4);st(dev,0,5,2);st(dev,0,6,0);
 st(dev,1,1,1);st(dev,1,4,1);
 Stage sample=VFunc<Stage>(dev,69);
 sample(dev,0,6,2);sample(dev,0,5,2);sample(dev,0,7,0);
 sample(dev,0,1,3);sample(dev,0,2,3);
}
// 高分辨率时字号不能与屏幕像素数完全线性增长。
// 以 480p 作为参考，随着视口变大只缓慢增大字形，降低它在画面中的相对占比。
float AutoScale(U32 height){
 float h=float(height)/480.0f;
 float factor=0.43f+(h-1.0f)*0.24f;
 if(factor<0.30f)factor=0.30f;
 if(factor>1.40f)factor=1.40f;
 return factor;
}
// 绘制前一次性快照三条消息，随后释放锁。
// 单条长对白可以占用三行，多条对白优先各保留一行，合计始终不超过三行。
void Draw(void* device){
 if(__atomic_load_n(&g_cutscene_active,__ATOMIC_ACQUIRE))return;
 subtitle_queue::Item visible[3]={};
 U32 active=0;
 LockSubtitles();
 subtitle_queue::Advance(g_subtitles,GetTickCount());
 active=g_subtitles.visible_count;
 for(U32 i=0;i<active;i++)visible[i]=g_subtitles.visible[i];
 UnlockSubtitles();
 if(!active)return;
 Context c=Snapshot();if(!c.texture){g_diag_missing_font++;if(!g_logged_missing_font){Log("NATIVE_FONT_CAPTURE_WAIT");g_logged_missing_font=1;}return;}
 Viewport vp={};if(VFunc<HRESULT(WINAPI*)(void*,Viewport*)>(device,48)(device,&vp)<0||vp.width<8||vp.height<8){
  g_diag_invalid_viewport++;
  if(ShouldLogEvent(g_diag_invalid_viewport))Log("D3D9_VIEWPORT_INVALID");
  FreeContext(&c);return;
 }
 if(g_diagnostics_enabled&&(g_diag_last_width!=vp.width||g_diag_last_height!=vp.height)){
  g_diag_last_width=vp.width;g_diag_last_height=vp.height;
  LogPair("D3D9_VIEWPORT_CHANGED width/height",vp.width,vp.height);
 }
 const float scale=AutoScale(vp.height);
 const float max_width=float(vp.width)*0.76f;
 float max_glyph_height=0;
 // 独立计算每条对白的字形，不让后到的文本覆盖之前的测量结果。
 for(U32 slot=0;slot<active;slot++){
  U32 count=0,cjk_count=0;float cjk_sum=0;
  for(U32 i=0;visible[slot].text[i]&&i+1<MAX_TEXT;i++){
   U16 ch=U16(visible[slot].text[i]);if(ch==10||ch==13)continue;
   void*ptr=((GlyphFn)g_orig_glyph)(c.font,nullptr,ch,c.variant,c.scale);
   Glyph glyph={};if(!ptr||!ReadMemory(ptr,&glyph,sizeof glyph)||glyph.codepoint!=ch){g_diag_missing_glyph++;continue;}
   float w=(glyph.u1-glyph.u0)*float(c.width),h=(glyph.v1-glyph.v0)*float(c.height);
   float advance=glyph.left+glyph.mid+glyph.right;if(advance<=0)advance=w;
   if(w<=0||h<=0||advance<=0)continue;
   g_metrics[slot][count++]={glyph,ch,w,h,advance};
   if(ch>=0x3400&&ch<=0x9FFF){cjk_sum+=h;cjk_count++;}
  }
  const float ref_height=cjk_count?cjk_sum/float(cjk_count):0.0f;
  for(U32 i=0;i<count;i++){
   Metric&m=g_metrics[slot][i];float script_scale=1.0f;
   if(ref_height>1.0f&&m.ch>=0x20&&m.ch<=0x7E&&m.h>ref_height*1.05f)
    script_scale=(ref_height*1.05f)/m.h;
   m.w*=script_scale*scale;m.h*=script_scale*scale;m.advance*=script_scale*scale;
   if(m.h>max_glyph_height)max_glyph_height=m.h;
  }
  g_metric_counts[slot]=count;
  // 先计算默认字号下需要几行，供三个屏幕行公平分配。
  U32 pos=0,needed=0;
  while(pos<count&&needed<3){
   const U32 first=pos;float width=0;
   while(pos<count){
    float next=g_metrics[slot][pos].advance;
    if(pos>first&&width+next>max_width)break;
    width+=next;pos++;
   }
   needed++;
  }
  g_requested_lines[slot]=needed?needed:1;
 }
 if(max_glyph_height<=0){FreeContext(&c);return;}
 // 先为每条对白分配一行，再把剩余行数分给需要换行的文本。
 unsigned budget[3]={};
 subtitle_queue::AllocateLineBudget(active,g_requested_lines,budget);
 U32 num=0;
 float line_y=float(vp.y)+float(vp.height)*0.14f;
 const float line_height=max_glyph_height*1.20f;
 // 一条消息可换为多行，但新消息必定从下一条屏幕行开始。
 for(U32 slot=0;slot<active;slot++){
  U32 pos=0,count=g_metric_counts[slot];
  for(U32 row=0;row<budget[slot]&&pos<count;row++){
   const U32 first=pos;float width=0,fit=1.0f;
   if(row+1==budget[slot]){
    // 已经没有额外行可用时压缩本条对白，而不是覆盖另一条对白。
    for(U32 j=pos;j<count;j++)width+=g_metrics[slot][j].advance;
    if(width>max_width)fit=max_width/width;
    pos=count;
   }else{
    while(pos<count){
     const float next=g_metrics[slot][pos].advance;
     if(pos>first&&width+next>max_width)break;
     width+=next;pos++;
    }
   }
   float x=float(vp.x)+(float(vp.width)-width*fit)*0.5f;
   const float baseline=line_y+max_glyph_height;
   for(U32 i=first;i<pos;i++){
    const Metric&m=g_metrics[slot][i];
    if(m.ch!=L' '&&num+6<=MAX_VERTICES){
     // 所有消息共享屏幕行高与字形底线，中英文数字沿用已通过的视觉比例。
     const float h=m.h*fit;
     const float top=baseline-h;
     Quad(g_shadow,&num,x+1.1f,top+1.1f,m.w*fit,h,m.glyph.u0,m.glyph.v0,m.glyph.u1,m.glyph.v1,0xFF555555);
     const U32 begin=num-6;
     for(U32 j=0;j<6;j++){
      Vertex v=g_shadow[begin+j];v.x-=1.1f;v.y-=1.1f;v.color=0xFFFFFFFF;
      g_main[begin+j]=v;
     }
    }
    x+=m.advance*fit;
   }
   line_y+=line_height;
  }
 }
 if(num){
  void*state=g_state_block;
  if(!state||device!=g_state_device){
   if(state)VFunc<U32(WINAPI*)(void*)>(state,2)(state);
   state=nullptr;g_state_device=device;
   VFunc<HRESULT(WINAPI*)(void*,U32,void**)>(device,59)(device,1,&state);
   g_state_block=state;
  }
  if(state){
   VFunc<HRESULT(WINAPI*)(void*)>(state,4)(state);
   Configure(device,c.texture);
   auto draw=VFunc<HRESULT(WINAPI*)(void*,U32,U32,const void*,U32)>(device,83);
   auto render=VFunc<HRESULT(WINAPI*)(void*,U32,U32)>(device,57);
   // 阴影只降低有字形亮度的位置背景亮度；黑底亮度=0，不产生矩形。
   render(device,19,1);render(device,20,4);
   HRESULT shadow_result=draw(device,4,num/3,g_shadow,sizeof(Vertex));
   // 主体亮度与目标亮度互补混合；贴图黑底不修改画面。
   render(device,19,2);render(device,20,4);
   HRESULT main_result=draw(device,4,num/3,g_main,sizeof(Vertex));
   VFunc<HRESULT(WINAPI*)(void*)>(state,5)(state);
   if(main_result>=0&&shadow_result>=0){
    g_diag_draw++;
    if(g_draw_seen++==0)Log("NPC_DRAW_SUBMITTED",num/3,true);
   }else{
    g_diag_draw_fail++;
    if(ShouldLogEvent(g_diag_draw_fail)){
     LogPair("NPC_DRAW_FAILED shadow_hresult/main_hresult",U32(shadow_result),U32(main_result));
    }
   }
  }
  if(g_draw_seen&&!g_seen_chinese){
   for(U32 slot=0;slot<active&&!g_seen_chinese;slot++)
    for(U32 k=0;k<g_metric_counts[slot];k++)
     if(g_metrics[slot][k].ch>=0x3400&&g_metrics[slot][k].ch<=0x9fff){
      g_seen_chinese=1;Log("NPC_NATIVE_GLYPH_CJK_DRAW");break;
     }
  }
 }
 FreeContext(&c);
}
void ResetResources(){if(g_state_block)VFunc<U32(WINAPI*)(void*)>(g_state_block,2)(g_state_block);g_state_block=nullptr;g_state_device=nullptr;ClearTextures();g_diag_resets++;Log("DEVICE_RESET_RELEASED");}
extern "C" HRESULT WINAPI EndSceneHook(void*device){if(!g_d3d_frame_seen++){Log("D3D9_ENDSCENE_ACTIVE");}g_diag_frames++;if(g_started)Draw(device);if(g_diagnostics_enabled)ReportStats();return g_prev_end_scene?((HRESULT(WINAPI*)(void*))g_prev_end_scene)(device):0;}
extern "C" HRESULT WINAPI ResetHook(void*device,PresentParams* pp){ResetResources();HRESULT result=g_prev_reset?((HRESULT(WINAPI*)(void*,PresentParams*))g_prev_reset)(device,pp):I32(0x8876086c);Log("DEVICE_RESET_COMPLETE",result>=0,true);
 if(result<0)Log("D3D9_DEVICE_RESET_FAILED",U32(result),true);
 return result;}
bool HookDevice(void*dev){if(!dev)return false;void**vt=*(void***)dev;if(!vt)return false;
 if(!g_prev_end_scene){g_end_slot=&vt[42];if(!PatchPointer(g_end_slot,(void*)&EndSceneHook,&g_prev_end_scene)){
  LogFailure("D3D9_ENDSCENE_PATCH_FAILED",GetLastError());return false;
 }}
 if(!g_prev_reset){g_reset_slot=&vt[16];if(!PatchPointer(g_reset_slot,(void*)&ResetHook,&g_prev_reset)){
  LogFailure("D3D9_RESET_PATCH_FAILED",GetLastError());return false;
 }}
 Log("D3D9_DEVICE_HOOK_READY");return true;
}
extern "C" HRESULT WINAPI CreateDeviceHook(void* d,U32 a,U32 t,void*w,U32 b,PresentParams*pp,void**out){HRESULT result=g_prev_create_device?((HRESULT(WINAPI*)(void*,U32,U32,void*,U32,PresentParams*,void**))g_prev_create_device)(d,a,t,w,b,pp,out):I32(0x8876086c);if(result>=0&&out&&*out){Log("D3D9_GAME_DEVICE_CREATED");if(!HookDevice(*out))Log("D3D9_GAME_DEVICE_HOOK_FAILED");}
 else if(result<0)Log("D3D9_CREATE_DEVICE_FAILED_HRESULT",U32(result),true);
 return result;}
bool HookD3D9Object(void*d){if(!d||g_prev_create_device)return d!=nullptr;void**vt=*(void***)d;if(!vt)return false;g_create_slot=&vt[16];bool ok=PatchPointer(g_create_slot,(void*)&CreateDeviceHook,&g_prev_create_device);if(ok)Log("D3D9_CREATE_DEVICE_HOOK_READY");return ok;}
extern "C" void* WINAPI D3DCreateHook(U32 sdk){void*result=g_prev_create9?((void*(WINAPI*)(U32))g_prev_create9)(sdk):nullptr;if(result)HookD3D9Object(result);return result;}
// 动态定位游戏的 Direct3DCreate9 导入，避免手工维护 Windows SDK 导入库。
bool HookIat(){if(g_prev_create9)return true;U32 pe=U32At(g_game+0x3C);U8*nt=g_game+pe;U32 imp=U32At(nt+24+0x68);if(!imp)return false;
 for(U32 i=0;i<64;i++){U8*desc=g_game+imp+i*20;U32 original=U32At(desc),name=U32At(desc+12),first=U32At(desc+16);if(!name||!first)break;char*dll=(char*)(g_game+name);if(!SameAscii(dll,"d3d9.dll"))continue;
 if(!original){Log("D3D9_IMPORT_NAMES_MISSING");return false;}
 for(U32 j=0;j<512;j++){U32 ptr=U32At(g_game+original+j*4);if(!ptr)break;if(ptr&0x80000000)continue;char* fn=(char*)(g_game+ptr+2);if(!SameAscii(fn,"Direct3DCreate9"))continue;
 void**slot=(void**)(g_game+first+j*4);if(PatchPointer(slot,(void*)&D3DCreateHook,&g_prev_create9)){Log("D3D9_IAT_HOOK_READY");return true;}return false;
 }}return false;}
// 从游戏原有 Hook 点取回经过游戏自己解密的原始目标。
// 各阶段独立提交。第 2 阶段只安装原生 Dialogue CALL；第 3 阶段才改字体槽位。
// 任一阶段失败时后续阶段不会执行，从日志即可确定崩溃落在哪一层。
bool InstallDialogueOnly(){
 g_dialogue_site=Addr(DIALOGUE_VA);
 U8 code[5]={};if(!ReadMemory(g_dialogue_site,code,5)||code[0]!=0xE8)return false;
 I32 disp=(I32)U32At(code+1);
 if(g_dialogue_site+5+disp!=Addr(LOOKUP_VA)){
  g_diag_dialogue_hook_retry++;
  if(g_diagnostics_enabled&&(g_diag_dialogue_hook_retry<=3||g_diag_dialogue_hook_retry%200==0)){
   LogPair("DIALOGUE_TARGET_WAIT expected/actual",LOOKUP_VA,IMAGE_BASE+U32(g_dialogue_site+5+disp-g_game));
  }
  return false;
 }
 g_orig_dialogue=Addr(LOOKUP_VA);
 void* previous=nullptr;
 if(!PatchCall(g_dialogue_site,(void*)&DialogueThunk,&previous,&g_dialogue_disp)){
  LogFailure("DIALOGUE_CALL_PATCH_FAILED",GetLastError());return false;
 }
 if(previous!=g_orig_dialogue){
  U8 original[5]={0xE8,U8(g_dialogue_disp),U8(g_dialogue_disp>>8),U8(g_dialogue_disp>>16),U8(g_dialogue_disp>>24)};
  Patch(g_dialogue_site,original,5);Log("DIALOGUE_HOOK_TARGET_CHANGED");return false;
 }
 Log("NPC_DIALOGUE_HOOK_READY");LogPair("DIALOGUE_HOOK_RETRIES /site_va",g_diag_dialogue_hook_retry,DIALOGUE_VA);return true;
}
bool InstallFontsOnly(){
 g_resource_site=Addr(RESOURCE_VA);g_glyph_slot=(void**)Addr(FONT_SLOT_VA);
 U8 code[5]={};if(!ReadMemory(g_resource_site,code,5)||code[0]!=0xE8)return false;
 if(g_resource_site+5+(I32)U32At(code+1)!=Addr(0x00E824D0u)){
  g_diag_font_hook_retry++;
  if(g_diagnostics_enabled&&(g_diag_font_hook_retry<=3||g_diag_font_hook_retry%200==0))
   Log("RESOURCE_CALL_TARGET_WAIT_OR_MISMATCH");
  return false;
 }
 void* glyph_previous=nullptr;
 if(!ReadMemory(g_glyph_slot,&glyph_previous,4)||!glyph_previous)return false;
 g_orig_glyph=glyph_previous;
 g_orig_resource=g_resource_site+5+(I32)U32At(code+1);
 void* current=nullptr;
 if(!PatchPointer(g_glyph_slot,(void*)&GlyphHook,&current)){
  LogFailure("GLYPH_SLOT_PATCH_FAILED",GetLastError());return false;
 }
 void* resource_previous=nullptr;
 if(!PatchCall(g_resource_site,(void*)&ResourceHook,&resource_previous,&g_resource_disp)){
  Patch(g_glyph_slot,&g_orig_glyph,4);return false;
 }
 Log("NPC_NATIVE_FONT_HOOK_READY");LogPair("FONT_HOOK_RETRIES /resource_site_va",g_diag_font_hook_retry,RESOURCE_VA);return true;
}
// 可靠的阶段性诊断：即使正常日志尚未创建，也单独写入 ASI 所在目录的状态文件。
// 它只使用已经在无 Hook 探针中实机验证可用的 Kernel32 文件接口。
// 每次更新都覆盖上一次内容，崩溃后可直接读取最后到达的位置。
void Checkpoint(const wchar_t* filename,const char* state){
 if(!g_diagnostics_enabled)return;
 wchar_t full[1024]={};U32 len=GetModuleFileNameW(g_self,full,1024);
 if(len<5||len>=1024)return;
 U32 at=len;while(at&&full[at-1]!=L'\\'&&full[at-1]!=L'/')--at;
 if(!at||at+WLen(filename)>=1024)return;
 U32 n=0;while(filename[n]){full[at+n]=filename[n];++n;}full[at+n]=0;
 Handle f=CreateFileW(full,0x40000000,3,nullptr,2,0x80,nullptr);
 if(f==(Handle)-1)return;
 char text[180]={};Cat(text,"NPCSubtitles v0.1e-log6 stage=",180);
 CatNum(text,NPC_DIAG_STAGE,180);Cat(text," checkpoint=",180);
 Cat(text,state,180);Cat(text," tick_ms=",180);CatNum(text,GetTickCount(),180);Cat(text,"\r\n",180);
 U32 w=0;WriteFile(f,text,Len(text),&w,nullptr);CloseHandle(f);
}
U32 WINAPI Worker(void*){
 Checkpoint(L"NPCSubtitles_phase.log","WORKER_STARTED");
 U32 n=GetModuleFileNameW(g_self,g_folder,1024);if(!n||n>=1024)return 0;U32 idx=n;while(idx&&g_folder[idx-1]!=L'\\'&&g_folder[idx-1]!=L'/')idx--;if(idx>=3&&g_folder[1]==L':'&&idx==3)g_folder[idx]=0;
 else g_folder[idx?idx-1:0]=0;
 Checkpoint(L"NPCSubtitles_phase.log","MODULE_PATH_OK");
 // 将常规日志的路径生成与文件创建拆开记录。即使系统拒绝创建日志，
 // 也能明确区分路径构造错误和 Win32 文件打开失败。
 Checkpoint(L"NPCSubtitles_phase.log","BEFORE_LOG_PATH");
 wchar_t logname[1024]={};
 if(!Path(logname,L"NPCSubtitles.log")){
  Checkpoint(L"NPCSubtitles_phase.log","LOG_PATH_OVERFLOW");return 0;
 }
 Checkpoint(L"NPCSubtitles_phase.log","LOG_PATH_READY");
 if(g_diagnostics_enabled){
  g_log=CreateFileW(logname,0x40000000,3,nullptr,2,0x80,nullptr);
  if(g_log==(Handle)-1){U32 code=GetLastError();g_log=nullptr;LogFailure("LOG_CREATE_FAILED",code);}
 }
 Checkpoint(L"NPCSubtitles_phase.log","LOG_OPEN_ATTEMPTED");
 LogPath("ASI_MODULE_DIRECTORY",g_folder);
 LogPath("LOG_FILE",logname);
 // 这些路径由实际 ASI 目录推导，仅检查文件是否存在，绝不修改原有字体或游戏资源。
 if(g_diagnostics_enabled){
  const wchar_t* assets[]={L"BSCNContents\\fonts\\ui_font_cnx.dat",L"BSCNContents\\fonts\\ui_font_cnx.bpr"};
  for(U32 i=0;i<2;i++){
   wchar_t path[1024]={};Path(path,assets[i]);LogPath("NATIVE_FONT_RESOURCE",path);
   U32 attr=GetFileAttributesW(path);
   LogPair("NATIVE_FONT_RESOURCE_STATUS index/present",i,attr!=kInvalidFile&&!(attr&0x10));
  }
 }
 Log("LOG_MODE=ENABLED cmdline_switch=--log timestamp=local timezone=system");
 Log("STARTING NativeLooseAssets_v0.9_compatible=1 ini=0 bfa=0 bli=0");
 // 不再让上千条字幕文本进入 DLL 的静态数据段：加载器只映射小型插件，
 // 读取 LOC 前才申请所需内存。日志能够区分启动阶段和真正的资源分配阶段。
 Checkpoint(L"NPCSubtitles_phase.log","BEFORE_PE_CHECK");
 g_game=(U8*)GetModuleHandleW(nullptr);if(!Validate()){Log("INVALID_BOND_EXE");return 0;}
 Log("GAME_SIGNATURE_OK");LogPair("GAME_PE_SIGNATURE timestamp/image_size",TIMESTAMP,IMAGE_SIZE);
 Checkpoint(L"NPCSubtitles_phase.log","PE_CHECK_PASSED");
 // 尽早安装 D3D9 的导入拦截，避免耗时的 LOC 索引使游戏先创建完设备。
 // 此时设备回调仅保存已有虚表，不绘制；对话完成并入队后才能画字幕。
 #if NPC_DIAG_STAGE == 4
 Checkpoint(L"NPCSubtitles_phase.log","BEFORE_EARLY_D3D9_IAT");
 if(!HookIat())Log("D3D9_EARLY_IAT_NOT_READY");
 Checkpoint(L"NPCSubtitles_phase.log","EARLY_D3D9_IAT_RETURNED");
 #endif
 Checkpoint(L"NPCSubtitles_phase.log","BEFORE_INDEX_ALLOC");
 g_entries_table=(Entry*)VirtualAlloc(nullptr,U32(sizeof(Entry))*MAX_RECORDS,0x3000,4);
 if(!g_entries_table){LogFailure("NPC_INDEX_ALLOCATE_FAILED",GetLastError());return 0;}
 Log("NPC_INDEX_ALLOCATED",U32(sizeof(Entry))*MAX_RECORDS,true);
 Checkpoint(L"NPCSubtitles_phase.log","BEFORE_LOC_INDEX");
 LoadIndex();LogPair("NPC_INDEX_SUMMARY accepted_total/total",g_accepted,g_entries);Checkpoint(L"NPCSubtitles_phase.log","LOC_INDEX_RETURNED");
 LogNativeEventRoute();Checkpoint(L"NPCSubtitles_phase.log","NATIVE_ROUTE_FLAG_READY");
 if(!g_entries){Log("NO_NPC_LOC_RECORDS");return 0;}
 #if NPC_DIAG_STAGE == 1
 Checkpoint(L"NPCSubtitles_phase.log","INDEX_ONLY_COMPLETE_NO_HOOKS");
 Log("STAGE_1_INDEX_ONLY_COMPLETE");return 0;
#else
 Checkpoint(L"NPCSubtitles_phase.log","BEFORE_DIALOGUE_HOOK");
 for(U32 i=0;i<1500;i++){
     if(InstallDialogueOnly()){g_started=1;break;}
     Sleep(10);
 }
 if(!g_started){Log("DIALOGUE_HOOK_INSTALL_FAILED");Checkpoint(L"NPCSubtitles_phase.log","DIALOGUE_HOOK_FAILED");return 0;}
 Checkpoint(L"NPCSubtitles_phase.log","DIALOGUE_HOOK_READY");
#if NPC_DIAG_STAGE == 2
 Log("STAGE_2_DIALOGUE_ONLY_COMPLETE");return 0;
#else
 Checkpoint(L"NPCSubtitles_phase.log","BEFORE_FONT_HOOKS");
 bool fonts_ready=false;
 for(U32 i=0;i<1500;i++){
  if(InstallFontsOnly()){fonts_ready=true;break;}
  Sleep(10);
 }
 if(!fonts_ready){Log("FONT_HOOK_INSTALL_FAILED");Checkpoint(L"NPCSubtitles_phase.log","FONT_HOOK_FAILED");return 0;}
 Checkpoint(L"NPCSubtitles_phase.log","FONT_HOOKS_READY");
#if NPC_DIAG_STAGE == 3
 Log("STAGE_3_FONT_HOOKS_COMPLETE_NO_D3D9");return 0;
#else
 Checkpoint(L"NPCSubtitles_phase.log","BEFORE_D3D9_HOOK");
 if(!HookIat())Log("D3D9_IAT_NOT_FOUND");
 Checkpoint(L"NPCSubtitles_phase.log","D3D9_HOOK_ATTEMPT_RETURNED");
#endif
#endif
#endif
 // 监听游戏专用 CutSceneStart/CutSceneEnd 事件，清空三行字幕与等待队列。
 // 即使入口被其他插件修改导致无法安装，也不阻断已验证的基本 NPC 字幕功能。
 #if NPC_DIAG_STAGE == 4
 if(!InstallCutsceneBus())Log("CUTSCENE_NATIVE_EVENTS_HOOK_UNAVAILABLE");
 #endif
 // 不创建临时 IDirect3D9 对象。第三方 d3d9 wrapper 可能给每个对象单独分配
 // 虚函数表；在临时对象释放后修改这个表会留下悬空地址。只从实际游戏创建的对象安装。
 Log("D3D9_BOOTSTRAP_SKIPPED reason=AVOID_TEMP_VTABLE");
 #if NPC_DIAG_STAGE == 4
 Checkpoint(L"NPCSubtitles_phase.log","FULL_SUBTITLE_HOOKS_ATTEMPTED");
 #endif
 Log("INITIALIZED");ReportStats();return 0;
}
}
extern "C" BOOL WINAPI DllMain(void*self,U32 reason,void*){
 if(reason==1){
  g_self=self;
  // 由实际 Bond.exe 命令行决定是否启用全部文件诊断；必须在第一个 Checkpoint 之前确定。
  g_diagnostics_enabled=CommandLineHasLogFlag()?1u:0u;
  // 先记录加载时的唯一检查点；即便工作线程尚未运行，也能找到它。
  Checkpoint(L"NPCSubtitles_boot.log","DLL_PROCESS_ATTACH");
  DisableThreadLibraryCalls(self);
  Handle thread=CreateThread(nullptr,0,&Worker,nullptr,0,nullptr);
  if(thread){
   CloseHandle(thread);
   Checkpoint(L"NPCSubtitles_boot.log","WORKER_THREAD_CREATED");
  }else{
   Checkpoint(L"NPCSubtitles_boot.log","WORKER_THREAD_CREATE_FAILED");
  }
 }
 return 1;
}
