/* webserver.c - dependency-free HTTP server for the read-only
 * dashboard. Same engineering rules as modbus_tcp.c: one thread,
 * bounded timeouts on every socket, requests handled one at a time
 * (LAN dashboard traffic; a slow client is cut off by SO_SNDTIMEO,
 * never stalling the recorder). */
#include "webserver.h"
#include "config.h"
#include "data_model.h"
#include "alarm.h"
#include "comm.h"
#include "version.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <io.h>
typedef SOCKET wsock_t;
#define WSOCK_BAD INVALID_SOCKET
#define wsock_close closesocket
static void web_msleep(int ms) { Sleep(ms); }
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <dirent.h>
typedef int wsock_t;
#define WSOCK_BAD (-1)
#define wsock_close close
static void web_msleep(int ms) { usleep(ms * 1000); }
#endif

int ui_group(void);   /* current display group (0-based), from ui.c */

static volatile int served;
int webserver_clients(void) { return served; }

/* ---- the dashboard, embedded as a single page ------------------------- */

static const char DASH_HTML[] =
"<!DOCTYPE html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>PR-40 Recorder</title><style>"
":root{--bg:#0d1b2a;--card:#152638;--line:#24405c;--ink:#e8eef4;"
"--mut:#8fa3b8;--acc:#4fc3f7;--alm:#ff6b6b;--warn:#e8a13c}"
"*{box-sizing:border-box;margin:0}"
"body{background:var(--bg);color:var(--ink);"
"font-family:system-ui,Segoe UI,sans-serif}"
"#wrap{max-width:1180px;margin:0 auto;padding:20px 24px 40px}"
"header{display:flex;justify-content:space-between;align-items:baseline;"
"flex-wrap:wrap;gap:8px;padding-bottom:16px;border-bottom:1px solid "
"var(--line);margin-bottom:20px}"
"h1{font-size:24px;letter-spacing:1px}h1 span{color:var(--acc);"
"font-weight:500}#clk{color:var(--mut);font-size:15px;"
"font-variant-numeric:tabular-nums}"
"#tiles{display:grid;grid-template-columns:repeat(auto-fit,"
"minmax(160px,1fr));gap:14px;margin-bottom:26px}"
".tile{background:var(--card);border:1px solid var(--line);"
"border-radius:10px;padding:14px 16px}"
".tile .k{color:var(--mut);font-size:12px;text-transform:uppercase;"
"letter-spacing:.6px;margin-bottom:6px}"
".tile .n{font-size:24px;font-weight:600;"
"font-variant-numeric:tabular-nums}"
".tile .n small{font-size:13px;color:var(--mut);font-weight:400}"
".dot{display:inline-block;width:10px;height:10px;border-radius:50%;"
"margin-right:8px;background:var(--mut)}"
".dot.ok{background:#41d69c}.dot.no{background:var(--alm)}"
"h2{font-size:14px;color:var(--mut);text-transform:uppercase;"
"letter-spacing:.8px;margin:0 0 12px}"
"#grid{display:grid;grid-template-columns:repeat(auto-fill,"
"minmax(200px,1fr));gap:14px;margin-bottom:28px}"
".ch{background:var(--card);border:1px solid var(--line);"
"border-radius:10px;padding:12px 14px;cursor:pointer;"
"transition:border-color .15s}"
".ch:hover{border-color:var(--acc)}"
".ch .hd{display:flex;justify-content:space-between;align-items:center;"
"color:var(--mut);font-size:12px;margin-bottom:8px}"
".chip{font-size:11px;font-weight:600;padding:2px 8px;"
"border-radius:99px;background:rgba(143,163,184,.15);"
"color:var(--mut);letter-spacing:.4px}"
".chip.ok{background:rgba(65,214,156,.14);color:#41d69c}"
".chip.alm{background:rgba(255,107,107,.16);color:var(--alm)}"
".chip.flt{background:rgba(232,161,60,.14);color:var(--warn)}"
".ch .val{font-size:30px;font-weight:600;"
"font-variant-numeric:tabular-nums;line-height:1.1}"
".ch .val small{font-size:14px;color:var(--mut);font-weight:400;"
"margin-left:5px}"
".ch.alm{border-color:var(--alm)}"
".ch .sp{display:flex;justify-content:space-between;color:var(--mut);"
"font-size:12px;border-top:1px solid var(--line);margin-top:10px;"
"padding-top:8px}"
"#files{display:flex;flex-direction:column;gap:8px;margin-bottom:8px}"
".day{display:flex;align-items:center;gap:18px;background:var(--card);"
"border:1px solid var(--line);border-radius:10px;padding:10px 16px;"
"font-size:14px}"
".day b{min-width:110px;font-weight:600}"
".day a{color:var(--acc);text-decoration:none}"
".day a:hover{text-decoration:underline}"
"#ov{display:none;position:fixed;inset:0;"
"background:rgba(6,13,20,.88);padding:4vh 3vw;z-index:9}"
"#box{background:var(--card);border:1px solid var(--line);"
"border-radius:12px;max-width:1040px;margin:0 auto;padding:18px 22px}"
"#bt{display:flex;justify-content:space-between;align-items:baseline;"
"margin-bottom:12px}"
"#bh{font-size:17px}#bh small{color:var(--mut);font-weight:400;"
"margin-left:10px}"
"#bx{cursor:pointer;color:var(--mut);border:1px solid var(--line);"
"border-radius:8px;padding:4px 12px;font-size:13px}"
"#bx:hover{color:var(--ink);border-color:var(--acc)}"
"#cvw{position:relative}canvas{width:100%;height:380px;display:block;"
"background:var(--bg);border:1px solid var(--line);border-radius:8px}"
"#tip{position:absolute;display:none;background:#1d3550;"
"border:1px solid var(--acc);border-radius:6px;padding:5px 10px;"
"font-size:13px;pointer-events:none;white-space:nowrap;"
"font-variant-numeric:tabular-nums}"
"#bs{color:var(--mut);font-size:13px;margin-top:10px;"
"font-variant-numeric:tabular-nums}"
"#ver{position:fixed;right:14px;bottom:10px;color:var(--mut);"
"font-size:12px;font-variant-numeric:tabular-nums;letter-spacing:.5px}"
"#nav{display:flex;gap:8px;margin-bottom:20px;flex-wrap:wrap}"
".tab{background:var(--card);border:1px solid var(--line);color:var(--mut);"
"border-radius:8px;padding:8px 18px;font-size:14px;cursor:pointer;"
"font-weight:600;letter-spacing:.3px}"
".tab:hover{color:var(--ink)}"
".tab.on{background:var(--acc);border-color:var(--acc);color:#07131f}"
".view{display:none}.view.on{display:block}"
"#bars{display:grid;grid-template-columns:repeat(auto-fit,minmax(96px,1fr));"
"gap:10px;align-items:end}"
".bcol{background:var(--card);border:1px solid var(--line);border-radius:10px;"
"padding:12px 6px 10px;text-align:center}"
".bcol.alm{border-color:var(--alm)}"
".btag{color:var(--mut);font-size:11px;white-space:nowrap;overflow:hidden;"
"text-overflow:ellipsis}"
".bval{font-size:22px;font-weight:700;margin:4px 0 1px;"
"font-variant-numeric:tabular-nums}"
".bunit{color:var(--mut);font-size:11px;margin-bottom:8px}"
".btrk{position:relative;height:230px;width:38px;margin:0 auto;"
"background:var(--bg);border:1px solid var(--line);border-radius:5px;"
"overflow:hidden}"
".bfill{position:absolute;left:0;right:0;bottom:0;border-radius:0 0 4px 4px}"
".bmk{position:absolute;left:0;right:0;height:2px;background:var(--alm)}"
".bsc{display:flex;justify-content:space-between;color:var(--mut);"
"font-size:10px;margin-top:5px;padding:0 2px;"
"font-variant-numeric:tabular-nums}"
"#polwrap{position:relative;width:100%;max-width:520px;margin:0 auto}"
"#polcv{width:100%;height:auto;background:transparent;border:none}"
"#polclk{position:absolute;left:50%;top:50%;transform:translate(-50%,-50%);"
"text-align:center;pointer-events:none}"
"#polclk .pt{font-size:26px;font-weight:700;"
"font-variant-numeric:tabular-nums}"
"#polclk .pd{font-size:13px;color:var(--mut);margin-top:2px}"
"#pleg{display:flex;flex-wrap:wrap;gap:6px 14px;justify-content:center;"
"margin-top:14px;font-size:12px;color:var(--mut)}"
"#pleg span{display:inline-flex;align-items:center;gap:6px}"
"#pleg i{width:11px;height:11px;border-radius:3px;display:inline-block}"
"</style></head><body><div id='wrap'>"
"<header><h1>JETPACE <span>PR-40 Recorder</span></h1>"
"<div id='clk'>connecting...</div></header>"
"<div id='nav'>"
"<div class='tab on' data-v='live'>Live</div>"
"<div class='tab' data-v='bar'>Bar graph</div>"
"<div class='tab' data-v='polar'>Polar</div>"
"<div class='tab' data-v='data'>Data</div>"
"</div>"
"<div id='view_live' class='view on'>"
"<div id='tiles'>"
"<div class='tile'><div class='k'>Card link</div>"
"<div class='n' id='t_link'><span class='dot'></span>-</div></div>"
"<div class='tile'><div class='k'>Active alarms</div>"
"<div class='n' id='t_alm'>-</div></div>"
"<div class='tile'><div class='k'>Channels healthy</div>"
"<div class='n' id='t_ok'>-</div></div>"
"<div class='tile'><div class='k'>Group</div>"
"<div class='n' id='t_grp'>-</div></div>"
"</div>"
"<h2>Channels &mdash; tap a card for today's trend</h2>"
"<div id='grid'></div></div>"
"<div id='view_bar' class='view'><h2>Bar graph</h2>"
"<div id='bars'></div></div>"
"<div id='view_polar' class='view'><h2>Polar &mdash; today, 24 h</h2>"
"<div id='polwrap'><canvas id='polcv' width='520' height='520'></canvas>"
"<div id='polclk'><div class='pt' id='pct'>--:--</div>"
"<div class='pd' id='pcd'></div></div></div>"
"<div id='pleg'></div></div>"
"<div id='view_data' class='view'><h2>Recorded data</h2>"
"<div id='files'></div></div>"
"<div id='ov'><div id='box'><div id='bt'><div id='bh'></div>"
"<span id='bx'>Close</span></div>"
"<div id='cvw'><canvas id='cv'></canvas><div id='tip'></div></div>"
"<div id='bs'></div></div></div>"
"<div id='ver'>v0.2.0</div>"
"</div><script>\n"
"const $=id=>document.getElementById(id);const grid=$('grid');"
"let cards=[],P=[],pu='',pl=0,ph=1,LIVE=null,curView='live';\n"
"const PAL=['#5DCAA5','#85B7EB','#F0997B','#ED93B1','#FAC775','#AFA9EC',"
"'#97C459','#F09595'];\n"
"const fmtv=c=>(c.status=='OK'||c.status=='HI'||c.status=='LO');\n"
"function mk(n){for(let i=0;i<n;i++){const d=document.createElement"
"('div');d.className='ch';d.innerHTML=`<div class='hd'><span></span>"
"<span class='chip'></span></div><div class='val'>-</div>"
"<div class='sp'><span></span><span></span></div>`;"
"d.onclick=()=>trend(i);grid.appendChild(d);cards.push(d);}}\n"
"async function live(){try{const r=await fetch('/api/live'),"
"j=await r.json();LIVE=j;if(!cards.length)mk(j.ch.length);\n"
"$('pct').textContent=j.time.slice(0,5);if(curView=='bar')bars();\n"
"$('clk').textContent=j.time;$('t_grp').textContent='Group '+j.grp;"
"$('ver').textContent='v'+j.fw;"
"$('t_alm').textContent=j.alarms;"
"$('t_alm').style.color=j.alarms?'var(--alm)':'var(--ink)';"
"$('t_link').innerHTML=`<span class='dot ${j.link?'ok':'no'}'></span>"
"${j.link?'ONLINE':'NO LINK'}`;\n"
"let ok=0;j.ch.forEach((c,i)=>{if(c.status=='OK')ok++;"
"const d=cards[i],hd=d.children[0],chip=hd.children[1];"
"hd.children[0].textContent=`CH${c.n}  ${c.tag}`;"
"chip.textContent=c.status;"
"chip.className='chip '+(c.status=='OK'?'ok':"
"(c.status=='HI'||c.status=='LO')?'alm':'flt');"
"d.classList.toggle('alm',c.status=='HI'||c.status=='LO');"
"d.children[1].innerHTML=fmtv(c)?"
"`${c.value.toFixed(1)}<small>${c.unit}</small>`:"
"`<span style='color:var(--mut)'>${c.status}</span>`;"
"const sp=d.children[2];sp.children[0].textContent='L '+c.lo;"
"sp.children[1].textContent='H '+c.hi;});"
"$('t_ok').innerHTML=`${ok}<small> / ${j.ch.length}</small>`;}"
"catch(e){$('clk').textContent='recorder offline';}}\n"
"async function days(){try{const r=await fetch('/api/days'),"
"j=await r.json();$('files').innerHTML=j.slice().reverse().map(d=>"
"`<div class='day'><b>${d}</b><a href='/logs/${d}.csv'>Process data"
"</a><a href='/logs/alarms-${d}.csv'>Alarms</a>"
"<a href='/logs/events-${d}.csv'>Events</a></div>`).join('');}"
"catch(e){}}\n"
"function draw(hov){const cv=$('cv'),r=window.devicePixelRatio||1,"
"W=cv.clientWidth,H=cv.clientHeight;cv.width=W*r;cv.height=H*r;"
"const x=cv.getContext('2d');x.scale(r,r);x.clearRect(0,0,W,H);"
"const L=58,R=16,T=18,B=34,pw=W-L-R,phh=H-T-B;\n"
"x.strokeStyle='#24405c';x.fillStyle='#8fa3b8';"
"x.font='12px system-ui';x.lineWidth=1;\n"
"for(let k=0;k<=4;k++){const y=T+phh*k/4,v=ph-(ph-pl)*k/4;"
"x.beginPath();x.moveTo(L,y);x.lineTo(W-R,y);x.stroke();"
"x.textAlign='right';x.fillText(v.toFixed(1),L-8,y+4);}\n"
"const n=P.length;x.textAlign='center';"
"for(let k=0;k<=5;k++){const i=Math.round((n-1)*k/5);"
"if(P[i])x.fillText(P[i].t,L+pw*k/5,H-12);}\n"
"x.strokeStyle='#4fc3f7';x.lineWidth=2;x.beginPath();"
"P.forEach((p,k)=>{const px=L+pw*k/Math.max(1,n-1),"
"py=T+phh*(1-(p.v-pl)/(ph-pl));k?x.lineTo(px,py):x.moveTo(px,py);});"
"x.stroke();\n"
"if(hov!=null&&P[hov]){const px=L+pw*hov/Math.max(1,n-1),"
"py=T+phh*(1-(P[hov].v-pl)/(ph-pl));"
"x.strokeStyle='rgba(79,195,247,.4)';x.lineWidth=1;x.beginPath();"
"x.moveTo(px,T);x.lineTo(px,T+phh);x.stroke();"
"x.fillStyle='#0d1b2a';x.beginPath();x.arc(px,py,5,0,7);x.fill();"
"x.strokeStyle='#4fc3f7';x.lineWidth=2;x.beginPath();"
"x.arc(px,py,5,0,7);x.stroke();\n"
"const tip=$('tip');tip.style.display='block';"
"tip.textContent=`${P[hov].t}   ${P[hov].v.toFixed(1)} ${pu}`;"
"const bw=tip.offsetWidth;"
"tip.style.left=Math.min(Math.max(px-bw/2,4),W-bw-4)+'px';"
"tip.style.top=(py<70?py+16:py-40)+'px';}"
"else $('tip').style.display='none';}\n"
"async function trend(i){const t=new Date(),"
"p=n=>String(n).padStart(2,'0'),"
"day=`${t.getFullYear()}-${p(t.getMonth()+1)}-${p(t.getDate())}`;"
"let txt='';try{const r=await fetch(`/logs/${day}.csv`);"
"if(!r.ok)throw 0;txt=await r.text();}"
"catch(e){alert('No stored samples today yet');return;}\n"
"const rows=txt.trim().split('\\n'),hdr=rows.shift().split(',');"
"P=[];rows.forEach(rw=>{const c=rw.split(',');const s=c[2+2*i];"
"if(s=='OK'||s=='HI'||s=='LO')"
"P.push({t:c[0].slice(11,16),v:parseFloat(c[1+2*i])});});\n"
"if(!P.length){alert('No valid samples for this channel today');"
"return;}\n"
"pu=(cards[i].children[1].querySelector('small')||{}).textContent||'';"
"$('bh').innerHTML=`CH${i+1}  ${hdr[1+2*i]||''}"
"<small>today &middot; ${P.length} samples</small>`;\n"
"let lo=Math.min(...P.map(p=>p.v)),hi=Math.max(...P.map(p=>p.v));"
"if(hi-lo<1e-6){lo-=1;hi+=1;}const pad=(hi-lo)*.08;"
"pl=lo-pad;ph=hi+pad;\n"
"const avg=P.reduce((a,p)=>a+p.v,0)/P.length;"
"$('bs').textContent=`Min ${lo.toFixed(1)}   Max ${hi.toFixed(1)}   "
"Avg ${avg.toFixed(1)} ${pu}`;\n"
"$('ov').style.display='block';draw(null);}\n"
"$('bx').onclick=()=>$('ov').style.display='none';"
"$('ov').onclick=e=>{if(e.target.id=='ov')"
"$('ov').style.display='none';};\n"
"$('cv').onmousemove=e=>{const rc=$('cv').getBoundingClientRect(),"
"L=58,pw=rc.width-58-16;"
"let i=Math.round((e.clientX-rc.left-L)/pw*(P.length-1));"
"if(i>=0&&i<P.length)draw(i);};"
"$('cv').onmouseleave=()=>draw(null);"
"window.onresize=()=>{if($('ov').style.display=='block')draw(null);};\n"
/* ---- tab switching ---- */
"function setView(v){curView=v;"
"document.querySelectorAll('.tab').forEach(t=>"
"t.classList.toggle('on',t.dataset.v==v));"
"['live','bar','polar','data'].forEach(x=>"
"$('view_'+x).classList.toggle('on',x==v));"
"if(v=='bar')bars();if(v=='polar')polar();}\n"
"document.querySelectorAll('.tab').forEach(t=>"
"t.onclick=()=>setView(t.dataset.v));\n"
/* ---- bar graph (JUMO-style vertical bars) ---- */
"function bars(){if(!LIVE)return;const g=$('bars');const C=LIVE.ch;"
"if(g.children.length!=C.length){g.innerHTML=C.map((c,i)=>"
"`<div class='bcol' id='bc${i}'><div class='btag'></div>"
"<div class='bval'>-</div><div class='bunit'></div>"
"<div class='bsc'><span></span><span class='sh'></span></div>"
"<div class='btrk'><div class='bfill'></div>"
"<div class='bmk mh' style='display:none'></div>"
"<div class='bmk ml' style='display:none'></div></div>"
"<div class='bsc'><span></span><span class='sl'></span></div>`).join('');}"
"C.forEach((c,i)=>{const b=$('bc'+i),ch=b.children;"
"const ok=(c.status=='OK'||c.status=='HI'||c.status=='LO');"
"const alm=(c.status=='HI'||c.status=='LO');"
"const col=alm?'var(--alm)':PAL[i%8];"
"ch[0].textContent=`CH${c.n} ${c.tag}`;"
"ch[1].textContent=ok?c.value.toFixed(1):c.status;"
"ch[1].style.color=ok?col:'var(--mut)';"
"ch[1].style.fontSize=ok?'22px':'15px';"
"ch[2].textContent=ok?c.unit:'';"
"b.querySelector('.sh').textContent=c.rhi;"
"b.querySelector('.sl').textContent=c.rlo;"
"b.classList.toggle('alm',alm);"
"const sp=(c.rhi-c.rlo)||1;let p=(c.value-c.rlo)/sp;"
"p=p<0?0:p>1?1:p;const f=b.querySelector('.bfill');"
"f.style.height=(ok?p*100:0)+'%';f.style.background=col;"
"const mh=b.querySelector('.mh'),ml=b.querySelector('.ml');"
"const mk=(el,v)=>{if(v>c.rlo&&v<c.rhi){el.style.display='block';"
"el.style.bottom=((v-c.rlo)/sp*100)+'%';}else el.style.display='none';};"
"mk(mh,c.hi);mk(ml,c.lo);});}\n"
/* ---- polar (24h circular chart) with centre clock ---- */
"function polDate(){const t=new Date(),p=n=>String(n).padStart(2,'0');"
"return `${t.getFullYear()}-${p(t.getMonth()+1)}-${p(t.getDate())}`;}\n"
"async function polar(){const day=polDate();"
"$('pcd').textContent=day;"
"let txt='';try{const r=await fetch('/logs/'+day+'.csv');"
"if(r.ok)txt=await r.text();}catch(e){}"
"const cv=$('polcv'),x=cv.getContext('2d'),W=520,cx=260,cy=260,"
"rin=54,rout=232;x.clearRect(0,0,W,W);\n"
"x.strokeStyle='#24405c';x.fillStyle='#8fa3b8';x.lineWidth=1;"
"for(let k=1;k<=4;k++){x.beginPath();"
"x.arc(cx,cy,rin+(rout-rin)*k/4,0,7);x.stroke();}"
"for(let h=0;h<12;h++){const a=h/12*2*Math.PI-Math.PI/2;"
"x.beginPath();x.moveTo(cx+rin*Math.cos(a),cy+rin*Math.sin(a));"
"x.lineTo(cx+rout*Math.cos(a),cy+rout*Math.sin(a));x.stroke();}"
"x.font='12px system-ui';x.textAlign='center';"
"x.fillText('00',cx,cy-rout-8);x.fillText('12',cx,cy+rout+16);"
"x.fillText('06',cx+rout+14,cy+4);x.fillText('18',cx-rout-14,cy+4);\n"
"if(!LIVE){return;}const NC=LIVE.ch.length;"
"const rows=txt.trim()?txt.trim().split('\\n'):[];const hdr=rows.shift();"
"for(let i=0;i<NC;i++){const c=LIVE.ch[i];"
"const sp=(c.rhi-c.rlo)||1;x.strokeStyle=PAL[i%8];x.lineWidth=2;"
"x.beginPath();let started=0;"
"rows.forEach(rw=>{const cc=rw.split(',');const st=cc[2+2*i];"
"if(st!='OK'&&st!='HI'&&st!='LO')return;"
"const tm=cc[0].slice(11);const sec=(+tm.slice(0,2))*3600+"
"(+tm.slice(3,5))*60+(+tm.slice(6,8));"
"const v=parseFloat(cc[1+2*i]);let n=(v-c.rlo)/sp;n=n<0?0:n>1?1:n;"
"const a=sec/86400*2*Math.PI-Math.PI/2,rr=rin+n*(rout-rin);"
"const px=cx+rr*Math.cos(a),py=cy+rr*Math.sin(a);"
"started?x.lineTo(px,py):x.moveTo(px,py);started=1;});x.stroke();}\n"
"$('pleg').innerHTML=LIVE.ch.map((c,i)=>"
"`<span><i style='background:${PAL[i%8]}'></i>CH${c.n} ${c.tag}</span>`)"
".join('');}\n"
"live();days();setInterval(live,2000);setInterval(days,60000);\n"
"setInterval(()=>{if(curView=='polar')polar();},30000);\n"
"</script></body></html>";

