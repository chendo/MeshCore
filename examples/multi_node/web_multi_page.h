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
tr.relayrow td{background:#12211a}
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
  <div class="card"><h3>Nodes nearby <span class="mut" id="nearby-count" style="font-weight:400;font-size:11px"></span></h3>
    <div style="overflow-x:auto"><table><thead><tr><th>name</th><th>kind</th><th>key</th><th>heard</th><th>last advert</th><th>dist</th></tr></thead>
    <tbody id="dash-nearby"><tr><td colspan=6 class=mut>listening for adverts...</td></tr></tbody></table></div>
  </div>
  <div class="card"><h3>Live <button class="sec" style="float:right;padding:2px 8px" onclick="loadStatsTab()">&#8635;</button></h3>
  <div style="overflow-x:auto"><table><thead><tr><th></th><th>recv</th><th>sent</th><th>flood tx</th><th>direct tx</th><th>flood rx</th><th>direct rx</th><th>rx errors</th><th>queue</th><th>err flags</th></tr></thead>
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
  <div class="card"><h3>Identity slots</h3>
    <div class="mut" style="font-size:12px;margin-bottom:8px">Each enabled slot is a full extra chat identity — its own
    keypair, contacts and app connection on its own TCP port. Enabling and disabling take effect <b>immediately, no
    reboot</b>; a disabled slot goes off air and its identity and data stay on the filesystem, so re-enabling brings the
    same node back. (Its RAM is only reclaimed at the next restart.)</div>
    <div id="slots-list" class="mut" style="font-size:13px">loading...</div>
    <div id="slots-status" class="mut" style="font-size:12px;margin-top:6px"></div>
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
  <table><thead><tr><th></th><th>recv</th><th>sent</th><th>flood tx</th><th>direct tx</th><th>flood rx</th><th>direct rx</th><th>rx errors</th><th>queue</th><th>err flags</th></tr></thead>
  <tbody id="dbg-stats"></tbody></table></div>
  <div class="card"><h3>Nodes nearby (repeater neighbours)</h3><pre id="dbg-nbrs" class="mut">-</pre></div>
  <div class="card"><h3>Radio packets <span class="mut" id="pkt-count"></span></h3>
    <div class="mut" style="font-size:11px;margin-bottom:6px">
      <span class="relay sure">&#8618; relayed</span> a neighbour passed on something we sent (2-byte+ hash — certain) ·
      <span class="relay weak">&#8618; relayed?</span> same, but a 1-byte hash, so ~1 in 256 could be coincidence
    </div>
  <div style="overflow-x:auto"><table><thead><tr><th>time</th><th>dir</th><th>route</th><th>type</th><th>src</th><th>info</th><th>len</th><th>SNR</th><th>RSSI</th></tr></thead>
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
function renderRail(){
  if(!lastDebug) return;
  const rp=lastDebug.stats.repeater.packets||{};
  const kinds=[["repeater","REPEATER",(rp.flood_tx||0)+(rp.direct_tx||0)+" fwd · "+(rp.recv_errors||0)+" err"],
    ["room","ROOM","serving"],
    ["companion","CHAT",contacts.length+" contacts · app "+(lastDebug.companion.client?"connected":"—")]];
  let h="";
  for(const [role,label,stat] of kinds){
    const pk=selfIds[role];
    h+="<div class=role><div class=rname><span class=dot"+(pk?"":" off")+"></span>"+label+"</div>"+
      "<div>"+esc(railNames[role]||(role==="companion"?($("self-name").textContent||"").replace(/[()]/g,""):""))+"</div>"+
      (pk?"<div class=rkey title='click to copy prefix' onclick=\"copyText('"+pk+"')\">"+pk+"</div>":"")+
      "<div class=rstat>"+esc(stat)+"</div></div>";
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
function statRow(name,p,c){
  if(!p) return "<tr><td>"+name+"</td><td colspan=9 class=mut>-</td></tr>";
  return "<tr><td>"+name+"</td><td>"+p.recv+"</td><td>"+p.sent+"</td><td>"+p.flood_tx+"</td><td>"+p.direct_tx+
   "</td><td>"+p.flood_rx+"</td><td>"+p.direct_rx+"</td><td>"+p.recv_errors+"</td><td>"+(c?c.queue_len:"-")+
   "</td><td>"+(c?c.errors:"-")+"</td></tr>";
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
    $("dbg-stats").innerHTML=statRow("repeater",d.stats.repeater.packets,d.stats.repeater.core)+
      statRow("room",d.stats.room.packets,d.stats.room.core);
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
  return {src,infoHtml,relay};
}

async function pollPkts(){
  try{
    const d=await (await api("/api/multi/packets?after="+lastSeq)).json();
    devNow=d.now;
    for(const p of d.pkts){
      lastSeq=Math.max(lastSeq,p.s);
      const tr=document.createElement("tr");
      const wd=new Date(Date.now()-(devNow-p.t));
      const when=wd.toTimeString().slice(0,8)+"."+String(wd.getMilliseconds()).padStart(3,"0");
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
        tr.innerHTML="<td>"+when+"</td><td style='color:#e08a4d' title='"+esc(why)+"'>"+
          (p.x===-7?"RX-CRC":"RX-ERR")+"</td><td>"+rt+"</td><td>"+ty+"</td><td>"+
          esc(a.src)+"</td><td class=mut>"+(p.raw?a.infoHtml:esc(why))+"</td><td>"+(p.l||"-")+"</td><td>"+
          (p.snr?snrSpan(p.snr):"")+"</td><td>"+(p.rssi?rssiSpan(p.rssi):"")+"</td>";
        if(p.raw){ tr.style.cursor="pointer"; tr.title="click to decode (corrupt)";
          tr.onclick=()=>openPktModal(p,when,why); }
      } else if(p.e===3){   // send deferred: a sibling identity held the radio
        const owner=(p.x>=0&&lastDebug)?(["repeater","room","companion","chat1","chat2","chat3","chat4","chat5"][p.x]||("port "+p.x)):"another identity";
        tr.innerHTML="<td>"+when+"</td><td style='color:#e0b34d'>TX-BUSY:"+esc(p.d)+"</td><td colspan=4 class=mut>"+
          "send deferred — "+esc(owner)+" was transmitting (will retry)</td><td>-</td><td></td><td></td>";
      } else if(p.e===2){   // TX never completed
        tr.innerHTML="<td>"+when+"</td><td style='color:#e05d5d'>TX-FAIL:"+esc(p.d)+"</td><td colspan=4 class=mut>"+
          "send timed out before TX-done (radio contention?)</td><td>-</td><td></td><td></td>";
      } else {
        const a=annot(p);
        // a received packet carrying our own hash = someone relayed us
        let badge="";
        if(rx&&a.relay){
          const sure=a.relay.width>=2;
          badge="<span class='relay "+(sure?"sure":"weak")+"' title=\""+
            (sure?"a neighbour relayed a packet we sent — "+a.relay.width+"-byte hash match, effectively certain"
                 :"possible relay of our packet — 1-byte hash, ~1 in 256 chance of coincidence")+
            "\">&#8618; relayed "+esc(a.relay.role)+(sure?"":"?")+"</span>";
          tr.className="relayrow";
        }
        tr.innerHTML="<td>"+when+"</td><td class="+(rx?"ok":"err")+">"+(rx?"RX":"TX:"+p.d)+"</td><td>"+
          ROUTES[p.h&3]+"</td><td>"+PTYPES[(p.h>>2)&15]+"</td><td>"+esc(a.src)+"</td><td class=mut>"+badge+a.infoHtml+
          "</td><td>"+p.l+"</td><td>"+(rx?snrSpan(p.snr):"")+"</td><td>"+(rx?rssiSpan(p.rssi):"")+"</td>";
        tr.style.cursor="pointer";
        tr.title="click to decode";
        tr.onclick=()=>openPktModal(p,when);
      }
      const tb=$("pkt-rows"); tb.insertBefore(tr,tb.firstChild);
      while(tb.children.length>120) tb.removeChild(tb.lastChild);
    }
    $("pkt-count").textContent="(seq "+lastSeq+")";
  }catch(e){}
}

// ---- mesh tab ----
const KINDS=["?","chat","repeater","room","sensor"];
function age(sec){ if(sec<=0)return"-"; if(sec<90)return sec+"s"; if(sec<5400)return Math.round(sec/60)+"m";
  if(sec<129600)return Math.round(sec/3600)+"h"; return Math.round(sec/86400)+"d"; }
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
    (rx?" · SNR "+snrSpan(p.snr)+" dB · RSSI "+rssiSpan(p.rssi)+" dBm":"")+"</div>";
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
    tile("Battery",core.battery_mv?(core.battery_mv/1000).toFixed(2)+" V":"-","")+
    tile("Uptime",upStr,"")+
    tile("Radio RX / TX",(d.radio?d.radio.rx:"-")+" / "+(d.radio?d.radio.tx:"-"),
      "packets · last rx "+(d.radio?age(d.radio.rx_age_s):"?")+" ago"+
      (d.radio&&d.radio.refused?" · <span class=err>"+d.radio.refused+" refused</span>":"")+
      (d.radio&&d.radio.recoveries?" · <span class=err>"+d.radio.recoveries+" radio resets</span>":"")+
      (d.radio&&d.radio.stuck?" · <span class=err>"+d.radio.stuck+" stuck TX</span>":""))+
    tile("Heard by peers",
      (d.radio&&d.radio.heard&&d.radio.heard.sent
        ? Math.round(100*d.radio.heard.confirmed/d.radio.heard.sent)+" %" : "-"),
      d.radio&&d.radio.heard
        ? d.radio.heard.confirmed+"/"+d.radio.heard.sent+" floods relayed on · "+
          (d.radio.heard.w2+d.radio.heard.w3)+" certain, "+d.radio.heard.w1+" weak (1-byte)"
        : "relay confirmations")+
    tile("NVS",(d.nvs&&d.nvs.total?Math.round(100*d.nvs.used/d.nvs.total)+" % used":"-"),
      d.nvs?(d.nvs.free+" entries free"):"identity mirror store")+
    tile("Noise floor",(d.radio&&d.radio.noise?d.radio.noise+" dBm":"-"),"")+
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
function renderNearby(){
  if(!contacts.length){ return; }
  const nowS=Math.floor(Date.now()/1000);
  const sorted=[...contacts].sort((a,b)=>b.lastAdvert-a.lastAdvert);   // most recently heard first
  const shown=sorted.slice(0,12);
  $("dash-nearby").innerHTML=shown.map(c=>{
    const heard=c.outPathLen===0?"<span class=ok>direct</span>":
      (c.pk in advHops)?(advHops[c.pk]===0?"<span class=ok>direct RF</span>":advHops[c.pk]+" hops"):
      "<span class=mut>via mesh</span>";
    const hasLoc=(c.lat||c.lon);
    return "<tr><td>"+esc(c.name||"?")+"</td><td>"+(KINDS[c.type]||c.type)+"</td><td class=mut>"+
      c.prefix.slice(0,8)+"</td><td>"+heard+"</td><td>"+age(nowS-c.lastAdvert)+"</td><td>"+
      (hasLoc&&selfLoc&&(selfLoc[0]||selfLoc[1])?distKm(selfLoc,[c.lat,c.lon])+" km":"-")+"</td></tr>";
  }).join("");
  $("nearby-count").textContent="("+shown.length+" of "+contacts.length+" known — full list in Mesh)";
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
    $("stats-live").innerHTML=statRow("repeater",lastDebug.stats.repeater.packets,lastDebug.stats.repeater.core)+
      statRow("room",lastDebug.stats.room.packets,lastDebug.stats.room.core);
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

// ---- settings: identity slots ----
async function loadSlots(){
  const txt=await cmd("slots");
  const rows=stripReply(txt).split("\n").filter(Boolean);
  let h="";
  for(const line of rows){
    const m=line.match(/^chat(\d+):\s*(enabled|disabled)(.*?)\s*\(app port (\d+)\)/);
    if(!m){ h+="<div class=mut style='margin-bottom:4px'>"+esc(line)+"</div>"; continue; }
    const [,n,state,note,port]=m;
    const on=state==="enabled";
    h+="<div class=row style='margin-bottom:2px'><label style='min-width:220px'>"+
      "<input type=checkbox "+(on?"checked":"")+" onchange=\"setSlot("+n+",this.checked)\"> chat "+n+
      " <span class=mut>· app port "+port+"</span></label>"+
      (note.trim()?"<span class=err style='font-size:12px'>"+esc(note.trim().replace(/^—\s*/,""))+"</span>":
        (on?"<span class=ok style='font-size:12px'>running</span>":""))+"</div>";
  }
  $("slots-list").innerHTML=h;
}
async function setSlot(n,on){
  const r=stripReply(await cmd("set slot.chat"+n+" "+(on?"on":"off")));
  $("slots-status").textContent=r+" — applied live, no reboot";
  setTimeout(loadSlots,1500);
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
// only while the Debug section is on screen.
const activeTab=()=>document.querySelector("nav button.on").dataset.t;
const jobs=[
  {name:"pkts",   fn:pollPkts,     period:()=>activeTab()==="debug"?4000:20000, due:0},
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
