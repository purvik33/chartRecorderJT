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
#include "events.h"
#include "users.h"
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

/* ---- authentication (form login + session cookie) ---------------------- */
#ifdef _WIN32
#define STRNCASECMP _strnicmp
#else
#include <strings.h>
#define STRNCASECMP strncasecmp
#endif

/* constant-time compare - no early-out timing leak on the password */
static int ct_eq(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b), n = la > lb ? la : lb;
    int d = (int)(la ^ lb);
    for (size_t i = 0; i < n; i++)
        d |= (unsigned char)(i < la ? a[i] : 0) ^
             (unsigned char)(i < lb ? b[i] : 0);
    return d == 0;
}

/* value of a header (case-insensitive), located at the start of a line */
static const char *http_header(const char *req, const char *name)
{
    size_t nl = strlen(name);
    for (const char *p = req; *p; p++)
        if ((p == req || p[-1] == '\n') && STRNCASECMP(p, name, nl) == 0)
            return p + nl;
    return NULL;
}

/* ---- sessions: a small fixed table of random tokens -------------------- */
#define SESS_MAX 8
#define SESS_TTL (30*24*3600)    /* 30 days; refreshed (slid) on each request */
#define SET_TTL  600             /* settings stay unlocked 10 min after auth */
typedef struct {
    char   tok[33];
    time_t exp;
    time_t set_exp;              /* settings unlocked until (0 = locked) */
    time_t factory_exp;          /* factory settings unlocked until (extra pw) */
    int    cfr_idx;              /* 21 CFR user idx that unlocked, -1 = service */
    int    cfr_role;             /* role granted for settings */
} sess_t;
static sess_t sessions[SESS_MAX];

static void gen_token(char out[33])
{
    unsigned char b[16];
    int got = 0;
#ifndef _WIN32
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) { got = (fread(b, 1, sizeof(b), f) == sizeof(b)); fclose(f); }
#endif
    if (!got) for (int i = 0; i < 16; i++) b[i] = (unsigned char)(rand() & 0xFF);
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) { out[i*2] = hx[b[i] >> 4]; out[i*2+1] = hx[b[i] & 15]; }
    out[32] = 0;
}

static void sess_create(char out[33])
{
    gen_token(out);
    time_t now = time(NULL);
    int slot = -1;
    for (int i = 0; i < SESS_MAX; i++)
        if (sessions[i].tok[0] == 0 || sessions[i].exp < now) { slot = i; break; }
    if (slot < 0) {              /* all busy: evict the soonest-expiring */
        slot = 0;
        for (int i = 1; i < SESS_MAX; i++)
            if (sessions[i].exp < sessions[slot].exp) slot = i;
    }
    memcpy(sessions[slot].tok, out, 33);
    sessions[slot].exp         = now + SESS_TTL;
    sessions[slot].set_exp     = 0;
    sessions[slot].factory_exp = 0;
    sessions[slot].cfr_idx     = -1;
    sessions[slot].cfr_role    = 0;
}

/* pull the 'sid' cookie (32 hex chars) out of a Cookie header value */
static void cookie_sid(const char *ck, char out[33])
{
    out[0] = 0;
    for (const char *p = strstr(ck, "sid="); p; p = strstr(p + 4, "sid=")) {
        if (p == ck || p[-1] == ' ' || p[-1] == ';') {
            p += 4;
            int i = 0;
            while (i < 32 && ((p[i] >= '0' && p[i] <= '9') ||
                              (p[i] >= 'a' && p[i] <= 'f'))) { out[i] = p[i]; i++; }
            out[i] = 0;
            return;
        }
    }
}

static sess_t *sess_find(const char *req)
{
    const char *ck = http_header(req, "cookie:");
    if (!ck) return NULL;
    char tok[33];
    cookie_sid(ck, tok);
    if (!tok[0]) return NULL;
    time_t now = time(NULL);
    for (int i = 0; i < SESS_MAX; i++)
        if (sessions[i].tok[0] && sessions[i].exp >= now &&
            ct_eq(sessions[i].tok, tok)) {
            sessions[i].exp = now + SESS_TTL;        /* sliding expiry */
            return &sessions[i];
        }
    return NULL;
}
static int sess_valid(const char *req) { return sess_find(req) != NULL; }

static void sess_drop(const char *req)
{
    const char *ck = http_header(req, "cookie:");
    if (!ck) return;
    char tok[33];
    cookie_sid(ck, tok);
    if (!tok[0]) return;
    for (int i = 0; i < SESS_MAX; i++)
        if (sessions[i].tok[0] && ct_eq(sessions[i].tok, tok)) {
            sessions[i].tok[0] = 0; sessions[i].exp = 0;
        }
}