/* ---- HTTP helpers ------------------------------------------------------ */

static void set_timeouts(wsock_t s)
{
#ifdef _WIN32
    DWORD to = 5000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&to, sizeof(to));
    to = 10000;
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&to, sizeof(to));
#else
    struct timeval rv = { 5, 0 }, sv = { 10, 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &rv, sizeof(rv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &sv, sizeof(sv));
#endif
}

static int send_all(wsock_t s, const void *buf, size_t n)
{
    const char *p = (const char *)buf;
    while (n > 0) {
        int w = (int)send(s, p, (int)(n > 16384 ? 16384 : n), 0);
        if (w <= 0) return -1;
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static void http_send(wsock_t s, const char *status, const char *ctype,
                      const void *body, size_t len)
{
    char hdr[256];
    int hl = snprintf(hdr, sizeof(hdr),
                      "HTTP/1.1 %s\r\nContent-Type: %s\r\n"
                      "Content-Length: %u\r\nCache-Control: no-store\r\n"
                      "Connection: close\r\n\r\n",
                      status, ctype, (unsigned)len);
    if (send_all(s, hdr, (size_t)hl) == 0 && len > 0)
        send_all(s, body, len);
}

static void http_404(wsock_t s)
{
    http_send(s, "404 Not Found", "text/plain", "not found", 9);
}

/* stream a log file with a download-friendly header */
static void send_file(wsock_t s, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { http_404(s); return; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    char hdr[256];
    int hl = snprintf(hdr, sizeof(hdr),
                      "HTTP/1.1 200 OK\r\nContent-Type: text/csv\r\n"
                      "Content-Length: %ld\r\nCache-Control: no-store\r\n"
                      "Connection: close\r\n\r\n", sz);
    if (send_all(s, hdr, (size_t)hl) == 0) {
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
            if (send_all(s, buf, n) != 0) break;
    }
    fclose(f);
}

/* ---- API handlers ------------------------------------------------------ */

static const char *st_txt(ch_status_t st)
{
    switch (st) {
    case CH_ALM_HI: return "HI";
    case CH_ALM_LO: return "LO";
    case CH_SKIP:   return "SKIP";
    case CH_UNDER:  return "UNDER";
    case CH_OVER:   return "OVER";
    case CH_OPEN:   return "OPEN";
    case CH_COMM:   return "COMM";
    default:        return "OK";
    }
}

/* copy src into dst as a JSON-safe string (quotes/backslashes) */
static void jesc(char *dst, int n, const char *src)
{
    int o = 0;
    for (; *src && o < n - 2; src++) {
        if (*src == '\"' || *src == '\\') {
            if (o >= n - 3) break;
            dst[o++] = '\\';
        }
        dst[o++] = *src;
    }
    dst[o] = 0;
}

static void api_live(wsock_t s)
{
    static char body[8192];
    int o = 0;

    time_t now = time(NULL);
    struct tm tm = *localtime(&now);
    o += snprintf(body + o, sizeof(body) - (size_t)o,
                  "{\"fw\":\"" FW_VERSION "\",\"grp\":%d,\"link\":%d,"
                  "\"alarms\":%d,\"time\":\"%02d:%02d:%02d\",\"ch\":[",
                  ui_group() + 1, comm_link_ok(), alarm_active_count(),
                  tm.tm_hour, tm.tm_min, tm.tm_sec);

    int nch = g_cfg.cards * CH_PER_GROUP;
    data_lock();
    for (int i = 0; i < nch && o < (int)sizeof(body) - 200; i++) {
        char tag[24], unit[16];
        jesc(tag, sizeof(tag), g_ch[i].tag);
        jesc(unit, sizeof(unit), g_ch[i].unit);
        o += snprintf(body + o, sizeof(body) - (size_t)o,
                      "%s{\"n\":%d,\"tag\":\"%s\",\"unit\":\"%s\","
                      "\"value\":%.3f,\"status\":\"%s\","
                      "\"lo\":%g,\"hi\":%g,\"rlo\":%g,\"rhi\":%g}",
                      i ? "," : "", i + 1, tag, unit,
                      (double)g_ch[i].value, st_txt(g_ch[i].status),
                      (double)g_ch[i].alm_lo, (double)g_ch[i].alm_hi,
                      (double)g_ch[i].lo, (double)g_ch[i].hi);
    }
    data_unlock();
    o += snprintf(body + o, sizeof(body) - (size_t)o, "]}");

    http_send(s, "200 OK", "application/json", body, (size_t)o);
}

static void api_days(wsock_t s)
{
    static char body[4096];
    int o = 0, cnt = 0;
    o += snprintf(body + o, sizeof(body) - (size_t)o, "[");

#ifdef _WIN32
    struct _finddata_t fd;
    intptr_t h = _findfirst("logs\\*.csv", &fd);
    if (h != -1) {
        do {
            if (strlen(fd.name) == 14 && fd.name[4] == '-' &&
                fd.name[7] == '-' && cnt < 64 &&
                o < (int)sizeof(body) - 32) {
                o += snprintf(body + o, sizeof(body) - (size_t)o,
                              "%s\"%.10s\"", cnt ? "," : "", fd.name);
                cnt++;
            }
        } while (_findnext(h, &fd) == 0);
        _findclose(h);
    }
#else
    DIR *d = opendir("logs");
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (strlen(e->d_name) == 14 &&
                strcmp(e->d_name + 10, ".csv") == 0 &&
                e->d_name[4] == '-' && e->d_name[7] == '-' &&
                cnt < 64 && o < (int)sizeof(body) - 32) {
                o += snprintf(body + o, sizeof(body) - (size_t)o,
                              "%s\"%.10s\"", cnt ? "," : "", e->d_name);
                cnt++;
            }
        }
        closedir(d);
    }
#endif
    o += snprintf(body + o, sizeof(body) - (size_t)o, "]");
    http_send(s, "200 OK", "application/json", body, (size_t)o);
}

/* /logs/<name>.csv - name restricted to letters, digits and '-' so
 * path traversal is impossible */
static void api_log(wsock_t s, const char *name)
{
    size_t ln = strlen(name);
    if (ln < 5 || ln > 40 || strcmp(name + ln - 4, ".csv") != 0) {
        http_404(s);
        return;
    }
    for (size_t i = 0; i + 4 < ln; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-')) {
            http_404(s);
            return;
        }
    }
    char path[64];
    snprintf(path, sizeof(path), "logs/%s", name);
    send_file(s, path);
}

