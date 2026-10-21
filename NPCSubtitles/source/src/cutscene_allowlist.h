#pragma once
// 过场遗漏台词白名单：只保存经过人工确认的原始资源键与哈希。
// 显示正文始终从 ASI 同目录的中文 LOC 读取，避免维护第二份翻译。
// 增删条目时必须同时核对原始资源和回归测试，未列入的过场对白维持游戏原生行为。
namespace cutscene_allowlist {
struct Entry { unsigned hash; const char* key; };
constexpr Entry kEntries[]={
 {0x9CF9A69Bu,"Bangkok_Escape_01_RG01_77"},
 {0x8350582Au,"Bangkok_Escape_01_RG02_116"},
 {0x4C746585u,"Istanbul_0_01_BG01_394"},
 {0xD6C1A0ABu,"Refinery_1_01_BOND_192"},
};
constexpr unsigned kCount=sizeof(kEntries)/sizeof(kEntries[0]);
inline bool IsHashAllowed(unsigned hash){
 for(unsigned i=0;i<kCount;i++)if(kEntries[i].hash==hash)return true;
 return false;
}
inline bool IsExactEntryAllowed(unsigned hash,const char* key){
 if(!key)return false;
 for(unsigned i=0;i<kCount;i++)if(kEntries[i].hash==hash){
  const char* expect=kEntries[i].key;
  unsigned j=0;
  // 同时比较键和哈希，避免不同剧情台词因偶然共享哈希而被误放行。
  while(expect[j]&&key[j]&&expect[j]==key[j])j++;
  return expect[j]==0&&key[j]==0;
 }
 return false;
}
} // namespace cutscene_allowlist