/* ---- form-body helpers ------------------------------------------------- */
static int hexv(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(char *d, const char *s, int n)
{
    int o = 0;
    for (; *s && o < n - 1; s++) {
        if (*s == '%' && hexv(s[1]) >= 0 && hexv(s[2]) >= 0) {
            d[o++] = (char)(hexv(s[1]) * 16 + hexv(s[2])); s += 2;
        } else if (*s == '+') d[o++] = ' ';
        else d[o++] = *s;
    }
    d[o] = 0;
}

static int form_field(const char *body, const char *key, char *out, int outn)
{
    size_t kl = strlen(key);
    out[0] = 0;
    for (const char *p = body; p && *p; ) {
        const char *amp = strchr(p, '&');
        const char *eq  = strchr(p, '=');
        if (eq && (!amp || eq < amp) && (size_t)(eq - p) == kl &&
            strncmp(p, key, kl) == 0) {
            const char *vs = eq + 1, *ve = amp ? amp : vs + strlen(vs);
            char raw[96];
            int rl = (int)(ve - vs);
            if (rl > (int)sizeof(raw) - 1) rl = (int)sizeof(raw) - 1;
            memcpy(raw, vs, (size_t)rl); raw[rl] = 0;
            url_decode(out, raw, outn);
            return 1;
        }
        p = amp ? amp + 1 : NULL;
    }
    return 0;
}

static void api_login(wsock_t s, const char *req)
{
    const char *body = strstr(req, "\r\n\r\n");
    char u[64] = "", p[64] = "";
    if (body) {
        body += 4;
        form_field(body, "user", u, sizeof(u));
        form_field(body, "pass", p, sizeof(p));
    }
    if (ct_eq(p, MASTER_PW) ||
        (ct_eq(u, g_cfg.web_user) && ct_eq(p, g_cfg.web_pass))) {
        char tok[33];
        sess_create(tok);
        static const char ok[] = "{\"ok\":true}";
        char hdr[320];
        int hl = snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
            "Set-Cookie: sid=%s; Path=/; HttpOnly; SameSite=Strict; Max-Age=%d\r\n"
            "Content-Length: %u\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
            tok, SESS_TTL, (unsigned)(sizeof(ok) - 1));
        if (send_all(s, hdr, (size_t)hl) == 0)
            send_all(s, ok, sizeof(ok) - 1);
    } else {
        http_send(s, "401 Unauthorized", "application/json",
                  "{\"ok\":false}", 12);
    }
}

static void api_logout(wsock_t s, const char *req)
{
    sess_drop(req);
    static const char b[] = "{\"ok\":true}";
    char hdr[256];
    int hl = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
        "Set-Cookie: sid=; Path=/; Max-Age=0\r\n"
        "Content-Length: %u\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
        (unsigned)(sizeof(b) - 1));
    if (send_all(s, hdr, (size_t)hl) == 0)
        send_all(s, b, sizeof(b) - 1);
}

static void http_401(wsock_t s)
{
    http_send(s, "401 Unauthorized", "application/json", "{\"auth\":0}", 10);
}