/* ---- server thread ----------------------------------------------------- */

/* Serve dashboard.html from disk if present (edit-and-refresh, no rebuild),
 * otherwise the compiled-in fallback page. cwd is the exe dir (build/). */
static void serve_dashboard(wsock_t s)
{
    FILE *f = fopen("dashboard.html", "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *buf = (sz > 0) ? (char *)malloc((size_t)sz) : NULL;
        if (buf && fread(buf, 1, (size_t)sz, f) == (size_t)sz) {
            http_send(s, "200 OK", "text/html", buf, (size_t)sz);
            free(buf);
            fclose(f);
            return;
        }
        free(buf);
        fclose(f);
    }
    http_send(s, "200 OK", "text/html", DASH_HTML, sizeof(DASH_HTML) - 1);
}

static void handle_client(wsock_t c)
{
    set_timeouts(c);
    int yes = 1;
    setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (const char *)&yes,
               sizeof(yes));

    char req[1024];
    int n = (int)recv(c, req, sizeof(req) - 1, 0);
    if (n <= 0) { wsock_close(c); return; }
    req[n] = 0;

    char path[128] = "";
    if (sscanf(req, "GET %127s HTTP", path) != 1) {
        http_send(c, "405 Method Not Allowed", "text/plain", "GET only",
                  8);
        wsock_close(c);
        return;
    }

    if (!strcmp(path, "/") || !strcmp(path, "/index.html"))
        serve_dashboard(c);
    else if (!strcmp(path, "/api/live"))
        api_live(c);
    else if (!strcmp(path, "/api/days"))
        api_days(c);
    else if (!strncmp(path, "/logs/", 6))
        api_log(c, path + 6);
    else
        http_404(c);

    served++;
    wsock_close(c);
}

