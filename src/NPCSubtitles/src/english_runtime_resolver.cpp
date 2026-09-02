#include "english_runtime_resolver.h"
#include "log.h"
namespace npcsub {
namespace {
bool ReadWindow(uintptr_t address,size_t bytes,std::vector<uint8_t>* out){ if(!address||!bytes||!out)return false; out->assign(bytes,0); SIZE_T got=0; if(!ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<const void*>(address),out->data(),bytes,&got)||got<8){out->clear();return false;} out->resize(got); return true; }
char UpperAscii(char c){return c>='a'&&c<='z'?char(c-'a'+'A'):c;}
std::string Upper(std::string s){for(char&c:s)c=UpperAscii(c);return s;}
bool ContainsAny(const std::string& s,std::initializer_list<const char*> n){for(auto* p:n)if(s.find(p)!=std::string::npos)return true;return false;}
std::wstring AsciiToWide(const std::string& s){std::wstring w;w.reserve(s.size());for(unsigned char c:s)w.push_back(wchar_t(c));return w;}
bool ExtractAscii(const std::vector<uint8_t>& b,size_t at,std::string* out){out->clear();if(at>=b.size())return false;for(size_t i=at;i<b.size()&&out->size()<512;++i){uint8_t c=b[i];if(!c)return out->size()>=4;if(c<0x20||c>0x7e)return false;out->push_back(char(c));}return false;}
bool ExtractWide(const std::vector<uint8_t>& b,size_t at,std::wstring* out){out->clear();if(at+2>b.size())return false;for(size_t i=at;i+1<b.size()&&out->size()<512;i+=2){uint16_t u=uint16_t(b[i])|(uint16_t(b[i+1])<<8);if(!u)return out->size()>=4;if(u<0x20||u==0x7f||u>0x7e)return false;out->push_back(wchar_t(u));}return false;}
bool LooksLikeKey(const std::string& s){if(s.size()<8||s.size()>120)return false;int under=0,alpha=0,digit=0;for(char c:s){if(c=='_')++under;else if((c>='A'&&c<='Z')||(c>='a'&&c<='z'))++alpha;else if(c>='0'&&c<='9')++digit;else return false;}return under>=2&&alpha>=4&&(digit>=1||s.find("Monaco")!=std::string::npos||s.find("Istanbul")!=std::string::npos);}
int ScoreText(const std::wstring& t){if(t.size()<6||t.size()>420)return -1;int letters=0,spaces=0,lower=0,punct=0,words=0;bool inword=false;for(wchar_t c:t){if((c>=L'A'&&c<=L'Z')||(c>=L'a'&&c<=L'z')){++letters;if(c>=L'a')++lower;if(!inword){++words;inword=true;}}else{inword=false;if(c==L' ')++spaces;else if(c==L'.'||c==L','||c==L'!'||c==L'?'||c==L'\''||c==L'-'||c==L':')++punct;else if(c<0x20||c>0x7e)return -1;}}
    if(letters<5||words<2||spaces<1)return -1; std::string a; a.reserve(t.size());for(wchar_t c:t)a.push_back(char(c));
    if(a.find('\\')!=std::string::npos||a.find('/')!=std::string::npos||a.find('_')!=std::string::npos||a.find(".loc")!=std::string::npos||a.find(".wav")!=std::string::npos||a.find("%s")!=std::string::npos)return -1;
    return letters*2+spaces*3+lower+punct*2+std::min<int>(int(t.size()),120);
}
void ConsiderText(const std::wstring& t,uintptr_t addr,const char* enc,bool log,EnglishResolveResult* best){int score=ScoreText(t);if(score<0)return;if(log&&score>best->score&&score>20)NPC_LOG_INFO("NPC_ENGLISH_CANDIDATE score=%d enc=%s addr=%p units=%u preview=%s",score,enc,reinterpret_cast<void*>(addr),unsigned(t.size()),WideToUtf8(t.substr(0,120)).c_str());if(score>best->score){best->text=t;best->score=score;best->source_address=addr;best->encoding=enc;}}
}

bool EnglishRuntimeResolver::IsHardExcludedKey(const std::string& key){if(key.empty())return false;const std::string u=Upper(key);return ContainsAny(u,{"BOND","NICOLE","DANTE","MON_DIA_","_TUTORIAL_","_INFO_","_CS_"});}
bool EnglishRuntimeResolver::LooksLikeDialogueText(const std::wstring& text,int* score){const int s=ScoreText(text);if(score)*score=s;return s>=0;}
void EnglishRuntimeResolver::ScanRegion(uintptr_t address,size_t bytes,int depth,bool log,EnglishResolveResult* best) const {
    std::vector<uint8_t> b; if(!ReadWindow(address,bytes,&b))return;
    for(size_t i=0;i<b.size();++i){std::string a;if(ExtractAscii(b,i,&a)){if(LooksLikeKey(a)&&best->key.empty())best->key=a;else ConsiderText(AsciiToWide(a),address+i,"ASCII",log,best);}if((i&1u)==0){std::wstring w;if(ExtractWide(b,i,&w))ConsiderText(w,address+i,"UTF16",log,best);}}
    if(depth<=0)return;
    for(size_t i=0;i+sizeof(uint32_t)<=b.size();i+=4){uint32_t p=0;memcpy(&p,b.data()+i,4);if(p<0x10000u)continue;std::vector<uint8_t> probe;if(!ReadWindow(uintptr_t(p),16,&probe))continue;ScanRegion(uintptr_t(p),384,depth-1,log,best);}
}
EnglishResolveResult EnglishRuntimeResolver::Resolve(void* result_object,bool log_candidates) const {
    EnglishResolveResult r{}; if(!result_object)return r; ScanRegion(reinterpret_cast<uintptr_t>(result_object),384,1,log_candidates,&r);
    if(!r.text.empty())NPC_LOG_INFO("NPC_ENGLISH_TEXT_HIT result=%p score=%d enc=%s key=%s units=%u",result_object,r.score,r.encoding,r.key.empty()?"UNKNOWN":r.key.c_str(),unsigned(r.text.size()));
    else NPC_LOG_WARN("NPC_ENGLISH_TEXT_MISS result=%p key=%s",result_object,r.key.empty()?"UNKNOWN":r.key.c_str());
    return r;
}
}
