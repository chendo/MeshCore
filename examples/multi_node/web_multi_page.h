#pragma once

// The unified web panel single-page app, served at "/". Extracted from
// web_multi.cpp to keep handlers and markup separately editable.

static const char MULTI_PAGE[] = R"MWPG(<!DOCTYPE html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>MeshCore Multi</title>
<style>
:root{--bg:#101418;--card:#1a2027;--line:#2a323c;--tx:#dbe4ee;--mut:#8b98a8;--acc:#4da3ff;--ok:#39c07a;--err:#e05d5d}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--tx);font:14px/1.45 system-ui,sans-serif}
#shell{display:grid;grid-template-columns:216px 1fr;min-height:100vh}
aside{border-right:1px solid var(--line);padding:12px 10px;display:flex;flex-direction:column;gap:10px;
  position:sticky;top:0;height:100vh;overflow-y:auto;box-sizing:border-box}
aside .brand{font-weight:700;font-size:15px;padding:2px 6px}
.role{background:var(--card);border:1px solid var(--line);border-radius:8px;padding:8px 10px;font-size:12px}
.role .rname{font-weight:600;display:flex;align-items:center;gap:6px}
.role .dot{width:8px;height:8px;border-radius:50%;background:var(--ok);flex:none}
.role .dot.off{background:#5c6875}
.role .dot.warn{background:#e0b34d}
.role .rkey{font-family:ui-monospace,monospace;font-size:10px;color:var(--mut);cursor:pointer;word-break:break-all}
.role .rstat{color:var(--mut);margin-top:2px}
nav{display:flex;flex-direction:column;gap:2px;margin-top:4px}
nav button{background:none;border:0;color:var(--mut);padding:7px 10px;border-radius:6px;cursor:pointer;
  text-align:left;font-size:13px}
nav button.on{color:var(--tx);background:#1e2731}
main{padding:14px 16px;max-width:1120px;box-sizing:border-box;min-width:0}
@media (max-width:760px){#shell{grid-template-columns:1fr}aside{position:static;height:auto;flex-direction:row;
  flex-wrap:wrap;align-items:center}nav{flex-direction:row;flex-wrap:wrap}aside .roles{display:flex;gap:6px;flex-wrap:wrap}}
.card{background:var(--card);border:1px solid var(--line);border-radius:8px;padding:12px;margin-bottom:12px}
.card h3{margin:0 0 8px;font-size:13px;color:var(--mut);text-transform:uppercase;letter-spacing:.06em}
table{width:100%;border-collapse:collapse;font-size:13px}
th,td{text-align:left;padding:3px 8px;border-bottom:1px solid var(--line);white-space:nowrap}
th{color:var(--mut);font-weight:500}
pre{margin:0;font:12px/1.5 ui-monospace,monospace;white-space:pre-wrap;word-break:break-all}
input,textarea,select{background:#12171d;color:var(--tx);border:1px solid var(--line);border-radius:6px;padding:7px 9px;font:inherit}
button.act{background:var(--acc);border:0;color:#08131f;padding:7px 14px;border-radius:6px;cursor:pointer;font-weight:600}
button.sec{background:none;border:1px solid var(--line);color:var(--tx);padding:7px 12px;border-radius:6px;cursor:pointer}
.row{display:flex;gap:8px;flex-wrap:wrap;align-items:center;margin-bottom:8px}
.grow{flex:1;min-width:120px}
.mut{color:var(--mut)}.ok{color:var(--ok)}.err{color:var(--err)}
.msg{padding:6px 9px;border-radius:6px;margin-bottom:6px;background:#12171d;border:1px solid var(--line)}
.msg .who{font-weight:600;color:var(--acc)}.msg.out .who{color:var(--ok)}
.msg .meta{font-size:11px;color:var(--mut);margin-left:6px}
#login{position:fixed;inset:0;background:rgba(6,9,12,.92);display:flex;align-items:center;justify-content:center}
#login .card{width:280px}
.list div{padding:6px 8px;border-radius:6px;cursor:pointer}
.list div:hover{background:#232b34}.list div.sel{background:#213247}
.tabpane{display:none}.tabpane.on{display:block}
.warn{background:#3a2d16;border:1px solid #6b5320;color:#e8c874;padding:8px 10px;border-radius:6px;margin-bottom:10px;font-size:13px}
.hop{cursor:help;border-bottom:1px dotted #5c6875}
.relay{display:inline-block;padding:0 5px;border-radius:4px;font-size:11px;font-weight:600;margin-right:6px}
.relay.sure{background:#14361f;color:#5fd694;border:1px solid #2c6b45}
.relay.weak{background:#3a2d16;color:#e8c874;border:1px solid #6b5320}
.relay.direct{background:#12293d;color:#6cb6f0;border:1px solid #2a5a80}
/* collisions, marked on the airtime itself: a proven overlap carries the ms it
   lost, a suspicion carries only the mark, because the two are different
   strengths of evidence and should not look alike */
.collx{color:#e05d5d;font-weight:700;cursor:help;margin-left:4px}
.collx.maybe{color:#d99b6c}
tr.relayrow td{background:#12211a}
/* heard straight off the air — no repeater in between. The left edge marks it
   so a burst of direct traffic is visible without reading every row. */
tr.directrow td{background:#111c26}
tr.directrow td:first-child{box-shadow:inset 3px 0 0 #3d7fb3}
/* filter applies to rows added later too, so it survives live polling */
tbody.directonly tr:not(.directrow){display:none}
.tiles{display:grid;grid-template-columns:repeat(auto-fill,minmax(148px,1fr));gap:8px;margin-bottom:4px}
.tile{background:#12171d;border:1px solid var(--line);border-radius:8px;padding:10px 12px}
.tile .val{font-size:21px;font-weight:650;margin-top:2px}
.tile .lbl{font-size:11px;color:var(--mut);text-transform:uppercase;letter-spacing:.05em}
.tile .sub{font-size:11px;color:var(--mut)}
</style></head><body>
<div id="shell">
<aside>
  <div class="brand" id="panel-title">MeshCore Multi</div>
  <div class="roles" id="rail-roles"></div>
  <nav>
    <button data-t="dash" class="on">System</button>
    <button data-t="mesh">Mesh</button>
    <button data-t="msgs">Messages</button>
    <button data-t="chans">Channels</button>
    <button data-t="room">Room</button>
    <button data-t="rep">Repeater</button>
    <button data-t="radio">Radio</button>
    <button data-t="pkts">Packets</button>
    <button data-t="debug">Debug</button>
    <button data-t="set">Settings</button>
  </nav>
</aside>
<main>

<div id="tab-dash" class="tabpane on">
  <div class="tiles" id="dash-tiles"></div>
  <div class="card"><h3>Trends</h3>
    <div class="row" style="gap:20px;align-items:flex-start">
      <div class="grow"><div class="mut" style="font-size:12px">battery (mV) <span id="dv-battery" class="mut"></span></div>
        <canvas id="dc-battery" width="420" height="60" style="width:100%"></canvas></div>
      <div class="grow"><div class="mut" style="font-size:12px">radio packets / min <span id="dv-packets" class="mut"></span></div>
        <canvas id="dc-packets" width="420" height="60" style="width:100%"></canvas></div>
    </div>
  </div>
  <div class="card"><h3>Node</h3><pre id="dash-info" class="mut">loading...</pre></div>
  <div class="card"><h3>Actions</h3>
    <div class="row">
      <button class="act" onclick="dashCmd('advert','repeater advert')">Repeater advert</button>
      <button class="act" onclick="dashCmd('advert','room advert')">Room advert</button>
      <button class="sec" onclick="if(confirm('Reboot the node?'))dashCmd('reboot','reboot')">Reboot</button>
      <button class="sec" onclick="logout()">Log out</button>
    </div>
    <div id="dash-status" class="mut" style="font-size:12px"></div>
  </div>
  <div class="card"><h3>Info</h3>
    <div class="row"><span class="mut" style="width:110px">Firmware</span><span id="dash-ver">-</span></div>
    <div class="row"><span class="mut" style="width:110px">Repeater key</span>
      <span id="dash-pubkey" style="font-family:monospace;font-size:11px;word-break:break-all">-</span>
      <button class="sec" style="padding:2px 8px" onclick="copyText($('dash-pubkey').textContent)">copy</button></div>
    <div class="row"><span class="mut" style="width:110px">Radio</span><span id="dash-radio">-</span></div>
  </div>
  <div class="card"><h3>Nodes in radio reach <span class="mut" id="nearby-count" style="font-weight:400;font-size:11px"></span></h3>
    <div class="mut" style="font-size:11px;margin-bottom:6px">
      learned from routing paths &mdash; <b>we hear</b> counts frames where the node was the
      last forwarder (so its SNR is our link to it); <b>hears us</b> counts times it relayed
      one of ours, and only 2-byte hash matches count &mdash; 1 byte is 1-in-256 and shown as "?".
      <b>clock</b> is that node's time minus ours, read from its own signed advert timestamp and
      only ever from a zero-hop advert &mdash; a relayed one would measure flood delay instead
    </div>
    <div style="overflow-x:auto"><table><thead><tr><th>node</th><th>hops</th><th>we hear</th>
      <th>hears us</th><th>SNR</th><th>relays</th><th>dist</th><th>clock</th><th>last</th></tr></thead>
    <tbody id="dash-nearby"><tr><td colspan=9 class=mut>listening...</td></tr></tbody></table></div>
  </div>
  <div class="card"><h3>GPS &amp; clock <span class="mut" id="gps-hdr" style="font-weight:400;font-size:11px"></span></h3>
    <div id="gps-body" class="mut">-</div>
  </div>
  <div class="card"><h3>Identities <button class="sec" style="float:right;padding:2px 8px" onclick="loadStatsTab()">&#8635;</button>
    <span class="mut" style="font-weight:400;font-size:11px">every identity, from the shared radio itself</span></h3>
  <div style="overflow-x:auto"><table><thead><tr><th>identity</th><th>state</th><th>last activity</th><th>rx</th><th>tx</th>
    <th>floods sent</th><th>heard back</th><th>deferred</th><th>flood rx</th><th>rx errors</th><th>pool full</th><th>queue</th></tr></thead>
  <tbody id="stats-live"></tbody></table></div>
  <pre id="stats-node" class="mut" style="margin-top:8px"></pre></div>
  <div class="card"><h3>History (24h, 1-min samples)</h3><div id="stats-charts"></div></div>
</div>

<div id="tab-rep" class="tabpane">
  <div class="card"><h3>Repeater settings</h3>
    <div class="row"><input id="rep-name" class="grow" placeholder="node name">
      <button class="sec" onclick="fieldSave('set name '+v('rep-name'),'rep-status')">Set name</button>
      <button class="sec" onclick="fieldLoad('get name','rep-name')">&#8635;</button></div>
    <div class="row"><input id="rep-clock" class="grow" readonly placeholder="clock">
      <button class="sec" onclick="fieldLoad('clock','rep-clock')">&#8635;</button>
      <button class="sec" onclick="syncClock()">Sync from browser</button></div>
    <div class="row"><input id="rep-lat" placeholder="lat"><input id="rep-lon" placeholder="lon">
      <button class="sec" onclick="fieldSave('set lat '+v('rep-lat'),'rep-status').then(()=>fieldSave('set lon '+v('rep-lon'),'rep-status'))">Set location</button>
      <button class="sec" onclick="fieldLoad('get lat','rep-lat');fieldLoad('get lon','rep-lon')">&#8635;</button></div>
    <div class="row"><textarea id="rep-owner" class="grow" rows="2" placeholder="owner info"></textarea>
      <button class="sec" onclick="fieldSave('set owner.info '+v('rep-owner'),'rep-status')">Save owner</button>
      <button class="sec" onclick="fieldLoad('get owner.info','rep-owner')">&#8635;</button></div>
    <div class="row"><input id="rep-prv" class="grow" type="password" placeholder="private key (hex)" autocomplete="off" data-1p-ignore>
      <button class="sec" onclick="fieldLoad('get prv.key','rep-prv')">Reveal</button>
      <button class="sec" onclick="copyText(v('rep-prv'))">Copy</button>
      <button class="sec" onclick="if(confirm('Overwrite the repeater private key?'))fieldSave('set prv.key '+v('rep-prv'),'rep-status')">Set</button></div>
    <div id="rep-status" class="mut" style="font-size:12px"></div>
  </div>
  <div class="card"><h3>Access</h3>
    <div class="row"><input id="rep-admin" class="grow" placeholder="new admin password" autocomplete="off" data-1p-ignore>
      <button class="sec" onclick="fieldSave('password '+v('rep-admin'),'rep-status')">Set admin password</button></div>
    <div class="row"><input id="rep-guest" class="grow" placeholder="new guest password" autocomplete="off" data-1p-ignore>
      <button class="sec" onclick="fieldSave('set guest.password '+v('rep-guest'),'rep-status')">Set guest password</button></div>
  </div>
  <div class="card"><h3>Advertising</h3>
    <div class="row"><span class="mut" style="width:170px">advert interval (min)</span><input id="rep-advint" style="width:90px">
      <button class="sec" onclick="fieldSave('set advert.interval '+v('rep-advint'),'rep-status')">Save</button>
      <button class="sec" onclick="fieldLoad('get advert.interval','rep-advint')">&#8635;</button></div>
    <div class="row"><span class="mut" style="width:170px">flood advert interval (h)</span><input id="rep-floodint" style="width:90px">
      <button class="sec" onclick="fieldSave('set flood.advert.interval '+v('rep-floodint'),'rep-status')">Save</button>
      <button class="sec" onclick="fieldLoad('get flood.advert.interval','rep-floodint')">&#8635;</button></div>
    <div class="row"><span class="mut" style="width:170px">flood max hops</span><input id="rep-floodmax" style="width:90px">
      <button class="sec" onclick="fieldSave('set flood.max '+v('rep-floodmax'),'rep-status')">Save</button>
      <button class="sec" onclick="fieldLoad('get flood.max','rep-floodmax')">&#8635;</button></div>
    <div class="row"><span class="mut" style="width:170px">flood max unscoped</span><input id="rep-floodmaxu" style="width:90px">
      <button class="sec" onclick="fieldSave('set flood.max.unscoped '+v('rep-floodmaxu'),'rep-status')">Save</button>
      <button class="sec" onclick="fieldLoad('get flood.max.unscoped','rep-floodmaxu')">&#8635;</button></div>
  </div>
  <div class="card"><h3>Ghost node mode</h3>
    <div class="mut" style="font-size:12px;margin-bottom:6px">Ghost mode stops the repeater forwarding and advertising
    (set repeat off, advert intervals 0). Turning it off restores the previous values.</div>
    <div class="row"><label><input type="checkbox" id="rep-ghost" onchange="ghostToggle(this.checked)"> ghost mode</label>
      <span id="rep-ghost-status" class="mut" style="font-size:12px"></span></div>
  </div>
</div>

<div id="tab-radio" class="tabpane">
  <div class="card"><h3>Shared radio</h3>
    <div class="mut" style="font-size:12px;margin-bottom:6px">One radio serves all three identities. Changes apply
    LIVE and persist — no reboot needed.</div>
    <div class="row"><span class="mut" style="width:110px">Current</span><span id="radio-cur">-</span>
      <button class="sec" onclick="radioRefresh()">&#8635;</button></div>
    <div class="row"><select id="radio-preset" class="grow"><option value="">Loading presets...</option></select>
      <button class="sec" onclick="loadPresets()">&#8635;</button>
      <button class="act" onclick="applyPreset()">Apply preset</button></div>
    <div class="row"><input id="radio-manual" class="grow" placeholder="manual: freq,bw,sf,cr  e.g. 916.575,62.5,7,8">
      <button class="sec" onclick="applyManualRadio()">Apply</button></div>
    <div id="radio-status" class="mut" style="font-size:12px"></div>
  </div>
  <div class="card"><h3>Routing / region</h3>
    <div class="row"><span class="mut" style="width:140px">path hash mode</span>
      <select id="radio-phm" title="firmware stores this 0-based: hash size = mode + 1 byte">
        <option value="0">1-byte hashes (mode 0)</option>
        <option value="1">2-byte hashes (mode 1)</option>
        <option value="2">3-byte hashes (mode 2)</option></select>
      <button class="sec" onclick="fieldSave('set path.hash.mode '+v('radio-phm'),'radio-status')">Save</button>
      <button class="sec" onclick="fieldLoad('get path.hash.mode','radio-phm')">&#8635;</button></div>
    <div class="row"><span class="mut" style="width:140px">region</span>
      <select id="radio-region" class="grow"><option value="">-</option></select>
      <button class="sec" onclick="saveRegion()">Save region</button></div>
  </div>
</div>


<div id="tab-set" class="tabpane">
  <div class="card"><h3>Firmware update (OTA)</h3>
    <div class="mut" style="font-size:12px;margin-bottom:6px">Upload a ThinkNode_M5_multi <b>firmware.bin</b>. The device
    writes it to the spare OTA slot, verifies, and reboots. Identities and settings are preserved.</div>
    <div class="row"><input type="file" id="fw-file" accept=".bin" class="grow">
      <button class="act" id="fw-btn" onclick="fwUpload()">Upload &amp; flash</button></div>
    <div id="fw-status" class="mut" style="font-size:12px"></div>
  </div>
  <div class="card"><h3>Power</h3>
    <div class="mut" style="font-size:12px;margin-bottom:6px">WiFi modem sleep is the single biggest saver (~half the
    idle draw); "min" adds a few ms of panel latency. GPS is unnecessary on a fixed node with lat/lon set — the
    hardware RTC keeps time.</div>
    <div class="row"><span class="mut" style="width:140px">WiFi powersave</span>
      <select id="pw-wifi"><option value="none">none (full power)</option>
        <option value="min">min (recommended)</option><option value="max">max</option></select>
      <button class="sec" onclick="fieldSave('set wifi.powersave '+v('pw-wifi'),'pw-status')">Apply</button></div>
    <div class="row"><span class="mut" style="width:140px">GPS</span>
      <button class="sec" onclick="fieldSave('gps off','pw-status')">Power down</button>
      <button class="sec" onclick="fieldSave('gps on','pw-status')">Power up</button></div>
    <div id="pw-status" class="mut" style="font-size:12px"></div>
  </div>
  <div class="card"><h3>MQTT uplink <span class="mut" id="mqtt-state" style="font-weight:400;font-size:11px"></span></h3>
    <div class="mut" style="font-size:12px;margin-bottom:8px">Publishes what this node hears to an MQTT broker.
    Authentication to the curated brokers uses a <b>token signed by this node's own mesh identity</b> — there is no
    password to set; the broker verifies the signature against the public key in the token. A <b>custom</b> broker
    skips that and uses the username/password below instead.</div>
    <div id="mqtt-brokers" style="margin-bottom:8px"></div>
    <div class="row" style="gap:6px;flex-wrap:wrap">
      <input id="mq-host" placeholder="custom host" style="width:190px" data-1p-ignore>
      <input id="mq-port" placeholder="port" style="width:70px" data-1p-ignore>
      <select id="mq-transport" style="width:90px"><option value="tcp">mqtt://</option><option value="wss">wss://</option></select>
    </div>
    <div class="row" style="gap:6px;flex-wrap:wrap;margin-top:4px">
      <input id="mq-user" placeholder="username" style="width:150px" data-1p-ignore>
      <input id="mq-pass" type="password" placeholder="password (write-only)" style="width:190px" data-1p-ignore>
      <button onclick="mqttSaveCustom()">save custom broker</button>
    </div>
    <div class="row" style="gap:6px;flex-wrap:wrap;margin-top:6px">
      <input id="mq-iata" placeholder="IATA" style="width:80px" data-1p-ignore>
      <input id="mq-email" placeholder="owner email" style="width:190px" data-1p-ignore>
      <button onclick="mqttSaveMeta()">save</button>
      <span id="mqtt-status" class="mut" style="font-size:12px"></span>
    </div>
  </div>
  <div class="card"><h3>Identity slots</h3>
    <div class="mut" style="font-size:12px;margin-bottom:8px">Five spare radio ports, each able to run a full extra
    identity with its own keypair and storage. A <b>chat</b> slot is a companion node driven by the phone app on its own
    TCP port; a <b>room</b> slot is a room server managed here. Switching a slot <b>off</b> takes effect immediately;
    changing it between chat and room needs a <b>reboot</b>, because the running node is the wrong class to convert in
    place. A slot's identity and files are keyed to the slot, not to its type, so retyping never loses a keypair — and a
    slot that is off keeps everything on the filesystem, ready to come back.</div>
    <div id="slots-list" class="mut" style="font-size:13px">loading...</div>
    <div id="slots-status" class="mut" style="font-size:12px;margin-top:6px"></div>
    <div id="slot-admin" style="margin-top:10px"></div>
  </div>
  <div class="card"><h3>Panel</h3>
    <div class="row"><a href="/app" style="color:var(--acc)">Classic panel (/app)</a>
      <span class="mut">·</span><a href="/stats" style="color:var(--acc)">Legacy stats (/stats)</a></div>
    <div class="row"><button class="sec" onclick="logout()">Log out</button></div>
  </div>
</div>

<div id="tab-debug" class="tabpane">
  <div class="card"><h3>Node</h3><pre id="dbg-info" class="mut">loading...</pre></div>
  <div class="card"><h3>Console</h3>
    <div class="row">
      <select id="cli-target" style="width:130px"><option value="">composition</option>
        <option value="repeater ">repeater</option><option value="room ">room</option>
        <option value="companion ">companion</option></select>
      <input id="cli-cmd" class="grow" placeholder="command, e.g. get name / identities / neighbors"
        onkeydown="cliKey(event)">
      <button class="act" onclick="cliRun()">Run</button></div>
    <pre id="cli-out" class="mut"></pre></div>
  <div class="card"><h3>Identity stats</h3>
  <div style="overflow-x:auto"><table><thead><tr><th>identity</th><th>state</th><th>last activity</th><th>rx</th><th>tx</th>
    <th>floods sent</th><th>heard back</th><th>deferred</th><th>flood rx</th><th>rx errors</th><th>pool full</th><th>queue</th></tr></thead>
  <tbody id="dbg-stats"></tbody></table></div></div>
  <div class="card"><h3>Nodes nearby (repeater neighbours)</h3><pre id="dbg-nbrs" class="mut">-</pre></div>
</div>

<div id="tab-pkts" class="tabpane">
  <div class="card"><h3>Radio packets <span class="mut" id="pkt-count"></span>
    <label style="float:right;font-weight:400;font-size:12px;cursor:pointer">
      <input type="checkbox" id="pkt-direct-only" onchange="applyPktFilter()"> direct only</label></h3>
    <div class="mut" style="font-size:11px;margin-bottom:6px">
      <span class="relay direct">&#9679; direct</span> heard straight off the air, zero hops — the sender is in RF range of this node ·
      <span class="relay sure">&#8618; relayed</span> a neighbour passed on something we sent (2-byte+ hash — certain) ·
      <span class="relay weak">&#8618; relayed?</span> same, but a 1-byte hash, so ~1 in 256 could be coincidence
    </div>
  <div style="margin:8px 0 10px">
    <canvas id="air-tl" height="118" style="width:100%;height:118px;display:block"></canvas>
    <div class="mut" style="font-size:11px;margin-top:3px" id="air-tl-legend">collecting…</div>
  </div>
  <div style="overflow-x:auto"><table><thead><tr><th>time</th><th>air</th><th>CR</th><th>SNR</th><th>RSSI</th><th>len</th><th>dir</th><th>route</th><th>type</th><th>hash</th><th>src</th><th>info</th></tr></thead>
  <tbody id="pkt-rows"></tbody></table></div></div>
</div>

<div id="tab-mesh" class="tabpane">
  <div class="card"><h3>Routing map
    <span style="float:right">
      <button class="sec" style="padding:2px 10px" id="view-log" onclick="meshView('log')">Logical</button>
      <button class="sec" style="padding:2px 10px" id="view-geo" onclick="meshView('geo')">Geo</button>
      <button class="sec" style="padding:2px 8px" onclick="buildMesh()">&#8635;</button>
    </span></h3>
    <div class="mut" style="font-size:12px;margin-bottom:6px">Solid: known direct routes (from contact paths and heard
    neighbours). Hop nodes show the 1-byte path hash; a name is attached when exactly one known node matches it.
    Dashed: contacts with no learned route (flood only). Use a node's <b>trace</b> button to measure per-hop SNR —
    the traced route lights up on the map.</div>
    <div id="trace-status" class="mut" style="font-size:12px;margin-bottom:6px"></div>
    <div id="mesh-log-wrap" style="overflow:auto"><svg id="mesh-svg" width="900" height="480" style="min-width:700px"></svg></div>
    <div id="mesh-geo-wrap" style="display:none"><div id="geo-map" style="height:480px;border-radius:8px"></div></div>
  </div>
  <div class="card"><h3>Known nodes</h3>
  <div style="overflow-x:auto"><table><thead><tr><th>name</th><th>pubkey</th><th>kind</th><th>route</th><th>last heard/advert</th><th>location</th><th>dist</th><th>SNR</th><th></th></tr></thead>
  <tbody id="mesh-rows"></tbody></table></div></div>
</div>

<div id="tab-room" class="tabpane">
  <div class="card"><h3>Room config <span id="room-title" class="mut" style="font-weight:400"></span></h3>
    <div class="row"><span class="mut" style="width:130px">room name</span><input id="room-name" class="grow" placeholder="room name">
      <button class="sec" onclick="roomCmd('set name '+v('room-name')).then(loadRoomTab)">Save</button></div>
    <div class="row"><span class="mut" style="width:130px">join password</span>
      <input id="room-pwd" class="grow" placeholder="room join (guest) password" autocomplete="off" data-1p-ignore>
      <button class="sec" onclick="roomCmd('set guest.password '+v('room-pwd')).then(loadRoomTab)">Save</button>
      <button class="sec" onclick="genRoomPwd()">Generate</button></div>
    <div class="mut" style="font-size:11px;margin:-4px 0 8px 138px">share this with members — new rooms get a random one
      (never a published default). Existing members already in the ACL keep access if you change it.</div>
    <div class="row"><span class="mut" style="width:130px">admin password</span>
      <input id="room-apwd" class="grow" placeholder="room admin password" autocomplete="off" data-1p-ignore>
      <button class="sec" onclick="roomCmd('password '+v('room-apwd'))">Save</button></div>
    <div class="row"><span class="mut" style="width:130px">private key</span>
      <input id="room-prv" class="grow" type="password" placeholder="128 hex (room identity)" autocomplete="off" data-1p-ignore>
      <button class="sec" onclick="roomCmd('get prv.key').then(r=>$('room-prv').value=stripReply(r))">Reveal</button>
      <button class="sec" onclick="copyText(v('room-prv'))">Copy</button>
      <button class="sec" onclick="setRoleKey('room','room-prv','room-key-state')">Set</button></div>
    <div id="room-key-state" class="mut" style="font-size:11px;margin:-4px 0 8px 138px">changing this replaces the
      room's identity — members must re-add it (reboot required)</div>
    <div class="row"><span class="mut" style="width:130px">location</span>
      <input id="room-lat" placeholder="lat" style="width:110px"><input id="room-lon" placeholder="lon" style="width:110px">
      <button class="sec" onclick="roomCmd('set lat '+v('room-lat')).then(()=>roomCmd('set lon '+v('room-lon')))">Save</button>
      <span class="mut" style="font-size:11px">(never advertised — room policy)</span></div>
    <div class="row"><button class="sec" onclick="roomCmd('get acl')">List members (ACL)</button>
      <button class="sec" onclick="roomCmd('clock')">Clock</button>
      <button class="sec" onclick="joinOwnRoom()">Join from this node's chat client</button></div>
    <div id="room-join-state" class="mut" style="font-size:12px"></div>
  </div>
  <div class="card"><h3>Privacy</h3>
    <div class="mut" style="font-size:12px;margin-bottom:8px">Rooms are <b>private by default</b>: a new room never
    advertises and gets a random join password, so it can only be found via the share link below and only entered with
    that password. Turning private mode off makes the room announce itself to the whole mesh.</div>
    <div class="row"><label><input type="checkbox" id="room-private" onchange="roomPrivacy(this.checked)"> private mode
      (never advertise)</label>
      <span id="room-priv-state" class="mut" style="font-size:12px"></span></div>
    <div class="row"><button class="act" onclick="roomCmd('advert').then(()=>$('room-priv-state').textContent='one advert sent')">Announce once now</button>
      <span class="mut" style="font-size:11px">manual one-shot advert, even in private mode</span></div>
  </div>
  <div class="card"><h3>Join this room</h3>
    <div class="mut" style="font-size:12px;margin-bottom:6px">Scan with the MeshCore app (or open the link) to add the
    room as a contact without it ever broadcasting. The member still needs the join password.</div>
    <div class="row" style="align-items:flex-start">
      <div id="room-qr" style="background:#fff;padding:8px;border-radius:6px"></div>
      <div class="grow" style="min-width:200px">
        <div id="room-uri" style="font-family:monospace;font-size:10px;word-break:break-all"></div>
        <button class="sec" style="margin-top:6px" onclick="copyText($('room-uri').textContent)">Copy link</button>
      </div>
    </div>
  </div>
  <div class="card"><h3>Stored posts <button class="sec" style="float:right;padding:2px 8px" onclick="loadRoomPosts()">&#8635;</button>
    <span class="mut" style="font-weight:400;font-size:11px">(last 32, held for members to sync)</span></h3>
    <div id="room-posts" class="mut">loading...</div>
  </div>
  <div class="card"><h3>Output <span class="mut" style="font-weight:400;font-size:11px">(full console lives in the Debug tab)</span></h3>
    <pre id="room-out" class="mut"></pre></div>
</div>

<div id="tab-msgs" class="tabpane">
  <div class="mut" style="font-size:12px;margin-bottom:8px">This view is fed by a non-consuming mirror: messages appear
  here automatically when the phone app syncs them. If no phone app is in use, press <b>Pull from device</b> to drain the
  companion's queue into this view.</div>
  <div id="msg-warn" class="warn" style="display:none">Phone app is connected. <b>Pull from device</b> consumes messages
  the app would otherwise receive — normally you can just wait for the mirror.</div>
  <div class="row">
    <div class="card" style="width:270px">
      <h3>Contacts <button class="sec" style="float:right;padding:2px 8px" onclick="loadContacts()">&#8635;</button></h3>
      <div id="contacts" class="list mut">not loaded</div>
      <div class="row" style="margin-top:10px"><input id="chat-name" class="grow" placeholder="rename chat client"
        onkeydown="if(event.key=='Enter')renameChat()">
      <button class="sec" onclick="renameChat()">Rename</button></div>
      <div id="chat-name-status" class="mut" style="font-size:11px"></div>
      <div class="row" style="margin-top:8px"><input id="chat-prv" class="grow" type="password"
        placeholder="private key: 128 hex" autocomplete="off" data-1p-ignore>
      <button class="sec" onclick="exportChatKey()">Reveal</button>
      <button class="sec" onclick="setRoleKey('companion','chat-prv','chat-key-status')">Set</button></div>
      <div id="chat-key-status" class="mut" style="font-size:11px">changing this replaces the chat identity —
        contacts must re-add you (reboot required)</div>
    </div>
    <div class="card grow">
      <h3>Messages <span id="self-name" class="mut"></span>
        <label style="float:right;font-size:12px" class="mut"><input type="checkbox" id="autosync"> auto-pull</label>
        <button class="sec" style="float:right;padding:2px 8px;margin-right:8px" onclick="syncMsgs()">Pull from device</button></h3>
      <div id="msg-log" style="max-height:380px;overflow-y:auto"></div>
      <div class="row" style="margin-top:8px"><input id="msg-text" class="grow" placeholder="message..."
        onkeydown="if(event.key=='Enter')sendMsg()">
      <button class="act" onclick="sendMsg()">Send</button></div>
      <div id="msg-status" class="mut" style="font-size:12px"></div>
    </div>
  </div>
</div>

<div id="tab-chans" class="tabpane">
  <div class="row">
    <div class="card" style="width:270px">
      <h3>Channels <button class="sec" style="float:right;padding:2px 8px" onclick="loadChannels()">&#8635;</button></h3>
      <div id="channels" class="list mut">not loaded</div>
      <div class="row" style="margin-top:10px"><input id="chan-join" class="grow" placeholder="#hashtag"
        onkeydown="if(event.key=='Enter')joinHashtag()">
      <button class="sec" onclick="joinHashtag()">Join</button></div>
      <div id="chan-join-status" class="mut" style="font-size:11px"></div>
    </div>
    <div class="card grow">
      <h3>Channel feed <span id="chan-name" class="mut"></span></h3>
      <div id="chan-log" style="max-height:380px;overflow-y:auto"></div>
      <div class="row" style="margin-top:8px"><input id="chan-text" class="grow" placeholder="message..."
        onkeydown="if(event.key=='Enter')sendChan()">
      <button class="act" onclick="sendChan()">Send</button></div>
      <div id="chan-status" class="mut" style="font-size:12px"></div>
    </div>
  </div>
</div>

</main>
</div>
<div id="pkt-modal" style="display:none;position:fixed;inset:0;background:rgba(6,9,12,.85);z-index:50;
  align-items:flex-start;justify-content:center;overflow-y:auto;padding:30px 10px"
  onclick="if(event.target===this)this.style.display='none'">
  <div class="card" style="width:680px;max-width:96vw" id="pkt-modal-body"></div>
</div>
<div id="login" style="display:none"><div class="card">
<h3>Unlock</h3><div class="row"><input id="pwd" type="password" class="grow" placeholder="admin password" autocomplete="off" data-1p-ignore
onkeydown="if(event.key=='Enter')doLogin()"></div>
<button class="act" onclick="doLogin()" style="width:100%">Log in</button>
<div id="login-err" class="err" style="font-size:12px;margin-top:6px"></div>
</div></div>
<script>
"use strict";
let TOKEN = localStorage.getItem("mp_token") || localStorage.getItem("repeater-token") ||
            sessionStorage.getItem("repeater-token") || "";   // share sessions with the classic /app panel
const $ = id => document.getElementById(id);
const v = id => $(id).value.trim();
const esc = s => s.replace(/[&<>"]/g, c => ({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;"}[c]));

function needLogin(){ $("login").style.display="flex"; $("pwd").focus(); }
async function doLogin(){
  const r = await fetch("/login",{method:"POST",body:$("pwd").value});
  if(!r.ok){ $("login-err").textContent="wrong password"; return; }
  TOKEN=(await r.text()).trim(); localStorage.setItem("mp_token",TOKEN);
  $("pwd").value="";               // a lingering value retriggers password managers on every nav
  $("login").style.display="none"; boot();
}
async function api(path,opts){
  opts=opts||{}; opts.headers=Object.assign({},opts.headers,{"X-Auth-Token":TOKEN});
  const r=await fetch(path,opts);
  if(r.status===401){ needLogin(); throw new Error("auth"); }
  return r;
}
async function cmd(c){ const r=await api("/api/command",{method:"POST",body:c}); return await r.text(); }

// ---- tabs ----
document.querySelectorAll("nav button").forEach(b=>b.onclick=()=>{
  document.querySelectorAll("nav button").forEach(x=>x.classList.remove("on"));
  document.querySelectorAll(".tabpane").forEach(x=>x.classList.remove("on"));
  b.classList.add("on"); $("tab-"+b.dataset.t).classList.add("on");
  if(b.dataset.t==="msgs" && !compReady) initComp();
  if(b.dataset.t==="chans" && !chansLoaded) loadChannels();
  if(b.dataset.t==="mesh"){ buildMesh(); }
  if(b.dataset.t==="dash"){ loadDash(); loadStatsTab(); }
  if(b.dataset.t==="rep") loadRepTab();
  if(b.dataset.t==="radio") loadRadioTab();
  if(b.dataset.t==="set") loadSlots();
  if(b.dataset.t==="room") loadRoomTab();
});

// ---- rail role cards ----
let railNames={};   // role -> display name
async function loadRailNames(){
  try{ railNames.repeater=stripReply(await cmd("get name")); }catch(e){}
  try{ railNames.room=stripReply(await cmd("room get name")); }catch(e){}
  renderRail();
}
// Cards reflect what each identity is ACTUALLY doing, from the arbiter's
// per-port counters — not merely that the identity exists.
const IDLE_WARN_S=600;                       // quiet this long = something's off
function portState(p){
  if(!p.active) return {cls:"off", word:"stopped"};
  if(!p.seen)   return {cls:"warn", word:"no traffic yet"};
  if(p.idle_s>IDLE_WARN_S) return {cls:"warn", word:"quiet "+age(p.idle_s)};
  return {cls:"", word:"active "+age(p.idle_s)+" ago"};
}
const LABELS={repeater:"REPEATER", room:"ROOM", companion:"CHAT"};
// a slot is labelled by its role, with its slot name kept as the subtitle
const ROLE_LABELS={room:"ROOM", chat:"CHAT"};
function renderRail(){
  if(!lastDebug||!lastDebug.ports) return;
  const rp=(lastDebug.stats.repeater||{}).packets||{};
  let h="";
  for(const p of lastDebug.ports){
    if(!p.active) continue;                  // disabled slots aren't roles you have
    const st=portState(p);
    const pk=selfIds[p.name]||"";
    let detail;
    // Describe a port by what it RUNS, not by where it sits in the list: a
    // slot may be a room, and quoting contacts/app-connected at it is wrong.
    const role=p.role||p.name;
    if(role==="repeater") detail=(rp.flood_tx||0)+" fwd · "+p.heard+"/"+p.sent+" heard back";
    else if(role==="room") detail=p.tx+" sent · "+p.rx+" rx";
    else if(role==="companion") detail=contacts.length+" contacts · app "+(lastDebug.companion.client?"connected":"—");
    else detail=p.tx+" sent · "+p.rx+" rx";
    h+="<div class=role><div class=rname><span class='dot "+st.cls+"'></span>"+
      (LABELS[p.name]||ROLE_LABELS[p.role]||p.name.toUpperCase())+(LABELS[p.name]?"":" <span class=mut style='font-size:10px'>"+esc(p.name)+"</span>")+"</div>"+
      "<div>"+esc(railNames[p.name]||(p.name==="companion"?($("self-name").textContent||"").replace(/[()]/g,""):""))+"</div>"+
      (pk?"<div class=rkey title='click to copy prefix' onclick=\"copyText('"+pk+"')\">"+pk+"</div>":"")+
      "<div class=rstat>"+esc(st.word)+"</div>"+
      "<div class=rstat>"+esc(detail)+"</div></div>";
  }
  $("rail-roles").innerHTML=h;
}
const stripReply=t=>t.replace(/^>\s*/,"").trim();
function copyText(t){ navigator.clipboard&&navigator.clipboard.writeText(t); }
function logout(){ localStorage.removeItem("mp_token"); localStorage.removeItem("repeater-token");
  sessionStorage.removeItem("repeater-token"); TOKEN=""; needLogin(); }
async function fieldLoad(c,id){
  const r=stripReply(await cmd(c));
  const el=$(id);
  if(el.tagName==="SELECT"){ el.value=r; } else el.value=r;
  return r;
}
async function fieldSave(c,statusId){
  const r=stripReply(await cmd(c));
  if(statusId) $(statusId).textContent=r||"OK";
  return r;
}

// ---- debug tab ----
const ROUTES=["T-FLOOD","FLOOD","DIRECT","T-DIRECT"];
const PTYPES=["REQ","RESPONSE","TXT_MSG","ACK","ADVERT","GRP_TXT","GRP_DATA","ANON_REQ","PATH","TRACE","MULTIPART","CONTROL","?","?","?","RAW"];
let lastSeq=0, devNow=0;
// One row per identity, sourced from the arbiter so the chat identities (which
// have no CLI and no stock stats) appear too. Mesh-level columns are filled in
// only for the identities that can report them.
function identityRows(d){
  if(!d.ports) return "";
  return d.ports.map(p=>{
    const st=portState(p);
    const mesh=(d.stats[p.name]||{}).packets;
    const core=(d.stats[p.name]||{}).core;
    const nm=railNames[p.name]||(p.name==="companion"?($("self-name").textContent||"").replace(/[()]/g,""):"");
    const dot="<span class='dot "+st.cls+"' style='display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:6px'></span>";
    return "<tr"+(p.active?"":" class=mut")+"><td>"+dot+esc(p.name)+(nm?" <span class=mut>"+esc(nm)+"</span>":"")+
      "</td><td>"+esc(st.word)+"</td><td>"+(p.seen?age(p.idle_s)+" ago":"—")+
      "</td><td>"+p.rx+"</td><td>"+p.tx+"</td><td>"+p.sent+"</td><td>"+
      (p.sent?p.heard+" ("+Math.round(100*p.heard/p.sent)+"%)":"—")+
      "</td><td>"+(p.busy||0)+"</td><td>"+(mesh?mesh.flood_rx:"—")+"</td><td>"+(mesh?mesh.recv_errors:"—")+
      "</td><td"+(p.poolfull?" class=err":"")+">"+(p.poolfull||0)+
      "</td><td>"+(core?core.queue_len:"—")+"</td></tr>";
  }).join("");
}
let selfIds={}, selfLoc=null, nbrsRaw="";
async function pollDebug(){
  try{
    const d=await (await api("/api/multi/debug")).json();
    lastDebug=d;
    const info=d.identities+"\n"+d.wifi+"\nuptime "+d.uptime_s+"s   heap "+
      Math.round(d.heap/1024)+"k   psram "+Math.round(d.psram/1024)+"k   companion tcp:"+
      (d.companion.tcp?"listening":"down")+" client:"+(d.companion.client?"connected":"none");
    $("dbg-info").textContent=info;
    $("dash-info").textContent=info;
    try{ renderDashTiles(d); }catch(e){}
    try{ renderRail(); }catch(e){}
    $("dbg-stats").innerHTML=identityRows(d);
    $("dbg-nbrs").textContent=d.neighbors||"(none heard yet)";
    $("msg-warn").style.display=d.companion.client?"":"none";
    nbrsRaw=(d.neighbors==="-none-")?"":d.neighbors;
    selfIds={};
    for(const m of d.identities.matchAll(/(\w+)=([0-9a-f]+)/g)) selfIds[m[1]]=m[2];
  }catch(e){}
}
function neighbours(){                      // [{prefix8, secsAgo, snr}]
  return nbrsRaw.split("\n").filter(Boolean).map(l=>{
    const p=l.split(":"); return {prefix:p[0]||"",secsAgo:+p[1]||0,snr:(+p[2]||0)/4};
  }).filter(n=>/^[0-9a-f]{8}$/i.test(n.prefix));
}

// ---- signal colouring: green = strong, orange = weak ----
function snrCol(v){ return v>=0?"#39c07a":v>=-10?"#e0b34d":"#e08a4d"; }
function rssiCol(v){ return v>=-90?"#39c07a":v>=-110?"#e0b34d":"#e08a4d"; }
const snrSpan=v=>"<span style='color:"+snrCol(v)+"'>"+v+"</span>";
const rssiSpan=v=>"<span style='color:"+rssiCol(v)+"'>"+v+"</span>";

// hop-depth per node, mined from advert packets seen in the trace
let advHops={};   // pk (full hex) -> smallest hop count seen

// ---- packet annotation (parses captured raw bytes) ----
const hx1=n=>(n??0).toString(16).padStart(2,"0");
const hexa=a=>a.map(hx1).join("");
function rdI32a(b,o){ return (b[o]|(b[o+1]<<8)|(b[o+2]<<16)|(b[o+3]<<24)); }
function distKm(a,b){
  const R=6371,dl=(b[0]-a[0])*Math.PI/180,dg=(b[1]-a[1])*Math.PI/180;
  const h=Math.sin(dl/2)**2+Math.cos(a[0]*Math.PI/180)*Math.cos(b[0]*Math.PI/180)*Math.sin(dg/2)**2;
  return (2*R*Math.asin(Math.sqrt(h))).toFixed(h>1e-6?1:2);
}
function hashName(h){
  const c=[];
  for(const x of contacts) if(parseInt(x.pk.slice(0,2),16)===h) c.push(x.name||x.prefix.slice(0,8));
  for(const [n,p] of Object.entries(selfIds)) if(parseInt(p.slice(0,2),16)===h) c.push("self:"+n);
  return c.join("/");
}
// candidates for a hop hash group (1-3 bytes): match by pubkey prefix against
// contacts, our own identities and heard neighbours — the known-nodes database
const gHex=g=>Array.isArray(g)?hexa(g):hx1(g);
function hopCands(g){
  const hex=gHex(g).toLowerCase();
  const c=[];
  for(const x of contacts) if(x.pk.startsWith(hex)) c.push((x.name||"?")+" ("+x.prefix.slice(0,8)+")");
  for(const [n,p] of Object.entries(selfIds)) if(p.startsWith(hex)) c.push("self:"+n+" ("+p+")");
  for(const nb of neighbours()){
    const pfx=nb.prefix.toLowerCase();
    if(pfx.startsWith(hex)&&!c.some(s=>s.includes(pfx.slice(0,8)))) c.push("neighbour "+pfx);
  }
  return c;
}
// rich candidate objects (for the packet modal): name, distance, last heard
function hopCandObjs(g){
  const hex=gHex(g).toLowerCase();
  const out=[];
  const nowS=Math.floor(Date.now()/1000);
  for(const x of contacts) if(x.pk.startsWith(hex)){
    out.push({name:x.name||x.prefix.slice(0,8),kind:KINDS[x.type]||"?",
      dist:(x.lat||x.lon)&&selfLoc&&(selfLoc[0]||selfLoc[1])?distKm(selfLoc,[x.lat,x.lon]):null,
      last:age(nowS-x.lastAdvert)});
  }
  for(const [n,p] of Object.entries(selfIds)) if(p.startsWith(hex)) out.push({name:"self:"+n,kind:"self",dist:0,last:"now"});
  return out;
}
// colon-separated hop groups; hover a segment for likely candidate nodes
function hopsHtml(groups){
  return groups.map(g=>{
    const cands=hopCands(g);
    const tip=cands.length?("could be: "+cands.join(", ")):"no known node matches hash "+gHex(g);
    return "<span class=hop title=\""+esc(tip)+"\">"+gHex(g)+"</span>";
  }).join(":");
}
// full packet parse with multi-byte path-hash support (hash size = mode+1, 1..3 bytes)
function parsePkt(p){
  if(!p.raw) return null;
  const b=p.raw.match(/../g).map(h=>parseInt(h,16));
  const route=p.h&3, type=(p.h>>2)&15;
  let o=1, tc=null;
  if(route===0||route===3){ tc=b.slice(o,o+4); o+=4; }   // transport codes
  if(o>=b.length) return null;
  const pl=b[o++], mode=pl>>6, hops=pl&63, hsz=mode+1;
  const groups=[];
  for(let i=0;i<hops&&o+hsz<=b.length;i++){ groups.push(b.slice(o,o+hsz)); o+=hsz; }
  return {route,type,hops,hsz,groups,pay:b.slice(o),tc,bytes:b};
}
function annot(p){
  const q=parsePkt(p);
  if(!q) return {src:"",infoHtml:""};
  const {type,hops,groups,pay}=q;
  let src="",info="";
  if(type===4&&pay.length>=100){                  // ADVERT: pk32 ts4 sig64 appdata
    const pk=hexa(pay.slice(0,32));
    if(!(pk in advHops)||hops<advHops[pk]) advHops[pk]=hops;   // hop depth for the mesh view
    const c=contacts.find(c=>c.pk===pk);
    const self=Object.entries(selfIds).find(([n,pf])=>pk.startsWith(pf));
    const ad=pay.slice(100);
    let name="",lat=null,lon=null;
    if(ad.length){ const fl=ad[0]; let i=1;
      if(fl&0x10){ lat=rdI32a(ad,i)/1e6; lon=rdI32a(ad,i+4)/1e6; i+=8; }
      if(fl&0x20)i+=2; if(fl&0x40)i+=2;
      if(fl&0x80) name=new TextDecoder().decode(new Uint8Array(ad.slice(i))).replace(/\0.*$/,"");
    }
    src=self?("self:"+self[0]):(c&&c.name)||name||("pk:"+pk.slice(0,8));
    info="pk "+pk.slice(0,8);
    if(lat!==null){ info+=" @"+lat.toFixed(4)+","+lon.toFixed(4);
      if(selfLoc&&(selfLoc[0]||selfLoc[1])) info+=" ~"+distKm(selfLoc,[lat,lon])+"km"; }
  } else if(type===3){ info="ack "+hexa(pay.slice(0,4));
  } else if(type===5||type===6){ info="chan hash "+hx1(pay[0]);
  } else if(type===7&&pay.length>=33){
    info="dest "+hx1(pay[0])+(hashName(pay[0])?"("+hashName(pay[0])+")":"")+" epk "+hexa(pay.slice(1,5))+"…";
  } else if(pay.length>=2){                       // REQ/RESPONSE/TXT_MSG/PATH/...
    src=hashName(pay[1])||("hash:"+hx1(pay[1]));
    const dn=hashName(pay[0]);
    info="src "+hx1(pay[1])+" → dest "+hx1(pay[0])+(dn?" ("+dn+")":"");
  }
  // Does this packet's path carry one of OUR hashes? If so a neighbour heard
  // something we sent and relayed it onward — direct proof we are being heard.
  // Only meaningful on receive: our own transmissions contain our hash by
  // definition, because we append it when forwarding.
  let relay=null;
  for(const g of groups){
    const hex=gHex(g).toLowerCase();
    for(const [role,pfx] of Object.entries(selfIds)){
      if(pfx.startsWith(hex)){ relay={role,width:Array.isArray(g)?g.length:1}; break; }
    }
    if(relay) break;
  }

  let infoHtml=esc(info);
  if(hops) infoHtml+=(infoHtml?"  |  ":"")+hops+" hop"+(hops>1?"s":"")+" "+hopsHtml(groups);
  return {src,infoHtml,relay,hops};
}



// Time on air. At this node's preset a small packet is already ~140 ms and a
// full one ~800 ms, so this is the dominant cost of everything the mesh does —
// worth seeing per packet rather than inferring from length.
function airCell(p){
  if(!p.a) return "<td class=mut>-</td>";
  // colour by how much of the channel one packet occupies: a mesh lives or dies
  // on airtime, and 500 ms is already a long time to hold a shared channel
  const c = p.a>=600?"#e05d5d" : p.a>=300?"#e0b34d" : "#8b98a5";
  // a receive is costed at the CR its own header carried, not at ours
  const why = (p.d==="rx"&&p.cr) ? "time on air, at the 4/"+p.cr+" this packet was sent with"
                                 : "time on air at the current radio settings";
  // the collision mark lands here, filled in later — how much of this airtime
  // was shared with another packet belongs next to the airtime itself
  return "<td style='color:"+c+";font-variant-numeric:tabular-nums' title='"+why+"'>"+
    (p.a>=1000?(p.a/1000).toFixed(2)+"s":p.a+"ms")+
    "<span id='ca-"+p.s+"'></span></td>";
}

// Coding rate, as the 4/x denominator the LoRa header carries. It is the other
// half of the airtime story next to the air column: 4/8 spends 60% longer on
// the channel than 4/5 for the same bytes, buying error correction with it.
//
// A RECEIVED packet's CR is the sender's own — it travels in the explicit
// header, so a neighbour running a different one still decodes here — and that
// is exactly the case worth flagging rather than assuming away: it means that
// node is on a different preset to this one.
let radioCr=0;                       // CR this node transmits at (from the API)
function crCell(p){
  if(!p.cr) return "<td class=mut>-</td>";
  const odd = p.d==="rx" && radioCr && p.cr!==radioCr;
  return "<td"+(odd?" style='color:#e0b34d' title='sender used 4/"+p.cr+
    ", this node runs 4/"+radioCr+"'":" class=mut")+">4/"+p.cr+"</td>";
}

// ---- airtime timeline ------------------------------------------------------
// Occupancy of the shared channel over the last minute. Each bar is one
// packet: WIDTH is the time it held the channel, HEIGHT is the SNR it arrived
// at, and COLOUR is the packet hash — so a flood and every forward of it share
// a colour, and the cost of that redundancy is visible as width rather than
// inferred from a row count.
//
// The two timestamps mean different things and must be drawn differently: a
// transmit is logged when it STARTS (SharedRadioCore::tryStartSend), a receive
// only once the packet has fully arrived. So a TX bar runs forward from its
// timestamp and an RX bar runs backward to it. Drawing both the same way would
// shift every received packet one airtime to the right — at this preset ~200 ms,
// enough to make a forward look like it preceded the packet it was forwarding.
// A minute, not five: at this preset a packet is a few hundred ms, so a 5-minute
// window squeezed each one into a sliver too narrow to compare and packed them
// too tightly to see which overlapped which. A minute is wide enough that a
// collision is visible as a collision.
const AIR_WINDOW_MS=60000;
const SNR_MIN=-20, SNR_MAX=12;       // dB range mapped onto bar height
let airLog=[], noiseLog=[];

// ---- collisions ------------------------------------------------------------
// This radio decodes one signal at a time, so two packets cannot share the
// channel and both survive. That makes an overlap between two airtime windows
// arithmetic rather than a guess: the windows are built from measured
// timestamps and real airtimes (a receive is stamped when it completes, a
// transmit when it starts), so if they intersect, two transmitters were talking
// at once.
//
// Three signals, strongest first:
//   1. an overlap where one side failed, or where one side is our own transmit
//   2. a CRC failure with an intact header — the payload was hit part-way
//      through, which is what a collision does to a packet; a damaged header is
//      what distance does to one
//   3. a strong signal carrying the SNR of a weak one — another transmitter's
//      energy lands in the RSSI without landing in the wanted signal
//
// The thing this CANNOT see is the ordinary case: the receiver locks onto one
// signal, so the interferer usually never becomes a log entry at all. A mark
// here is strong evidence of a collision; the absence of one is no evidence of
// its absence, and the legend says so.

// Two CLEAN receives that overlap are physically impossible, so such a pair is
// pure timestamp skew — the log stamp is taken in the main loop, not in the
// radio's ISR. Live traces put that skew at 4-7 ms, so under this is noise,
// over it is real, and impossible pairs are dropped rather than reported.
const COLL_JITTER_MS=12;
// ---- usable SNR floor ------------------------------------------------------
// The lowest SNR we have actually pulled a whole packet out of. This is an
// OBSERVED bound on what this receiver can do, not a theoretical one, and it is
// one-sided: it says "something this weak decoded", never "anything weaker
// fails". A quiet hour with only strong neighbours talking leaves it looking
// optimistic, which is why it is drawn as a trailing minimum rather than a
// single all-time figure — one lucky packet should not define the line forever.
const SNR_FLOOR_WINDOW_MS=300000;    // trailing 5 minutes
let snrLog=[];
// The chip stops reporting SNR somewhere around +12 dB, so cap the expectation
// there before holding a packet's SNR against its RSSI.
const SNR_CEIL_DB=12, SNR_DEFICIT_DB=8;
const PKT_ERR_CRC=-7;                // RadioLib's RADIOLIB_ERR_CRC_MISMATCH

function analyseCollisions(){
  const es=airLog.slice().sort((a,b)=>a.start-b.start);
  for(const e of es){ e.hit=null; e.shape=null; }
  for(let i=0;i<es.length;i++){
    for(let j=i+1;j<es.length;j++){
      const a=es[i], b=es[j];
      if(b.start>=a.end) break;              // sorted by start: nothing later can reach back
      const lap=Math.min(a.end,b.end)-b.start;
      if(lap<COLL_JITTER_MS) continue;
      if(a.rx&&b.rx&&!a.err&&!b.err) continue;   // impossible, so it is skew, not a collision
      const kind=(!a.rx||!b.rx)?"ours":"hidden";
      // keep the worst overlap per packet: one badge that means something beats
      // a list of near-misses
      if(!a.hit||lap>a.hit.lap) a.hit={other:b,lap,kind};
      if(!b.hit||lap>b.hit.lap) b.hit={other:a,lap,kind};
    }
  }
  // Lone evidence, for the collisions whose other half we never heard — which
  // is most of them.
  const nf=noiseLog.length?noiseLog[noiseLog.length-1].n:null;
  for(const e of es){
    if(!e.rx) continue;
    const why=[];
    if(e.err&&e.code===PKT_ERR_CRC) why.push("header parsed but the payload did not — something arrived mid-frame");
    if(nf!==null&&e.rssi!=null&&e.snr!=null){
      const deficit=Math.min(e.rssi-nf,SNR_CEIL_DB)-e.snr;
      if(deficit>=SNR_DEFICIT_DB){
        why.push("SNR "+deficit.toFixed(0)+" dB below what "+e.rssi+" dBm over a "+nf+
                 " dBm floor should give — strong signal, ruined by something");
      }
    }
    if(why.length) e.shape=why;
  }
  return es;
}

// WHO was transmitting, which is the question a collision raises: two radios
// were keying up at once and the useful thing to know is which two.
//
// The last hop a packet carries is the node that put it on the air — every
// forwarder appends its own hash, so the tail of the path is the transmitter,
// not the originator. At zero hops there is no path and the two are the same
// node, so the identity comes from the payload instead: an advert's public key,
// or the source hash every other type carries in its second byte.
function txPrefix(p){
  if(p.d!=="rx") return {label:p.d};                    // our own transmit: we know exactly
  const q=parsePkt(p);
  if(!q) return null;
  if(q.groups.length) return {g:q.groups[q.groups.length-1], note:"last hop, so this is who transmitted it"};
  if(q.type===4&&q.pay.length>=32) return {g:q.pay.slice(0,4), note:"zero hops, so the originator transmitted it"};
  if(q.pay.length>=2) return {g:[q.pay[1]], note:"zero hops, so the originator transmitted it"};
  return null;
}

// A hoverable prefix, matched against the known-nodes database the same way
// path hops are — contacts, our own identities, heard neighbours.
function prefixChip(t){
  if(!t) return "<span class=mut>?</span>";
  if(t.label) return "<span class=hop title='transmitted by this node'>"+esc(t.label)+"</span>";
  const cands=hopCands(t.g);
  const tip=(t.note?t.note+" — ":"")+
    (cands.length?"could be: "+cands.join(", "):"no known node matches hash "+gHex(t.g));
  return "<span class=hop title=\""+esc(tip)+"\">"+gHex(t.g)+"</span>";
}

// The pair, for the info column: which two transmitters were talking over each
// other. Hover either prefix for who we think it was.
function collInfo(e){
  if(!e.hit||!e.pkt) return "";
  return "<span class=collx style='margin:0'>!</span> "+prefixChip(txPrefix(e.pkt))+
    " &#10005; "+prefixChip(txPrefix(e.hit.other.pkt))+" &nbsp;";
}

// The mark that sits beside a packet's airtime. A proven overlap says how many
// of those milliseconds were shared with another packet — that number is the
// damage. A suspicion gets the bare mark and explains itself only on hover,
// because it has no millisecond to offer: nothing else was ever decoded.
function collMark(e){
  if(e.hit){
    const o=e.hit.other, when=stamp(o.rx?o.end:o.start);
    const who=!e.rx ? "this transmit overlapped a packet arriving"
            : !o.rx ? "our own transmit ran over this"
                    : "another transmitter ran over this";
    return "<span class='collx' title=\""+who+" — "+e.hit.lap+" ms shared with "+
      (o.rx?"the packet received":"the transmit started")+" at "+when+
      (o.err?", which failed":"")+". Two transmitters on the channel at once.\">!"+
      e.hit.lap+"ms</span>";
  }
  if(e.shape){
    return "<span class='collx maybe' title=\""+esc(e.shape.join("; "))+
      ". Nothing in the log overlaps it, so there is no figure to give — the "+
      "other transmitter was never decoded, which is the normal case.\">!</span>";
  }
  return "";
}

// Marks are painted after the fact: a packet only turns out to have collided
// once the packet that hit it has been received, which is always later.
function paintCollisions(es){
  for(const e of es){
    const air=$("ca-"+e.s);
    if(air) air.innerHTML=collMark(e);
    const info=$("ci-"+e.s);
    if(info) info.innerHTML=collInfo(e);
  }
}

function stamp(t){
  const d=new Date(Date.now()-((devNow||0)-t));
  return d.toTimeString().slice(0,8)+"."+String(d.getMilliseconds()).padStart(3,"0");
}

// Diagonal hatch, used to mark packets that failed CRC. A colour alone would
// not do: a corrupt packet still has a hash and a colour, and the point is to
// see at a glance that a chunk of airtime was spent and produced nothing.
function hatch(ctx,x,y,w,h,col){
  ctx.save(); ctx.beginPath(); ctx.rect(x,y,w,h); ctx.clip();
  ctx.strokeStyle=col; ctx.lineWidth=1;
  for(let i=-h;i<w+h;i+=4){
    ctx.beginPath(); ctx.moveTo(x+i,y+h); ctx.lineTo(x+i+h,y); ctx.stroke();
  }
  ctx.restore();
}

// Ring a bar that collided. The shared slice of time is drawn as well, but at a
// 60-second window a 38 ms overlap is a third of a pixel — accurate and
// invisible. Outlining both packets is what actually shows you the pair.
function outline(ctx,x,y,w,h){
  ctx.strokeStyle="#e05d5d"; ctx.lineWidth=1;
  ctx.strokeRect(Math.round(x)-0.5,Math.round(y)-0.5,Math.round(w)+1,Math.round(h)+1);
}

function drawAirTimeline(){
  const cv=$("air-tl"); if(!cv) return;
  const w=cv.clientWidth||600;
  if(cv.width!==w) cv.width=w;            // match CSS width or it renders blurry
  const h=cv.height, ctx=cv.getContext("2d");
  ctx.clearRect(0,0,w,h);
  const now=devNow||0, t0=now-AIR_WINDOW_MS;
  airLog=airLog.filter(p=>p.end>t0);
  noiseLog=noiseLog.filter(n=>n.t>t0);
  const es=analyseCollisions();
  paintCollisions(es);

  // bands: rx bars grow up from the midline, tx down from it, noise below
  const rxBase=58, txTop=60, txH=16, nsTop=84, nsH=26;
  const X=t=>(t-t0)/AIR_WINDOW_MS*w;

  ctx.strokeStyle="#243040"; ctx.lineWidth=1; ctx.font="9px system-ui";
  for(let s=0;s<=6;s++){                  // a gridline every 10 s across the minute
    const x=Math.round(w-(s/6)*w)+0.5;
    ctx.beginPath(); ctx.moveTo(x,0); ctx.lineTo(x,nsTop+nsH); ctx.stroke();
    if(s){ ctx.fillStyle="#5c6875"; ctx.fillText("-"+(s*10)+"s",Math.max(2,x+2),h-2); }
  }
  ctx.strokeStyle="#35414f";
  ctx.beginPath(); ctx.moveTo(0,rxBase+0.5); ctx.lineTo(w,rxBase+0.5); ctx.stroke();
  ctx.fillStyle="#5c6875";
  ctx.fillText("rx  ↑ SNR",2,10); ctx.fillText("tx",2,txTop+12); ctx.fillText("noise",2,nsTop+10);

  let busy=0, crc=0;
  for(const p of airLog){
    const x1=X(p.start), bw=Math.max(1.5,X(p.end)-x1);
    busy+=Math.min(p.end,now)-Math.max(p.start,t0);
    const col=p.ph?hashColor(p.ph):"hsl(210,10%,45%)";
    ctx.fillStyle=col;
    if(p.rx){
      // height carries SNR; a floor of 3px keeps a very weak packet visible
      const f=Math.max(0,Math.min(1,((p.snr==null?SNR_MIN:p.snr)-SNR_MIN)/(SNR_MAX-SNR_MIN)));
      const bh=Math.max(3,Math.round(f*(rxBase-6)));
      ctx.fillRect(x1,rxBase-bh,bw,bh);
      if(p.err){ crc++; hatch(ctx,x1,rxBase-bh,bw,bh,"#0d1218"); }
      if(p.hit) outline(ctx,x1,rxBase-bh,bw,bh);
    }else{
      ctx.fillRect(x1,txTop,bw,txH);
      if(p.hit) outline(ctx,x1,txTop,bw,txH);
    }
  }

  // The usable-SNR floor, on the same scale as the bars so it can be read
  // against them: every bar standing above the line is a packet that arrived
  // with margin to spare, and one touching it is a packet we only just got.
  // Sampled across the window rather than drawn as one flat line, because the
  // floor moves — it is the trailing minimum, and a weak decode drops it for
  // the next five minutes and then lets it recover.
  snrLog=snrLog.filter(v=>v.t>now-SNR_FLOOR_WINDOW_MS*2);
  const snrY=v=>rxBase-Math.max(3,Math.round(Math.max(0,Math.min(1,(v-SNR_MIN)/(SNR_MAX-SNR_MIN)))*(rxBase-6)));
  let floorNow=null;
  if(snrLog.length){
    ctx.strokeStyle="#5fd694"; ctx.lineWidth=1; ctx.setLineDash([3,3]);
    ctx.beginPath();
    let drawn=false;
    for(let px=0;px<=w;px+=6){
      const t=t0+(px/w)*AIR_WINDOW_MS;
      let lo=null;
      for(const v of snrLog){ if(v.t<=t&&v.t>t-SNR_FLOOR_WINDOW_MS&&(lo===null||v.snr<lo)) lo=v.snr; }
      if(lo===null){ drawn=false; continue; }
      floorNow=lo;
      const y=snrY(lo)+0.5;
      drawn?ctx.lineTo(px,y):ctx.moveTo(px,y);
      drawn=true;
    }
    ctx.stroke(); ctx.setLineDash([]);
    if(floorNow!==null){
      ctx.fillStyle="#5fd694";
      ctx.fillText(floorNow.toFixed(1)+" dB floor",2,Math.max(8,snrY(floorNow)-3));
    }
  }

  // Collisions. A proven overlap is drawn as the slice of time the two packets
  // actually shared — that band IS the collision, and its width is how much of
  // one packet the other one ruined. A packet whose damage merely looks like a
  // collision gets a caret instead: same suspicion, weaker evidence, and the
  // difference has to be visible or the picture overclaims.
  let pairs=new Set(), shaped=0;
  for(const p of airLog){
    if(p.hit){
      const o=p.hit.other, key=Math.min(p.s,o.s)+":"+Math.max(p.s,o.s);
      if(pairs.has(key)) continue;
      pairs.add(key);
      const xa=X(Math.max(p.start,o.start)), xb=Math.max(X(Math.min(p.end,o.end)),xa+2);
      ctx.fillStyle="rgba(224,93,93,0.28)";
      ctx.fillRect(xa,0,xb-xa,nsTop);
      ctx.strokeStyle="#e05d5d"; ctx.lineWidth=1;
      ctx.beginPath();
      ctx.moveTo(xa+0.5,0); ctx.lineTo(xa+0.5,nsTop);
      ctx.moveTo(xb-0.5,0); ctx.lineTo(xb-0.5,nsTop);
      ctx.stroke();
    }else if(p.shape){
      shaped++;
      const xm=(X(p.start)+X(p.end))/2;
      ctx.fillStyle="#d99b6c";
      ctx.beginPath(); ctx.moveTo(xm,7); ctx.lineTo(xm-4,1); ctx.lineTo(xm+4,1); ctx.fill();
    }
  }

  // noise floor over the window, on its own scale so it cannot be confused with SNR
  if(noiseLog.length>1){
    const vs=noiseLog.map(n=>n.n);
    let lo=Math.min(...vs), hi=Math.max(...vs);
    if(hi-lo<4){ const mid=(hi+lo)/2; lo=mid-2; hi=mid+2; }
    ctx.strokeStyle="#4da3ff"; ctx.lineWidth=1.2; ctx.beginPath();
    noiseLog.forEach((n,i)=>{
      const x=X(n.t), y=nsTop+nsH-((n.n-lo)/(hi-lo))*nsH;
      i?ctx.lineTo(x,y):ctx.moveTo(x,y);
    });
    ctx.stroke();
    ctx.fillStyle="#5c6875";
    ctx.fillText(hi.toFixed(0),w-24,nsTop+8); ctx.fillText(lo.toFixed(0),w-24,nsTop+nsH-1);
  }

  const pct=busy/AIR_WINDOW_MS*100;
  const cls=pct>=25?"err":pct>=10?"":"ok";
  const style=cls===""?" style='color:#e0b34d'":"";
  const nf=noiseLog.length?noiseLog[noiseLog.length-1].n:null;
  $("air-tl-legend").innerHTML=
    "last 60 s &middot; <b>"+airLog.length+"</b> packets &middot; <b>"+(busy/1000).toFixed(2)+
    "s</b> on air &middot; busy <span class='"+cls+"'"+style+">"+pct.toFixed(1)+"%</span>"+
    (crc?" &middot; <span class=err>"+crc+" CRC fail</span>":"")+
    (pairs.size?" &middot; <span class=err>"+pairs.size+" collision"+(pairs.size>1?"s":"")+"</span>":"")+
    (shaped?" &middot; <span style='color:#d99b6c'>"+shaped+" collision-shaped</span>":"")+
    (nf!==null?" &middot; noise <b>"+nf+"</b> dBm":"")+
    (floorNow!==null?" &middot; decoded down to <b title='lowest SNR a whole packet has"+
      " survived in the last 5 min — an observed bound, not a limit: nothing weaker"+
      " may have been sent'>"+floorNow.toFixed(1)+"</b> dB":"")+
    " <span class=mut>&mdash; width = airtime, height = SNR, colour = packet hash,"+
    " hatched = CRC failure, red band = two transmitters at once, caret = damage that"+
    " looks like a collision whose other half we never heard, dashed green = lowest"+
    " SNR still decoding</span>";
}

// ---- packet hash colouring ----------------------------------------------
// MeshCore's packet hash covers the PAYLOAD and not the path, so every copy of
// one flood — the originator's transmission and each repeater's forward — shares
// a hash. Colouring by it turns "is this new traffic or the same packet going
// round again?" into something you can see without reading a single field.
//
// The colour is derived from the hash itself rather than allocated from a
// palette, so it is stable across reloads and across devices: the same packet is
// the same colour on two different nodes watching the same mesh.
const pktSeen=new Map();          // hash -> times seen in this session
function hashHue(h){
  let x=0;
  for(let i=0;i<h.length;i++) x=(x*31+h.charCodeAt(i))>>>0;
  return x%360;
}
function hashColor(h){ return "hsl("+hashHue(h)+",80%,74%)"; }
function hashCell(p){
  if(!p.ph) return "<td class=mut>-</td>";
  const n=(pktSeen.get(p.ph)||0)+1;
  pktSeen.set(p.ph,n);
  // bound the map: the trace is a rolling window, not a permanent ledger
  if(pktSeen.size>800){ const k=pktSeen.keys().next().value; pktSeen.delete(k); }
  const hue=hashHue(p.ph);
  const style="background:hsl("+hue+",55%,20%);color:"+hashColor(p.ph)+";"+
    "padding:1px 5px;border-radius:4px;font-family:monospace;font-size:11px";
  // A repeat is the interesting case, so say so explicitly as well as by colour
  const rep=n>1?" <span class=mut style='font-size:10px' title='copy number "+n+
    " of this packet seen by this node'>&#8635;"+n+"</span>":"";
  return "<td><span style='"+style+"' title=\"MeshCore packet hash (payload only, "+
    "path excluded) — copies of one packet share it\">"+p.ph+"</span>"+rep+"</td>";
}
// CSS-driven so rows arriving from the next poll obey it without re-filtering
function applyPktFilter(){
  $("pkt-rows").classList.toggle("directonly", $("pkt-direct-only").checked);
}
async function pollPkts(){
  try{
    const d=await (await api("/api/multi/packets?after="+lastSeq)).json();
    devNow=d.now;
    if(d.cr) radioCr=d.cr;
    for(const p of d.pkts){
      lastSeq=Math.max(lastSeq,p.s);
      if(p.a){   // see drawAirTimeline for why rx and tx anchor differently
        const rx=p.d==="rx";
        // rssi and the error code ride along for the collision heuristics; the
        // seq is what ties an entry back to its row once a later packet reveals
        // that this one was collided with
        airLog.push({start:rx?p.t-p.a:p.t, end:rx?p.t:p.t+p.a, air:p.a, ph:p.ph, rx,
                     snr:rx?p.snr:null, err:p.e===1, s:p.s, rssi:rx?p.rssi:null, code:p.x, pkt:p});
        // only a WHOLE packet counts towards the usable floor — a CRC failure
        // proves the opposite of what this line is measuring
        if(rx&&p.e!==1&&p.snr!=null) snrLog.push({t:p.t,snr:p.snr});
        if(airLog.length>400) airLog.splice(0,airLog.length-400);
      }
      const tr=document.createElement("tr");
      const when=stamp(p.t);
      const rx=p.d==="rx";
      if(p.e===1){          // RX failure, labelled by RadioLib error code
        const why=p.x===-7?"CRC mismatch":p.x===-16?"LoRa header damaged":
          p.x===-6?"RX timeout":"receive error ("+p.x+")";
        // decode the damaged bytes anyway — fields may be wrong, but a corrupt
        // copy is still evidence (often the same packet arrives clean later)
        let a={src:"",infoHtml:""};
        try{ if(p.raw) a=annot(p); }catch(e){}
        // rendered in the same columns as a good packet — the RX-CRC label and
        // the modal's banner carry the "this may be wrong" warning
        const rt=p.raw?ROUTES[p.h&3]:"-", ty=p.raw?PTYPES[(p.h>>2)&15]:"-";
        tr.innerHTML="<td>"+when+"</td>"+airCell(p)+crCell(p)+
          "<td>"+(p.snr?snrSpan(p.snr):"")+"</td><td>"+(p.rssi?rssiSpan(p.rssi):"")+"</td><td>"+(p.l||"-")+"</td>"+
          "<td style='color:#e08a4d' title='"+esc(why)+"'>"+
          (p.x===-7?"RX-CRC":"RX-ERR")+"</td><td>"+rt+"</td><td>"+ty+"</td>"+hashCell(p)+"<td>"+
          esc(a.src)+"</td><td class=mut><span id='ci-"+p.s+"'></span>"+
          (p.raw?a.infoHtml:esc(why))+"</td>";
        if(p.raw){ tr.style.cursor="pointer"; tr.title="click to decode (corrupt)";
          tr.onclick=()=>openPktModal(p,when,why); }
      } else if(p.e===3){   // send deferred: a sibling identity held the radio
        const owner=(p.x>=0&&lastDebug)?(["repeater","room","companion","chat1","chat2","chat3","chat4","chat5"][p.x]||("port "+p.x)):"another identity";
        tr.innerHTML="<td>"+when+"</td>"+airCell(p)+crCell(p)+"<td class=mut>-</td><td class=mut>-</td><td class=mut>-</td>"+
          "<td style='color:#e0b34d'>TX-BUSY:"+esc(p.d)+"</td><td colspan=5 class=mut><span id='ci-"+p.s+"'></span>"+
          "send deferred — "+esc(owner)+" was transmitting (will retry)</td>";
      } else if(p.e===2){   // TX never completed
        tr.innerHTML="<td>"+when+"</td>"+airCell(p)+crCell(p)+"<td class=mut>-</td><td class=mut>-</td><td class=mut>-</td>"+
          "<td style='color:#e05d5d'>TX-FAIL:"+esc(p.d)+"</td><td colspan=5 class=mut><span id='ci-"+p.s+"'></span>"+
          "send timed out before TX-done (radio contention?)</td>";
      } else {
        const a=annot(p);
        // An empty path means no repeater has appended its hash yet, so this is
        // the originator's own transmission reaching us over the air. Anything
        // with hops has been through at least one relay.
        let badge="";
        if(rx&&a.hops===0){
          badge+="<span class='relay direct' title=\"zero hops — heard straight from the sender, "+
            "no repeater in between\">&#9679; direct</span>";
          tr.className="directrow";
        }
        // a received packet carrying our own hash = someone relayed us
        if(rx&&a.relay){
          const sure=a.relay.width>=2;
          // can't collide with the direct badge — a relayed packet has hops by
          // definition, so it is never zero-hop — but append rather than assign
          badge+="<span class='relay "+(sure?"sure":"weak")+"' title=\""+
            (sure?"a neighbour relayed a packet we sent — "+a.relay.width+"-byte hash match, effectively certain"
                 :"possible relay of our packet — 1-byte hash, ~1 in 256 chance of coincidence")+
            "\">&#8618; relayed "+esc(a.relay.role)+(sure?"":"?")+"</span>";
          tr.className="relayrow";
        }
        tr.innerHTML="<td>"+when+"</td>"+airCell(p)+crCell(p)+
          "<td>"+(rx?snrSpan(p.snr):"")+"</td><td>"+(rx?rssiSpan(p.rssi):"")+"</td><td>"+p.l+"</td>"+
          "<td class="+(rx?"ok":"err")+">"+(rx?"RX":"TX:"+p.d)+"</td><td>"+
          ROUTES[p.h&3]+"</td><td>"+PTYPES[(p.h>>2)&15]+"</td>"+hashCell(p)+"<td>"+esc(a.src)+
          "</td><td class=mut><span id='ci-"+p.s+"'></span>"+badge+a.infoHtml+"</td>";
        tr.style.cursor="pointer";
        tr.title="click to decode";
        tr.onclick=()=>openPktModal(p,when);
      }
      const tb=$("pkt-rows"); tb.insertBefore(tr,tb.firstChild);
      while(tb.children.length>120) tb.removeChild(tb.lastChild);
    }
    if(lastDebug&&lastDebug.radio&&typeof lastDebug.radio.noise==="number"&&devNow){
      const last=noiseLog[noiseLog.length-1];
      if(!last||devNow-last.t>2000) noiseLog.push({t:devNow,n:lastDebug.radio.noise});
      if(noiseLog.length>600) noiseLog.splice(0,noiseLog.length-600);
    }
    $("pkt-count").textContent="(seq "+lastSeq+")";
    drawAirTimeline();
  }catch(e){}
}

// ---- mesh tab ----
const KINDS=["?","chat","repeater","room","sensor"];
function age(sec){ if(sec<=0)return"-"; if(sec<90)return sec+"s"; if(sec<5400)return Math.round(sec/60)+"m";
  if(sec<129600)return Math.round(sec/3600)+"h"; return Math.round(sec/86400)+"d"; }
// A peer's clock minus ours, signed. Colour is about consequence, not neatness:
// MeshCore stamps messages and adverts with the SENDER's clock, so a node tens
// of seconds out shows its messages in the wrong order in any client that sorts
// by timestamp, and one minutes out makes advert freshness comparisons wrong.
// Blank (not zero) when we have never heard the node advert at zero hops.
function skewCell(p){
  if(p.clk_d===undefined||p.clk_d===null) return "<span class=mut>-</span>";
  const d=p.clk_d, a=Math.abs(d);
  const mag=a<90?a+"s":a<5400?Math.round(a/60)+"m":a<129600?Math.round(a/3600)+"h":Math.round(a/86400)+"d";
  const txt=(d<0?"-":"+")+mag;
  // deltas are quantised to whole seconds and the advert sat in a queue for
  // some of that, so anything inside a few seconds is measurement noise
  const style=a<=5?"class=ok":a<=60?"style='color:#e0b34d'":"class=err";
  // age() renders 0 as "-", which reads as "unknown" rather than "just now"
  const when=p.clk_age_s>0?age(p.clk_age_s)+" ago":"just now";
  const t="their clock is "+(d<0?mag+" behind":d>0?mag+" ahead of":"level with")+" ours · measured "+
          when+" from "+p.clk_n+" zero-hop advert"+(p.clk_n===1?"":"s");
  return "<span "+style+" title='"+t+"'>"+txt+"</span>";
}
async function buildMesh(){
  if(!compReady){ try{ await initComp(); }catch(e){} }
  const nbrs=neighbours();
  const nowS=Math.floor(Date.now()/1000);

  // ---- table ----
  let rows="";
  [...contacts].sort((a,b)=>b.lastAdvert-a.lastAdvert).forEach(c=>{
    const ci=contacts.indexOf(c);
    const heard=(c.pk in advHops)?(advHops[c.pk]===0?"direct RF":"heard "+advHops[c.pk]+" hop"+(advHops[c.pk]>1?"s":"")+" away"):"";
    const route=(c.outPathLen===255?"flood (no route)":c.outPathLen===0?"direct":
      c.outPathLen+" hop"+(c.outPathLen>1?"s":"")+" "+hopsHtml(c.outPath))+(heard?" · "+heard:"");
    const hasLoc=(c.lat||c.lon);
    const traceable=c.outPathLen>0&&c.outPathLen<=64;
    rows+="<tr><td>"+esc(c.name||"?")+"</td><td class=mut>"+c.prefix.slice(0,8)+"</td><td>"+(KINDS[c.type]||c.type)+
      "</td><td>"+route+"</td><td>"+age(nowS-c.lastAdvert)+"</td><td>"+(hasLoc?c.lat.toFixed(4)+","+c.lon.toFixed(4):"-")+
      "</td><td>"+(hasLoc&&selfLoc&&(selfLoc[0]||selfLoc[1])?distKm(selfLoc,[c.lat,c.lon])+"km":"-")+"</td><td>-</td><td>"+
      (traceable?"<button class=sec style='padding:1px 8px' onclick='traceContact("+ci+")'>trace</button>":"")+"</td></tr>";
  });
  for(const n of nbrs){
    if(contacts.some(c=>c.pk.startsWith(n.prefix.toLowerCase()))) continue;   // already listed
    rows+="<tr><td class=mut>(neighbour)</td><td class=mut>"+n.prefix.toLowerCase()+"</td><td>repeater?</td>"+
      "<td>direct (heard)</td><td>"+age(n.secsAgo)+"</td><td>-</td><td>-</td><td>"+snrSpan(n.snr.toFixed(1))+"</td><td></td></tr>";
  }
  // remembered neighbours (persisted snapshot; survive reboot/OTA)
  if(lastDebug&&lastDebug.saved_nbrs){
    for(const l of lastDebug.saved_nbrs.split("\n")){
      const p=l.split(",");
      if(p.length<3||!/^[0-9a-f]{8}$/i.test(p[0])) continue;
      const pfx=p[0].toLowerCase();
      if(nbrs.some(n=>n.prefix.toLowerCase()===pfx)) continue;               // live row exists
      if(contacts.some(c=>c.pk.startsWith(pfx))) continue;                   // contact row exists
      const agoS=Math.max(0,lastDebug.epoch-(+p[1]||0));
      rows+="<tr><td class=mut>(remembered)</td><td class=mut>"+pfx+"</td><td>repeater?</td>"+
        "<td class=mut>heard before restart</td><td>"+age(agoS)+"</td><td>-</td><td>-</td><td class=mut>"+
        ((+p[2]||0)/4).toFixed(1)+"</td><td></td></tr>";
    }
  }
  $("mesh-rows").innerHTML=rows||"<tr><td colspan=9 class=mut>nothing heard yet — wait for adverts or send one</td></tr>";

  // ---- graph ----
  // nodes keyed by id; levels = hop distance from self
  const nodes=new Map(), edges=[];
  const addNode=(id,label,kind,level)=>{ const n=nodes.get(id);
    if(n){ if(level<n.level)n.level=level; return n; }
    const nn={id,label,kind,level}; nodes.set(id,nn); return nn; };
  const addEdge=(a,b,label,dashed)=>{ if(!edges.some(e=>e.a===a&&e.b===b)) edges.push({a,b,label,dashed}); };
  addNode("self","this node","self",0);
  const hopId=h=>{
    const m=contacts.filter(c=>parseInt(c.pk.slice(0,2),16)===h);
    if(m.length===1) return m[0].pk;                    // unique contact match
    const nm=nbrs.filter(n=>parseInt(n.prefix.slice(0,2),16)===h);
    if(m.length===0&&nm.length===1) return "nbr:"+nm[0].prefix.toLowerCase();
    return "hop:"+hx1(h);
  };
  for(const n of nbrs){
    const cm=contacts.find(c=>c.pk.startsWith(n.prefix.toLowerCase()));
    const id=cm?cm.pk:"nbr:"+n.prefix.toLowerCase();
    addNode(id,cm?(cm.name||cm.prefix.slice(0,8)):n.prefix.toLowerCase().slice(0,8),cm?"contact":"nbr",1);
    addEdge("self",id,"snr "+n.snr.toFixed(1),false);
  }
  for(const c of contacts){
    if(c.outPathLen===0){
      addNode(c.pk,c.name||c.prefix.slice(0,8),"contact",1);
      addEdge("self",c.pk,"direct",false);
    } else if(c.outPathLen>0&&c.outPathLen<=64){
      let prev="self";
      c.outPath.forEach((h,i)=>{
        const id=hopId(h);
        if(id!==c.pk){
          const known=nodes.get(id);
          addNode(id,known?known.label:(id.startsWith("hop:")?"?"+hx1(h):id.slice(4,12)),
            id.startsWith("hop:")?"hop":"nbr",i+1);
        }
        if(id!==prev) addEdge(prev,id,null,false);
        prev=id;
      });
      addNode(c.pk,c.name||c.prefix.slice(0,8),"contact",c.outPathLen+1);
      if(prev!==c.pk) addEdge(prev,c.pk,null,false);
    } else {                                            // 255: no route learned
      addNode(c.pk,c.name||c.prefix.slice(0,8),"contact",2);
      addEdge("self",c.pk,"flood",true);
    }
  }
  meshCtx={nodes,edges,nbrs};
  renderMeshSvg(nodes,edges);
  if(!meshViewChosen){                      // default to geo when we have coordinates
    meshViewChosen=true;
    if(contacts.some(c=>c.lat||c.lon)) meshView("geo");
  }
}
let meshViewChosen=false;
let meshCtx=null, traceChain=null;
function hopIdG(h){
  const m=contacts.filter(c=>parseInt(c.pk.slice(0,2),16)===h);
  if(m.length===1) return m[0].pk;
  const nm=(meshCtx?meshCtx.nbrs:[]).filter(n=>parseInt(n.prefix.slice(0,2),16)===h);
  if(m.length===0&&nm.length===1) return "nbr:"+nm[0].prefix.toLowerCase();
  return "hop:"+hx1(h);
}
async function traceContact(ci){
  const c=contacts[ci];
  if(!meshCtx) await buildMesh();
  const tag=crypto.getRandomValues(new Uint32Array(1))[0];
  const body=[36,...le32(tag),...le32(0),0,...c.outPath];   // CMD_SEND_TRACE_PATH, auth=0, flags=0 (1-byte hashes)
  $("trace-status").textContent="tracing route to "+(c.name||c.prefix.slice(0,8))+"...";
  const fs=await frames(body);
  const sent=fs.find(f=>f[0]===6);
  if(!sent){ $("trace-status").textContent="trace failed to send ("+fs.map(f=>f[0])+")"; return; }
  const timeout=Math.min(rdU32(sent,6)+3000,30000);
  const t0=Date.now(), seen=archSeq;
  while(Date.now()-t0<timeout){
    await new Promise(r=>setTimeout(r,700));
    try{
      const r=await api("/api/multi/comp/archive?after="+seen);
      const buf=new Uint8Array(await r.arrayBuffer());
      let o=4;
      while(o+6<=buf.length){
        const l=buf[o+4]|(buf[o+5]<<8); const f=buf.slice(o+6,o+6+l); o+=6+l;
        if(f[0]===0x89&&rdU32(f,4)===tag){ showTrace(c,f,Date.now()-t0); return; }
      }
    }catch(e){}
  }
  $("trace-status").textContent="trace to "+(c.name||"node")+" timed out — path may be stale or node offline";
}
function showTrace(c,f,rtt){
  const pl=f[2];
  const hashes=Array.from(f.slice(12,12+pl));
  const snrs=Array.from(f.slice(12+pl,12+pl+pl)).map(x=>((x<<24)>>24)/4);
  const fin=((f[12+pl+pl]<<24)>>24)/4;
  const ids=["self",...hashes.map(hopIdG)];
  traceChain=[];
  let desc="trace "+esc(c.name||c.prefix.slice(0,8))+" ("+rtt+"ms round trip): ";
  for(let i=0;i<hashes.length;i++){
    traceChain.push({a:ids[i],b:ids[i+1],label:snrs[i].toFixed(1)+"dB"});
    desc+=(i?" &rarr; ":"")+hopsHtml([hashes[i]])+" ("+snrs[i].toFixed(1)+"dB)";
  }
  desc+=" &rarr; back to self ("+fin.toFixed(1)+"dB)";
  $("trace-status").innerHTML=desc;
  if(meshCtx) renderMeshSvg(meshCtx.nodes,meshCtx.edges);
}

// ---- geo view ----
let geoMap=null;
function meshView(v){
  $("mesh-log-wrap").style.display=v==="log"?"":"none";
  $("mesh-geo-wrap").style.display=v==="geo"?"":"none";
  if(v==="geo") geoRender();
}
async function ensureLeaflet(){
  if(window.L) return;
  await new Promise((res,rej)=>{
    const l=document.createElement("link"); l.rel="stylesheet";
    l.href="https://unpkg.com/leaflet@1.9.4/dist/leaflet.css"; document.head.appendChild(l);
    const s=document.createElement("script"); s.src="https://unpkg.com/leaflet@1.9.4/dist/leaflet.js";
    s.onload=res; s.onerror=rej; document.body.appendChild(s);
  });
}
async function geoRender(){
  try{ await ensureLeaflet(); }
  catch(e){ $("trace-status").textContent="couldn't load map library (browser needs internet for tiles)"; return; }
  if(!compReady){ try{ await initComp(); }catch(e){} }
  if(!geoMap){
    geoMap=L.map("geo-map");
    L.tileLayer("https://tile.openstreetmap.org/{z}/{x}/{y}.png",{maxZoom:19,attribution:"&copy; OpenStreetMap"}).addTo(geoMap);
    geoMap._lg=L.layerGroup().addTo(geoMap);
  }
  const g=geoMap._lg; g.clearLayers();
  const pts=[];
  const haveSelf=selfLoc&&(selfLoc[0]||selfLoc[1]);
  if(haveSelf){ pts.push(selfLoc);
    L.circleMarker(selfLoc,{radius:9,color:"#4da3ff",weight:2}).bindPopup("<b>this node</b>").addTo(g); }
  for(const c of contacts){
    if(!(c.lat||c.lon)) continue;
    const p=[c.lat,c.lon]; pts.push(p);
    const route=c.outPathLen===255?"no route (flood)":c.outPathLen===0?"direct":c.outPathLen+" hops";
    const heard=(c.pk in advHops)?("<br>heard "+(advHops[c.pk]===0?"directly":advHops[c.pk]+" hops away")):"";
    L.circleMarker(p,{radius:7,color:c.outPathLen===0?"#4da3ff":"#39c07a",weight:2})
      .bindPopup("<b>"+esc(c.name||c.prefix.slice(0,8))+"</b><br>"+(KINDS[c.type]||"?")+" · "+route+heard+
        (haveSelf?"<br>~"+distKm(selfLoc,p)+" km":"")).addTo(g);
    if(haveSelf) L.polyline([selfLoc,p],{color:"#5c6875",weight:1.2,
      dashArray:c.outPathLen===255?"4,6":null}).addTo(g);
  }
  // Radio neighbours from the peer table, drawn over the contact layer. These
  // are the nodes we can actually reach by radio rather than through the mesh,
  // so they get solid links and their own colours: blue = one hop (we have
  // heard its own transmission), amber = two.
  const peers=(lastDebug&&lastDebug.peers)||[];
  for(const p of peers){
    if(!p.hops||p.hops>2) continue;
    const id=peerIdent(p);
    if(!(id.lat||id.lon)) continue;
    const q=[id.lat,id.lon]; pts.push(q);
    const one=p.hops===1;
    const col=one?"#4da3ff":"#e0b34d";
    const conf=p.heard_us>0?"<br><b>confirmed to hear us</b> ("+p.heard_us+"x)":
               (p.hu1>0?"<br><span style='color:#8b98a5'>"+p.hu1+" x 1-byte match only \u2014 unconfirmed</span>":"");
    L.circleMarker(q,{radius:one?9:7,color:col,weight:3,fillOpacity:0.25}).bindPopup(
      "<b>"+esc(id.name||p.h)+"</b><br>"+p.hops+" hop"+(p.hops>1?"s":"")+
      " \u00b7 we heard it "+p.direct+"x"+
      (p.snr!==null&&p.snr!==undefined?"<br>SNR "+p.snr+" dB":"")+conf+
      (haveSelf?"<br>~"+distKm(selfLoc,q)+" km":"")).addTo(g);
    if(haveSelf) L.polyline([selfLoc,q],{color:col,weight:one?2.6:1.8,opacity:0.85}).addTo(g);
  }
  if(pts.length){ geoMap.fitBounds(pts,{padding:[40,40]}); }
  else $("trace-status").textContent="no nodes with a location yet — locations come from adverts";
  setTimeout(()=>geoMap.invalidateSize(),150);
}

function renderMeshSvg(nodes,edges){
  const W=900,H=480,cx=W/2,cy=H/2;
  const byLevel={};
  for(const n of nodes.values()){ (byLevel[n.level]=byLevel[n.level]||[]).push(n); }
  for(const [lvl,arr] of Object.entries(byLevel)){
    const r=+lvl===0?0:70+(+lvl)*95;
    arr.forEach((n,i)=>{ const a=(2*Math.PI*i)/arr.length-Math.PI/2+(+lvl%2?0.35:0);
      n.x=cx+r*Math.cos(a); n.y=cy+r*Math.sin(a)*(H-80)/(W-80); });
  }
  const col={self:"#4da3ff",contact:"#39c07a",nbr:"#c9a227",hop:"#5c6875"};
  let s="";
  for(const e of edges){
    const a=nodes.get(e.a),b=nodes.get(e.b); if(!a||!b) continue;
    s+="<line x1="+a.x.toFixed(0)+" y1="+a.y.toFixed(0)+" x2="+b.x.toFixed(0)+" y2="+b.y.toFixed(0)+
      " stroke=#3a4552 stroke-width=1.5"+(e.dashed?" stroke-dasharray=\"5,5\"":"")+"/>";
    if(e.label) s+="<text x="+((a.x+b.x)/2).toFixed(0)+" y="+((a.y+b.y)/2-4).toFixed(0)+
      " fill=#8b98a8 font-size=10 text-anchor=middle>"+esc(e.label)+"</text>";
  }
  if(traceChain){                       // traced route drawn on top, green + per-hop SNR
    for(const e of traceChain){
      const a=nodes.get(e.a),b=nodes.get(e.b); if(!a||!b) continue;
      s+="<line x1="+a.x.toFixed(0)+" y1="+a.y.toFixed(0)+" x2="+b.x.toFixed(0)+" y2="+b.y.toFixed(0)+
        " stroke=#39c07a stroke-width=3 stroke-opacity=0.85 />";
      s+="<text x="+((a.x+b.x)/2).toFixed(0)+" y="+((a.y+b.y)/2+12).toFixed(0)+
        " fill=#39c07a font-size=11 font-weight=600 text-anchor=middle>"+esc(e.label)+"</text>";
    }
  }
  for(const n of nodes.values()){
    const r=n.kind==="self"?22:n.kind==="hop"?10:16;
    s+="<circle cx="+n.x.toFixed(0)+" cy="+n.y.toFixed(0)+" r="+r+" fill="+col[n.kind]+
      " fill-opacity=0.18 stroke="+col[n.kind]+" stroke-width=1.6 />";
    s+="<text x="+n.x.toFixed(0)+" y="+(n.y+r+13).toFixed(0)+" fill=#dbe4ee font-size=11 text-anchor=middle>"+
      esc(n.label)+"</text>";
  }
  $("mesh-svg").innerHTML=s;
}

// ---- packet decode modal ----
function openPktModal(p,when,corruptWhy){
  const q=parsePkt(p);
  let h="<h3 style='margin:0 0 8px'>Packet @ "+when+" <button class=sec style='float:right;padding:2px 10px' onclick=\"$('pkt-modal').style.display='none'\">close</button></h3>";
  if(corruptWhy){
    h+="<div class=warn style='margin-bottom:8px'><b>Corrupt frame — "+esc(corruptWhy)+".</b> "+
       "Everything below is a best-effort decode of damaged bytes: any field may be wrong, and the length itself is "+
       "suspect. Compare it with a clean copy of the same packet later in the burst to see how much was hit.</div>";
  }
  const rx=p.d==="rx";
  h+="<div class=mut style='font-size:12px;margin-bottom:8px'>"+(rx?"received":"sent by "+esc(p.d))+
    " · "+ROUTES[p.h&3]+" · "+PTYPES[(p.h>>2)&15]+" · "+p.l+" bytes on air"+
    (p.cr?" · CR 4/"+p.cr+(rx?" (the sender's"+(radioCr&&p.cr!==radioCr?", not our 4/"+radioCr:"")+")":""):"")+
    (rx?" · SNR "+snrSpan(p.snr)+" dB · RSSI "+rssiSpan(p.rssi)+" dBm":"")+"</div>";
  // If this packet shared the channel, say so here too, with both transmitters
  // named — the reader is already looking at one of them.
  const ce=airLog.find(e=>e.s===p.s);
  if(ce&&ce.hit){
    const o=ce.hit.other;
    h+="<div class=warn style='margin-bottom:8px'><b>Collided</b> — "+ce.hit.lap+
       " ms of this packet's airtime was shared with "+(o.rx?"a packet received":"a transmit started")+
       " at "+stamp(o.rx?o.end:o.start)+(o.err?" (which failed)":"")+".<br>Transmitters: "+
       prefixChip(txPrefix(p))+" &#10005; "+prefixChip(txPrefix(o.pkt))+
       " <span class=mut>— hover a prefix for the node it is likely to be</span></div>";
  }
  if(q){
    h+="<div style='font-size:13px'>";
    h+="<div><span class=mut>header</span> 0x"+hx1(p.h)+" — route "+ROUTES[q.route]+", type "+PTYPES[q.type]+", ver "+((p.h>>6)&3)+"</div>";
    if(q.tc) h+="<div><span class=mut>transport codes</span> "+hexa(q.tc.slice(0,2))+" / "+hexa(q.tc.slice(2,4))+"</div>";
    if(q.hops) h+="<div><span class=mut>path</span> "+q.hops+" hop"+(q.hops>1?"s":"")+" ("+q.hsz+"-byte hashes): "+hopsHtml(q.groups)+"</div>";
    // per-type decode
    const pay=q.pay;
    if(q.type===4&&pay.length>=100){
      const pk=hexa(pay.slice(0,32));
      const ts=rdU32a(pay,32);
      const ad=pay.slice(100);
      let name="",lat=null,lon=null;
      if(ad.length){ const fl=ad[0]; let i=1;
        if(fl&0x10){ lat=rdI32a(ad,i)/1e6; lon=rdI32a(ad,i+4)/1e6; i+=8; }
        if(fl&0x20)i+=2; if(fl&0x40)i+=2;
        if(fl&0x80) name=new TextDecoder().decode(new Uint8Array(ad.slice(i))).replace(/\0.*$/,"");
      }
      const c=contacts.find(c=>c.pk===pk);
      h+="<div style='margin-top:6px'><b>Advert</b> from "+esc(name||(c&&c.name)||pk.slice(0,12))+"</div>";
      h+="<div><span class=mut>pubkey</span> <span style='font-family:monospace;font-size:11px;word-break:break-all'>"+pk+"</span></div>";
      h+="<div><span class=mut>advert time</span> "+new Date(ts*1000).toLocaleString()+"</div>";
      if(lat!==null){
        h+="<div><span class=mut>origin location</span> "+lat.toFixed(5)+", "+lon.toFixed(5)+
          (selfLoc&&(selfLoc[0]||selfLoc[1])?" — <b>"+distKm(selfLoc,[lat,lon])+" km from here</b>":"")+"</div>";
      }
    } else if(q.type===3){
      h+="<div style='margin-top:6px'><b>ACK</b> code "+hexa(pay.slice(0,4))+"</div>";
    } else if(pay.length>=2&&q.type!==5&&q.type!==6){
      h+="<div style='margin-top:6px'><span class=mut>dest hash</span> "+hx1(pay[0])+
        (hashName(pay[0])?" — likely "+esc(hashName(pay[0])):"")+
        " &nbsp; <span class=mut>src hash</span> "+hx1(pay[1])+
        (hashName(pay[1])?" — likely "+esc(hashName(pay[1])):"")+"</div>";
    }
    // likely transmitter: last hop for flooded packets, origin when zero-hop RX
    if(rx){
      h+="<div style='margin-top:10px'><b>Likely transmitted by</b></div>";
      let cands=[];
      if(q.hops>0) cands=hopCandObjs(q.groups[q.groups.length-1]);
      else if(q.type===4&&pay.length>=32){ cands=hopCandObjs(Array.from(pay.slice(0,4))); if(!cands.length) cands=[{name:"the advert origin (heard directly)",kind:"",dist:null,last:""}]; }
      if(cands.length){
        h+=cands.map(c=>"<div style='font-size:12px'>· "+esc(c.name)+(c.kind?" <span class=mut>("+c.kind+")</span>":"")+
          (c.dist!==null&&c.dist!==0?" — ~"+c.dist+" km away":"")+(c.last&&c.last!=="now"?" <span class=mut>last advert "+c.last+" ago</span>":"")+"</div>").join("");
      } else {
        h+="<div class=mut style='font-size:12px'>no match in the known-nodes database"+
          (q.hops?" for hash "+gHex(q.groups[q.groups.length-1]):"")+"</div>";
      }
    }
    h+="</div>";
    // hex dump
    h+="<div class=mut style='margin-top:10px;font-size:11px'>raw ("+q.bytes.length+" bytes captured)</div><pre style='font-size:11px'>";
    for(let i=0;i<q.bytes.length;i+=16){
      h+=String(i).padStart(4,"0")+"  "+q.bytes.slice(i,i+16).map(hx1).join(" ")+"\n";
    }
    h+="</pre>";
  } else h+="<div class=mut>no captured bytes to decode</div>";
  $("pkt-modal-body").innerHTML=h;
  $("pkt-modal").style.display="flex";
}
function rdU32a(b,o){ return (b[o]|(b[o+1]<<8)|(b[o+2]<<16)|((b[o+3]<<24)>>>0))>>>0; }

// ---- room tab ----
async function roomCmd(c){
  if(!c) return;
  const out=$("room-out");
  const r=await cmd("room "+c);
  out.textContent=("> room "+c+"\n"+r+"\n\n"+out.textContent).slice(0,4000);
  return r;
}
let fullKeys={};
async function loadFullKeys(){
  if(fullKeys.room) return;
  const r=await cmd("identities full");
  for(const m of r.matchAll(/(\w+)=([0-9a-f]{64})/g)) fullKeys[m[1]]=m[2];
}
async function loadRoomTab(){
  try{
    const name=stripReply(await roomCmd("get name"));
    $("room-name").value=name; $("room-title").textContent="— "+name;
    railNames.room=name;
    $("room-pwd").value=stripReply(await cmd("room get guest.password"));
    $("room-lat").value=stripReply(await cmd("room get lat"));
    $("room-lon").value=stripReply(await cmd("room get lon"));
    const adv=parseInt(stripReply(await cmd("room get advert.interval")))||0;
    const fadv=parseInt(stripReply(await cmd("room get flood.advert.interval")))||0;
    $("room-private").checked=(adv===0&&fadv===0);
    $("room-priv-state").textContent=(adv===0&&fadv===0)?"room is silent (no periodic adverts)":
      "announcing: local every "+adv+"min, flood every "+fadv+"h";
    await loadFullKeys();
    buildRoomShare(name);
  }catch(e){}
  loadRoomPosts();
}
// Log this box's own chat client into this box's room. Works because the
// shared-radio arbiter loops locally-transmitted frames back to the sibling
// identities (they share one antenna and can't otherwise hear each other).
async function joinOwnRoom(){
  const st=$("room-join-state");
  st.textContent="joining...";
  if(!compReady) await initComp();
  await loadFullKeys();
  const pk=fullKeys.room, pw=v("room-pwd");
  if(!pk){ st.textContent="room pubkey unavailable"; return; }
  if(!pw){ st.textContent="load or set the join password first"; return; }
  await loadContacts();
  if(!contacts.some(c=>c.pk===pk)){
    st.textContent="the chat client hasn't heard the room yet — press 'Announce once now', wait a few seconds, retry";
    return;
  }
  const seen=archSeq;
  const body=[26,...pk.match(/../g).map(h=>parseInt(h,16)),...Array.from(new TextEncoder().encode(pw))];
  const fs=await frames(body,6000,600);
  if(!fs.some(f=>f[0]===6)){ st.textContent="login not sent ("+fs.map(f=>f[0]).join(",")+")"; return; }
  st.textContent="login sent, waiting for the room to answer...";
  for(let i=0;i<12;i++){
    await new Promise(r=>setTimeout(r,1500));
    try{
      const r=await api("/api/multi/comp/archive?after="+seen);
      const buf=new Uint8Array(await r.arrayBuffer());
      let o=4;
      while(o+6<=buf.length){
        const l=buf[o+4]|(buf[o+5]<<8), f=buf.slice(o+6,o+6+l); o+=6+l;
        if(f[0]===0x85){ st.innerHTML="<span class=ok>joined — the chat client is now a member</span>"; return; }
        if(f[0]===0x86){ st.innerHTML="<span class=err>rejected — wrong join password</span>"; return; }
      }
    }catch(e){}
  }
  st.textContent="no answer yet (the room may still be busy) — check the ACL";
}
function genRoomPwd(){
  const ab="abcdefghijkmnopqrstuvwxyz23456789";
  const r=crypto.getRandomValues(new Uint8Array(12));
  $("room-pwd").value=Array.from(r,b=>ab[b%33]).join("");
  $("room-priv-state").textContent="generated — press Save to apply, then share it with members";
}
async function roomPrivacy(on){
  if(on){
    localStorage.setItem("mp_room_adv",JSON.stringify({
      a:stripReply(await cmd("room get advert.interval")),
      f:stripReply(await cmd("room get flood.advert.interval"))}));
    await cmd("room set advert.interval 0"); await cmd("room set flood.advert.interval 0");
    $("room-priv-state").textContent="room is now silent — remember to set a strong join password";
  }else{
    let prev={}; try{ prev=JSON.parse(localStorage.getItem("mp_room_adv")||"{}"); }catch(e){}
    await cmd("room set advert.interval "+(parseInt(prev.a)||60));
    await cmd("room set flood.advert.interval "+(parseInt(prev.f)||12));
    $("room-priv-state").textContent="announcing restored";
  }
}
function buildRoomShare(name){
  const pk=fullKeys.room;
  if(!pk){ $("room-uri").textContent="(pubkey unavailable)"; return; }
  const uri="meshcore://contact/add?name="+encodeURIComponent(name||"Room")+"&public_key="+pk+"&type=3";
  $("room-uri").textContent=uri;
  renderQr("room-qr",uri);
}
async function ensureQrLib(){
  if(window.qrcode) return;
  await new Promise((res,rej)=>{
    const s=document.createElement("script");
    s.src="https://unpkg.com/qrcode-generator@1.4.4/qrcode.js";
    s.onload=res; s.onerror=rej; document.body.appendChild(s);
  });
}
async function renderQr(id,text){
  try{
    await ensureQrLib();
    const qr=qrcode(0,"M"); qr.addData(text); qr.make();
    $(id).innerHTML=qr.createImgTag(3,6);
  }catch(e){ $(id).innerHTML="<span class=mut style='color:#333;font-size:11px'>QR lib offline — use the link</span>"; }
}
async function loadRoomPosts(){
  try{
    const posts=await (await api("/api/multi/room/posts")).json();
    if(!posts.length){ $("room-posts").innerHTML="<span class=mut>no stored posts yet — members' messages appear here</span>"; return; }
    $("room-posts").innerHTML=posts.map(p=>{
      const who=contactName(p.a)||p.a.slice(0,8);
      return "<div class=msg><span class=who>"+esc(who)+"</span><span class=meta>"+
        new Date(p.t*1000).toLocaleString()+"</span><br>"+esc(p.x)+"</div>";
    }).join("");
  }catch(e){ $("room-posts").textContent="failed to load posts"; }
}

// ---- companion frame protocol ----
const u8=a=>new Uint8Array(a);
function le32(n){ return [n&255,(n>>8)&255,(n>>16)&255,(n>>>24)&255]; }
function rdU32(b,o){ return b[o]|(b[o+1]<<8)|(b[o+2]<<16)|((b[o+3]<<24)>>>0); }
function hex(b,o,n){ let s=""; for(let i=o;i<o+n;i++) s+=b[i].toString(16).padStart(2,"0"); return s; }
function txt(b,o,n){ let e=o; while(e<o+n && b[e]!==0) e++; return new TextDecoder().decode(b.slice(o,e)); }
async function frames(bytes,t,i){
  const q="?t="+(t||2500)+"&i="+(i||300);
  const r=await api("/api/multi/comp/frame"+q,{method:"POST",body:u8(bytes)});
  const buf=new Uint8Array(await r.arrayBuffer());
  const out=[]; let o=0;
  while(o+2<=buf.length){ const l=buf[o]|(buf[o+1]<<8); out.push(buf.slice(o+2,o+2+l)); o+=2+l; }
  return out;
}
const now=()=>Math.floor(Date.now()/1000);

let compReady=false, contacts=[], selContact=null, chansLoaded=false, channels=[], selChan=null;
let msgLog=JSON.parse(localStorage.getItem("mp_msgs")||"[]");
let chanLog=JSON.parse(localStorage.getItem("mp_chans")||"[]");

async function initComp(){
  try{
    await frames([22,3]);                                   // DEVICE_QUERY: protocol v3
    const fs=await frames([1,0,0,0,0,0,0,0, 77,117,108,116,105,87,101,98]); // APP_START "MultiWeb"
    for(const f of fs) if(f[0]===5){
      $("self-name").textContent="("+txt(f,58,f.length-58)+")";
      selfLoc=[rdI32(f,36)/1e6, rdI32(f,40)/1e6];           // for advert distance calc
    }
    compReady=true;
    await loadContacts();
    renderMsgs();
  }catch(e){ $("msg-status").textContent="companion init failed: "+e; }
}
function rdI32(b,o){ return (b[o]|(b[o+1]<<8)|(b[o+2]<<16)|(b[o+3]<<24)); }
async function loadContacts(){
  const fs=await frames([4],5000,500);                      // GET_CONTACTS
  contacts=[];
  for(const f of fs){
    if(f[0]!==3||f.length<148) continue;
    const opl=f[35];
    contacts.push({pk:hex(f,1,32),prefix:hex(f,1,6),type:f[33],name:txt(f,100,32),
      lastAdvert:rdU32(f,132),outPathLen:opl,
      outPath:(opl>0&&opl<=64)?Array.from(f.slice(36,36+opl)):[],
      lat:rdI32(f,136)/1e6,lon:rdI32(f,140)/1e6});
  }
  const el=$("contacts");
  if(!contacts.length){ el.textContent="no contacts yet — they appear when other nodes advert"; return; }
  el.innerHTML=contacts.map((c,i)=>"<div data-i="+i+" class="+(selContact&&c.pk===selContact.pk?"sel":"")+">"+
    esc(c.name||c.prefix)+" <span class=mut style=font-size:11px>"+c.prefix.slice(0,8)+"</span></div>").join("");
  el.querySelectorAll("div").forEach(d=>d.onclick=()=>{ selContact=contacts[+d.dataset.i]; loadContactsRender(); renderMsgs(); });
}
function loadContactsRender(){
  $("contacts").querySelectorAll("div").forEach(d=>
    d.classList.toggle("sel",selContact&&contacts[+d.dataset.i].pk===selContact.pk));
}
function saveLogs(){
  localStorage.setItem("mp_msgs",JSON.stringify(msgLog.slice(-200)));
  localStorage.setItem("mp_chans",JSON.stringify(chanLog.slice(-200)));
}
function renderMsgs(){
  const el=$("msg-log");
  const items=selContact?msgLog.filter(m=>m.prefix===selContact.prefix):msgLog;
  el.innerHTML=items.map(m=>"<div class='msg"+(m.out?" out":"")+"'><span class=who>"+
    esc(m.out?"you → "+(m.name||m.prefix):(m.name||m.prefix))+"</span><span class=meta>"+
    new Date(m.ts*1000).toLocaleString()+(m.snr!==undefined?" SNR "+m.snr:"")+"</span><br>"+esc(m.text)+"</div>").join("")
    ||"<div class=mut>no messages"+(selContact?" with "+esc(selContact.name):"")+"</div>";
  el.scrollTop=el.scrollHeight;
}
function contactName(prefix){ const c=contacts.find(c=>c.prefix.startsWith(prefix)); return c?c.name:null; }
// Set any identity's private key (128 hex). Goes through the composition so
// the filesystem copy AND the NVS mirror are both written; takes effect on
// reboot. For the repeater/room the stock 'set prv.key' would also work, but
// it wouldn't update the mirror — which would then restore the OLD key.
async function setRoleKey(role,inputId,statusId){
  const k=v(inputId).replace(/\s+/g,"");
  const st=$(statusId);
  if(!/^[0-9a-fA-F]{128}$/.test(k)&&!/^[0-9a-fA-F]{192}$/.test(k)){
    st.textContent="expected 128 hex characters (private key)"; return;
  }
  if(!confirm("Replace the "+role+" identity? Its current key is lost unless you have a copy, and peers must re-add it."))return;
  const r=stripReply(await cmd("set identity."+role+" "+k));
  st.textContent=r;
  if(/^OK/.test(r)&&confirm("Reboot now to apply?")) cmd("reboot");
}
async function exportChatKey(){
  if(!compReady) await initComp();
  const fs=await frames([23]);                       // CMD_EXPORT_PRIVATE_KEY
  const f=fs.find(f=>f[0]===14);                     // RESP_CODE_PRIVATE_KEY
  if(!f){ $("chat-key-status").textContent="export unavailable"; return; }
  $("chat-prv").value=hex(f,1,f.length-1);
  $("chat-key-status").textContent="private key shown — keep it secret";
}
async function renameChat(){
  const name=v("chat-name"); if(!name) return;
  if(!compReady) await initComp();
  const fs=await frames([8,...Array.from(new TextEncoder().encode(name))]);   // CMD_SET_ADVERT_NAME
  if(fs.some(f=>f[0]===0)){
    $("chat-name-status").textContent="renamed — adverts will announce '"+name+"'";
    $("self-name").textContent="("+name+")";
    $("chat-name").value="";
    renderRail();
  } else $("chat-name-status").textContent="rename failed ("+fs.map(f=>f[0]).join(",")+")";
}
async function sendMsg(){
  if(!selContact){ $("msg-status").textContent="select a contact first"; return; }
  const text=v("msg-text"); if(!text) return;
  const pk=selContact.prefix.match(/../g).map(h=>parseInt(h,16));
  const body=[2,0,0,...le32(now()),...pk,...Array.from(new TextEncoder().encode(text))];
  const fs=await frames(body);
  for(const f of fs){
    if(f[0]===6){ $("msg-status").textContent="sent ("+(f[1]?"flood":"direct")+")";
      msgLog.push({out:true,prefix:selContact.prefix,name:selContact.name,ts:now(),text}); saveLogs(); renderMsgs();
      $("msg-text").value=""; return; }
    if(f[0]===1){ $("msg-status").textContent="send failed (err "+(f.length>1?f[1]:"?")+")"; return; }
  }
  $("msg-status").textContent="no response";
}
// Pull: drains the device's offline queue. Message content arrives in this UI
// via the archive mirror (the mux copies every synced frame), so pulled frames
// are NOT added to the log here — that would double them up.
async function syncMsgs(){
  if(!compReady) await initComp();
  let got=0;
  for(let i=0;i<32;i++){
    const fs=await frames([10]);                            // SYNC_NEXT_MESSAGE
    if(!fs.length) break;
    const f=fs[0];
    if(f[0]===10) break;                                    // NO_MORE_MESSAGES
    if(f[0]===16||f[0]===17||f[0]===27||f[0]===7||f[0]===8) got++;
  }
  $("msg-status").textContent="pulled "+got+" message(s) from device queue";
  if(got) pollArchive();
}

// Archive mirror: non-consuming message view
let archSeq=+(localStorage.getItem("mp_arch_seq")||0);
function addMsgFrame(f){
  if(f[0]===16&&f.length>=16){                              // contact msg v3
    const prefix=hex(f,4,6);
    msgLog.push({prefix,name:contactName(prefix)||prefix.slice(0,8),ts:rdU32(f,12),
      snr:(f[1]<<24>>24)/4,text:txt(f,16,f.length-16)});
    return true;
  }
  if(f[0]===17&&f.length>=11){                              // channel msg v3
    chanLog.push({chan:f[4],ts:rdU32(f,7),snr:(f[1]<<24>>24)/4,text:txt(f,11,f.length-11)});
    return true;
  }
  return false;
}
async function pollArchive(){
  try{
    const r=await api("/api/multi/comp/archive?after="+archSeq);
    const buf=new Uint8Array(await r.arrayBuffer());
    if(buf.length<4) return;
    const latest=rdU32(buf,0);
    if(archSeq>latest){ archSeq=0; localStorage.setItem("mp_arch_seq","0"); return; } // device rebooted
    let o=4, got=0;
    while(o+6<=buf.length){
      const s=rdU32(buf,o), l=buf[o+4]|(buf[o+5]<<8);
      const f=buf.slice(o+6,o+6+l); o+=6+l;
      if(s>archSeq){ archSeq=s; if(addMsgFrame(f)) got++; }
    }
    if(got){ localStorage.setItem("mp_arch_seq",String(archSeq)); saveLogs(); renderMsgs(); renderChan(); }
    else localStorage.setItem("mp_arch_seq",String(archSeq));
  }catch(e){}
}

// ---- channels ----
let freeChanIdx=-1;
async function loadChannels(){
  channels=[]; freeChanIdx=-1;
  for(let i=0;i<20;i++){
    const fs=await frames([31,i],1200,150);                 // GET_CHANNEL
    const f=fs.find(f=>f[0]===18);
    if(!f){ if(freeChanIdx<0) freeChanIdx=i; continue; }    // slot exists but errored -> treat as free
    const name=txt(f,2,32);
    if(name) channels.push({idx:f[1],name});
    else if(freeChanIdx<0) freeChanIdx=i;
  }
  chansLoaded=true;
  const el=$("channels");
  el.innerHTML=channels.map((c,i)=>"<div data-i="+i+">"+c.idx+": "+esc(c.name)+"</div>").join("")
    ||"<div class=mut>no channels configured</div>";
  el.querySelectorAll("div").forEach(d=>d.onclick=()=>{ selChan=channels[+d.dataset.i];
    $("chan-name").textContent="#"+selChan.idx+" "+selChan.name;
    el.querySelectorAll("div").forEach(x=>x.classList.toggle("sel",x===d)); renderChan(); });
  if(channels.length&&!selChan){ selChan=channels[0]; $("chan-name").textContent="#"+selChan.idx+" "+selChan.name; renderChan(); }
}
function renderChan(){
  if(!selChan) return;
  const el=$("chan-log");
  el.innerHTML=chanLog.filter(m=>m.chan===selChan.idx).map(m=>
    "<div class='msg"+(m.out?" out":"")+"'><span class=who>"+esc(m.out?"you":"")+"</span><span class=meta>"+
    new Date(m.ts*1000).toLocaleString()+(m.snr!==undefined?" SNR "+m.snr:"")+"</span><br>"+esc(m.text)+"</div>").join("")
    ||"<div class=mut>no messages on this channel</div>";
  el.scrollTop=el.scrollHeight;
}
// Join a public hashtag channel: key = first 16 bytes of sha256("#name")
// (per docs/companion_protocol.md — e.g. #test -> 9cd8fcf22a47333b591d96a2b848b73f)
async function joinHashtag(){
  const st=$("chan-join-status");
  let name=v("chan-join").toLowerCase().replace(/\s+/g,"");
  if(!name){ st.textContent="enter a channel name, e.g. #general"; return; }
  if(!name.startsWith("#")) name="#"+name;
  if(name.length>31){ st.textContent="name too long"; return; }
  if(!chansLoaded) await loadChannels();
  if(channels.some(c=>c.name.toLowerCase()===name)){ st.textContent=name+" already joined"; return; }
  if(freeChanIdx<0){ st.textContent="no free channel slots (max 20)"; return; }
  const digest=new Uint8Array(await crypto.subtle.digest("SHA-256",new TextEncoder().encode(name)));
  const secret=Array.from(digest.slice(0,16));
  const nameBytes=Array.from(new TextEncoder().encode(name));
  while(nameBytes.length<32) nameBytes.push(0);
  const fs=await frames([32,freeChanIdx,...nameBytes,...secret]);   // CMD_SET_CHANNEL
  if(fs.some(f=>f[0]===0)){
    st.textContent="joined "+name+" (slot "+freeChanIdx+")";
    $("chan-join").value="";
    await loadChannels();
  } else st.textContent="join failed ("+fs.map(f=>f[0]).join(",")+")";
}
async function sendChan(){
  if(!selChan){ $("chan-status").textContent="select a channel"; return; }
  const text=v("chan-text"); if(!text) return;
  const body=[3,0,selChan.idx,...le32(now()),...Array.from(new TextEncoder().encode(text))];
  const fs=await frames(body);
  if(fs.some(f=>f[0]===0)){ $("chan-status").textContent="sent";
    chanLog.push({out:true,chan:selChan.idx,ts:now(),text}); saveLogs(); renderChan(); $("chan-text").value="";
  } else $("chan-status").textContent="send failed";
}

// ---- dashboard ----
async function dashCmd(label,c){
  const r=stripReply(await cmd(c));
  $("dash-status").textContent=label+": "+(r||"OK");
}
// Battery percentage with a small inline bar. Colour tracks how much runtime
// is actually left, not just the number: below ~10% this node has minutes, not
// hours, because the discharge curve falls off a cliff under 3.5 V.
function battBar(pct){
  const p=Math.max(0,Math.min(100,pct|0));
  const col=p<=10?"#e05d5d":p<=25?"#e0b34d":"#5fd694";
  return p+" %<div style='margin-top:5px;height:5px;border-radius:3px;background:#243040;overflow:hidden'>"+
    "<div style='height:100%;width:"+p+"%;background:"+col+"'></div></div>";
}
function tile(lbl,val,sub){
  return "<div class=tile><div class=lbl>"+lbl+"</div><div class=val>"+val+"</div>"+
    (sub?"<div class=sub>"+sub+"</div>":"")+"</div>";
}
function renderDashTiles(d){
  const core=d.stats.repeater.core||{};
  const rp=d.stats.repeater.packets||{}, ro=d.stats.room.packets||{};
  const wifiRssi=(d.wifi.match(/rssi=(-?\d+)/)||[])[1];
  const up=d.uptime_s;
  const upStr=up>=86400?Math.floor(up/86400)+"d "+Math.floor(up%86400/3600)+"h":
    up>=3600?Math.floor(up/3600)+"h "+Math.floor(up%3600/60)+"m":Math.floor(up/60)+"m "+(up%60)+"s";
  $("dash-tiles").innerHTML=
    // percentage leads, volts as the detail — the curve is measured from this
    // board's own discharge, and 0% is a real 3.20V (it cannot boot below that)
    tile("Battery",
      (d.batt_pct!==undefined&&d.batt_mv)?battBar(d.batt_pct):
        (core.battery_mv?(core.battery_mv/1000).toFixed(2)+" V":"-"),
      d.batt_mv?((d.batt_mv/1000).toFixed(3)+" V"+
        (d.batt_mv>=4150?" · charged":d.batt_mv<=3350?" · <span class=err>critical</span>":
         d.batt_mv<=3600?" · low":"")):"")+
    tile("Uptime",upStr,"")+
    tile("Radio RX / TX",(d.radio?d.radio.rx:"-")+" / "+(d.radio?d.radio.tx:"-"),
      "packets · last rx "+(d.radio?age(d.radio.rx_age_s):"?")+" ago"+
      (d.radio&&d.radio.refused?" · <span class=err>"+d.radio.refused+" refused</span>":"")+
      (d.radio&&d.radio.recoveries?" · <span class=err>"+d.radio.recoveries+" radio resets</span>":"")+
      (d.radio&&d.radio.stuck?" · <span class=err>"+d.radio.stuck+" stuck TX</span>":"")+
      (d.radio&&d.radio.rxdrop?" · <span class=err>"+d.radio.rxdrop+" rx queue drops</span>":""))+
    tile("Heard by peers",
      (d.radio&&d.radio.heard&&d.radio.heard.sent
        ? Math.round(100*d.radio.heard.confirmed/d.radio.heard.sent)+" %" : "-"),
      d.radio&&d.radio.heard
        ? d.radio.heard.confirmed+"/"+d.radio.heard.sent+" floods relayed on · "+
          (d.radio.heard.w2+d.radio.heard.w3)+" certain, "+d.radio.heard.w1+" weak (1-byte)"
        : "relay confirmations")+
    tile("NVS",(d.nvs&&d.nvs.total?Math.round(100*d.nvs.used/d.nvs.total)+" % used":"-"),
      d.nvs?(d.nvs.free+" entries free"):"identity mirror store")+
    tile("Noise floor",(d.radio&&d.radio.noise?d.radio.noise+" dBm":"-"),
      d.radio?("listen-before-talk "+(d.radio.cad?"on":"off")+
        (d.radio.thresh?" · RSSI guard +"+d.radio.thresh+" dB":" · no RSSI guard")):"")+
    (function(){
      const e=d.radio&&d.radio.err; if(!e) return "";
      const bad=e.crc+e.header+e.timeout+e.other, all=bad+e.ok;
      return tile("RX failures",(all?Math.round(100*bad/all)+" %":"-"),
        "crc "+e.crc+" · header "+e.header+" · timeout "+e.timeout+" · other "+e.other);
    })()+
    tile("WiFi",wifiRssi?wifiRssi+" dBm":"offline","")+
    tile("CPU load",(d.load!==undefined?d.load+" %":"-"),(d.lps?d.lps+" loops/s":"main task duty"))+
    tile("Clock",(((d.clock||"").match(/utc=(\S+)/)||[])[1]||"-").replace("Z","")+" UTC",
      "source: "+(((d.clock||"").match(/source=(\S+)/)||[])[1]||"?")+
      ((d.clock||"").includes("gps=on")?" · GPS acquiring":"")+
      ((((d.clock||"").match(/drift=\S+\(([-\d.]+)s\/day\)/)||[])[1]!==undefined&&
        +(((d.clock||"").match(/drift=\S+\(([-\d.]+)s\/day\)/)||[])[1])!==0)
        ? " · drift "+((d.clock||"").match(/drift=\S+\(([-\d.]+)s\/day\)/)||[])[1]+" s/day" : ""))+
    tile("Free heap",Math.round(d.heap/1024)+" k","psram "+Math.round(d.psram/1024)+" k")+
    tile("Contacts",contacts.length||"-","known nodes")+
    tile("Forwarded",(rp.flood_tx||0)+(rp.direct_tx||0),"rx errors "+((rp.recv_errors||0)+(ro.recv_errors||0)))+
    tile("Phone app",d.companion.client?"connected":(d.companion.tcp?"waiting":"down"),"tcp/5000");
}
// nodes nearby (System page): contacts ranked by how "close" they are —
// direct first, then by advert hop depth, then by advert recency
// The real "nodes nearby": who is within radio reach, from the arbiter's peer
// table, enriched with names/locations from cached adverts. Relays alone only
// prove we can hear them; "hears us" is the other, independent direction.
// The receiver is powered down between scheduled syncs, so "no fix" is the
// normal idle state rather than a fault — the wording has to distinguish
// "not looking" from "looking and failing".
function renderGps(){
  const g=lastDebug&&lastDebug.gps;
  if(!g){ return; }
  const hunting=g.searching_s?(" for "+age(g.searching_s)):"";
  const lock=g.lock?"<span class=ok>fix</span>":
             (g.powered?"<span style='color:#e0b34d'>searching</span>"+hunting
                       :"<span class=mut>idle (powered down)</span>");
  const sats=g.sats>0?g.sats:0;
  const bars=(n)=>{ let h=""; for(let i=1;i<=8;i++)
      h+="<span style='display:inline-block;width:4px;margin-right:2px;height:"+(4+i*1.4)+
         "px;background:"+(i<=n?(n>=5?"#5fd694":"#e0b34d"):"#243040")+";vertical-align:bottom'></span>";
    return h; };
  // The MEDIAN over the retained history. Not the last interval (which carries
  // the full measurement error of both its endpoints) and not the mean, which
  // a few samples taken across a stalled loop drag a long way: measured here,
  // mean 28.8 ppm vs median 23.9 on the same history.
  const drift=g.drift_samples>0?(g.drift_ppm_est.toFixed(2)+" ppm ("+
      (g.drift_ppm_est*86400/1e6).toFixed(2)+" s/day) <span class=mut>median of "+g.drift_samples+
      " usable sample"+(g.drift_samples===1?"":"s")+", last "+g.drift_ppm.toFixed(2)+" ppm</span>")
      :"not measured yet";
  // Whether the number above means anything at all. With no I2C RTC the clock
  // being read IS the one SNTP sets, so it can only ever measure itself as
  // perfect — worth saying out loud rather than showing a confident 0.00 ppm.
  const rtcWhat=g.rtc_hw?("<span class=ok>"+esc(g.rtc)+"</span>"+
      (g.subsec?" <span class=mut>· sub-second phase measurement</span>"
               :" <span class=mut>· whole-second fallback</span>"))
    :"<span style='color:#e0b34d'>none found</span> <span class=mut>&mdash; using the ESP32 clock "+
     "that NTP itself sets, so drift here is unmeasurable</span>";
  const trim=!g.rtc_hw?"<span class=mut>n/a</span>"
    :(g.trim?("<span class=ok>on</span> <span class=mut>&middot; "+g.trim_steps+
       " step"+(g.trim_steps===1?"":"s")+" applied, "+(g.trim_pending_ms/1000).toFixed(1)+
       " s carried into this interval</span>")
      :"<span class=mut>off</span>");
  const nextS=g.every_h?(g.next_s>0?age(g.next_s):"due now"):"never (auto-sync off)";
  const lastSync=g.last_sync?(new Date(g.last_sync*1000).toLocaleString()):"never";
  $("gps-hdr").innerHTML=g.enabled?("auto-sync every "+g.every_h+" h"):"auto-sync disabled";
  $("gps-body").innerHTML=
    "<div class=row><span class=mut style='width:150px'>Status</span><span>"+lock+
      " &middot; "+esc(g.state)+"</span></div>"+
    "<div class=row><span class=mut style='width:150px'>Satellites</span><span>"+bars(sats)+
      " &nbsp;"+sats+"</span></div>"+
    (g.lat!==undefined?"<div class=row><span class=mut style='width:150px'>Fix position</span><span>"+
      g.lat.toFixed(5)+", "+g.lon.toFixed(5)+" &middot; "+g.alt+" m</span></div>":"")+
    "<div class=row><span class=mut style='width:150px'>Clock source</span><span>"+
      (g.clock_source==="gps"?"<span class=ok>GPS</span>":"<span style='color:#e0b34d'>manual / unset</span>")+
      "</span></div>"+
    "<div class=row><span class=mut style='width:150px'>Last GPS sync</span><span>"+esc(lastSync)+
      " &middot; "+g.syncs+" total</span></div>"+
    "<div class=row><span class=mut style='width:150px'>Next attempt</span><span>"+nextS+"</span></div>"+
    "<div class=row><span class=mut style='width:150px'>RTC hardware</span><span>"+rtcWhat+"</span></div>"+
    "<div class=row><span class=mut style='width:150px'>RTC drift</span><span>"+drift+"</span></div>"+
    "<div class=row><span class=mut style='width:150px'>Drift correction</span><span>"+trim+"</span></div>"+
    (g.skips_low_batt?"<div class=row><span class=mut style='width:150px'>Skipped</span><span class=mut>"+
      g.skips_low_batt+" x battery below threshold</span></div>":"");
}
// Resolve a peer to an identity. The firmware only learns a name when it
// happens to catch that node's advert; the browser's contact list is built from
// every advert the companion has ever synced, so it fills the long tail. Match
// on the firmware-supplied pubkey first (exact), then fall back to the path
// hash prefix (which can be as short as one byte, hence the length guard).
function peerName(p){
  const hex=typeof p==="string"?p:p.h;
  const pub=(typeof p==="object"&&p.pub)?p.pub:null;
  if(pub){
    const c=contacts.find(c=>c.pk.startsWith(pub)||pub.startsWith(c.prefix));
    if(c) return c;
  }
  if(hex.length>=4){
    const c=contacts.find(c=>c.prefix.startsWith(hex));
    if(c) return c;
  }
  return null;
}
// name/location for a peer, firmware first then the browser's advert cache
function peerIdent(p){
  const c=peerName(p);
  return {
    name: p.name || (c&&c.name) || "",
    lat:  (p.lat!==undefined?p.lat:(c?c.lat:0))||0,
    lon:  (p.lon!==undefined?p.lon:(c?c.lon:0))||0,
    kind: c?(KINDS[c.type]||"?"):""
  };
}
function renderNearby(){
  renderGps();
  const peers=(lastDebug&&lastDebug.peers)||[];
  if(!peers.length){ return; }
  // Ordered by how many of their transmissions we have actually received, which
  // is the honest measure of "in reach" — relays only prove they are busy, and
  // heard-us is the opposite direction of the link.
  const rows=[...peers].sort((a,b)=>(b.direct-a.direct)||(b.heard_us-a.heard_us)||(b.relays-a.relays));
  $("dash-nearby").innerHTML=rows.slice(0,20).map(p=>{
    const id=peerIdent(p);
    const nm=id.name?esc(id.name):"<span class=mut>"+p.h+"</span>";
    const dist=((id.lat||id.lon)&&selfLoc&&(selfLoc[0]||selfLoc[1]))?distKm(selfLoc,[id.lat,id.lon])+" km":"-";
    // a 1-byte-only entry can never be confirmed, so say so rather than showing 0
    const hu=p.heard_us>0?"<span class=ok>"+p.heard_us+"</span>":
             (p.hu1>0?"<span class=mut title='1-byte matches only \u2014 not proof'>"+p.hu1+"?</span>":"-");
    const hop=p.hops?(p.hops===1?"<span class=ok>1</span>":p.hops):"-";
    return "<tr><td>"+nm+" <span class=mut style='font-size:10px'>"+p.h+"</span></td><td>"+hop+
      "</td><td>"+(p.direct||"-")+"</td><td>"+hu+"</td><td>"+
      (p.snr!==null&&p.snr!==undefined?snrSpan(Math.round(p.snr*4)):"-")+"</td><td class=mut>"+p.relays+
      "</td><td>"+dist+"</td><td>"+skewCell(p)+"</td><td class=mut>"+age(p.direct_age_s!==null&&p.direct_age_s!==undefined?p.direct_age_s:p.age_s)+"</td></tr>";
  }).join("");
  const conf=(lastDebug&&lastDebug.peers_confirmed)||0;
  $("nearby-count").innerHTML="("+rows.length+" seen \u00b7 <span class=ok>"+conf+
    " confirmed to hear us</span>)";
}
async function loadDash(){
  renderNearby();
  try{
    $("dash-ver").textContent=stripReply(await cmd("ver"));
    $("dash-pubkey").textContent=stripReply(await cmd("get public.key"));
    $("dash-radio").textContent=stripReply(await cmd("get radio"))+"  (freq,bw,sf,cr)";
    const name=stripReply(await cmd("get name"));
    if(name) $("panel-title").textContent=name;
  }catch(e){}
  for(const k of ["battery","packets"]){
    try{
      const d=await (await api("/api/multi/stats?series="+k)).json();
      drawSpark("dc-"+k,"dv-"+k,d.points);
    }catch(e){}
  }
}

// ---- repeater tab ----
async function loadRepTab(){
  for(const [c,id] of [["get name","rep-name"],["clock","rep-clock"],["get lat","rep-lat"],
    ["get lon","rep-lon"],["get owner.info","rep-owner"],["get advert.interval","rep-advint"],
    ["get flood.advert.interval","rep-floodint"],["get flood.max","rep-floodmax"],
    ["get flood.max.unscoped","rep-floodmaxu"]]){
    try{ await fieldLoad(c,id); }catch(e){}
  }
  try{ $("rep-ghost").checked=stripReply(await cmd("get repeat")).toLowerCase().includes("off"); }catch(e){}
}
async function syncClock(){
  const ep=Math.floor(Date.now()/1000);
  await cmd("time "+ep); await cmd("time.force "+ep);
  await fieldLoad("clock","rep-clock");
  $("rep-status").textContent="clock synced from browser";
}
async function ghostToggle(on){
  const st=$("rep-ghost-status");
  if(on){
    const prev={repeat:stripReply(await cmd("get repeat")),
      adv:stripReply(await cmd("get advert.interval")),
      flood:stripReply(await cmd("get flood.advert.interval"))};
    localStorage.setItem("mp_ghost_prev",JSON.stringify(prev));
    await cmd("set repeat off"); await cmd("set advert.interval 0"); await cmd("set flood.advert.interval 0");
    st.textContent="ghost mode ON (not forwarding, not advertising)";
  }else{
    let prev={};
    try{ prev=JSON.parse(localStorage.getItem("mp_ghost_prev")||"{}"); }catch(e){}
    await cmd("set repeat "+((prev.repeat||"on").includes("off")?"on":(prev.repeat||"on").replace(/[^a-z]/g,"")||"on"));
    await cmd("set advert.interval "+(parseInt(prev.adv)||60));
    await cmd("set flood.advert.interval "+(parseInt(prev.flood)||12));
    st.textContent="ghost mode off — previous advert settings restored";
  }
}

// ---- radio tab ----
// Built-in presets (what MeshCore ships with) are always available; the
// community list from api.meshcore.nz is appended when reachable.
const BUILTIN_PRESETS=[
  {title:"Australia / NZ (MeshCore default)",frequency:"915.800",bandwidth:"250",sf:"10",cr:"5"},
  {title:"Australia (Narrow)",frequency:"916.575",bandwidth:"62.5",sf:"7",cr:"8"},
  {title:"Europe / UK (MeshCore default)",frequency:"869.525",bandwidth:"250",sf:"11",cr:"5"},
  {title:"USA / Canada (Recommended)",frequency:"910.525",bandwidth:"62.5",sf:"7",cr:"5"},
];
let presets=[];
function normPreset(p){       // api.meshcore.nz uses snake_case strings
  return {title:String(p.title||"preset"),
    frequency:p.frequency, bandwidth:p.bandwidth,
    sf:p.sf||p.spreading_factor||p.spreadingFactor,
    cr:p.cr||p.coding_rate||p.codingRate};
}
async function radioRefresh(){ $("radio-cur").textContent=stripReply(await cmd("get radio")); }
async function loadPresets(){
  const sel=$("radio-preset");
  presets=BUILTIN_PRESETS.map(normPreset);
  const render=(extraLabel)=>{
    sel.innerHTML="<option value=''>-- presets"+(extraLabel||"")+" --</option>"+
      presets.map((p,i)=>"<option value="+i+">"+esc(p.title)+" ("+p.frequency+"/"+p.bandwidth+"/SF"+p.sf+"/CR"+p.cr+")</option>").join("");
  };
  render(" (built-in)");
  try{
    const r=await fetch("https://api.meshcore.nz/api/v1/config",{cache:"no-store"});
    const cfg=await r.json();
    const community=((cfg.suggested_radio_settings&&cfg.suggested_radio_settings.entries)||[]).map(normPreset)
      .filter(p=>p.frequency&&p.bandwidth&&p.sf&&p.cr);
    // de-dupe against built-ins by params
    const key=p=>p.frequency+"/"+p.bandwidth+"/"+p.sf+"/"+p.cr;
    const seen=new Set(presets.map(key));
    for(const p of community){ if(!seen.has(key(p))){ seen.add(key(p)); presets.push(p); } }
    render("");
  }catch(e){ /* offline: built-ins remain */ }
}
async function applyPreset(){
  const p=presets[+$("radio-preset").value];
  if(!p){ $("radio-status").textContent="pick a preset first"; return; }
  const r=await cmd("set radio "+p.frequency+","+p.bandwidth+","+p.sf+","+p.cr);
  $("radio-status").textContent=stripReply(r);
  radioRefresh();
}
async function applyManualRadio(){
  const m=v("radio-manual"); if(!m) return;
  $("radio-status").textContent=stripReply(await cmd("set radio "+m));
  radioRefresh();
}
// AU state regions (same fixed list the stock panel offers); the current
// selection is detected from `region list allowed`.
const AU_REGIONS=[["au-act","ACT"],["au-nsw","New South Wales"],["au-qld","Queensland"],
  ["au-sa","South Australia"],["au-tas","Tasmania"],["au-vic","Victoria"],
  ["au-wa","Western Australia"],["au-nt","Northern Territory"]];
async function loadRadioTab(){
  radioRefresh();
  if(!presets.length) loadPresets();
  try{ await fieldLoad("get path.hash.mode","radio-phm"); }catch(e){}
  const sel=$("radio-region");
  sel.innerHTML="<option value=''>Select state</option>"+
    AU_REGIONS.map(([val,label])=>"<option value='"+val+"'>"+esc(label)+" ("+val+")</option>").join("");
  try{
    const allowed=stripReply(await cmd("region list allowed")).toLowerCase();
    for(const tok of allowed.split(/[\s,]+/)){
      if(tok.startsWith("au-")&&AU_REGIONS.some(([val])=>val===tok)){ sel.value=tok; break; }
    }
  }catch(e){}
}
async function saveRegion(){
  const reg=v("radio-region"); if(!reg){ $("radio-status").textContent="pick a state region first"; return; }
  for(const c of ["region put au","region put "+reg,"region allowf au","region allowf "+reg,"region save"]){
    const r=stripReply(await cmd(c));
    if(/^err/i.test(r)){ $("radio-status").textContent="failed at '"+c+"': "+r; return; }
  }
  $("radio-status").textContent="region saved: au + "+reg;
}

// ---- stats tab ----
const SERIES=[["battery","battery (mV)"],["load","CPU load (%)"],["heap","free heap (kB)"],
  ["wifi_rssi","wifi RSSI (dBm)"],["noise","radio noise floor (dBm)"],["packets","radio packets / min"]];
let lastDebug=null;
async function loadStatsTab(){
  if(lastDebug){
    $("stats-live").innerHTML=identityRows(lastDebug);
    const c=lastDebug.stats.repeater.core;
    $("stats-node").textContent="battery "+(c?c.battery_mv+" mV":"-")+"   uptime "+lastDebug.uptime_s+
      "s   heap "+Math.round(lastDebug.heap/1024)+"k   psram "+Math.round(lastDebug.psram/1024)+"k";
  }
  const wrap=$("stats-charts"); wrap.innerHTML="";
  for(const [key,label] of SERIES){
    try{
      const d=await (await api("/api/multi/stats?series="+key)).json();
      const div=document.createElement("div");
      div.innerHTML="<div class=mut style='font-size:12px;margin:8px 0 2px'>"+label+
        " <span class=mut id='sv-"+key+"'></span></div><canvas id='sc-"+key+"' width=860 height=70 style='width:100%;max-width:860px'></canvas>";
      wrap.appendChild(div);
      drawSpark("sc-"+key,"sv-"+key,d.points);
    }catch(e){}
  }
}
function drawSpark(cid,vid,pts){
  const cv=$(cid),ctx=cv.getContext("2d");
  ctx.clearRect(0,0,cv.width,cv.height);
  if(!pts||pts.length<2){ $(vid).textContent="(collecting — samples arrive once a minute)"; return; }
  const vs=pts.map(p=>p[1]),min=Math.min(...vs),max=Math.max(...vs),span=(max-min)||1;
  ctx.strokeStyle="#4da3ff"; ctx.lineWidth=1.5; ctx.beginPath();
  pts.forEach((p,i)=>{
    const x=i/(pts.length-1)*(cv.width-4)+2;
    const y=cv.height-6-((p[1]-min)/span)*(cv.height-12);
    i?ctx.lineTo(x,y):ctx.moveTo(x,y);
  });
  ctx.stroke();
  $(vid).textContent="now "+vs[vs.length-1]+"  min "+min+"  max "+max;
}


// ---- settings: MQTT uplink -------------------------------------------------
// Everything here drives the firmware's own `mqtt` / `set mqtt.*` commands, so
// the panel cannot drift from the CLI or hold its own idea of the config.
const MQTT_BROKERS=["eastmesh-au","meshmapper","waev","letsmesh-eu","letsmesh-us","custom"];
async function loadMqtt(){
  let txt;
  try{ txt=stripReply(await cmd("mqtt")); }catch(e){ return; }
  if(!txt||txt.indexOf("not started")>=0){ $("mqtt-state").textContent="(starting…)"; return; }
  const on={};
  const bm=txt.match(/brokers:(.*)/);
  if(bm) for(const tok of bm[1].trim().split(/\s+/)){
    const [k,v]=tok.split("="); on[k]=(v==="on");
  }
  $("mqtt-brokers").innerHTML=MQTT_BROKERS.map(b=>
    "<label style='margin-right:12px;font-size:13px;cursor:pointer'><input type=checkbox "+
    (on[b]?"checked":"")+" onchange=\"mqttBroker('"+b+"',this.checked)\"> "+b+"</label>").join("");
  const cm=txt.match(/custom: host=(\S+) port=(\d+) transport=(\S+) user=(\S+) pass=(\S+)/);
  if(cm){
    if($("mq-host")!==document.activeElement) $("mq-host").value=cm[1]==="-"?"":cm[1];
    if($("mq-port")!==document.activeElement) $("mq-port").value=cm[2]==="0"?"":cm[2];
    $("mq-transport").value=(cm[3]||"").indexOf("wss")>=0?"wss":"tcp";
    if($("mq-user")!==document.activeElement) $("mq-user").value=cm[4]==="-"?"":cm[4];
    $("mq-pass").placeholder=cm[5]==="set"?"password (set — type to replace)":"password (write-only)";
  }
  // first line of `mqtt` is the firmware's own status summary
  $("mqtt-state").textContent=txt.split("\n")[0].slice(0,90);
}
async function mqttBroker(name,on){
  $("mqtt-status").textContent=stripReply(await cmd("set mqtt.broker "+name+" "+(on?"on":"off")));
  setTimeout(loadMqtt,800);
}
async function mqttSaveCustom(){
  const q=[];
  if(v("mq-host")) q.push("set mqtt.host "+v("mq-host"));
  if(v("mq-port")) q.push("set mqtt.port "+v("mq-port"));
  q.push("set mqtt.transport "+$("mq-transport").value);
  if(v("mq-user")) q.push("set mqtt.user "+v("mq-user"));
  if(v("mq-pass")) q.push("set mqtt.pass "+v("mq-pass"));
  let last="";
  for(const c of q) last=stripReply(await cmd(c));
  $("mq-pass").value="";                       // never leave a secret on screen
  $("mqtt-status").textContent=last||"saved";
  setTimeout(loadMqtt,800);
}
async function mqttSaveMeta(){
  let last="";
  if(v("mq-iata"))  last=stripReply(await cmd("set mqtt.iata "+v("mq-iata")));
  if(v("mq-email")) last=stripReply(await cmd("set mqtt.email "+v("mq-email")));
  $("mqtt-status").textContent=last||"saved";
  setTimeout(loadMqtt,800);
}
// ---- settings: identity slots ----
// Parses the firmware's `slots` listing, which is the single source of truth
// for what each slot is and whether it is up:
//   slot 1 (chat1) type=room running
//   slot 2 (chat2) type=chat stopped tcp/5001
//   slot 3 (chat3) type=off
let slotState=[];
async function loadSlots(){
  const txt=await cmd("slots");
  const rows=stripReply(txt).split("\n").filter(Boolean);
  let h=""; slotState=[];
  for(const line of rows){
    const m=line.match(/^slot (\d+) \((\w+)\) type=(off|chat|room)(?:\s+(running|stopped))?(?:\s+tcp\/(\d+))?/);
    if(!m){ h+="<div class=mut style='margin-bottom:4px'>"+esc(line)+"</div>"; continue; }
    const [,n,name,type,run,port]=m;
    slotState.push({n:+n,name,type,running:run==="running"});
    const sel=["off","chat","room"].map(t=>
      "<option value="+t+(t===type?" selected":"")+">"+t+"</option>").join("");
    // "running" is the honest signal, not the configured type: after retyping,
    // the slot is stopped and stays that way until a reboot, and saying so is
    // the difference between "it didn't work" and "it needs a restart".
    const status = type==="off" ? "<span class=mut style='font-size:12px'>off</span>"
      : run==="running" ? "<span class=ok style='font-size:12px'>running</span>"
      : "<span style='color:#e0b34d;font-size:12px'>stopped &mdash; reboot to start</span>";
    h+="<div class=row style='margin-bottom:3px'>"+
      "<span style='min-width:150px'>slot "+n+" <span class=mut>("+esc(name)+")</span></span>"+
      "<select onchange=\"setSlotType("+n+",this.value)\" style='margin-right:8px'>"+sel+"</select>"+
      status+(port?" <span class=mut style='font-size:12px'>&middot; app port "+port+"</span>":"")+"</div>";
  }
  $("slots-list").innerHTML=h;
  renderSlotAdmin();
  loadMqtt();
}
async function setSlotType(n,t){
  const r=stripReply(await cmd("set slot."+n+" type "+t));
  $("slots-status").textContent=r;
  setTimeout(loadSlots,1500);
}

// Per-slot management. Room slots expose the same controls as the fixed room
// because `slot N <cmd>` routes straight into that identity's own CLI — there
// is no separate command surface to keep in step.
function renderSlotAdmin(){
  const rooms=slotState.filter(s=>s.type==="room"&&s.running);
  if(!rooms.length){ $("slot-admin").innerHTML=""; return; }
  let h="<div class=mut style='font-size:12px;margin:8px 0 4px'>Room slots &mdash; name and join password are "+
        "read from and written to each room's own identity.</div>";
  for(const s of rooms){
    h+="<div class=row style='margin-bottom:4px;gap:6px'>"+
      "<span style='min-width:150px'>slot "+s.n+" <span class=mut>("+esc(s.name)+")</span></span>"+
      "<input id=sl-nm-"+s.n+" placeholder='name' style='width:150px' data-1p-ignore>"+
      "<button onclick=\"slotSet("+s.n+",'set name','sl-nm-"+s.n+"')\">set name</button>"+
      "<input id=sl-pw-"+s.n+" placeholder='join password' style='width:150px' data-1p-ignore>"+
      "<button onclick=\"slotSet("+s.n+",'password','sl-pw-"+s.n+"')\">set password</button>"+
      "<span id=sl-st-"+s.n+" class=mut style='font-size:12px'></span></div>";
  }
  $("slot-admin").innerHTML=h;
  for(const s of rooms) slotLoadName(s.n);
}
async function slotLoadName(n){
  try{ const v=stripReply(await cmd("slot "+n+" get name")); if(v) $("sl-nm-"+n).value=v.trim(); }catch(e){}
}
async function slotSet(n,verb,inputId){
  const v=$(inputId).value.trim();
  if(!v){ $("sl-st-"+n).textContent="enter a value first"; return; }
  const r=stripReply(await cmd("slot "+n+" "+verb+" "+v));
  $("sl-st-"+n).textContent=r||"OK";
  if(verb==="password") $(inputId).value="";     // don't leave it on screen
  setTimeout(()=>{ $("sl-st-"+n).textContent=""; },4000);
}

// ---- settings: firmware OTA upload ----
// compact MD5 (public-domain style implementation) for the X-Firmware-MD5 header
function md5hex(buf){
  const r=[7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
    4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21];
  const k=Array.from({length:64},(_, i)=>Math.floor(Math.abs(Math.sin(i+1))*4294967296));
  const add=(a,b)=>(a+b)>>>0, rol=(n,c)=>(n<<c)|(n>>>(32-c));
  const bytes=new Uint8Array(buf), n=bytes.length;
  const withPad=((n+8)>>6)+1<<4;   // words
  const w=new Uint32Array(withPad);
  for(let i=0;i<n;i++) w[i>>2]|=bytes[i]<<((i%4)*8);
  w[n>>2]|=0x80<<((n%4)*8);
  w[withPad-2]=(n*8)>>>0; w[withPad-1]=Math.floor(n*8/4294967296);
  let a=1732584193,b=-271733879>>>0,c=-1732584194>>>0,d=271733878;
  for(let i=0;i<withPad;i+=16){
    const [oa,ob,oc,od]=[a,b,c,d];
    for(let j=0;j<64;j++){
      let f,g;
      if(j<16){ f=(b&c)|(~b&d); g=j; }
      else if(j<32){ f=(d&b)|(~d&c); g=(5*j+1)%16; }
      else if(j<48){ f=b^c^d; g=(3*j+5)%16; }
      else{ f=c^(b|~d); g=(7*j)%16; }
      const t=d; d=c; c=b;
      b=add(b,rol(add(add(a,f),add(k[j],w[i+g])),r[j])); a=t;
    }
    a=add(a,oa); b=add(b,ob); c=add(c,oc); d=add(d,od);
  }
  return [a,b,c,d].map(x=>Array.from({length:4},(_,i)=>((x>>>(i*8))&255).toString(16).padStart(2,"0")).join("")).join("");
}
async function fwUpload(){
  const f=$("fw-file").files[0];
  if(!f){ $("fw-status").textContent="choose a firmware.bin first"; return; }
  if(!confirm("Flash "+f.name+" ("+Math.round(f.size/1024)+" kB) to the spare OTA slot and reboot?")) return;
  $("fw-btn").disabled=true;
  try{
    $("fw-status").textContent="hashing...";
    const buf=await f.arrayBuffer();
    const md5=md5hex(buf);
    $("fw-status").textContent="uploading "+Math.round(buf.byteLength/1024)+" kB (md5 "+md5.slice(0,8)+"…)...";
    const r=await fetch("/api/firmware-update",{method:"POST",
      headers:{"X-Auth-Token":TOKEN,"X-Firmware-MD5":md5},body:buf});
    if(!r.ok){ $("fw-status").textContent="update failed: "+await r.text(); $("fw-btn").disabled=false; return; }
    let left=15;
    const t=setInterval(()=>{
      $("fw-status").textContent="flashed OK — rebooting, reloading in "+left+"s";
      if(--left<0){ clearInterval(t); location.reload(); }
    },1000);
  }catch(e){ $("fw-status").textContent="update failed: "+e; $("fw-btn").disabled=false; }
}

// ---- debug console ----
let cliHist=JSON.parse(localStorage.getItem("mp_cli_hist")||"[]"), cliPos=-1;
async function cliRun(){
  const c=v("cli-cmd"); if(!c) return;
  cliHist=cliHist.filter(x=>x!==c); cliHist.unshift(c); cliHist=cliHist.slice(0,10);
  localStorage.setItem("mp_cli_hist",JSON.stringify(cliHist));
  cliPos=-1;
  $("cli-cmd").value="";
  const full=$("cli-target").value+c;
  const r=await cmd(full);
  $("cli-out").textContent=("> "+full+"\n"+r+"\n\n"+$("cli-out").textContent).slice(0,6000);
}
function cliKey(e){
  if(e.key==="Enter"){ cliRun(); return; }
  if(e.key==="ArrowUp"){
    e.preventDefault();
    if(cliPos<cliHist.length-1){ cliPos++; $("cli-cmd").value=cliHist[cliPos]; }
  }else if(e.key==="ArrowDown"){
    e.preventDefault();
    if(cliPos>0){ cliPos--; $("cli-cmd").value=cliHist[cliPos]; }
    else{ cliPos=-1; $("cli-cmd").value=""; }
  }
}

// ---- boot ----
// Polling is load on the node (each request = TLS + up to 7 console commands
// for /debug), and the panel's HTTPS server only has a few sockets. All
// periodic work therefore runs through ONE sequential scheduler: jobs are
// awaited one at a time, so the poller never has two requests in flight, and
// nothing runs while the browser tab is hidden. The packet trace polls fast
// only while the Packets section is on screen.
const activeTab=()=>document.querySelector("nav button.on").dataset.t;
const jobs=[
  {name:"pkts",   fn:pollPkts,     period:()=>activeTab()==="pkts"?4000:20000, due:0},
  {name:"debug",  fn:pollDebug,    period:15000, due:1000},
  {name:"archive",fn:pollArchive,  period:15000, due:5000},
  {name:"nearby", fn:async()=>renderNearby(), period:15000, due:8000},   // local render, no request
  {name:"dash",   fn:async()=>{ if($("tab-dash").classList.contains("on")) await loadDash(); }, period:120000, due:120000},
  {name:"autosync",fn:async()=>{ if($("autosync").checked) await syncMsgs(); }, period:15000, due:12000},
];
let pumpBusy=false;
async function pumpJobs(){
  if(pumpBusy||document.hidden) return;
  pumpBusy=true;
  try{
    for(const j of jobs){
      if(Date.now()>=j.due){
        j.due=Date.now()+(typeof j.period==="function"?j.period():j.period);
        try{ await j.fn(); }catch(e){}
      }
    }
  } finally{ pumpBusy=false; }
}
async function boot(){
  await pollDebug(); await loadDash(); loadStatsTab(); loadRailNames();
  setInterval(pumpJobs,1000);
  document.addEventListener("visibilitychange",()=>{
    if(!document.hidden){ for(const j of jobs) j.due=0; pumpJobs(); }
  });
  document.addEventListener("keydown",e=>{ if(e.key==="Escape") $("pkt-modal").style.display="none"; });
  initComp().then(()=>{ pollArchive(); renderNearby(); renderRail(); }).catch(()=>{});
}
(async()=>{
  try{ const r=await fetch("/api/session",{headers:{"X-Auth-Token":TOKEN}}); if(r.status===401) return needLogin(); }
  catch(e){ return needLogin(); }
  boot();
})();
</script></body></html>
)MWPG";
