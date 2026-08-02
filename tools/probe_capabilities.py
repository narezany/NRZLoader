import re,struct,sys,zipfile
z=zipfile.ZipFile(sys.argv[1])
n=sorted([x for x in z.namelist() if x.endswith("libminecraftpe.so")],key=lambda s:"arm64" not in s)
img=z.read(n[0])
o,=struct.unpack_from("<Q",img,0x28); e,c=struct.unpack_from("<HH",img,0x3A)
S=[struct.unpack_from("<IIQQQQIIQQ",img,o+i*e) for i in range(c)]
sym={}
for s in S:
    if s[1] not in (2,11) or s[9]==0: continue
    t=S[s[6]]; st=img[t[4]:t[4]+t[5]]
    for i in range(s[5]//s[9]):
        nm,_,_,_,v,_=struct.unpack_from("<IBBHQQ",img,s[4]+i*s[9])
        if nm: sym.setdefault(st[nm:st.find(b"\0",nm)].decode("utf8","replace"),v)
print("total:",len(sym))
GROUPS = {
 "v8 FunctionTemplate": ["16FunctionTemplate3New","16FunctionTemplate11GetFunction"],
 "v8 External/Object":  ["8External3New","6Object3SetE","6Object20SetAccessorProperty"],
 "v8 Extension ctor":   ["9ExtensionC1E","9ExtensionC2E"],
 "cohtml ExecuteScript":["ExecuteScript","4View","ViewImpl"],
 "cohtml binding":      ["Binder","RegisterForEvent","TriggerEvent","6Bind"],
 "cohtml input":        ["MouseEvent","KeyEvent","TouchEvent"],
 "renoir draw":         ["13CommandBuffer","DrawCall","10RendererGL","BeginFrame"],
 "leveldb":             ["7leveldb2DB3Get","7leveldb2DB3Put","7leveldb2DB4Open"],
 "minecraft jni":       ["Java_com_mojang_minecraftpe"],
}
for label,pats in GROUPS.items():
    print("\n== %s ==" % label)
    seen=0
    for p in pats:
        m=sorted(k for k in sym if p in k)
        if not m: continue
        print("  [%s] %d" % (p,len(m)))
        for k in m[:5]:
            print("    0x%08x %s" % (sym[k],k[:150]))
        seen+=len(m)
    if not seen: print("  NONE")
