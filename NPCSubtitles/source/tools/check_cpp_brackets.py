from pathlib import Path
# 从自身源码工程定位 src，不能依赖开发容器路径。
root=Path(__file__).resolve().parents[1]/'src'
issues=[]
for file in sorted(root.iterdir()):
 if file.suffix not in ('.cpp','.h'):continue
 s=file.read_text('utf-8');stack=[];state='normal';i=0
 while i<len(s):
  c=s[i];n=s[i+1] if i+1<len(s) else ''
  if state=='normal':
   if c=='/' and n=='/':state='line';i+=2;continue
   if c=='/' and n=='*':state='block';i+=2;continue
   if c=='"':state='double';i+=1;continue
   if c=="'":state='single';i+=1;continue
   if c in '{[(':stack.append((c,i))
   if c in '}])':
    match={'}':'{',']':'[',')':'('}[c]
    if not stack or stack[-1][0]!=match:issues.append((file.name,i,'unexpected '+c));break
    stack.pop()
  elif state=='line':
   if c=='\n':state='normal'
  elif state=='block':
   if c=='*' and n=='/':state='normal';i+=2;continue
  elif state=='double':
   if c=='\\':i+=2;continue
   if c=='"':state='normal'
  elif state=='single':
   if c=='\\':i+=2;continue
   if c=="'":state='normal'
  i+=1
 if stack:issues.append((file.name,len(s),'unclosed '+str(stack[-3:])))
 if state not in ('normal','line'):issues.append((file.name,len(s),'unclosed state '+state))
print('C++ 括号结构检查:',len(list(root.glob('*.cpp'))),'CPP+',len(list(root.glob('*.h'))),'headers;',('PASS' if not issues else issues[:12]))
if issues:raise SystemExit(1)
