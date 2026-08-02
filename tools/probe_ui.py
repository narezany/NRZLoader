import zipfile,sys,json,re

def tolerant(text):
    """Minecraft's interface files carry comments and trailing commas."""
    out=[]; i=0; n=len(text); in_str=False; esc=False
    while i<n:
        c=text[i]
        if in_str:
            out.append(c)
            if esc: esc=False
            elif c=='\\': esc=True
            elif c=='"': in_str=False
            i+=1; continue
        if c=='"': in_str=True; out.append(c); i+=1; continue
        if c=='/' and i+1<n and text[i+1]=='/':
            while i<n and text[i]!='\n': i+=1
            continue
        if c=='/' and i+1<n and text[i+1]=='*':
            i+=2
            while i+1<n and not (text[i]=='*' and text[i+1]=='/'): i+=1
            i+=2; continue
        out.append(c); i+=1
    return re.sub(r',(\s*[}\]])', r'\1', ''.join(out))

z=zipfile.ZipFile(sys.argv[1])
n='assets/assets/resource_packs/vanilla/ui/start_screen.json'
raw=z.read(n).decode('utf8','replace')
try:
    d=json.loads(tolerant(raw))
except json.JSONDecodeError as e:
    print("still unparsable:",e)
    print(repr(tolerant(raw)[max(0,e.pos-200):e.pos+200]))
    sys.exit(1)

print("top-level keys:",len(d))
keys=[k for k in d if 'button' in k.lower()]
print("\n== button definitions ==",len(keys))
for k in keys[:30]: print("  ",k)
print("\n== containers holding them ==")
for k,v in d.items():
    if isinstance(v,dict) and isinstance(v.get('controls'),list):
        names=[list(c)[0] for c in v['controls'] if isinstance(c,dict) and c]
        if any(('play' in x.lower() or 'settings' in x.lower() or 'button' in x.lower()) for x in names):
            print("\n  container:",k)
            print("   type:",v.get('type'),"orientation:",v.get('orientation'))
            for x in names[:25]: print("      ",x)
