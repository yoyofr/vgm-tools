import sys
import sys
d=open(sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].isdigit() else 'song6_cpu.bin','rb').read()
n=d[0]; chans=list(d[2:2+n])
offs=[int.from_bytes(d[0x24+4*i:0x28+4*i],'little') for i in range(n+1)]
U=0.594/192
print("tracks",n,"chans",chans,"offsets",[hex(o) for o in offs])
alltracks={}
for k in range(n):
    s,e=offs[k],offs[k+1]
    p=s; t=0; ev=[]
    while p<e:
        dl=d[p]; p+=1
        if dl==0xf8: t+=248; continue
        t+=dl
        if p+1>e: break
        a=d[p]; b=d[p+1]; p+=2
        ev.append((t*U,a,b,p-3))
    alltracks[k]=ev
    ons=[x for x in ev if x[1]<0x80 and x[2]>0]
    cmds=[x for x in ev if x[1]>=0x80]
    print("=== track %d ch %2d %4x..%4x (%4d B) note-ons %3d first %6.2f last %6.2f end %6.1f s"%(k,chans[k],s,e,e-s,len(ons),ons[0][0] if ons else -1,ons[-1][0] if ons else -1,t*U))
    print("    cmds:"," ".join("%.1f:%02x%02x@%x"%(c[0],c[1],c[2],c[3]) for c in cmds))
if len(sys.argv)>1:
    k=int(sys.argv[1])
    for t,a,b,pos in alltracks[k]: print("  %7.2f @%4x %02x %02x"%(t,pos,a,b))