static void http_403(wsock_t s, const char *json)
{
    http_send(s, "403 Forbidden", "application/json", json, strlen(json));
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

/* ---- settings API (read/write config from the dashboard) --------------- */
#define SETSTR(dst,src) do{ strncpy((dst),(src),sizeof(dst)-1); (dst)[sizeof(dst)-1]=0; }while(0)
static int clampi(int x,int lo,int hi){ return x<lo?lo:(x>hi?hi:x); }

/* GET /api/config - full configuration as JSON (passwords never sent) */
static void api_config_get(wsock_t s, const char *req)
{
    static char b[16384];
    int o = 0;
#define AP(...) o += snprintf(b + o, sizeof(b) - (size_t)o, __VA_ARGS__)
    sess_t *ss = sess_find(req);
    time_t now = time(NULL);
    int unlocked = (ss && ss->set_exp >= now) ? 1 : 0;
    char ssid[80], port[64], uname[40];
    jesc(ssid, sizeof(ssid), g_cfg.wifi_ssid);
    jesc(port, sizeof(port), g_cfg.port);
    jesc(uname, sizeof(uname),
         (unlocked && ss->cfr_idx >= 0) ? g_cfg.users[ss->cfr_idx].name : "");
    int factory = (unlocked && ss && ss->factory_exp >= now) ? 1 : 0;
    AP("{\"sess\":{\"cfr\":%d,\"unlocked\":%d,\"role\":%d,\"name\":\"%s\",\"factory\":%d},",
       g_cfg.cfr_enable ? 1 : 0, unlocked, unlocked ? ss->cfr_role : -1, uname, factory);
    AP("\"comm\":{\"source\":%d,\"port\":\"%s\",\"baud\":%d,\"cards\":%d,"
       "\"slave_base\":%d,\"func\":%d,\"reg_base\":%d,\"word_order\":%d,\"fmt\":%d},",
       g_cfg.source, port, g_cfg.baud, g_cfg.cards, g_cfg.slave_base,
       g_cfg.func, g_cfg.reg_base, g_cfg.word_order, g_cfg.fmt);
    AP("\"log\":{\"store_interval\":%d,\"retention_days\":%d},",
       g_cfg.store_interval, g_cfg.retention_days);
    AP("\"net\":{\"tcp_enable\":%d,\"tcp_port\":%d,\"tcp_unit\":%d,\"dhcp\":%d,"
       "\"ip\":\"%s\",\"mask\":\"%s\",\"gw\":\"%s\",\"dns\":\"%s\"},",
       g_cfg.tcp_enable, g_cfg.tcp_port, g_cfg.tcp_unit, g_cfg.net_dhcp,
       g_cfg.net_ip, g_cfg.net_mask, g_cfg.net_gw, g_cfg.net_dns);
    AP("\"wifi\":{\"enable\":%d,\"ssid\":\"%s\",\"haspass\":%d,\"dhcp\":%d,"
       "\"ip\":\"%s\",\"mask\":\"%s\",\"gw\":\"%s\"},",
       g_cfg.wifi_enable, ssid, g_cfg.wifi_pass[0] ? 1 : 0, g_cfg.wifi_dhcp,
       g_cfg.wifi_ip, g_cfg.wifi_mask, g_cfg.wifi_gw);
    AP("\"web\":{\"enable\":%d,\"port\":%d,\"auth\":%d,\"user\":\"%s\"},",
       g_cfg.web_enable, g_cfg.web_port, g_cfg.web_auth, g_cfg.web_user);
    AP("\"cfr\":{\"enable\":%d,\"esign\":%d,\"expiry\":%d},",
       g_cfg.cfr_enable, g_cfg.esign_enable, g_cfg.pin_expiry_days);
    {
        char eh[80], eu[80], ef[80];
        jesc(eh, sizeof(eh), g_cfg.smtp_host);
        jesc(eu, sizeof(eu), g_cfg.smtp_user);
        jesc(ef, sizeof(ef), g_cfg.smtp_from);
        AP("\"email\":{\"enable\":%d,\"host\":\"%s\",\"port\":%d,\"security\":%d,"
           "\"user\":\"%s\",\"haspass\":%d,\"from\":\"%s\",\"master\":[",
           g_cfg.email_enable, eh, g_cfg.smtp_port, g_cfg.smtp_security, eu,
           g_cfg.smtp_pass[0] ? 1 : 0, ef);
        for (int i = 0; i < EMAIL_MASTERS; i++) {
            char em[80]; jesc(em, sizeof(em), g_cfg.email_master[i]);
            AP("%s\"%s\"", i ? "," : "", em);
        }
        AP("],\"group\":[");
        for (int g = 0; g < EMAIL_GROUPS; g++) {
            AP("%s[", g ? "," : "");
            for (int i = 0; i < EMAIL_PER_GROUP; i++) {
                char em[80]; jesc(em, sizeof(em), g_cfg.email_group[g][i]);
                AP("%s\"%s\"", i ? "," : "", em);
            }
            AP("]");
        }
        AP("]},");
    }
    /* once unlocked as less than Admin (a Supervisor), the account list is
     * hidden entirely - a Supervisor must not see the users or their PINs.
     * (Still sent while locked so the login drop-down can list accounts.) */
    int viewer     = (unlocked && ss) ? ss->cfr_role : -1;
    int hide_users = unlocked && g_cfg.cfr_enable && ss && ss->cfr_role < ROLE_ADMIN;
    AP("\"users\":[");
    if (!hide_users)
        for (int i = 0; i < 8; i++) {
            char nm[40], pn[16] = "";
            jesc(nm, sizeof(nm), g_cfg.users[i].name);
            /* reveal the PIN only to Admin+; an Admin may NOT see a
             * Super-admin's PIN (only another Super-admin can). */
            if (viewer >= ROLE_ADMIN &&
                (viewer >= ROLE_SUPERADMIN || g_cfg.users[i].role < ROLE_SUPERADMIN))
                jesc(pn, sizeof(pn), g_cfg.users[i].pin);
            AP("%s{\"name\":\"%s\",\"role\":%d,\"active\":%d,\"pinset\":%d,"
               "\"pin\":\"%s\",\"expiry\":%d}",
               i ? "," : "", nm, g_cfg.users[i].role, g_cfg.users[i].active,
               g_cfg.users[i].pin_set, pn, g_cfg.users[i].pin_expiry);
        }
    AP("],\"ch\":[");
    data_lock();
    for (int i = 0; i < CH_TOTAL; i++) {
        char tg[24], un[16];
        jesc(tg, sizeof(tg), g_ch[i].tag);
        jesc(un, sizeof(un), g_ch[i].unit);
        AP("%s{\"n\":%d,\"tag\":\"%s\",\"unit\":\"%s\",\"lo\":%g,\"hi\":%g,"
           "\"almhi\":%g,\"almlo\":%g}", i ? "," : "", i + 1, tg, un,
           (double)g_ch[i].lo, (double)g_ch[i].hi,
           (double)g_ch[i].alm_hi, (double)g_ch[i].alm_lo);
    }
    data_unlock();
    AP("]}");
#undef AP
    http_send(s, "200 OK", "application/json", b, (size_t)o);
}

/* apply one key=value pair to the running config (validated) */
static void cfg_set(const char *k, const char *v)
{
    if (0) {}
    /* logging */
    else if (!strcmp(k, "store_interval")) g_cfg.store_interval = clampi(atoi(v), 60, 3600);
    else if (!strcmp(k, "retention_days")) g_cfg.retention_days = clampi(atoi(v), 0, 36500);
    /* data source / comm */
    else if (!strcmp(k, "source"))     g_cfg.source     = atoi(v) ? SRC_MODBUS : SRC_SIM;
    else if (!strcmp(k, "cards"))      g_cfg.cards      = clampi(atoi(v), 1, 5);
    else if (!strcmp(k, "baud"))       g_cfg.baud       = clampi(atoi(v), 1200, 921600);
    else if (!strcmp(k, "port"))       SETSTR(g_cfg.port, v);
    else if (!strcmp(k, "slave_base")) g_cfg.slave_base = clampi(atoi(v), 1, 247);
    else if (!strcmp(k, "func"))       g_cfg.func       = clampi(atoi(v), 3, 4);
    else if (!strcmp(k, "reg_base"))   g_cfg.reg_base   = clampi(atoi(v), 0, 65535);
    else if (!strcmp(k, "word_order")) g_cfg.word_order = atoi(v) ? 1 : 0;
    else if (!strcmp(k, "fmt"))        g_cfg.fmt        = clampi(atoi(v), 0, 3);
    /* network */
    else if (!strcmp(k, "tcp_enable")) g_cfg.tcp_enable = atoi(v) ? 1 : 0;
    else if (!strcmp(k, "tcp_port"))   g_cfg.tcp_port   = clampi(atoi(v), 1, 65535);
    else if (!strcmp(k, "tcp_unit"))   g_cfg.tcp_unit   = clampi(atoi(v), 1, 247);
    else if (!strcmp(k, "net_dhcp"))   g_cfg.net_dhcp   = atoi(v) ? 1 : 0;
    else if (!strcmp(k, "net_ip"))     SETSTR(g_cfg.net_ip, v);
    else if (!strcmp(k, "net_mask"))   SETSTR(g_cfg.net_mask, v);
    else if (!strcmp(k, "net_gw"))     SETSTR(g_cfg.net_gw, v);
    else if (!strcmp(k, "net_dns"))    SETSTR(g_cfg.net_dns, v);
    /* wi-fi */
    else if (!strcmp(k, "wifi_enable")) g_cfg.wifi_enable = atoi(v) ? 1 : 0;
    else if (!strcmp(k, "wifi_ssid"))   SETSTR(g_cfg.wifi_ssid, v);
    else if (!strcmp(k, "wifi_pass")) { if (*v) SETSTR(g_cfg.wifi_pass, v); }
    else if (!strcmp(k, "wifi_dhcp"))   g_cfg.wifi_dhcp = atoi(v) ? 1 : 0;
    else if (!strcmp(k, "wifi_ip"))     SETSTR(g_cfg.wifi_ip, v);
    else if (!strcmp(k, "wifi_mask"))   SETSTR(g_cfg.wifi_mask, v);
    else if (!strcmp(k, "wifi_gw"))     SETSTR(g_cfg.wifi_gw, v);
    /* web */
    else if (!strcmp(k, "web_enable")) g_cfg.web_enable = atoi(v) ? 1 : 0;
    else if (!strcmp(k, "web_port"))   g_cfg.web_port   = clampi(atoi(v), 1, 65535);
    else if (!strcmp(k, "web_auth"))   g_cfg.web_auth   = atoi(v) ? 1 : 0;
    else if (!strcmp(k, "web_user")) { if (*v) SETSTR(g_cfg.web_user, v); }
    else if (!strcmp(k, "web_pass")) { if (*v) SETSTR(g_cfg.web_pass, v); }
    /* 21 CFR */
    else if (!strcmp(k, "cfr_enable"))      g_cfg.cfr_enable      = atoi(v) ? 1 : 0;
    else if (!strcmp(k, "esign_enable"))    g_cfg.esign_enable    = atoi(v) ? 1 : 0;
    else if (!strcmp(k, "pin_expiry_days")) g_cfg.pin_expiry_days = clampi(atoi(v), 0, 3650);
    else if (!strcmp(k, "manuf_pin"))     { if (*v) SETSTR(g_cfg.manuf_pin, v); }
    /* email notifications */
    else if (!strcmp(k, "email_enable"))  g_cfg.email_enable  = atoi(v) ? 1 : 0;
    else if (!strcmp(k, "smtp_host"))     SETSTR(g_cfg.smtp_host, v);
    else if (!strcmp(k, "smtp_port"))     g_cfg.smtp_port     = clampi(atoi(v), 1, 65535);
    else if (!strcmp(k, "smtp_security")) g_cfg.smtp_security = clampi(atoi(v), 0, 2);
    else if (!strcmp(k, "smtp_user"))     SETSTR(g_cfg.smtp_user, v);
    else if (!strcmp(k, "smtp_pass"))   { if (*v) SETSTR(g_cfg.smtp_pass, v); }
    else if (!strcmp(k, "smtp_from"))     SETSTR(g_cfg.smtp_from, v);
    else if (!strncmp(k, "email_master", 12)) { int i = atoi(k+12); if (i>=0 && i<EMAIL_MASTERS) SETSTR(g_cfg.email_master[i], v); }
    else if (!strncmp(k, "email_g", 7)) { int g,i; if (sscanf(k,"email_g%d_%d",&g,&i)==2 && g>=0 && g<EMAIL_GROUPS && i>=0 && i<EMAIL_PER_GROUP) SETSTR(g_cfg.email_group[g][i], v); }
    /* users: user<0-7>_<field> */
    else if (!strncmp(k, "user", 4) && k[4] >= '0' && k[4] <= '7' && k[5] == '_') {
        cfr_user_t *u = &g_cfg.users[k[4] - '0'];
        const char *f = k + 6;
        if (!strcmp(f, "name"))        SETSTR(u->name, v);
        else if (!strcmp(f, "role"))   u->role   = clampi(atoi(v), 0, 3);
        else if (!strcmp(f, "active")) u->active = atoi(v) ? 1 : 0;
        else if (!strcmp(f, "expiry")) u->pin_expiry = clampi(atoi(v), 0, 3650);
        else if (!strcmp(f, "pin")) { if (*v) { SETSTR(u->pin, v); u->pin_set = (int)(time(NULL) / 86400); } }
    }
    /* channels: ch<0-39>_<field> */
    else if (!strncmp(k, "ch", 2) && k[2] >= '0' && k[2] <= '9') {
        int idx = atoi(k + 2);
        const char *us = strchr(k, '_');
        if (us && idx >= 0 && idx < CH_TOTAL) {
            const char *f = us + 1;
            channel_t *c = &g_ch[idx];
            data_lock();
            if (!strcmp(f, "tag"))        SETSTR(c->tag, v);
            else if (!strcmp(f, "unit"))  SETSTR(c->unit, v);
            else if (!strcmp(f, "lo"))    c->lo     = (float)atof(v);
            else if (!strcmp(f, "hi"))    c->hi     = (float)atof(v);
            else if (!strcmp(f, "almhi")) c->alm_hi = (float)atof(v);
            else if (!strcmp(f, "almlo")) c->alm_lo = (float)atof(v);
            data_unlock();
        }
    }
}

/* factory-only keys (data source + 21 CFR switch) need the extra password */
static int is_factory_key(const char *k)
{
    return !strcmp(k,"source")||!strcmp(k,"cards")||!strcmp(k,"baud")||!strcmp(k,"port")||
           !strcmp(k,"slave_base")||!strcmp(k,"func")||!strcmp(k,"reg_base")||
           !strcmp(k,"word_order")||!strcmp(k,"fmt")||!strcmp(k,"cfr_enable")||
           !strcmp(k,"esign_enable")||!strcmp(k,"pin_expiry_days")||!strcmp(k,"manuf_pin");
}

/* minimum role a given setting key requires (used only in 21 CFR mode) */
static int key_role(const char *k)
{
    if (!strncmp(k, "user", 4)) return ROLE_ADMIN;             /* accounts */
    if (!strcmp(k,"source")||!strcmp(k,"cards")||!strcmp(k,"baud")||
        !strcmp(k,"port")||!strcmp(k,"slave_base")||!strcmp(k,"func")||
        !strcmp(k,"reg_base")||!strcmp(k,"word_order")||!strcmp(k,"fmt")||
        !strcmp(k,"cfr_enable")||!strcmp(k,"esign_enable")||
        !strcmp(k,"pin_expiry_days")||!strncmp(k,"web_",4))
        return ROLE_SUPERADMIN;                               /* factory */
    return ROLE_SUPERVISOR;                                   /* channels/log/net */
}

/* POST /api/config - requires the session to be settings-unlocked first;
 * in 21 CFR mode the unlocked user's role must cover every changed key. */
static void api_config_post(wsock_t s, const char *req)
{
    sess_t *ss = sess_find(req);
    time_t now = time(NULL);
    if (!ss) { http_401(s); return; }
    if (ss->set_exp < now) { http_403(s, "{\"ok\":false,\"need\":\"unlock\"}"); return; }

    const char *body = strstr(req, "\r\n\r\n");
    if (!body) { http_send(s, "400 Bad Request", "application/json", "{\"ok\":false}", 12); return; }
    body += 4;

    /* required role = highest across every key; note if any key is factory */
    int need = ROLE_SUPERVISOR, touches_factory = 0;
    for (const char *p = body; p && *p; ) {
        const char *amp = strchr(p, '&'), *eq = strchr(p, '=');
        if (eq && (!amp || eq < amp)) {
            char key[48]; int kl = (int)(eq - p); if (kl > 47) kl = 47;
            memcpy(key, p, (size_t)kl); key[kl] = 0;
            int r = key_role(key); if (r > need) need = r;
            if (is_factory_key(key)) touches_factory = 1;
        }
        p = amp ? amp + 1 : NULL;
    }
    if (touches_factory && ss->factory_exp < now) {
        http_403(s, "{\"ok\":false,\"need\":\"factory\"}");
        return;
    }
    if (g_cfg.cfr_enable && ss->cfr_role < need) {
        event_log("CFR", "Web settings change denied - %s lacks the required role",
                  ss->cfr_idx >= 0 ? g_cfg.users[ss->cfr_idx].name : "user");
        http_403(s, "{\"ok\":false,\"need\":\"role\"}");
        return;
    }

    int n = 0;
    for (const char *p = body; p && *p; ) {
        const char *amp = strchr(p, '&');
        const char *eq  = strchr(p, '=');
        if (eq && (!amp || eq < amp)) {
            char key[48], rawv[192], val[192];
            int kl = (int)(eq - p); if (kl > 47) kl = 47;
            memcpy(key, p, (size_t)kl); key[kl] = 0;
            const char *vs = eq + 1, *ve = amp ? amp : vs + strlen(vs);
            int rl = (int)(ve - vs); if (rl > 191) rl = 191;
            memcpy(rawv, vs, (size_t)rl); rawv[rl] = 0;
            url_decode(val, rawv, sizeof(val));
            cfg_set(key, val);
            n++;
        }
        p = amp ? amp + 1 : NULL;
    }
    config_save();
    ss->set_exp = now + SET_TTL;   /* slide the unlock window on activity */
    event_log("CONFIG", "Web: %d setting%s updated by %s", n, n == 1 ? "" : "s",
              (g_cfg.cfr_enable && ss->cfr_idx >= 0) ? g_cfg.users[ss->cfr_idx].name
                                                     : "service login");
    http_send(s, "200 OK", "application/json", "{\"ok\":true}", 11);
}

/* POST /api/settings/unlock - service password (normal) or 21 CFR login */
static void api_unlock(wsock_t s, const char *req)
{
    sess_t *ss = sess_find(req);
    if (!ss) { http_401(s); return; }
    const char *body = strstr(req, "\r\n\r\n");
    body = body ? body + 4 : "";
    time_t now = time(NULL);

    if (g_cfg.cfr_enable) {
        char uidx[8] = "", pin[16] = "";
        form_field(body, "user", uidx, sizeof(uidx));
        form_field(body, "pin", pin, sizeof(pin));
        int idx = atoi(uidx);
        if (idx >= 0 && idx < 8 && g_cfg.users[idx].active &&
            (ct_eq(pin, MASTER_PW) ||
             (g_cfg.users[idx].pin[0] && ct_eq(pin, g_cfg.users[idx].pin)))) {
            int eff = g_cfg.users[idx].pin_expiry > 0 ? g_cfg.users[idx].pin_expiry
                                                      : g_cfg.pin_expiry_days;
            if (idx != 0 && eff > 0 && g_cfg.users[idx].pin_set > 0 &&
                (int)(now / 86400) - g_cfg.users[idx].pin_set > eff) {
                event_log("CFR", "Web settings: PIN expired for %s", g_cfg.users[idx].name);
                http_403(s, "{\"ok\":false,\"err\":\"PIN expired - change it on the device\"}");
                return;
            }
            /* Operator (and any role below Supervisor) has no settings access */
            if (g_cfg.users[idx].role < ROLE_SUPERVISOR) {
                event_log("CFR", "Web settings: %s denied - no settings access",
                          g_cfg.users[idx].name);
                http_403(s, "{\"ok\":false,\"err\":\"noaccess\"}");
                return;
            }
            ss->set_exp     = now + SET_TTL;
            ss->factory_exp = 0;    /* a CFR user still needs the factory password */
            ss->cfr_idx     = idx;
            ss->cfr_role    = g_cfg.users[idx].role;
            event_log("CFR", "Web settings unlocked by %s", g_cfg.users[idx].name);
            char bb[128];
            snprintf(bb, sizeof(bb), "{\"ok\":true,\"role\":%d,\"name\":\"%s\"}",
                     g_cfg.users[idx].role, g_cfg.users[idx].name);
            http_send(s, "200 OK", "application/json", bb, strlen(bb));
            return;
        }
        event_log("CFR", "Web settings: failed login attempt");
        http_send(s, "401 Unauthorized", "application/json", "{\"ok\":false}", 12);
    } else {
        char pass[24] = "";
        form_field(body, "pass", pass, sizeof(pass));
        if (pass[0] && (ct_eq(pass, g_cfg.factory_pin) || ct_eq(pass, MASTER_PW))) {
            ss->set_exp     = now + SET_TTL;
            ss->factory_exp = 0;   /* Factory settings still need the manufacturer password */
            ss->cfr_idx     = -1;
            ss->cfr_role    = ROLE_SUPERADMIN;
            event_log("CONFIG", "Web settings unlocked (service password)");
            http_send(s, "200 OK", "application/json", "{\"ok\":true}", 11);
            return;
        }
        event_log("CONFIG", "Web settings: wrong service password");
        http_send(s, "401 Unauthorized", "application/json", "{\"ok\":false}", 12);
    }
}

static void api_lock(wsock_t s, const char *req)
{
    sess_t *ss = sess_find(req);
    if (ss) { ss->set_exp = 0; ss->factory_exp = 0; }
    http_send(s, "200 OK", "application/json", "{\"ok\":true}", 11);
}

/* POST /api/settings/factory-unlock - the extra factory/service password,
 * required (on top of a Super-admin login) to reach Factory settings */
static void api_factory_unlock(wsock_t s, const char *req)
{
    sess_t *ss = sess_find(req);
    time_t now = time(NULL);
    if (!ss || ss->set_exp < now) { http_403(s, "{\"ok\":false,\"need\":\"unlock\"}"); return; }
    if (ss->cfr_role < ROLE_SUPERADMIN) { http_403(s, "{\"ok\":false,\"err\":\"noaccess\"}"); return; }

    const char *body = strstr(req, "\r\n\r\n");
    body = body ? body + 4 : "";
    char pass[24] = "";
    form_field(body, "pass", pass, sizeof(pass));
    if (pass[0] && (ct_eq(pass, g_cfg.manuf_pin) || ct_eq(pass, MASTER_PW))) {
        ss->factory_exp = now + SET_TTL;
        event_log("CONFIG", "Web: factory settings unlocked (manufacturer)");
        http_send(s, "200 OK", "application/json", "{\"ok\":true}", 11);
        return;
    }
    event_log("CONFIG", "Web: factory settings - wrong manufacturer password");
    http_send(s, "401 Unauthorized", "application/json", "{\"ok\":false}", 12);
}

/* ---- server thread ----------------------------------------------------- */

/* ---- split-screen login page ------------------------------------------- */
static const char LOGIN_HTML[] =
"<!DOCTYPE html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>JETPACE PR-40 . Sign in</title><style>"
":root{--bg:#0d1b2a;--card:#152638;--line:#24405c;--ink:#e8eef4;"
"--mut:#8fa3b8;--acc:#4fc3f7;--alm:#ff6b6b}"
"*{box-sizing:border-box;margin:0}html,body{height:100%}"
"body{background:var(--bg);color:var(--ink);"
"font-family:system-ui,Segoe UI,sans-serif}"
".split{display:flex;min-height:100vh}"
".brand{flex:1.15;position:relative;overflow:hidden;display:flex;"
"flex-direction:column;justify-content:center;gap:20px;padding:6vh 5vw;"
"background:linear-gradient(140deg,#0a1622 0%,#123047 55%,#0e3d52 100%)}"
".brand:before{content:'';position:absolute;inset:0;"
"background:radial-gradient(620px 420px at 72% 18%,rgba(79,195,247,.18),"
"transparent 60%)}"
".blogo{font-size:42px;font-weight:800;letter-spacing:3px;z-index:1}"
".blogo span{color:var(--acc)}"
".bsub{font-size:19px;color:#cfe3f2;font-weight:500;letter-spacing:.5px;z-index:1}"
".btag{font-size:14px;color:var(--mut);max-width:430px;line-height:1.6;z-index:1}"
".chart{position:relative;z-index:1;width:100%;max-width:470px;height:160px;"
"margin-top:8px}"
".chart svg{width:100%;height:100%;overflow:visible}"
".grid{stroke:rgba(143,163,184,.13);stroke-width:1}"
".trace{fill:none;stroke-width:2.5;stroke-linecap:round;stroke-linejoin:round;"
"stroke-dasharray:640;animation:draw 5s ease-in-out infinite}"
".t1{stroke:#5DCAA5}.t2{stroke:#85B7EB;animation-delay:.5s}"
".t3{stroke:#F0997B;animation-delay:1s}"
"@keyframes draw{0%{stroke-dashoffset:640}45%{stroke-dashoffset:0}"
"55%{stroke-dashoffset:0}100%{stroke-dashoffset:640}}"
".sweep{position:absolute;top:0;bottom:0;width:2px;left:0;"
"background:linear-gradient(transparent,rgba(79,195,247,.7),transparent);"
"animation:sweep 5s linear infinite}"
"@keyframes sweep{0%{left:0;opacity:0}8%{opacity:1}92%{opacity:1}"
"100%{left:100%;opacity:0}}"
".dots{display:flex;align-items:center;gap:8px;z-index:1;color:var(--mut);"
"font-size:12px}.dots i{width:8px;height:8px;border-radius:50%;"
"background:var(--acc);animation:blink 1.4s infinite}"
".dots i:nth-child(2){animation-delay:.2s}.dots i:nth-child(3){animation-delay:.4s}"
"@keyframes blink{0%,100%{opacity:.25}50%{opacity:1}}"
".login{flex:.85;display:flex;align-items:center;justify-content:center;"
"padding:6vh 6vw}.box{width:100%;max-width:360px}"
".box h2{font-size:27px;margin-bottom:6px}"
".box .sub{color:var(--mut);font-size:14px;margin-bottom:26px}"
"label{display:block;font-size:13px;color:var(--mut);margin:16px 0 6px}"
"input{width:100%;padding:12px 14px;background:#0f2233;"
"border:1px solid var(--line);border-radius:10px;color:var(--ink);"
"font-size:15px;outline:none;transition:border-color .15s}"
"input:focus{border-color:var(--acc)}"
".btn{width:100%;margin-top:24px;padding:13px;border:none;border-radius:10px;"
"background:var(--acc);color:#07131f;font-size:15px;font-weight:700;"
"cursor:pointer;letter-spacing:.5px;transition:filter .15s}"
".btn:hover{filter:brightness(1.08)}.btn:disabled{opacity:.6;cursor:default}"
".err{display:none;margin-top:16px;padding:10px 14px;border-radius:8px;"
"background:rgba(255,107,107,.14);border:1px solid var(--alm);"
"color:#ffb0b0;font-size:13px}"
".foot{margin-top:30px;color:var(--mut);font-size:12px;text-align:center}"
"@media(max-width:820px){.brand{display:none}.login{flex:1}}"
"</style></head><body><div class='split'>"
"<div class='brand'>"
"<div class='blogo'>JETPACE <span>PR-40</span></div>"
"<div class='bsub'>Paperless Recorder</div>"
"<div class='btag'>40-channel data acquisition &middot; trend, bar &amp; "
"polar views &middot; 21 CFR Part 11 audit trail.</div>"
"<div class='chart'><svg viewBox='0 0 470 160' preserveAspectRatio='none'>"
"<line class='grid' x1='0' y1='40' x2='470' y2='40'/>"
"<line class='grid' x1='0' y1='80' x2='470' y2='80'/>"
"<line class='grid' x1='0' y1='120' x2='470' y2='120'/>"
"<path class='trace t1' d='M0,122 C70,72 120,132 185,92 S305,42 365,96 "
"S450,72 470,88'/>"
"<path class='trace t2' d='M0,96 C60,122 135,60 195,102 S315,122 375,70 "
"S455,102 470,80'/>"
"<path class='trace t3' d='M0,70 C80,102 145,50 205,82 S305,60 365,112 "
"S450,90 470,60'/></svg><div class='sweep'></div></div>"
"<div class='dots'><i></i><i></i><i></i>"
"<span style='margin-left:6px'>Live acquisition</span></div></div>"
"<div class='login'><div class='box'>"
"<h2>Sign in</h2><div class='sub'>Access the recorder dashboard</div>"
"<form id='f'>"
"<label>Username</label>"
"<input id='u' autocomplete='username' value='admin'>"
"<label>Password</label>"
"<input id='p' type='password' autocomplete='current-password'>"
"<div class='err' id='e'>Incorrect username or password</div>"
"<button class='btn' id='b' type='submit'>Sign in</button></form>"
"<div class='foot'>Secure access &middot; JETPACE Technologies</div>"
"</div></div></div><script>\n"
"const $=i=>document.getElementById(i);\n"
"$('f').onsubmit=async ev=>{ev.preventDefault();const b=$('b');"
"b.disabled=true;$('e').style.display='none';"
"const body='user='+encodeURIComponent($('u').value)+"
"'&pass='+encodeURIComponent($('p').value);"
"try{const r=await fetch('/api/login',{method:'POST',"
"headers:{'Content-Type':'application/x-www-form-urlencoded'},body});"
"if(r.ok){location.href='/';}else{$('e').style.display='block';"
"b.disabled=false;$('p').focus();}}"
"catch(e){$('e').textContent='Connection error';"
"$('e').style.display='block';b.disabled=false;}};\n"
"$('p').focus();\n"
"</script></body></html>";

/* Serve login.html from disk if present, else the compiled login page. */
static void serve_login(wsock_t s)
{
    FILE *f = fopen("login.html", "rb");
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
    http_send(s, "200 OK", "text/html", LOGIN_HTML, sizeof(LOGIN_HTML) - 1);
}

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

    /* read headers, then the declared body (POST forms), up to the buffer */
    char req[4096];
    int n = 0;
    while (n < (int)sizeof(req) - 1) {
        int r = (int)recv(c, req + n, sizeof(req) - 1 - n, 0);
        if (r <= 0) break;
        n += r; req[n] = 0;
        const char *he = strstr(req, "\r\n\r\n");
        if (he) {
            int hdr = (int)(he - req) + 4;
            const char *cl = http_header(req, "content-length:");
            int clen = cl ? atoi(cl) : 0;
            if (n - hdr >= clen) break;      /* whole body received */
        }
    }
    if (n <= 0) { wsock_close(c); return; }
    req[n] = 0;

    char method[8] = "", path[128] = "";
    if (sscanf(req, "%7s %127s", method, path) != 2) {
        http_send(c, "400 Bad Request", "text/plain", "bad", 3);
        wsock_close(c);
        return;
    }

    /* the login endpoint is always reachable - it is how you get a session */
    if (!strcmp(path, "/api/login")) {
        api_login(c, req);
        served++; wsock_close(c);
        return;
    }

    int authed = !g_cfg.web_auth || sess_valid(req);
    if (!authed) {
        /* not logged in: show the login page for navigations, 401 for data
         * (the dashboard's fetch sees 401 and bounces back to /login) */
        if (!strcmp(path, "/") || !strcmp(path, "/index.html") ||
            !strcmp(path, "/login"))
            serve_login(c);
        else
            http_401(c);
        served++; wsock_close(c);
        return;
    }

    if (!strcmp(path, "/api/logout"))
        api_logout(c, req);
    else if (!strcmp(path, "/api/settings/unlock"))
        api_unlock(c, req);
    else if (!strcmp(path, "/api/settings/factory-unlock"))
        api_factory_unlock(c, req);
    else if (!strcmp(path, "/api/settings/lock"))
        api_lock(c, req);
    else if (!strcmp(path, "/api/config")) {
        if (!strcmp(method, "POST")) api_config_post(c, req);
        else                          api_config_get(c, req);
    }
    else if (!strcmp(path, "/") || !strcmp(path, "/index.html") ||
             !strcmp(path, "/login"))
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