static void *web_thread(void *arg)
{
    (void)arg;
#ifdef _WIN32
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);   /* no-op if already started */
#endif

    wsock_t lst = WSOCK_BAD;
    int lst_port = -1;

    while (1) {
        if (!g_cfg.web_enable || lst_port != g_cfg.web_port) {
            if (lst != WSOCK_BAD) { wsock_close(lst); lst = WSOCK_BAD; }
            lst_port = -1;
        }
        if (!g_cfg.web_enable) { web_msleep(1000); continue; }

        if (lst == WSOCK_BAD) {
            lst = socket(AF_INET, SOCK_STREAM, 0);
            if (lst != WSOCK_BAD) {
                int yes = 1;
                setsockopt(lst, SOL_SOCKET, SO_REUSEADDR,
                           (const char *)&yes, sizeof(yes));
                struct sockaddr_in a;
                memset(&a, 0, sizeof(a));
                a.sin_family = AF_INET;
                a.sin_addr.s_addr = INADDR_ANY;
                a.sin_port = htons((uint16_t)g_cfg.web_port);
                if (bind(lst, (struct sockaddr *)&a, sizeof(a)) != 0 ||
                    listen(lst, 4) != 0) {
                    wsock_close(lst);
                    lst = WSOCK_BAD;
                }
            }
            if (lst == WSOCK_BAD) { web_msleep(2000); continue; }
            lst_port = g_cfg.web_port;
        }

        /* wait for a connection, but wake regularly to follow the
         * enable switch / port changes */
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(lst, &rd);
        struct timeval tv = { 1, 0 };
        int r = select((int)lst + 1, &rd, NULL, NULL, &tv);
        if (r <= 0) continue;

        wsock_t c = accept(lst, NULL, NULL);
        if (c != WSOCK_BAD) handle_client(c);
    }
    return NULL;
}

void webserver_init(void)
{
    pthread_t t;
    pthread_create(&t, NULL, web_thread, NULL);
}
