#include "web_server.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/md.h"
#include "mbedtls/pkcs5.h"
#include "mbedtls/platform_util.h"

#include "paradox_audit.h"
#include "paradox_event_log.h"
#include "paradox_json_guard.h"
#include "paradox_raw_tap.h"
#include "paradox_same_origin.h"
#include "paradox_state.h"
#include "paradox1738.h"
#include "paradox_config_store.h"

#define WEB_PASSWORD_MIN_LENGTH 4u
#define WEB_PASSWORD_MAX_LENGTH 128u
#define WEB_REQUEST_BODY_MAX 512u
#define WEB_SESSION_SECONDS 1800u
#define WEB_LOGIN_MAX_FAILURES 5u
#define WEB_LOGIN_LOCK_SECONDS 60u
#define WEB_PBKDF2_ITERATIONS 20000u
/* Route slots reserved in the HTTP server. Counted by hand once, and the count went
 * wrong: the table reached exactly max_uri_handlers entries, adding one more route
 * returned ESP_ERR_HTTPD_HANDLERS_FULL, and web_server_start() took the whole UI down
 * with it - while HomeKit carried on, so the module still appeared healthy in Apple
 * Home and had no web interface at all. The route count is asserted against this
 * number at compile time now; the first failure was found in the boot log by
 * test/device/smoke.sh, which fails on any error-level line. */
#define WEB_URI_HANDLER_SLOTS 24u
#define WEB_DEFAULT_PASSWORD "admin"

static const char *TAG = "web_server";
static const char *INDEX_HTML =
    "<!doctype html><html lang=\"en\"><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>Paradox Bridge</title><style>"
    ":root{font:16px system-ui;color:#17212b;background:#f4f6f8}"
    "*{box-sizing:border-box}body{max-width:56rem;margin:auto;padding:1rem}"
    "h1{font-size:1.55rem}h2{font-size:1.2rem}button,input,select{font:inherit;"
    "padding:.55rem;border:1px solid #aeb8c2;border-radius:.35rem}"
    "button{background:#e8eef5;cursor:pointer}button:focus,input:focus,select:focus"
    "{outline:2px solid #3578bd}nav{display:flex;gap:.4rem;overflow:auto;margin:1rem 0}"
    "nav button{white-space:nowrap}section,.card{background:white;border:1px solid #d5dce3;"
    "border-radius:.5rem;padding:1rem;margin:.75rem 0}label{display:block;margin:.5rem 0}"
    "input,select{width:100%;margin-top:.2rem}.grid{display:grid;grid-template-columns:"
    "repeat(auto-fit,minmax(14rem,1fr));gap:.5rem}.zone{border:1px solid #d5dce3;"
    "padding:.65rem;border-radius:.4rem}.zone button{width:100%;margin-top:.5rem}"
    ".hidden{display:none!important}.muted{color:#536271;font-size:.92rem}"
    ".warn{color:#8a1f1f;background:#fdecea;border:.06rem solid #f0b7b3;"
    "padding:.5rem .65rem;border-radius:.35rem}"
    "pre{white-space:pre-wrap;overflow-wrap:anywhere}#notice{color:#7a3d00}"
    "@media(max-width:390px){body{padding:.65rem}nav{gap:.25rem}nav button{padding:.45rem}}"
    "</style><h1>Paradox Bridge</h1><p id=\"message\" role=\"status\" aria-live=\"polite\"></p>"
    "<section id=\"login-box\"><h2>Sign in</h2><form id=\"login\">"
    "<label>Password<input id=\"password\" type=\"password\" required "
    "autocomplete=\"current-password\"></label><button>Sign in</button></form></section>"
    "<main id=\"app\" class=\"hidden\"><nav aria-label=\"Main navigation\">"
    "<button data-tab=\"overview\">Overview</button><button data-tab=\"zones\">Zones</button>"
    "<button data-tab=\"events\">Events</button><button data-tab=\"debug\">Debug</button>"
    "<button data-tab=\"settings\">Settings</button></nav>"
    "<section data-panel=\"overview\"><h2>Live status</h2><p id=\"system-info\" class=\"muted\"></p>"
    "<p id=\"stream-info\" class=\"muted\">Connecting to live status…</p>"
    "<div id=\"status-grid\" class=\"grid\"></div></section>"
    "<section data-panel=\"zones\" class=\"hidden\"><h2>Zone configuration</h2>"
    "<p class=\"muted\">Edits stay on the page until you save them; one save writes every zone you changed.</p>"
    "<p id=\"homekit-pending\" class=\"warn hidden\">HomeKit is still showing the accessory names and "
    "types from before the last change. <button id=\"restart-homekit\">Restart the bridge now</button></p>"
    "<p><button id=\"save-zones\" disabled>Save changes</button> "
    "<span id=\"zone-dirty\" class=\"muted\" role=\"status\"></span></p>"
    "<div id=\"zone-list\" class=\"grid\"></div></section>"
    "<section data-panel=\"events\" class=\"hidden\"><h2>Panel events</h2>"
    "<button id=\"refresh-events\">Refresh</button> <button id=\"clear-events\">Clear log</button>"
    "<div id=\"event-list\"></div></section>"
    "<section data-panel=\"debug\" class=\"hidden\"><h2>Panel debug</h2>"
    "<label><input id=\"debug-enabled\" type=\"checkbox\" style=\"width:auto\"> Enable panel debug</label>"
    "<button id=\"refresh-debug\">Refresh debug data</button><pre id=\"debug-data\" class=\"muted\"></pre>"
    "<h3>Recorded panel simulator</h3><p class=\"muted\">A short test pauses live panel reads, "
    "replays recorded events through the decoder and HomeKit, then restores the previous state. "
    "Simulated alarms may briefly appear in HomeKit.</p><button id=\"run-simulator\" type=\"button\">"
    "Run simulator test</button><p id=\"simulator-status\" class=\"muted\" role=\"status\"></p>"
    "<h3>Raw panel UART capture</h3><p class=\"muted\">Byte-for-byte copy of everything received on the"
    " panel UART, including the bytes the decoder discards while resynchronising. Shown as hex so the"
    " link framing can be investigated over Wi-Fi without a serial cable.</p>"
    "<label><input id=\"raw-live\" type=\"checkbox\" style=\"width:auto\"> Stream live</label>"
    " <button id=\"refresh-raw\">Refresh snapshot</button> <button id=\"clear-raw\" type=\"button\">Clear view</button>"
    "<p id=\"raw-info\" class=\"muted\" role=\"status\"></p><pre id=\"raw-data\" class=\"muted\"></pre>"
    "</section><section data-panel=\"settings\" class=\"hidden\"><h2>Device and Wi-Fi</h2>"
    "<p id=\"device-info\"></p><p>Wi-Fi provisioning uses ESP-IDF's stock BLE provisioning flow."
    " Wi-Fi credentials are not entered on this page.</p><h2>Firmware update</h2>"
    "<p id=\"ota-info\" class=\"muted\"></p><form id=\"ota-form\">"
    "<label>Firmware image (.bin)<input id=\"firmware\" type=\"file\" accept=\".bin,application/octet-stream\" required></label>"
    "<button>Upload and restart</button></form><hr><h2>Change password</h2>"
    "<form id=\"password-form\"><label>Current password<input id=\"current\" type=\"password\" "
    "autocomplete=\"current-password\"></label><label>New password (4–128 printable ASCII characters)"
    "<input id=\"next\" type=\"password\" minlength=\"4\" maxlength=\"128\" required "
    "autocomplete=\"new-password\"></label><button>Save password</button></form>"
    "<button id=\"logout\" type=\"button\">Sign out</button></section></main>"
    "<script>"
    "const $=s=>document.querySelector(s),msg=$('#message');let stream=null,simulatorPoll=null,rawStream=null,rawLines=[];"
    "const request=async(path,options={})=>{const r=await fetch(path,{credentials:'same-origin',...options});"
    "let j=null;const t=await r.text();try{j=JSON.parse(t)}catch{}if(!r.ok)throw Error(j?.error||`HTTP ${r.status}`);return j};"
    "const jsonOptions=(method,data)=>({method,headers:{'Content-Type':'application/json'},body:JSON.stringify(data)});"
    "const showError=e=>{msg.textContent=e.message||String(e)};"
    "const formatUptime=ms=>{const n=Math.floor(ms/1000);return `${Math.floor(n/3600)}:${String(Math.floor(n/60)%60).padStart(2,'0')}:${String(n%60).padStart(2,'0')}`};"
    "const renderStatus=s=>{const grid=$('#status-grid');grid.replaceChildren();"
    "for(const z of s.zones||[]){const d=document.createElement('div');d.className='zone';"
    "d.textContent=`Zone ${z.id}: ${z.open?'Open':'Closed'} · alarm ${z.alarm?'active':'clear'} · "
    "tamper ${z.tampered?'active':'clear'} · bypass ${z.bypassed?'yes':'no'} · uptime ${formatUptime(z.last_change_ms)}`;grid.append(d)}"
    "const f=document.createElement('div');f.className='zone';f.textContent=`Fire: ${s.fire?.alarm?'Alarm':'Clear'} · "
    "fault ${s.fire?.fault?'active':'clear'} · uptime ${formatUptime(s.fire?.last_change_ms||0)}`;grid.append(f)};"
    "const loadStatus=async()=>renderStatus(await request('/api/status'));"
    "const startStream=()=>{if(stream)stream.close();stream=new EventSource('/api/status/stream');"
    "stream.addEventListener('status',e=>{try{renderStatus(JSON.parse(e.data));$('#stream-info').textContent='Live updates connected'}catch{}});"
    "stream.onerror=()=>$('#stream-info').textContent='Live stream reconnecting…'};"
    "const loadSystem=async()=>{const s=await request('/api/system');$('#system-info').textContent="
    "`Wi-Fi ${s.wifi_connected?'connected':'disconnected'} · ${s.version} · ${s.partition}`;"
    "$('#device-info').textContent=`Firmware ${s.version} on ${s.partition}; Wi-Fi ${s.wifi_connected?'connected':'disconnected'}; "
    "heap ${s.free_heap_bytes} free (low-water ${s.min_free_heap_bytes}), web task stack ${s.web_stack_free_bytes} spare.`;"
    "$('#ota-info').textContent=`Upload limit ${s.update_partition_bytes} bytes. Device will restart after validation.`;"
    "$('#debug-enabled').checked=s.panel_debug_enabled};"
    /* One save button for the whole tab instead of one per zone. The editor used to write each zone
     * the moment its own button was pressed, so renaming six zones meant six round trips, six chances
     * for one of them to be refused, and a page holding a mix of saved and unsaved zones with nothing
     * on it saying which was which. What the device last accepted is kept beside every input, and a
     * zone counts as unsaved while the two disagree. */
    "let zoneRows=new Map();"
    "const zoneChanged=()=>{const out=[];for(const [id,row] of zoneRows){"
    "if(row.name.value!==row.saved.name||row.type.value!==row.saved.type||row.enabled.checked!==row.saved.enabled)out.push(id)}"
    "return out};"
    "const markZoneEdits=()=>{const changed=zoneChanged();$('#save-zones').disabled=!changed.length;"
    "$('#zone-dirty').textContent=changed.length?`${changed.length} not saved (zones ${changed.join(', ')})`:''};"
    "$('#save-zones').onclick=async()=>{const changed=zoneChanged();if(!changed.length)return;"
    "const button=$('#save-zones');button.disabled=true;const refused=[];let written=0;"
    "for(const id of changed){const row=zoneRows.get(id);const next={name:row.name.value,type:row.type.value,enabled:row.enabled.checked};"
    "try{await request(`/api/zones/${id}`,jsonOptions('PUT',next));row.saved=next;written++}"
    "catch(e){refused.push(`zone ${id}: ${e.message||String(e)}`)}}"
    "markZoneEdits();if(written)$('#homekit-pending').classList.remove('hidden');"
    "if(refused.length)showError(Error(`${written} saved, ${refused.length} refused (${refused.join('; ')})`));"
    "else msg.textContent=`${written} zone${written===1?'':'s'} saved; names and types reach HomeKit after a bridge restart.`};"
    /* Typed edits lost to a browser refresh is the rude failure of this editor, so the page warns. It
     * is disarmed by clearing zoneRows wherever the page is about to reload for a reason the user chose,
     * and switching tabs never discards them: entering the Zones tab only re-reads a clean page. */
    "window.addEventListener('beforeunload',e=>{if(zoneChanged().length){e.preventDefault();e.returnValue=''}});"
    "const loadZones=async()=>{const data=await request('/api/zones'),root=$('#zone-list');root.replaceChildren();"
    "$('#homekit-pending').classList.toggle('hidden',!data.reboot_required_for_homekit);"
    "zoneRows=new Map();$('#save-zones').disabled=true;$('#zone-dirty').textContent='';"
    "for(const z of data.zones){const box=document.createElement('div');box.className='zone';"
    "const title=document.createElement('strong');title.textContent=`Zone ${z.id}`;box.append(title);"
    "const name=document.createElement('input');name.value=z.name;name.setAttribute('aria-label',`Zone ${z.id} name`);box.append(name);"
    "const type=document.createElement('select');type.setAttribute('aria-label',`Zone ${z.id} type`);"
    "for(const t of ['contact','motion','smoke','leak']){const o=document.createElement('option');o.value=t;o.textContent=t;o.selected=t===z.type;type.append(o)}box.append(type);"
    "const label=document.createElement('label'),enabled=document.createElement('input');enabled.type='checkbox';enabled.checked=z.enabled;enabled.style.width='auto';"
    "label.append(enabled,document.createTextNode(' Enabled'));box.append(label);"
    "zoneRows.set(z.id,{id:z.id,name,type,enabled,saved:{name:z.name,type:z.type,enabled:z.enabled}});"
    "for(const el of [name,type,enabled]){el.addEventListener('input',markZoneEdits);el.addEventListener('change',markZoneEdits)}"
    "root.append(box)}};"
    "const loadEvents=async()=>{const d=await request('/api/events?limit=64'),root=$('#event-list');root.replaceChildren();"
    "if(!d.events.length){root.textContent='No events recorded since boot.';return}for(const e of d.events){const p=document.createElement('p');"
    "p.textContent=`Uptime ${formatUptime(e.timestamp_ms)} · ${e.event} · ${e.class} ${e.category_id}`;root.append(p)}};"
    "const loadDebug=async()=>{const d=await request('/api/debug/panel');$('#debug-data').textContent=JSON.stringify(d,null,2)};"
    "const renderRaw=()=>{$('#raw-data').textContent=rawLines.join('\\n')};const groupHex=h=>(h.match(/../g)||[]).join(' ');"
    "const appendRaw=line=>{rawLines.push(line);if(rawLines.length>300)rawLines.shift();renderRaw()};"
    "const stopRawStream=()=>{if(rawStream)rawStream.close();rawStream=null};const clearRaw=()=>{rawLines=[];renderRaw()};"
    "const loadRawSnapshot=async()=>{const d=await request('/api/debug/raw?bytes=512');"
    "$('#raw-info').textContent=`Ring ${d.capacity} bytes · recorded ${d.bytes_recorded} · aged out ${d.bytes_lost} · showing ${d.hex.length/2} from #${d.first_seq}`;"
    "rawLines=[`snapshot from #${d.first_seq}`,groupHex(d.hex)];renderRaw()};"
    "const startRawStream=()=>{stopRawStream();rawStream=new EventSource('/api/debug/raw/stream');"
    "rawStream.addEventListener('raw',e=>{let d=null;try{d=JSON.parse(e.data)}catch{return}"
    "if(d.dropped)appendRaw(`gap: ${d.dropped} bytes aged out of the ring`);appendRaw(`#${d.seq} ${groupHex(d.hex)}`)});"
    "rawStream.onerror=()=>{$('#raw-info').textContent='Raw stream reconnecting…'};};"
    "const loadSimulator=async()=>{const d=await request('/api/debug/simulator'),button=$('#run-simulator');"
    "button.disabled=!d.supported||d.active;$('#simulator-status').textContent=!d.supported?'Unavailable in the continuous simulator build':"
    "d.active?'Simulator test running; previous state will be restored automatically.':'Ready';"
    "if(simulatorPoll)clearTimeout(simulatorPoll);if(d.active)simulatorPoll=setTimeout(()=>loadSimulator().catch(showError),500)};"
    "const initialize=async()=>{try{await Promise.all([loadSystem(),loadStatus(),loadZones(),loadEvents()]);"
    "if($('#debug-enabled').checked)await loadDebug();startStream()}catch(e){showError(e)}};"
    "$('#login').onsubmit=async e=>{e.preventDefault();try{await request('/api/auth/login',jsonOptions('POST',{password:$('#password').value}));"
    "$('#password').value='';$('#login-box').classList.add('hidden');$('#app').classList.remove('hidden');msg.textContent='Signed in.';await initialize()}catch(e){showError(e)}};"
    "document.querySelectorAll('[data-tab]').forEach(b=>b.onclick=()=>{document.querySelectorAll('[data-panel]').forEach(p=>p.classList.toggle('hidden',p.dataset.panel!==b.dataset.tab));"
    "if(b.dataset.tab==='zones'){if(zoneChanged().length)msg.textContent=`${zoneChanged().length} zone edits are unsaved and stay on the page.`;else loadZones().catch(showError)}"
    "if(b.dataset.tab==='events')loadEvents().catch(showError);"
    "if(b.dataset.tab==='debug'){loadDebug().catch(showError);loadSimulator().catch(showError);loadRawSnapshot().catch(e=>$('#raw-info').textContent=e.message)}"
    "else{stopRawStream();$('#raw-live').checked=false}});"
    "$('#refresh-events').onclick=()=>loadEvents().catch(showError);$('#clear-events').onclick=async()=>{try{await request('/api/events/clear',jsonOptions('POST',{}));await loadEvents()}catch(e){showError(e)}};"
    "$('#debug-enabled').onchange=async e=>{try{await request('/api/debug',jsonOptions('PUT',{enabled:e.target.checked}));"
    "if(e.target.checked){await loadDebug();await loadRawSnapshot()}else{stopRawStream();$('#raw-live').checked=false;clearRaw();"
    "$('#debug-data').textContent='Panel debug disabled';$('#raw-info').textContent='Panel debug disabled'}}catch(x){e.target.checked=!e.target.checked;showError(x)}};"
    "$('#refresh-debug').onclick=()=>{loadDebug().catch(showError);loadSimulator().catch(showError)};"
    "$('#refresh-raw').onclick=()=>{stopRawStream();$('#raw-live').checked=false;loadRawSnapshot().catch(e=>$('#raw-info').textContent=e.message)};"
    "$('#clear-raw').onclick=clearRaw;"
    "$('#raw-live').onchange=e=>{if(e.target.checked){try{startRawStream();$('#raw-info').textContent='Streaming raw panel bytes…'}catch(x){e.target.checked=false;showError(x)}}else{stopRawStream();$('#raw-info').textContent='Live stream stopped'}};"
    "$('#run-simulator').onclick=async()=>{if(!confirm('Run the recorded panel test? Simulated zone and fire events will briefly appear in HomeKit.'))return;"
    "const button=$('#run-simulator');button.disabled=true;try{await request('/api/debug/simulator',jsonOptions('POST',{}));"
    "msg.textContent='Simulator test started.';await loadSimulator()}catch(e){button.disabled=false;showError(e)}};"
    "$('#password-form').onsubmit=async e=>{e.preventDefault();try{await request('/api/auth/password',jsonOptions('POST',{current_password:$('#current').value,new_password:$('#next').value}));"
    "$('#current').value='';$('#next').value='';msg.textContent='Password changed.'}catch(x){showError(x)}};"
    "$('#logout').onclick=async()=>{try{await request('/api/auth/logout',jsonOptions('POST',{}));if(stream)stream.close();stopRawStream();$('#raw-live').checked=false;$('#app').classList.add('hidden');$('#login-box').classList.remove('hidden');msg.textContent='Signed out.'}catch(e){showError(e)}};"
    "$('#ota-form').onsubmit=async e=>{e.preventDefault();const file=$('#firmware').files[0];if(!file)return;"
    "if(!confirm(`Upload ${file.name} (${file.size} bytes) and restart the device?`))return;"
    "zoneRows.clear();"
    "try{msg.textContent='Uploading firmware…';await request('/api/ota',{method:'POST',headers:{'Content-Type':'application/octet-stream'},body:file});msg.textContent='Firmware accepted; device restarting.'}catch(x){showError(x)}};"
    "const waitForBridgeBack=async()=>{for(let i=0;i<90;i++){try{const r=await fetch('/api/health');if(r.ok){location.reload();return}}catch{}"
    "await new Promise(r=>setTimeout(r,1000))}msg.textContent='The device has not come back within 90 s; check its address and reload.'};"
    "$('#restart-homekit').onclick=async()=>{if(!confirm('Restart the bridge now? Every HomeKit controller - Apple Home, hubs and automations - loses its session for several seconds, and sensors answer no response meanwhile.'))return;"
    "const button=$('#restart-homekit');button.disabled=true;msg.textContent='Restarting the bridge; HomeKit is unreachable for a few seconds.';"
    "zoneRows.clear();"
    "sessionStorage.setItem('bridge-restarted','1');"
    "try{await request('/api/homekit/restart',jsonOptions('POST',{confirm:true}))}catch(e){button.disabled=false;showError(e);return}"
    "await waitForBridgeBack()};"
    "if(sessionStorage.getItem('bridge-restarted')){sessionStorage.removeItem('bridge-restarted');"
    "msg.textContent='Bridge restarted, and the accessory database was rebuilt. Sign in again to continue.'}"
    "const resume=async()=>{try{const a=await request('/api/auth/status');if(a.authenticated){$('#login-box').classList.add('hidden');"
    "$('#app').classList.remove('hidden');await initialize()}}catch{}};resume();"
    "</script></html>";

static paradox_config_t *s_config;
static httpd_handle_t s_server;
static char s_session_token[65];
static char s_session_cookie[160];
static uint64_t s_session_expires;
static uint64_t s_login_blocked_until;
static unsigned s_login_failures;
static SemaphoreHandle_t s_config_mutex;
static volatile bool s_server_running;
static portMUX_TYPE s_session_mux = portMUX_INITIALIZER_UNLOCKED;

/*
 * True once a zone edit has landed that the running HomeKit bridge cannot
 * apply. The bridge copies accessory names, service types and which zones
 * exist out of the zone configuration when it builds, and a paired bridge's
 * accessory database cannot be rebuilt in place, so those fields only follow an
 * edit after a restart.
 *
 * It is RAM-only on purpose: a restart is the only thing that can clear it, and
 * a restart is exactly when the bridge stops being stale. So it starts false at
 * every boot - the bridge has just been built from the current configuration -
 * and the only way back to false is to do the thing the flag asks for.
 *
 * A conservative true: an edit that writes back the same values still reports
 * HomeKit as stale, because deciding otherwise would mean comparing against the
 * snapshot the bridge was built from, which is not kept.
 */
static volatile bool s_homekit_pending;
/* Set by the bridge once its accessory database is complete; see
 * web_server_set_homekit_dump(). Read without a lock because the hub task writes
 * it exactly once, before any value it guards is useful. */
static web_homekit_dump_fn_t s_homekit_dump;

/* Live Server-Sent Events sessions. Each one owns an async httpd request and a
 * task of its own, so a client that disappears without a clean close still has to
 * leave these back at zero: the next send or heartbeat fails, the task exits, and
 * the count goes down. They are reported by GET /api/debug/raw so a test can watch
 * that happen rather than trust it, and a leak would show as a count that climbs
 * and never comes back. */
static int s_status_stream_clients;
static int s_raw_stream_clients;

static void stream_clients_add(int *counter, int delta)
{
    portENTER_CRITICAL(&s_session_mux);
    *counter += delta;
    portEXIT_CRITICAL(&s_session_mux);
}

static uint64_t uptime_seconds(void)
{
    return (uint64_t)esp_timer_get_time() / 1000000ULL;
}

static bool bytes_are_zero(const uint8_t *bytes, size_t size)
{
    uint8_t value = 0;
    for (size_t i = 0; i < size; i++) {
        value |= bytes[i];
    }
    return value == 0;
}

static bool constant_time_equal(const uint8_t *left, const uint8_t *right,
                                size_t size)
{
    uint8_t difference = 0;
    for (size_t i = 0; i < size; i++) {
        difference |= left[i] ^ right[i];
    }
    return difference == 0;
}

static void delete_json_strings(cJSON *json)
{
    for (cJSON *item = json == NULL ? NULL : json->child;
         item != NULL; item = item->next) {
        if (cJSON_IsString(item) && item->valuestring != NULL) {
            mbedtls_platform_zeroize(item->valuestring,
                                     strlen(item->valuestring));
        }
    }
    cJSON_Delete(json);
}

static esp_err_t derive_password_hash(const char *password, size_t password_size,
                                     const uint8_t *salt, uint8_t *hash)
{
    int result = mbedtls_pkcs5_pbkdf2_hmac_ext(
        MBEDTLS_MD_SHA256, (const unsigned char *)password, password_size,
        salt, PARADOX_CONFIG_WEB_AUTH_SALT_SIZE, WEB_PBKDF2_ITERATIONS,
        PARADOX_CONFIG_WEB_AUTH_HASH_SIZE, hash);
    if (result != 0) {
        ESP_LOGE(TAG, "Password hash derivation failed: -0x%x", -result);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static bool valid_password(const char *password, size_t *password_size)
{
    if (password == NULL) {
        return false;
    }
    size_t length = strnlen(password, WEB_PASSWORD_MAX_LENGTH + 1u);
    if (length < WEB_PASSWORD_MIN_LENGTH ||
        length > WEB_PASSWORD_MAX_LENGTH) {
        return false;
    }
    for (size_t i = 0; i < length; i++) {
        if ((unsigned char)password[i] < 0x20u ||
            (unsigned char)password[i] > 0x7eu) {
            return false;
        }
    }
    *password_size = length;
    return true;
}

static esp_err_t generate_salt_and_hash(const char *password, size_t password_size,
                                        uint8_t *salt, uint8_t *hash)
{
    esp_fill_random(salt, PARADOX_CONFIG_WEB_AUTH_SALT_SIZE);
    return derive_password_hash(password, password_size, salt, hash);
}

static esp_err_t send_json(httpd_req_t *request, const char *json)
{
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
    return httpd_resp_sendstr(request, json);
}

static const char *method_name(httpd_method_t method)
{
    switch (method) {
    case HTTP_GET: return "GET";
    case HTTP_POST: return "POST";
    case HTTP_PUT: return "PUT";
    case HTTP_DELETE: return "DELETE";
    case HTTP_HEAD: return "HEAD";
    case HTTP_OPTIONS: return "OPTIONS";
    case HTTP_PATCH: return "PATCH";
    default: return "OTHER";
    }
}

/* One credential exists today, so the honest answer is admin or anonymous. The
 * field is here now so that the split into operator and admin does not have to
 * retrofit a trail that recorded nobody, and so that a refusal is visibly a
 * refusal by someone who was not who they claimed to be.
 *
 * The principal has to be read without replying to the request. The tempting
 * predicate is request_is_authorized(), and it is a trap: it is not a pure
 * question, it answers the request with a 401 or 403 on the way to returning
 * false. Asking it inside the recorder replaced every unauthenticated response -
 * including a successful sign-in - with an error, which is what the robustness
 * battery caught on hardware. request_has_session() is the side-effect-free
 * question, and it is also the more truthful one for a record: a cross-origin
 * refusal was still made by someone holding a session, and the status says 403. */
static bool request_has_session(httpd_req_t *request);

static const char *request_principal(httpd_req_t *request)
{
    return request_has_session(request) ? "admin" : "anonymous";
}

/* Records the request line, never the request body. A body is where a password
 * lives, and nothing the trail has to prove needs one: which zone, which command
 * and what was decided is all in the fields. paradox_audit.c keeps every field
 * inside printable ASCII, so a client cannot forge a record boundary. */
static void web_audit(httpd_req_t *request, const char *status, const char *note)
{
    paradox_audit_record(esp_timer_get_time() / 1000, request_principal(request),
                         method_name(request->method), request->uri, status, note);
}

static esp_err_t send_error(httpd_req_t *request, const char *status,
                            const char *message)
{
    httpd_resp_set_hdr(request, "Connection", "close");
    httpd_resp_set_status(request, status);
    /* Every refusal the web layer makes passes through this function, which is why
     * the record belongs here rather than in the handlers: a refusal cannot be
     * missed by being recorded in the wrong place, and a new endpoint inherits the
     * record without its author remembering to add one. Reads are excluded because
     * a browser polling /api/status would fill the 16 records before anything
     * interesting happened, and the question is always about a write. */
    if (request->method != HTTP_GET && request->method != HTTP_HEAD) {
        char code[4] = {'?', '?', '?', '\0'};
        for (size_t i = 0; i < 3u && status[i] != '\0'; ++i) {
            code[i] = status[i];
        }
        web_audit(request, code, "refused");
    }
    /* Two ways this used to turn a caller's correct 4xx into a misleading 500: a
     * message longer than the buffer, and a message carrying a double quote, which
     * lands raw inside the JSON and makes the body unparsable. Messages are kept
     * short and quote-free for that reason; the size check and the fallback below
     * are what make an oversight visible instead of silent. */
    char body[160];
    int length = snprintf(body, sizeof(body), "{\"error\":\"%s\"}", message);
    if (length < 0 || (size_t)length >= sizeof(body)) {
        ESP_LOGE(TAG, "Error message for %s does not fit the body buffer (%d bytes); "
                      "sending the status with no body",
                 status, length);
        /* Keep the status the caller chose. Reporting 500 for a client's bad request
         * hides the finding and points the next reader at the server. */
        return httpd_resp_send(request, NULL, 0);
    }
    return send_json(request, body);
}

/* Read and parse a JSON body, replying to the request when the body is unusable.
 * Returns ESP_OK only when *json is a parsed object the caller now owns. Returns
 * ESP_FAIL when a 4xx has already been sent and *json is NULL, so the caller must
 * return without touching it. Returning the reply's own status was a defect: a
 * successfully sent 400 is ESP_OK, so the callers' `receive_json(...) != ESP_OK`
 * guard never fired for a rejected body and the handler went on to walk a NULL
 * cJSON tree, which rebooted the device from a truncated PUT. */
static esp_err_t receive_json(httpd_req_t *request, cJSON **json)
{
    if (request->content_len == 0) {
        (void)send_error(request, "400 Bad Request", "JSON body required");
        return ESP_FAIL;
    }
    if (request->content_len > WEB_REQUEST_BODY_MAX) {
        httpd_resp_set_hdr(request, "Connection", "close");
        (void)send_error(request, "413 Payload Too Large", "Request too large");
        return ESP_FAIL;
    }

    char content_type[64];
    if (httpd_req_get_hdr_value_str(request, "Content-Type", content_type,
                                    sizeof(content_type)) != ESP_OK ||
        strncmp(content_type, "application/json", 16) != 0 ||
        (content_type[16] != '\0' && content_type[16] != ';' &&
         content_type[16] != ' ')) {
        (void)send_error(request, "415 Unsupported Media Type",
                         "Expected application/json");
        return ESP_FAIL;
    }

    char body[WEB_REQUEST_BODY_MAX + 1u];
    size_t received = 0;
    while (received < request->content_len) {
        int result = httpd_req_recv(request, body + received,
                                    request->content_len - received);
        if (result <= 0) {
            (void)send_error(request, "400 Bad Request", "Incomplete body");
            return ESP_FAIL;
        }
        received += (size_t)result;
    }
    body[received] = '\0';
    if (memchr(body, '\0', received) != NULL) {
        (void)send_error(request, "400 Bad Request", "Invalid JSON object");
        return ESP_FAIL;
    }
    /* Bound the nesting before the parser sees it. cJSON recurses once per
     * container level, and the web task stack is 8 KiB, so a body built out of
     * nothing but open brackets overflowed that stack and rebooted the module.
     * Every body this API accepts is a flat object, so the limit costs nothing. */
    if (!paradox_json_depth_ok(body, received, PARADOX_JSON_MAX_DEPTH, NULL)) {
        (void)send_error(request, "400 Bad Request", "JSON body nests too deeply");
        return ESP_FAIL;
    }
    const char *parse_end = NULL;
    *json = cJSON_ParseWithLengthOpts(body, received, &parse_end, false);
    if (*json != NULL) {
        const char *body_end = body + received;
        while (parse_end < body_end &&
               (*parse_end == ' ' || *parse_end == '\t' ||
                *parse_end == '\r' || *parse_end == '\n')) {
            parse_end++;
        }
        if (parse_end != body_end) {
            cJSON_Delete(*json);
            *json = NULL;
        }
    }
    if (*json == NULL || !cJSON_IsObject(*json)) {
        cJSON_Delete(*json);
        *json = NULL;
        (void)send_error(request, "400 Bad Request", "Invalid JSON object");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static bool same_origin_url(const char *url, size_t url_size, const char *host)
{
    return paradox_same_origin_url(url, url_size, host);
}

static bool request_is_same_origin(httpd_req_t *request)
{
    char host[128];
    if (httpd_req_get_hdr_value_str(request, "Host", host, sizeof(host)) != ESP_OK) {
        return false;
    }

    char origin[192];
    size_t origin_size = httpd_req_get_hdr_value_len(request, "Origin");
    if (origin_size != 0 && origin_size < sizeof(origin) &&
        httpd_req_get_hdr_value_str(request, "Origin", origin,
                                    sizeof(origin)) == ESP_OK) {
        return same_origin_url(origin, origin_size, host);
    }

    size_t referer_size = httpd_req_get_hdr_value_len(request, "Referer");
    return referer_size != 0 && referer_size < sizeof(origin) &&
           httpd_req_get_hdr_value_str(request, "Referer", origin,
                                       sizeof(origin)) == ESP_OK &&
           same_origin_url(origin, referer_size, host);
}

static bool request_has_session(httpd_req_t *request)
{
    char expected_token[sizeof(s_session_token)];
    uint64_t expires;
    portENTER_CRITICAL(&s_session_mux);
    memcpy(expected_token, s_session_token, sizeof(expected_token));
    expires = s_session_expires;
    if (uptime_seconds() >= expires) {
        memset(s_session_token, 0, sizeof(s_session_token));
        s_session_expires = 0;
    }
    portEXIT_CRITICAL(&s_session_mux);
    if (expected_token[0] == '\0' || uptime_seconds() >= expires) {
        mbedtls_platform_zeroize(expected_token, sizeof(expected_token));
        return false;
    }

    char cookie[256];
    if (httpd_req_get_hdr_value_str(request, "Cookie", cookie,
                                    sizeof(cookie)) != ESP_OK) {
        mbedtls_platform_zeroize(expected_token, sizeof(expected_token));
        return false;
    }

    const char *cursor = cookie;
    while (*cursor != '\0') {
        while (*cursor == ' ' || *cursor == ';') {
            cursor++;
        }
        const char *end = strchr(cursor, ';');
        size_t length = end == NULL ? strlen(cursor) : (size_t)(end - cursor);
        static const char key[] = "session=";
        if (length == sizeof(key) - 1u + 64u &&
            memcmp(cursor, key, sizeof(key) - 1u) == 0 &&
            constant_time_equal((const uint8_t *)(cursor + sizeof(key) - 1u),
                                (const uint8_t *)expected_token, 64u)) {
            mbedtls_platform_zeroize(expected_token, sizeof(expected_token));
            return true;
        }
        if (end == NULL) {
            break;
        }
        cursor = end + 1;
    }
    mbedtls_platform_zeroize(expected_token, sizeof(expected_token));
    return false;
}

static bool session_is_current(const char *token, uint64_t expires)
{
    portENTER_CRITICAL(&s_session_mux);
    bool current = uptime_seconds() < expires &&
                   s_session_expires == expires &&
                   constant_time_equal((const uint8_t *)token,
                                       (const uint8_t *)s_session_token,
                                       sizeof(s_session_token));
    portEXIT_CRITICAL(&s_session_mux);
    return current;
}

static bool request_is_authorized(httpd_req_t *request)
{
    bool mutating = request->method != HTTP_GET &&
                    request->method != HTTP_HEAD;
    if (mutating && !request_is_same_origin(request)) {
        (void)send_error(request, "403 Forbidden", "Same-origin request required");
        return false;
    }
    if (!request_has_session(request)) {
        (void)send_error(request, "401 Unauthorized", "Authentication required");
        return false;
    }
    return true;
}

static esp_err_t issue_session(httpd_req_t *request)
{
    static const char HEX[] = "0123456789abcdef";
    uint8_t random_bytes[32];
    char token[sizeof(s_session_token)];
    esp_fill_random(random_bytes, sizeof(random_bytes));
    for (size_t i = 0; i < sizeof(random_bytes); i++) {
        token[i * 2u] = HEX[random_bytes[i] >> 4];
        token[i * 2u + 1u] = HEX[random_bytes[i] & 0x0fu];
    }
    mbedtls_platform_zeroize(random_bytes, sizeof(random_bytes));
    token[64] = '\0';

    portENTER_CRITICAL(&s_session_mux);
    memcpy(s_session_token, token, sizeof(s_session_token));
    s_session_expires = uptime_seconds() + WEB_SESSION_SECONDS;
    int length = snprintf(s_session_cookie, sizeof(s_session_cookie),
                          "session=%s; Path=/; Max-Age=%u; HttpOnly; SameSite=Strict",
                          s_session_token, WEB_SESSION_SECONDS);
    if (length <= 0 || (size_t)length >= sizeof(s_session_cookie)) {
        memset(s_session_token, 0, sizeof(s_session_token));
        memset(s_session_cookie, 0, sizeof(s_session_cookie));
        s_session_expires = 0;
        portEXIT_CRITICAL(&s_session_mux);
        mbedtls_platform_zeroize(token, sizeof(token));
        return ESP_ERR_INVALID_SIZE;
    }
    portEXIT_CRITICAL(&s_session_mux);
    mbedtls_platform_zeroize(token, sizeof(token));
    esp_err_t err = httpd_resp_set_hdr(request, "Set-Cookie", s_session_cookie);
    if (err != ESP_OK) {
        portENTER_CRITICAL(&s_session_mux);
        memset(s_session_token, 0, sizeof(s_session_token));
        memset(s_session_cookie, 0, sizeof(s_session_cookie));
        s_session_expires = 0;
        portEXIT_CRITICAL(&s_session_mux);
    }
    return err;
}

static bool config_lock(void)
{
    return s_config_mutex != NULL &&
           xSemaphoreTake(s_config_mutex, portMAX_DELAY) == pdTRUE;
}

static void config_unlock(void)
{
    xSemaphoreGive(s_config_mutex);
}

static esp_err_t send_json_object(httpd_req_t *request, cJSON *json)
{
    if (json == NULL) {
        return send_error(request, "500 Internal Server Error",
                          "Response allocation failed");
    }
    char *text = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (text == NULL) {
        return send_error(request, "500 Internal Server Error",
                          "Response serialization failed");
    }
    esp_err_t err = send_json(request, text);
    free(text);
    return err;
}

/* Rendering lives in the pure config module; only the display fallback for a
 * service value that a decoded config cannot produce stays here. */
static const char *service_name(paradox_hk_service_t service)
{
    const char *name = paradox_config_service_name(service);
    return name == NULL ? "unknown" : name;
}

static const char *class_name(paradox_class_t cls)
{
    switch (cls) {
    case PARADOX_CLS_ZONE: return "zone";
    case PARADOX_CLS_PARTITION_STATUS: return "partition_status";
    case PARADOX_CLS_NON_REPORT: return "non_report";
    case PARADOX_CLS_REMOTE: return "remote";
    case PARADOX_CLS_ACCESS_CODE: return "access_code";
    case PARADOX_CLS_SPECIAL_ALARM: return "special_alarm";
    case PARADOX_CLS_SPECIAL_ARM: return "special_arm";
    case PARADOX_CLS_SPECIAL_DISARM: return "special_disarm";
    case PARADOX_CLS_TROUBLE: return "trouble";
    case PARADOX_CLS_SPECIAL_REPORT: return "special_report";
    default: return "unknown";
    }
}

static bool parse_event_class(const char *name, paradox_class_t *cls)
{
    for (int value = PARADOX_CLS_UNKNOWN; value <= PARADOX_CLS_SPECIAL_REPORT;
         value++) {
        if (strcmp(name, class_name((paradox_class_t)value)) == 0) {
            *cls = (paradox_class_t)value;
            return true;
        }
    }
    return false;
}

static cJSON *state_snapshot_json(const paradox_state_snapshot_t *snapshot)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *zones = cJSON_CreateArray();
    cJSON *fire = cJSON_CreateObject();
    if (root == NULL || zones == NULL || fire == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(zones);
        cJSON_Delete(fire);
        return NULL;
    }
    if (cJSON_AddNumberToObject(root, "generation",
                                (double)snapshot->generation) == NULL ||
        /* When this JSON was produced, on the device clock. A client measuring
         * the panel-to-HomeKit path cannot line its own clock up with the
         * device's, but pushed_ms minus a zone's last_change_ms is a difference
         * of two device stamps, so it needs no clock synchronisation. */
        cJSON_AddNumberToObject(root, "pushed_ms",
                                (double)(esp_timer_get_time() / 1000)) == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(zones);
        cJSON_Delete(fire);
        return NULL;
    }
    for (int i = 0; i < PARADOX_MAX_ZONES; i++) {
        const paradox_zone_state_t *state = &snapshot->zones[i];
        cJSON *zone = cJSON_CreateObject();
        if (zone == NULL ||
            cJSON_AddNumberToObject(zone, "id", i + 1) == NULL ||
            cJSON_AddBoolToObject(zone, "open", state->open) == NULL ||
            cJSON_AddBoolToObject(zone, "alarm", state->in_alarm) == NULL ||
            cJSON_AddBoolToObject(zone, "tampered", state->tampered) == NULL ||
            cJSON_AddBoolToObject(zone, "bypassed", state->bypassed) == NULL ||
            cJSON_AddNumberToObject(zone, "last_change_ms",
                                    (double)state->last_change_ms) == NULL) {
            cJSON_Delete(zone);
            cJSON_Delete(root);
            cJSON_Delete(zones);
            cJSON_Delete(fire);
            return NULL;
        }
        if (!cJSON_AddItemToArray(zones, zone)) {
            cJSON_Delete(zone);
            cJSON_Delete(root);
            cJSON_Delete(zones);
            cJSON_Delete(fire);
            return NULL;
        }
    }
    if (cJSON_AddBoolToObject(fire, "alarm", snapshot->fire_alarm) == NULL ||
        cJSON_AddBoolToObject(fire, "restored", snapshot->fire_restored) == NULL ||
        cJSON_AddBoolToObject(fire, "fault", snapshot->fire_fault) == NULL ||
        cJSON_AddNumberToObject(fire, "last_change_ms",
                                (double)snapshot->fire_last_change_ms) == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(zones);
        cJSON_Delete(fire);
        return NULL;
    }
    if (!cJSON_AddItemToObject(root, "zones", zones)) {
        cJSON_Delete(root);
        cJSON_Delete(zones);
        cJSON_Delete(fire);
        return NULL;
    }
    if (!cJSON_AddItemToObject(root, "fire", fire)) {
        cJSON_Delete(root);
        cJSON_Delete(fire);
        return NULL;
    }
    return root;
}

static esp_err_t zones_get_handler(httpd_req_t *request)
{
    if (!request_is_authorized(request)) {
        return ESP_OK;
    }
    paradox_config_t config;
    if (!config_lock()) {
        return send_error(request, "500 Internal Server Error",
                          "Configuration unavailable");
    }
    config = *s_config;
    config_unlock();

    paradox_state_snapshot_t snapshot;
    if (paradox_state_snapshot(&snapshot) != PARADOX_STATE_OK) {
        return send_error(request, "503 Service Unavailable",
                          "State unavailable");
    }
    cJSON *root = cJSON_CreateObject();
    cJSON *zones = cJSON_CreateArray();
    if (root == NULL || zones == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(zones);
        return send_error(request, "500 Internal Server Error",
                          "Response allocation failed");
    }
    if (cJSON_AddBoolToObject(root, "reboot_required_for_homekit",
                              s_homekit_pending) == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(zones);
        return send_error(request, "500 Internal Server Error",
                          "Response allocation failed");
    }
    for (int i = 0; i < PARADOX_MAX_ZONES; i++) {
        const paradox_zone_config_t *config_zone = &config.zones[i];
        const paradox_zone_state_t *state_zone = &snapshot.zones[i];
        cJSON *zone = cJSON_CreateObject();
        if (zone == NULL ||
            cJSON_AddNumberToObject(zone, "id", i + 1) == NULL ||
            cJSON_AddStringToObject(zone, "name", config_zone->name) == NULL ||
            cJSON_AddStringToObject(zone, "type",
                                    service_name(config_zone->service)) == NULL ||
            cJSON_AddBoolToObject(zone, "enabled", config_zone->enabled) == NULL ||
            cJSON_AddBoolToObject(zone, "open", state_zone->open) == NULL ||
            cJSON_AddBoolToObject(zone, "alarm", state_zone->in_alarm) == NULL ||
            cJSON_AddBoolToObject(zone, "tampered", state_zone->tampered) == NULL ||
            cJSON_AddBoolToObject(zone, "bypassed", state_zone->bypassed) == NULL ||
            cJSON_AddNumberToObject(zone, "last_change_ms",
                                    (double)state_zone->last_change_ms) == NULL) {
            cJSON_Delete(zone);
            cJSON_Delete(root);
            cJSON_Delete(zones);
            return send_error(request, "500 Internal Server Error",
                              "Response allocation failed");
        }
        if (!cJSON_AddItemToArray(zones, zone)) {
            cJSON_Delete(zone);
            cJSON_Delete(root);
            cJSON_Delete(zones);
            return send_error(request, "500 Internal Server Error",
                              "Response allocation failed");
        }
    }
    if (!cJSON_AddItemToObject(root, "zones", zones)) {
        cJSON_Delete(root);
        cJSON_Delete(zones);
        return send_error(request, "500 Internal Server Error",
                          "Response allocation failed");
    }
    return send_json_object(request, root);
}

static bool parse_zone_id(const char *uri, int *zone_id)
{
    static const char prefix[] = "/api/zones/";
    if (strncmp(uri, prefix, sizeof(prefix) - 1u) != 0) {
        return false;
    }
    /* The digits rule is the pure module's, so index bounds are testable
     * without esp_http_server. */
    return paradox_config_zone_index_from_text(uri + sizeof(prefix) - 1u,
                                               zone_id);
}

static bool json_has_only_zone_fields(const cJSON *json)
{
    /* NULL is not "no fields": a caller that was already rejected must not reach
     * here, but walking a null tree must never be fatal either. */
    for (const cJSON *item = json == NULL ? NULL : json->child;
         item != NULL; item = item->next) {
        if (!paradox_config_zone_field_is_known(item->string)) {
            return false;
        }
    }
    return true;
}

static esp_err_t zone_put_handler(httpd_req_t *request)
{
    if (!request_is_authorized(request)) {
        return ESP_OK;
    }
    int zone_id;
    if (!parse_zone_id(request->uri, &zone_id)) {
        return send_error(request, "400 Bad Request", "Invalid zone index");
    }
    cJSON *json = NULL;
    if (receive_json(request, &json) != ESP_OK) {
        return ESP_OK;
    }
    if (!json_has_only_zone_fields(json)) {
        delete_json_strings(json);
        return send_error(request, "400 Bad Request", "Unknown zone field");
    }
    cJSON *name_item = cJSON_GetObjectItemCaseSensitive(json, "name");
    cJSON *type_item = cJSON_GetObjectItemCaseSensitive(json, "type");
    cJSON *enabled_item = cJSON_GetObjectItemCaseSensitive(json, "enabled");
    paradox_hk_service_t service = PARADOX_HK_SERVICE_CONTACT;
    if ((name_item != NULL && !cJSON_IsString(name_item)) ||
        (type_item != NULL &&
         (!cJSON_IsString(type_item) ||
          !paradox_config_service_from_name(type_item->valuestring,
                                           &service))) ||
        (enabled_item != NULL && !cJSON_IsBool(enabled_item)) ||
        (name_item == NULL && type_item == NULL && enabled_item == NULL)) {
        delete_json_strings(json);
        return send_error(request, "400 Bad Request", "Invalid zone fields");
    }

    paradox_config_result_t result;
    if (!config_lock()) {
        delete_json_strings(json);
        return send_error(request, "500 Internal Server Error",
                          "Configuration unavailable");
    }
    paradox_config_t candidate = *s_config;
    result = paradox_config_update_zone(
        &candidate, zone_id,
        name_item == NULL ? NULL : name_item->valuestring,
        name_item != NULL, (int)service, type_item != NULL,
        enabled_item != NULL && cJSON_IsTrue(enabled_item),
        enabled_item != NULL);
    if (result == PARADOX_CONFIG_OK) {
        esp_err_t err = paradox_config_store_save(&candidate);
        if (err == ESP_OK) {
            *s_config = candidate;
        } else {
            config_unlock();
            delete_json_strings(json);
            ESP_LOGE(TAG, "Cannot save zone %d configuration: %s", zone_id,
                     esp_err_to_name(err));
            return send_error(request, "500 Internal Server Error",
                              "Zone configuration could not be saved");
        }
    }
    config_unlock();
    delete_json_strings(json);
    if (result == PARADOX_CONFIG_ZONE_OUT_OF_RANGE) {
        return send_error(request, "400 Bad Request", "Invalid zone index");
    }
    if (result == PARADOX_CONFIG_NAME_TOO_LONG ||
        result == PARADOX_CONFIG_NAME_EMPTY ||
        result == PARADOX_CONFIG_INVALID_VALUE) {
        return send_error(request, "400 Bad Request", "Invalid zone configuration");
    }
    if (result != PARADOX_CONFIG_OK) {
        return send_error(request, "400 Bad Request", "Invalid zone configuration");
    }
    /* The write is in NVS and visible to the API from here on, but HomeKit is
     * showing the accessories as they were when the bridge built them. */
    s_homekit_pending = true;
    char note[PARADOX_AUDIT_NOTE_MAX];
    snprintf(note, sizeof(note), "zone %d configuration written", zone_id);
    web_audit(request, "200", note);
    cJSON *response = cJSON_CreateObject();
    if (response == NULL ||
        cJSON_AddBoolToObject(response, "ok", true) == NULL ||
        cJSON_AddNumberToObject(response, "zone_id", zone_id) == NULL ||
        cJSON_AddBoolToObject(response, "reboot_required_for_homekit",
                              s_homekit_pending) == NULL) {
        cJSON_Delete(response);
        return send_error(request, "500 Internal Server Error",
                          "Response allocation failed");
    }
    return send_json_object(request, response);
}

static esp_err_t status_get_handler(httpd_req_t *request)
{
    if (!request_is_authorized(request)) {
        return ESP_OK;
    }
    paradox_state_snapshot_t snapshot;
    if (paradox_state_snapshot(&snapshot) != PARADOX_STATE_OK) {
        return send_error(request, "503 Service Unavailable",
                          "State unavailable");
    }
    cJSON *root = state_snapshot_json(&snapshot);
    return send_json_object(request, root);
}

static cJSON *event_json(const paradox_event_log_entry_t *entry)
{
    const paradox_event_t *event = &entry->event;
    cJSON *json = cJSON_CreateObject();
    if (json == NULL ||
        cJSON_AddNumberToObject(json, "timestamp_ms",
                                (double)entry->timestamp_ms) == NULL ||
        cJSON_AddNumberToObject(json, "event_id", event->event_id) == NULL ||
        cJSON_AddNumberToObject(json, "category_id", event->category_id) == NULL ||
        cJSON_AddStringToObject(json, "class", class_name(event->cls)) == NULL ||
        cJSON_AddStringToObject(json, "event", paradox_event_name(event->event_id)) == NULL) {
        cJSON_Delete(json);
        return NULL;
    }
    return json;
}

static bool query_value(const char *query, const char *key,
                        char *value, size_t value_size,
                        bool *present)
{
    esp_err_t err = httpd_query_key_value(query, key, value, value_size);
    if (err == ESP_ERR_NOT_FOUND) {
        *present = false;
        return true;
    }
    *present = err == ESP_OK;
    return err == ESP_OK;
}

static esp_err_t events_get_handler(httpd_req_t *request)
{
    if (!request_is_authorized(request)) {
        return ESP_OK;
    }
    char query[128] = {0};
    size_t query_length = httpd_req_get_url_query_len(request);
    if (query_length >= sizeof(query) ||
        (query_length != 0 &&
         httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK)) {
        return send_error(request, "400 Bad Request", "Invalid query");
    }
    char value[32];
    bool present;
    bool class_filter_set = false;
    paradox_class_t class_filter = PARADOX_CLS_UNKNOWN;
    if (!query_value(query, "class", value, sizeof(value), &present)) {
        return send_error(request, "400 Bad Request", "Invalid class filter");
    }
    if (present) {
        if (!parse_event_class(value, &class_filter)) {
            return send_error(request, "400 Bad Request", "Invalid class filter");
        }
        class_filter_set = true;
    }
    int zone_filter = 0;
    if (!query_value(query, "zone", value, sizeof(value), &present)) {
        return send_error(request, "400 Bad Request", "Invalid zone filter");
    }
    if (present) {
        char *end = NULL;
        long parsed = strtol(value, &end, 10);
        if (end == value || *end != '\0' || parsed < 1 ||
            parsed > PARADOX_MAX_ZONES) {
            return send_error(request, "400 Bad Request", "Invalid zone filter");
        }
        zone_filter = (int)parsed;
    }
    size_t limit = PARADOX_EVENT_LOG_CAPACITY;
    if (!query_value(query, "limit", value, sizeof(value), &present)) {
        return send_error(request, "400 Bad Request", "Invalid event limit");
    }
    if (present) {
        char *end = NULL;
        long parsed = strtol(value, &end, 10);
        if (end == value || *end != '\0' || parsed < 1 ||
            parsed > PARADOX_EVENT_LOG_CAPACITY) {
            return send_error(request, "400 Bad Request", "Invalid event limit");
        }
        limit = (size_t)parsed;
    }

    /* The whole log goes on the heap, never on the stack: capacity * 24 bytes is
     * ~1.5 KiB on top of everything else this task already uses, and that is
     * what overflowed the httpd task stack while serving /api/events. The heap
     * also turns a failed allocation into a clean 500 instead of a reboot.
     *
     * The snapshot still takes the full log even when a filter or a small limit
     * is in play, because "limit" counts matching events, not raw log slots:
     * stopping at `limit` newest entries would drop older events that match. */
    paradox_event_log_entry_t *entries = malloc(
        sizeof(*entries) * PARADOX_EVENT_LOG_CAPACITY);
    if (entries == NULL) {
        return send_error(request, "500 Internal Server Error",
                          "Response allocation failed");
    }
    size_t count = paradox_event_log_snapshot(entries,
                                              PARADOX_EVENT_LOG_CAPACITY);
    cJSON *root = cJSON_CreateObject();
    cJSON *events = cJSON_CreateArray();
    if (root == NULL || events == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(events);
        free(entries);
        return send_error(request, "500 Internal Server Error",
                          "Response allocation failed");
    }
    size_t added = 0;
    bool alloc_failed = false;
    for (size_t i = 0; i < count && added < limit; i++) {
        const paradox_event_t *event = &entries[i].event;
        if (!paradox_event_log_matches(event, class_filter_set, class_filter,
                                      zone_filter != 0,
                                      (unsigned)zone_filter)) {
            continue;
        }
        cJSON *item = event_json(&entries[i]);
        if (item == NULL) {
            alloc_failed = true;
            break;
        }
        if (!cJSON_AddItemToArray(events, item)) {
            cJSON_Delete(item);
            alloc_failed = true;
            break;
        }
        added++;
    }
    if (!alloc_failed &&
        (cJSON_AddNumberToObject(root, "count", (double)added) == NULL ||
         !cJSON_AddItemToObject(root, "events", events))) {
        alloc_failed = true;
    }
    free(entries);
    if (alloc_failed) {
        /* Neither root nor events is attached to anything yet at this point. */
        cJSON_Delete(root);
        cJSON_Delete(events);
        return send_error(request, "500 Internal Server Error",
                          "Response allocation failed");
    }
    return send_json_object(request, root);
}

static esp_err_t events_clear_handler(httpd_req_t *request)
{
    if (!request_is_authorized(request)) {
        return ESP_OK;
    }
    paradox_event_log_clear();
    web_audit(request, "200", "panel event log cleared");
    return send_json(request, "{\"ok\":true}");
}

static esp_err_t debug_set_handler(httpd_req_t *request)
{
    if (!request_is_authorized(request)) {
        return ESP_OK;
    }
    cJSON *json = NULL;
    if (receive_json(request, &json) != ESP_OK) {
        return ESP_OK;
    }
    cJSON *enabled = cJSON_GetObjectItemCaseSensitive(json, "enabled");
    if (json->child == NULL || json->child->next != NULL ||
        enabled == NULL || !cJSON_IsBool(enabled)) {
        delete_json_strings(json);
        return send_error(request, "400 Bad Request",
                          "Expected only an enabled boolean");
    }
    if (!config_lock()) {
        delete_json_strings(json);
        return send_error(request, "500 Internal Server Error",
                          "Configuration unavailable");
    }
    paradox_config_t candidate = *s_config;
    paradox_config_result_t result = paradox_config_set_panel_debug(
        &candidate, cJSON_IsTrue(enabled));
    delete_json_strings(json);
    if (result != PARADOX_CONFIG_OK) {
        config_unlock();
        return send_error(request, "400 Bad Request", "Invalid debug setting");
    }
    esp_err_t err = paradox_config_store_save(&candidate);
    if (err == ESP_OK) {
        *s_config = candidate;
        /* The raw UART tap follows the same switch: nothing is recorded while
         * panel debug is off, and turning it on starts from an empty ring. */
        if (candidate.panel_debug_enabled) {
            paradox_raw_tap_clear();
        }
        paradox_raw_tap_set_enabled(candidate.panel_debug_enabled);
    }
    config_unlock();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot save panel debug setting: %s",
                 esp_err_to_name(err));
        return send_error(request, "500 Internal Server Error",
                          "Debug setting could not be saved");
    }
    web_audit(request, "200", candidate.panel_debug_enabled
                                      ? "panel debug enabled"
                                      : "panel debug disabled");
    return send_json(request, "{\"ok\":true}");
}

static esp_err_t debug_simulator_status_handler(httpd_req_t *request)
{
    if (!request_is_authorized(request)) {
        return ESP_OK;
    }

    char body[80];
    int length = snprintf(body, sizeof(body),
                          "{\"supported\":%s,\"active\":%s}",
                          paradox1738_simulator_supported() ? "true" : "false",
                          paradox1738_simulator_active() ? "true" : "false");
    if (length < 0 || (size_t)length >= sizeof(body)) {
        return send_error(request, "500 Internal Server Error",
                          "Simulator status unavailable");
    }
    return send_json(request, body);
}

static esp_err_t debug_simulator_start_handler(httpd_req_t *request)
{
    if (!request_is_authorized(request)) {
        return ESP_OK;
    }
    if (!paradox1738_simulator_supported()) {
        return send_error(request, "501 Not Implemented",
                          "Continuous simulator build is already enabled");
    }
    if (!paradox1738_simulator_start()) {
        return send_error(request, "409 Conflict",
                          "Simulator test is already running");
    }
    web_audit(request, "202", "panel simulator replay started");
    httpd_resp_set_status(request, "202 Accepted");
    return send_json(request, "{\"ok\":true,\"active\":true}");
}

static esp_err_t debug_panel_handler(httpd_req_t *request)
{
    if (!request_is_authorized(request)) {
        return ESP_OK;
    }
    if (!config_lock()) {
        return send_error(request, "500 Internal Server Error",
                          "Configuration unavailable");
    }
    bool enabled = s_config->panel_debug_enabled;
    config_unlock();
    if (!enabled) {
        return send_error(request, "404 Not Found", "Panel debug is disabled");
    }

    paradox_stats_t stats;
    paradox1738_stats(&stats);
    paradox_event_log_entry_t entries[16];
    size_t count = paradox_event_log_snapshot(entries, 16);
    cJSON *root = cJSON_CreateObject();
    cJSON *frames = cJSON_CreateArray();
    cJSON *counters = cJSON_CreateObject();
    if (root == NULL || frames == NULL || counters == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(frames);
        cJSON_Delete(counters);
        return send_error(request, "500 Internal Server Error",
                          "Response allocation failed");
    }
    if (cJSON_AddNumberToObject(counters, "frames_ok", stats.frames_ok) == NULL ||
        cJSON_AddNumberToObject(counters, "framing_errors",
                                stats.framing_errors) == NULL ||
        cJSON_AddNumberToObject(counters, "bytes_discarded",
                                stats.bytes_discarded) == NULL ||
        cJSON_AddNumberToObject(counters, "overruns", stats.overruns) == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(frames);
        cJSON_Delete(counters);
        return send_error(request, "500 Internal Server Error",
                          "Response allocation failed");
    }
    for (size_t i = 0; i < count; i++) {
        const paradox_event_t *event = &entries[i].event;
        char raw[12];
        (void)snprintf(raw, sizeof(raw), "%02x%02x%02x%02x",
                       event->raw_frame[0], event->raw_frame[1],
                       event->raw_frame[2], event->raw_frame[3]);
        cJSON *frame = cJSON_CreateObject();
        if (frame == NULL ||
            cJSON_AddStringToObject(frame, "raw", raw) == NULL ||
            cJSON_AddNumberToObject(frame, "event_id", event->event_id) == NULL ||
            cJSON_AddNumberToObject(frame, "category_id",
                                    event->category_id) == NULL ||
            cJSON_AddStringToObject(frame, "class",
                                    class_name(event->cls)) == NULL ||
            cJSON_AddStringToObject(frame, "event",
                                    paradox_event_name(event->event_id)) == NULL ||
            cJSON_AddNumberToObject(frame, "timestamp_ms",
                                    (double)entries[i].timestamp_ms) == NULL) {
            cJSON_Delete(frame);
            cJSON_Delete(root);
            cJSON_Delete(frames);
            cJSON_Delete(counters);
            return send_error(request, "500 Internal Server Error",
                              "Response allocation failed");
        }
        if (!cJSON_AddItemToArray(frames, frame)) {
            cJSON_Delete(frame);
            cJSON_Delete(root);
            cJSON_Delete(frames);
            cJSON_Delete(counters);
            return send_error(request, "500 Internal Server Error",
                              "Response allocation failed");
        }
    }
    if (!cJSON_AddItemToObject(root, "counters", counters)) {
        cJSON_Delete(root);
        cJSON_Delete(counters);
        cJSON_Delete(frames);
        return send_error(request, "500 Internal Server Error",
                          "Response allocation failed");
    }
    if (!cJSON_AddItemToObject(root, "frames", frames)) {
        cJSON_Delete(root);
        cJSON_Delete(frames);
        return send_error(request, "500 Internal Server Error",
                          "Response allocation failed");
    }
    return send_json_object(request, root);
}

static bool panel_debug_enabled(void)
{
    if (!config_lock()) {
        return false;
    }
    bool enabled = s_config->panel_debug_enabled;
    config_unlock();
    return enabled;
}

/* Hex-encode raw panel bytes. The panel buffer is never NUL-terminated, so it
 * must never reach a %s conversion; fixed-width hex keeps that rule intact. */
static char *raw_bytes_hex(const uint8_t *bytes, size_t len)
{
    char *hex = malloc(len * 2u + 1u);
    if (hex == NULL) {
        return NULL;
    }
    hex[0] = '\0';
    for (size_t i = 0; i < len; i++) {
        (void)snprintf(hex + i * 2u, 3u, "%02x", bytes[i]);
    }
    return hex;
}

void web_server_set_homekit_dump(web_homekit_dump_fn_t dumper)
{
    s_homekit_dump = dumper;
}

/* T6.5: what the accessory database advertises, for comparison with /api/status.
 * Gated on panel debug like the other debug views, and authenticated like
 * everything else. The payload is malloc'd by the bridge: the web task's stack has
 * roughly 4.5 KiB spare and the JSON is near 8 KiB, which is the lesson the
 * /api/events stack overflow taught. */
static esp_err_t debug_homekit_handler(httpd_req_t *request)
{
    if (!request_is_authorized(request)) {
        return ESP_OK;
    }
    if (!panel_debug_enabled()) {
        return send_error(request, "404 Not Found", "Panel debug is disabled");
    }
    web_homekit_dump_fn_t dumper = s_homekit_dump;
    if (!dumper) {
        return send_error(request, "503 Service Unavailable",
                          "HomeKit bridge has not finished registering accessories");
    }
    char *json = dumper();
    if (!json) {
        return send_error(request, "503 Service Unavailable",
                          "Could not serialise the accessory database");
    }
    httpd_resp_set_type(request, "application/json");
    esp_err_t result = httpd_resp_sendstr(request, json);
    free(json);
    return result;
}

static esp_err_t debug_raw_handler(httpd_req_t *request)
{
    if (!request_is_authorized(request)) {
        return ESP_OK;
    }
    if (!panel_debug_enabled()) {
        return send_error(request, "404 Not Found", "Panel debug is disabled");
    }

    uint64_t bytes = 256u;
    char query[64] = {0};
    size_t query_length = httpd_req_get_url_query_len(request);
    if (query_length >= sizeof(query) ||
        (query_length != 0 &&
         httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK)) {
        return send_error(request, "400 Bad Request", "Invalid query");
    }
    char value[16];
    bool present = false;
    if (!query_value(query, "bytes", value, sizeof(value), &present)) {
        return send_error(request, "400 Bad Request",
                          "Invalid bytes parameter");
    }
    if (present) {
        char *end = NULL;
        unsigned long parsed = strtoul(value, &end, 10);
        if (end == value || (end != NULL && *end != '\0')) {
            return send_error(request, "400 Bad Request",
                              "bytes must be a number");
        }
        bytes = parsed;
    }
    if (bytes > PARADOX_RAW_TAP_CAPACITY) {
        bytes = PARADOX_RAW_TAP_CAPACITY;
    }

    paradox_raw_tap_stats_t stats;
    paradox_raw_tap_stats(&stats);
    portENTER_CRITICAL(&s_session_mux);
    int status_clients = s_status_stream_clients;
    int raw_clients = s_raw_stream_clients;
    portEXIT_CRITICAL(&s_session_mux);
    /* The snapshot is the newest `bytes` still in the ring, so a client can poll
     * it without an open stream. */
    size_t count = bytes > stats.bytes_retained ? (size_t)stats.bytes_retained
                                                : (size_t)bytes;
    uint8_t *buffer = malloc(count == 0 ? 1u : count);
    if (buffer == NULL) {
        return send_error(request, "500 Internal Server Error",
                          "Response allocation failed");
    }
    paradox_raw_tap_cursor_t cursor = {.seq = 0, .dropped = 0};
    paradox_raw_tap_seek(&cursor, count);
    count = paradox_raw_tap_read(buffer, count, &cursor);
    char *hex = raw_bytes_hex(buffer, count);
    free(buffer);
    if (hex == NULL) {
        return send_error(request, "500 Internal Server Error",
                          "Response allocation failed");
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL ||
        cJSON_AddNumberToObject(root, "capacity", stats.capacity) == NULL ||
        cJSON_AddNumberToObject(root, "bytes_recorded",
                                stats.bytes_recorded) == NULL ||
        cJSON_AddNumberToObject(root, "bytes_lost", stats.bytes_lost) == NULL ||
        cJSON_AddNumberToObject(root, "bytes_retained",
                                stats.bytes_retained) == NULL ||
        cJSON_AddNumberToObject(root, "first_seq", cursor.seq - count) == NULL ||
        cJSON_AddNumberToObject(root, "status_stream_clients",
                                status_clients) == NULL ||
        cJSON_AddNumberToObject(root, "raw_stream_clients", raw_clients) == NULL ||
        cJSON_AddStringToObject(root, "hex", hex) == NULL) {
        free(hex);
        cJSON_Delete(root);
        return send_error(request, "500 Internal Server Error",
                          "Response allocation failed");
    }
    free(hex);
    return send_json_object(request, root);
}

typedef struct {
    httpd_req_t *request;
    char session_token[sizeof(s_session_token)];
    uint64_t session_expires;
} raw_stream_task_arg_t;

static void raw_stream_task(void *argument)
{
    raw_stream_task_arg_t *task_arg = argument;
    httpd_req_t *request = task_arg->request;
    char session_token[sizeof(task_arg->session_token)];
    memcpy(session_token, task_arg->session_token, sizeof(session_token));
    uint64_t session_expires = task_arg->session_expires;
    free(task_arg);

    if (httpd_resp_set_type(request, "text/event-stream") != ESP_OK ||
        httpd_resp_set_hdr(request, "Cache-Control", "no-cache") != ESP_OK ||
        httpd_resp_set_hdr(request, "X-Accel-Buffering", "no") != ESP_OK) {
        ESP_LOGW(TAG, "Cannot initialize raw stream response");
        mbedtls_platform_zeroize(session_token, sizeof(session_token));
        (void)httpd_req_async_handler_complete(request);
        vTaskDelete(NULL);
        return;
    }

    /* Start at the live edge: a client that wants history uses the snapshot
     * endpoint, and starting there keeps the stream from replaying whatever the
     * ring happens to still hold from before the connection. */
    paradox_raw_tap_cursor_t cursor = {.seq = 0, .dropped = 0};
    paradox_raw_tap_seek(&cursor, 0);

    enum {
        RAW_STREAM_CHUNK = 64u,
    };
    uint8_t bytes[RAW_STREAM_CHUNK];
    char *chunk = malloc(RAW_STREAM_CHUNK * 2u + 96u);
    int64_t last_heartbeat = esp_timer_get_time();
    stream_clients_add(&s_raw_stream_clients, 1);
    if (chunk != NULL) {
        while (s_server_running &&
               session_is_current(session_token, session_expires)) {
            size_t count = paradox_raw_tap_read(bytes, sizeof(bytes), &cursor);
            int64_t now = esp_timer_get_time();
            if (count > 0) {
                char *hex = raw_bytes_hex(bytes, count);
                if (hex == NULL) {
                    break;
                }
                int length = snprintf(chunk, RAW_STREAM_CHUNK * 2u + 96u,
                                      "event: raw\ndata: {\"seq\":%" PRIu64
                                      ",\"dropped\":%" PRIu64 ",\"hex\":\"%s\"}"
                                      "\n\n",
                                      cursor.seq - count, cursor.dropped, hex);
                free(hex);
                if (length < 0 ||
                    (size_t)length >= RAW_STREAM_CHUNK * 2u + 96u ||
                    httpd_resp_send_chunk(request, chunk, (size_t)length) !=
                        ESP_OK) {
                    break;
                }
                cursor.dropped = 0;
                last_heartbeat = now;
                continue;
            }
            if (!panel_debug_enabled()) {
                static const char notice[] =
                    "event: error\ndata: panel debug disabled\n\n";
                (void)httpd_resp_send_chunk(request, notice,
                                            sizeof(notice) - 1u);
                break;
            }
            if (now - last_heartbeat >= 15000000LL) {
                static const char heartbeat[] = ": keepalive\n\n";
                if (httpd_resp_send_chunk(request, heartbeat,
                                          sizeof(heartbeat) - 1u) != ESP_OK) {
                    break;
                }
                last_heartbeat = now;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    free(chunk);
    mbedtls_platform_zeroize(session_token, sizeof(session_token));
    stream_clients_add(&s_raw_stream_clients, -1);
    (void)httpd_req_async_handler_complete(request);
    vTaskDelete(NULL);
}

static esp_err_t debug_raw_stream_handler(httpd_req_t *request)
{
    if (!request_is_authorized(request)) {
        return ESP_OK;
    }
    if (!panel_debug_enabled()) {
        return send_error(request, "404 Not Found", "Panel debug is disabled");
    }
    httpd_req_t *async_request = NULL;
    esp_err_t err = httpd_req_async_handler_begin(request, &async_request);
    if (err != ESP_OK) {
        return send_error(request, "503 Service Unavailable",
                          "Raw stream unavailable");
    }
    raw_stream_task_arg_t *argument = malloc(sizeof(*argument));
    if (argument == NULL) {
        (void)send_error(async_request, "503 Service Unavailable",
                         "Raw stream unavailable");
        (void)httpd_req_async_handler_complete(async_request);
        return ESP_OK;
    }
    argument->request = async_request;
    portENTER_CRITICAL(&s_session_mux);
    memcpy(argument->session_token, s_session_token,
           sizeof(argument->session_token));
    argument->session_expires = s_session_expires;
    portEXIT_CRITICAL(&s_session_mux);
    if (xTaskCreate(raw_stream_task, "raw_stream", 4096, argument, 4, NULL) !=
        pdPASS) {
        mbedtls_platform_zeroize(argument->session_token,
                                 sizeof(argument->session_token));
        free(argument);
        (void)send_error(async_request, "503 Service Unavailable",
                         "Raw stream unavailable");
        (void)httpd_req_async_handler_complete(async_request);
        return ESP_OK;
    }
    return ESP_OK;
}

typedef struct {
    httpd_req_t *request;
    char session_token[sizeof(s_session_token)];
    uint64_t session_expires;
} status_stream_task_arg_t;

static void status_stream_task(void *argument)
{
    status_stream_task_arg_t *task_arg = argument;
    httpd_req_t *request = task_arg->request;
    char session_token[sizeof(task_arg->session_token)];
    memcpy(session_token, task_arg->session_token, sizeof(session_token));
    uint64_t session_expires = task_arg->session_expires;
    free(task_arg);
    if (httpd_resp_set_type(request, "text/event-stream") != ESP_OK ||
        httpd_resp_set_hdr(request, "Cache-Control", "no-cache") != ESP_OK ||
        httpd_resp_set_hdr(request, "X-Accel-Buffering", "no") != ESP_OK) {
        ESP_LOGW(TAG, "Cannot initialize status stream response");
        mbedtls_platform_zeroize(session_token, sizeof(session_token));
        (void)httpd_req_async_handler_complete(request);
        vTaskDelete(NULL);
        return;
    }

    uint64_t last_generation = UINT64_MAX;
    int64_t last_heartbeat = esp_timer_get_time();
    stream_clients_add(&s_status_stream_clients, 1);
    while (s_server_running &&
           session_is_current(session_token, session_expires)) {
        paradox_state_snapshot_t snapshot;
        if (paradox_state_snapshot(&snapshot) != PARADOX_STATE_OK) {
            ESP_LOGE(TAG, "Cannot snapshot state for status stream");
            static const char error[] =
                "event: error\ndata: status unavailable\n\n";
            (void)httpd_resp_send_chunk(request, error, sizeof(error) - 1u);
            break;
        }
        int64_t now = esp_timer_get_time();
        if (snapshot.generation != last_generation) {
            cJSON *json = state_snapshot_json(&snapshot);
            char *text = json == NULL ? NULL : cJSON_PrintUnformatted(json);
            cJSON_Delete(json);
            if (text == NULL) {
                ESP_LOGE(TAG, "Cannot allocate status stream event");
                static const char error[] =
                    "event: error\ndata: status unavailable\n\n";
                (void)httpd_resp_send_chunk(request, error, sizeof(error) - 1u);
                break;
            }
            size_t needed = strlen(text) + sizeof("event: status\ndata: \n\n");
            char *chunk = malloc(needed);
            if (chunk == NULL) {
                free(text);
                ESP_LOGE(TAG, "Cannot allocate status stream chunk");
                static const char error[] =
                    "event: error\ndata: status unavailable\n\n";
                (void)httpd_resp_send_chunk(request, error, sizeof(error) - 1u);
                break;
            }
            int length = snprintf(chunk, needed, "event: status\ndata: %s\n\n",
                                  text);
            free(text);
            esp_err_t send_err = length < 0 || (size_t)length >= needed ?
                ESP_ERR_INVALID_SIZE :
                httpd_resp_send_chunk(request, chunk, (size_t)length);
            free(chunk);
            if (send_err != ESP_OK) {
                break;
            }
            last_generation = snapshot.generation;
            last_heartbeat = now;
        } else if (now - last_heartbeat >= 15000000LL) {
            static const char heartbeat[] = ": keepalive\n\n";
            if (httpd_resp_send_chunk(request, heartbeat,
                                     sizeof(heartbeat) - 1u) != ESP_OK) {
                break;
            }
            last_heartbeat = now;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    mbedtls_platform_zeroize(session_token, sizeof(session_token));
    stream_clients_add(&s_status_stream_clients, -1);
    (void)httpd_req_async_handler_complete(request);
    vTaskDelete(NULL);
}

static esp_err_t status_stream_handler(httpd_req_t *request)
{
    if (!request_is_authorized(request)) {
        return ESP_OK;
    }
    httpd_req_t *async_request = NULL;
    esp_err_t err = httpd_req_async_handler_begin(request, &async_request);
    if (err != ESP_OK) {
        return send_error(request, "503 Service Unavailable",
                          "Status stream unavailable");
    }
    status_stream_task_arg_t *argument = malloc(sizeof(*argument));
    if (argument == NULL) {
        (void)send_error(async_request, "503 Service Unavailable",
                         "Status stream unavailable");
        (void)httpd_req_async_handler_complete(async_request);
        return ESP_OK;
    }
    argument->request = async_request;
    portENTER_CRITICAL(&s_session_mux);
    memcpy(argument->session_token, s_session_token,
           sizeof(argument->session_token));
    argument->session_expires = s_session_expires;
    portEXIT_CRITICAL(&s_session_mux);
    if (xTaskCreate(status_stream_task, "status_stream", 4096, argument, 4,
                    NULL) != pdPASS) {
        mbedtls_platform_zeroize(argument->session_token,
                                 sizeof(argument->session_token));
        free(argument);
        (void)send_error(async_request, "503 Service Unavailable",
                         "Status stream unavailable");
        (void)httpd_req_async_handler_complete(async_request);
        return ESP_OK;
    }
    return ESP_OK;
}

static esp_err_t index_handler(httpd_req_t *request)
{
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(request, "Referrer-Policy", "no-referrer");
    httpd_resp_set_hdr(request, "Content-Security-Policy",
                       "default-src 'none'; script-src 'unsafe-inline'; "
                       "style-src 'unsafe-inline'; connect-src 'self'; "
                       "form-action 'self'; base-uri 'none'");
    return httpd_resp_sendstr(request, INDEX_HTML);
}

static esp_err_t health_handler(httpd_req_t *request)
{
    return send_json(request, "{\"ok\":true}");
}

static esp_err_t system_info_handler(httpd_req_t *request)
{
    if (!request_is_authorized(request)) {
        return ESP_OK;
    }

    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);
    wifi_ap_record_t access_point;
    bool wifi_connected = esp_wifi_sta_get_ap_info(&access_point) == ESP_OK;
    bool debug_enabled = false;
    if (!config_lock()) {
        return send_error(request, "500 Internal Server Error",
                          "Configuration unavailable");
    }
    debug_enabled = s_config->panel_debug_enabled;
    config_unlock();

    /* Memory headroom, for the 24 h soak (T8.4) and for confirming that the web
     * task still has stack to spare (T8.2). This handler runs on the web
     * server's own task, so uxTaskGetStackHighWaterMark(NULL) is that task's
     * smallest free stack since boot - the quantity that was blown through when
     * /api/events kept its event-log snapshot on the stack. The heap figures
     * cover internal RAM only, which is what HomeKit, the web server and the
     * panel tasks allocate from; "min" is the low-water mark since boot. */
    uint64_t uptime_ms = (uint64_t)(esp_timer_get_time() / 1000);
    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t min_free_heap = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    size_t largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    UBaseType_t stack_words = uxTaskGetStackHighWaterMark(NULL);

    cJSON *root = cJSON_CreateObject();
    if (root == NULL ||
        cJSON_AddStringToObject(root, "version", app->version) == NULL ||
        cJSON_AddStringToObject(root, "project", app->project_name) == NULL ||
        cJSON_AddStringToObject(root, "partition",
                                running == NULL ? "unknown" : running->label) == NULL ||
        cJSON_AddNumberToObject(root, "update_partition_bytes",
                                update == NULL ? 0 : (double)update->size) == NULL ||
        cJSON_AddBoolToObject(root, "wifi_connected", wifi_connected) == NULL ||
        cJSON_AddBoolToObject(root, "panel_debug_enabled", debug_enabled) == NULL ||
        cJSON_AddNumberToObject(root, "uptime_ms", (double)uptime_ms) == NULL ||
        cJSON_AddNumberToObject(root, "free_heap_bytes",
                                (double)free_heap) == NULL ||
        cJSON_AddNumberToObject(root, "min_free_heap_bytes",
                                (double)min_free_heap) == NULL ||
        cJSON_AddNumberToObject(root, "largest_heap_block_bytes",
                                (double)largest_block) == NULL ||
        cJSON_AddNumberToObject(root, "web_stack_free_bytes",
                                (double)stack_words * sizeof(StackType_t)) == NULL) {
        cJSON_Delete(root);
        return send_error(request, "500 Internal Server Error",
                          "Response allocation failed");
    }
    return send_json_object(request, root);
}

static esp_err_t ota_upload_handler(httpd_req_t *request)
{
    if (!request_is_authorized(request)) {
        return ESP_OK;
    }
    if (request->content_len == 0) {
        return send_error(request, "400 Bad Request", "Firmware image is empty");
    }

    char content_type[64];
    if (httpd_req_get_hdr_value_str(request, "Content-Type", content_type,
                                    sizeof(content_type)) != ESP_OK ||
        strcmp(content_type, "application/octet-stream") != 0) {
        return send_error(request, "415 Unsupported Media Type",
                          "Expected application/octet-stream");
    }

    const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);
    if (update == NULL) {
        return send_error(request, "503 Service Unavailable",
                          "No OTA update partition is available");
    }
    if (request->content_len > update->size) {
        return send_error(request, "413 Payload Too Large",
                          "Firmware image exceeds the update partition");
    }

    esp_ota_handle_t ota_handle = 0;
    esp_err_t err = esp_ota_begin(update, request->content_len, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot begin OTA update: %s", esp_err_to_name(err));
        return send_error(request, "500 Internal Server Error",
                          "Cannot start firmware update");
    }

    uint8_t *buffer = malloc(1024);
    if (buffer == NULL) {
        (void)esp_ota_abort(ota_handle);
        return send_error(request, "503 Service Unavailable",
                          "Insufficient memory for firmware upload");
    }
    size_t remaining = request->content_len;
    while (remaining > 0) {
        size_t chunk_size = remaining < 1024u ? remaining : 1024u;
        int received = httpd_req_recv(request, (char *)buffer, chunk_size);
        if (received <= 0) {
            free(buffer);
            (void)esp_ota_abort(ota_handle);
            ESP_LOGW(TAG, "OTA upload ended before the complete image arrived");
            return send_error(request, "400 Bad Request",
                              "Firmware image upload was incomplete");
        }
        err = esp_ota_write(ota_handle, buffer, (size_t)received);
        if (err != ESP_OK) {
            free(buffer);
            (void)esp_ota_abort(ota_handle);
            ESP_LOGE(TAG, "Cannot write OTA image: %s", esp_err_to_name(err));
            return send_error(request, "500 Internal Server Error",
                              "Firmware image write failed");
        }
        remaining -= (size_t)received;
    }
    free(buffer);

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "OTA image validation failed: %s", esp_err_to_name(err));
        return send_error(request, "400 Bad Request",
                          "Firmware image validation failed");
    }
    esp_app_desc_t image_description;
    err = esp_ota_get_partition_description(update, &image_description);
    if (err != ESP_OK ||
        strcmp(image_description.project_name,
               esp_app_get_description()->project_name) != 0) {
        ESP_LOGW(TAG, "OTA image project mismatch or unreadable description");
        return send_error(request, "400 Bad Request",
                          "Firmware image is not for this device");
    }

    err = esp_ota_set_boot_partition(update);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot select OTA image: %s", esp_err_to_name(err));
        return send_error(request, "500 Internal Server Error",
                          "Firmware image could not be selected");
    }
    /* The image has been written to the inactive slot and validated; from here the
     * module reboots into it, so this is the record of the decision. */
    web_audit(request, "202", "firmware image accepted");
    esp_err_t response_err = httpd_resp_set_status(request, "202 Accepted");
    if (response_err == ESP_OK) {
        response_err = send_json(request, "{\"ok\":true,\"restarting\":true}");
    }
    if (response_err != ESP_OK) {
        ESP_LOGW(TAG, "OTA image selected but restart response failed: %s",
                 esp_err_to_name(response_err));
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return response_err;
}

/* Restart the process so a zone rename or type change reaches HomeKit.
 *
 * There is no in-place alternative, and that is the SDK's own statement rather than
 * an assumption: hap_stop() and hap_deinit() are both documented "This currently does
 * not do anything", hap_service_add_characteristic() says characteristics may be added
 * only "before hap_start()", and this bridge builds its accessories once in
 * hub_task_entry() before hap_start(). Nothing in the vendored esp-hap frees or
 * replaces an accessory, and silently reusing an AID with a different service type
 * would corrupt what paired controllers already believe. So rebuilding the accessory
 * database means starting the process again, which is what the pending banner has
 * always told a person to do - through an action they could not take.
 *
 * The pairing survives: keys live in NVS, and every OTA in this module's log has
 * rebooted it with the pairing intact (mDNS keeps answering _hap._tcp with sf=0).
 *
 * The confirmation body is not ceremony. This drops every HomeKit controller's
 * session for several seconds, which the house experiences as sensors briefly
 * answering "no response". A single misdirected click should not be able to cause
 * that, so the caller has to say so explicitly. */
static esp_err_t homekit_restart_handler(httpd_req_t *request)
{
    if (!request_is_authorized(request)) {
        return ESP_OK;
    }
    cJSON *json = NULL;
    if (receive_json(request, &json) != ESP_OK) {
        return ESP_OK;
    }
    bool confirmed = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "confirm"));
    bool pending = s_homekit_pending;
    delete_json_strings(json);
    if (!confirmed) {
        ESP_LOGI(TAG, "HomeKit bridge restart refused: confirm was not set true "
                      "(pending changes: %s)", pending ? "yes" : "no");
        return send_error(request, "400 Bad Request",
                          "Body must set confirm true; the restart drops every "
                          "HomeKit session briefly");
    }
    /* Logged before the restart rather than after, because the boot log of the new
     * process has no way to say who asked or why. */
    ESP_LOGW(TAG, "Restarting for HomeKit accessory changes (pending: %s)",
             pending ? "yes" : "no");
    char body[96];
    int length = snprintf(body, sizeof(body),
                          "{\"ok\":true,\"restarting\":true,\"pending\":%s}",
                          pending ? "true" : "false");
    if (length < 0 || (size_t)length >= sizeof(body)) {
        return send_error(request, "500 Internal Server Error",
                          "Restart status unavailable");
    }
    web_audit(request, "202", "bridge restart confirmed");
    esp_err_t response_err = httpd_resp_set_status(request, "202 Accepted");
    if (response_err == ESP_OK) {
        response_err = send_json(request, body);
    }
    if (response_err != ESP_OK) {
        ESP_LOGW(TAG, "Restart response failed before HomeKit bridge restart: %s",
                 esp_err_to_name(response_err));
    }
    /* Same grace the OTA path uses, and for the same reason: esp_restart() from inside
     * a request handler kills the socket, so the response has to be on the wire and the
     * client has to have had a chance to read it. */
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return response_err;
}

/* The bridge's own record of what it was asked to do, newest first. Read-only and
 * authenticated: the trail names who was refused, which is exactly what should not
 * be readable by a stranger. It is deliberately not affected by the panel-debug
 * switch, because the reason someone wants it is usually that something unexpected
 * already happened. */
#define WEB_AUDIT_JSON_BYTES 4096u

/* esp_http_server answers an unmatched URI or method by itself, so those refusals
 * never reach send_error() and would be invisible in the trail. The asymmetry is
 * worth closing on one side only: a POST to a route that does not exist yet is
 * precisely the attempt a control-plane audit has to show (an arm command aimed at
 * a bridge that refuses to have one), and a request using the wrong verb against a
 * route that does exist is a probe rather than a mistake. A GET that hits nothing
 * is not recorded, because a browser typo, a favicon request or a scan would push
 * real records out of a 16-deep ring. */
static esp_err_t unmatched_route_handler(httpd_req_t *request, httpd_err_code_t error)
{
    const bool wrong_method = (error == HTTPD_405_METHOD_NOT_ALLOWED);
    const bool read_only = (request->method == HTTP_GET ||
                            request->method == HTTP_HEAD);
    if (wrong_method || !read_only) {
        web_audit(request, wrong_method ? "405" : "404", "no matching route");
    }
    httpd_resp_set_hdr(request, "Connection", "close");
    httpd_resp_set_status(request, wrong_method ? "405 Method Not Allowed"
                                                : "404 Not Found");
    return send_json(request, "{\"error\":\"No such route\"}");
}

static esp_err_t audit_get_handler(httpd_req_t *request)
{
    if (!request_is_authorized(request)) {
        return ESP_OK;
    }
    char *body = malloc(WEB_AUDIT_JSON_BYTES);
    if (body == NULL) {
        return send_error(request, "503 Service Unavailable",
                          "No memory for the audit trail");
    }
    const size_t length = paradox_audit_json(body, WEB_AUDIT_JSON_BYTES);
    if (length == 0u) {
        free(body);
        return send_error(request, "503 Service Unavailable",
                          "Audit trail does not fit the response buffer");
    }
    const esp_err_t err = send_json(request, body);
    free(body);
    return err;
}

static esp_err_t auth_status_handler(httpd_req_t *request)
{
    char body[96];
    int length = snprintf(body, sizeof(body),
                          "{\"password_change_required\":%s,\"authenticated\":%s}",
                          s_config->web_auth_password_set ? "false" : "true",
                          request_has_session(request) ? "true" : "false");
    if (length < 0 || (size_t)length >= sizeof(body)) {
        return send_error(request, "500 Internal Server Error",
                          "Authentication status unavailable");
    }
    return send_json(request, body);
}

static esp_err_t login_handler(httpd_req_t *request)
{
    if (!request_is_same_origin(request)) {
        return send_error(request, "403 Forbidden", "Same-origin request required");
    }
    uint64_t now = uptime_seconds();
    if (now < s_login_blocked_until) {
        return send_error(request, "429 Too Many Requests", "Try again later");
    }

    cJSON *json = NULL;
    if (receive_json(request, &json) != ESP_OK) {
        return ESP_OK;
    }
    cJSON *password_item = cJSON_GetObjectItemCaseSensitive(json, "password");
    size_t password_size = 0;
    if (!cJSON_IsString(password_item) ||
        !valid_password(password_item->valuestring, &password_size)) {
        delete_json_strings(json);
        s_login_failures++;
        if (s_login_failures >= WEB_LOGIN_MAX_FAILURES) {
            s_login_blocked_until = now + WEB_LOGIN_LOCK_SECONDS;
            s_login_failures = 0;
        }
        return send_error(request, "401 Unauthorized", "Invalid credentials");
    }

    uint8_t candidate[PARADOX_CONFIG_WEB_AUTH_HASH_SIZE];
    esp_err_t err = derive_password_hash(password_item->valuestring,
                                         password_size,
                                         s_config->web_auth_salt, candidate);
    bool valid = err == ESP_OK &&
                 constant_time_equal(candidate, s_config->web_auth_hash,
                                     sizeof(candidate));
    mbedtls_platform_zeroize(candidate, sizeof(candidate));
    delete_json_strings(json);
    if (!valid) {
        if (err != ESP_OK) {
            return send_error(request, "500 Internal Server Error",
                              "Authentication unavailable");
        }
        s_login_failures++;
        if (s_login_failures >= WEB_LOGIN_MAX_FAILURES) {
            s_login_blocked_until = now + WEB_LOGIN_LOCK_SECONDS;
            s_login_failures = 0;
        }
        return send_error(request, "401 Unauthorized", "Invalid credentials");
    }

    s_login_failures = 0;
    s_login_blocked_until = 0;
    err = issue_session(request);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot issue web session cookie: %s",
                 esp_err_to_name(err));
        return send_error(request, "500 Internal Server Error",
                          "Session could not be created");
    }
    /* auth_status_handler() reports the session of the request it is handed,
     * and this request is the one that has no cookie yet - the session is only
     * being set in the response. Answer for the login itself instead, or a
     * client that reads the login response sees authenticated:false after a
     * successful sign-in. */
    /* A signed-in is a record too: "someone succeeded" is the other half of a
     * brute-force trail, and the login lockout is only believable if the successes
     * are visible next to the failures. */
    web_audit(request, "200", "web session issued");
    char body[96];
    int length = snprintf(body, sizeof(body),
                          "{\"password_change_required\":%s,\"authenticated\":true}",
                          s_config->web_auth_password_set ? "false" : "true");
    if (length < 0 || (size_t)length >= sizeof(body)) {
        return send_error(request, "500 Internal Server Error",
                          "Authentication status unavailable");
    }
    return send_json(request, body);
}

static esp_err_t change_password_handler(httpd_req_t *request)
{
    if (!request_is_authorized(request)) {
        return ESP_OK;
    }
    cJSON *json = NULL;
    if (receive_json(request, &json) != ESP_OK) {
        return ESP_OK;
    }

    cJSON *new_item = cJSON_GetObjectItemCaseSensitive(json, "new_password");
    cJSON *current_item = cJSON_GetObjectItemCaseSensitive(json,
                                                           "current_password");
    size_t new_size = 0;
    if (!cJSON_IsString(new_item) ||
        !valid_password(new_item->valuestring, &new_size)) {
        delete_json_strings(json);
        return send_error(request, "400 Bad Request",
                          "Password must be 4-128 printable ASCII characters");
    }

    if (s_config->web_auth_password_set) {
        size_t current_size = 0;
        if (!cJSON_IsString(current_item) ||
            !valid_password(current_item->valuestring, &current_size)) {
            delete_json_strings(json);
            return send_error(request, "401 Unauthorized",
                              "Current password required");
        }
        uint8_t current_hash[PARADOX_CONFIG_WEB_AUTH_HASH_SIZE];
        esp_err_t current_err = derive_password_hash(
            current_item->valuestring, current_size, s_config->web_auth_salt,
            current_hash);
        bool current_valid = current_err == ESP_OK &&
            constant_time_equal(current_hash, s_config->web_auth_hash,
                                sizeof(current_hash));
        mbedtls_platform_zeroize(current_hash, sizeof(current_hash));
        if (!current_valid) {
            delete_json_strings(json);
            if (current_err != ESP_OK) {
                return send_error(request, "500 Internal Server Error",
                                  "Password verification unavailable");
            }
            return send_error(request, "401 Unauthorized",
                              "Current password is incorrect");
        }
    }

    uint8_t salt[PARADOX_CONFIG_WEB_AUTH_SALT_SIZE];
    uint8_t hash[PARADOX_CONFIG_WEB_AUTH_HASH_SIZE];
    esp_err_t err = generate_salt_and_hash(new_item->valuestring, new_size,
                                           salt, hash);
    delete_json_strings(json);
    if (err != ESP_OK) {
        return send_error(request, "500 Internal Server Error",
                          "Password update unavailable");
    }

    paradox_config_t previous = *s_config;
    paradox_config_result_t result = paradox_config_set_web_auth(
        s_config, salt, sizeof(salt), hash, sizeof(hash), true);
    mbedtls_platform_zeroize(salt, sizeof(salt));
    mbedtls_platform_zeroize(hash, sizeof(hash));
    if (result != PARADOX_CONFIG_OK) {
        *s_config = previous;
        return send_error(request, "500 Internal Server Error",
                          "Password update failed");
    }
    err = paradox_config_store_save(s_config);
    if (err != ESP_OK) {
        *s_config = previous;
        ESP_LOGE(TAG, "Cannot save updated web password: %s",
                 esp_err_to_name(err));
        return send_error(request, "500 Internal Server Error",
                          "Password could not be saved");
    }
    web_audit(request, "200", "web password changed");
    return send_json(request, "{\"ok\":true}");
}

static esp_err_t logout_handler(httpd_req_t *request)
{
    if (!request_is_authorized(request)) {
        return ESP_OK;
    }
    portENTER_CRITICAL(&s_session_mux);
    memset(s_session_token, 0, sizeof(s_session_token));
    memset(s_session_cookie, 0, sizeof(s_session_cookie));
    s_session_expires = 0;
    portEXIT_CRITICAL(&s_session_mux);
    esp_err_t err = httpd_resp_set_hdr(
        request, "Set-Cookie",
        "session=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot clear web session cookie: %s",
                 esp_err_to_name(err));
        return send_error(request, "500 Internal Server Error",
                          "Session cookie could not be cleared");
    }
    web_audit(request, "200", "web session ended");
    return send_json(request, "{\"ok\":true}");
}

static esp_err_t prepare_bootstrap_password(void)
{
    if (s_config->web_auth_password_set) {
        if (bytes_are_zero(s_config->web_auth_hash,
                           sizeof(s_config->web_auth_hash)) ||
            bytes_are_zero(s_config->web_auth_salt,
                           sizeof(s_config->web_auth_salt))) {
            ESP_LOGE(TAG, "Configured web password record is invalid");
            return ESP_ERR_INVALID_STATE;
        }
        return ESP_OK;
    }

    /* The default is a public constant, so it needs no zeroization. */
    uint8_t salt[PARADOX_CONFIG_WEB_AUTH_SALT_SIZE];
    uint8_t hash[PARADOX_CONFIG_WEB_AUTH_HASH_SIZE];
    esp_err_t err = generate_salt_and_hash(WEB_DEFAULT_PASSWORD,
                                           strlen(WEB_DEFAULT_PASSWORD),
                                           salt, hash);
    if (err != ESP_OK) {
        mbedtls_platform_zeroize(salt, sizeof(salt));
        mbedtls_platform_zeroize(hash, sizeof(hash));
        return err;
    }

    paradox_config_t previous = *s_config;
    paradox_config_result_t result = paradox_config_set_web_auth(
        s_config, salt, sizeof(salt), hash, sizeof(hash), false);
    mbedtls_platform_zeroize(salt, sizeof(salt));
    mbedtls_platform_zeroize(hash, sizeof(hash));
    if (result != PARADOX_CONFIG_OK) {
        *s_config = previous;
        return ESP_ERR_INVALID_STATE;
    }
    err = paradox_config_store_save(s_config);
    if (err != ESP_OK) {
        *s_config = previous;
        ESP_LOGE(TAG, "Cannot save bootstrap password hash: %s",
                 esp_err_to_name(err));
        return err;
    }

    ESP_LOGW(TAG, "Initial web password \"%s\" (change it after sign-in)",
             WEB_DEFAULT_PASSWORD);
    return ESP_OK;
}

esp_err_t web_server_start(paradox_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_server != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_config_mutex == NULL) {
        s_config_mutex = xSemaphoreCreateMutex();
        if (s_config_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    s_config = config;
    /* The raw UART tap mirrors the panel-debug switch, which is already in the
     * saved configuration at boot. */
    paradox_raw_tap_set_enabled(config->panel_debug_enabled);
    esp_err_t err = prepare_bootstrap_password();
    if (err != ESP_OK) {
        s_config = NULL;
        return err;
    }

    httpd_config_t server_config = HTTPD_DEFAULT_CONFIG();
    server_config.server_port = 80;
    server_config.stack_size = 8192;
    server_config.max_uri_handlers = WEB_URI_HANDLER_SLOTS;
    server_config.max_open_sockets = 4;
    server_config.lru_purge_enable = true;
    server_config.uri_match_fn = httpd_uri_match_wildcard;
    err = httpd_start(&s_server, &server_config);
    if (err != ESP_OK) {
        s_config = NULL;
        ESP_LOGE(TAG, "Cannot start HTTP server: %s", esp_err_to_name(err));
        return err;
    }
    s_server_running = true;

    /* The trail starts empty at every boot, and says so. A reader who finds only
     * this record knows the bridge began here rather than that nothing was ever
     * asked of it - which matters because the trail is 16 records deep and a
     * reader cannot otherwise tell an empty device from a busy one. */
    paradox_audit_reset();
    paradox_audit_record(esp_timer_get_time() / 1000, "system", "-", "-", "-",
                         "web server started");

    const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = index_handler},
        {.uri = "/api/health", .method = HTTP_GET, .handler = health_handler},
        {.uri = "/api/system", .method = HTTP_GET,
         .handler = system_info_handler},
        {.uri = "/api/ota", .method = HTTP_POST, .handler = ota_upload_handler},
        {.uri = "/api/auth/status", .method = HTTP_GET,
         .handler = auth_status_handler},
        {.uri = "/api/auth/login", .method = HTTP_POST, .handler = login_handler},
        {.uri = "/api/auth/password", .method = HTTP_POST,
         .handler = change_password_handler},
        {.uri = "/api/auth/logout", .method = HTTP_POST,
         .handler = logout_handler},
        {.uri = "/api/zones", .method = HTTP_GET, .handler = zones_get_handler},
        {.uri = "/api/zones/*", .method = HTTP_PUT,
         .handler = zone_put_handler},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_get_handler},
        {.uri = "/api/status/stream", .method = HTTP_GET,
         .handler = status_stream_handler},
        {.uri = "/api/events", .method = HTTP_GET, .handler = events_get_handler},
        {.uri = "/api/events/clear", .method = HTTP_POST,
         .handler = events_clear_handler},
        {.uri = "/api/debug", .method = HTTP_PUT, .handler = debug_set_handler},
        {.uri = "/api/debug/simulator", .method = HTTP_GET,
         .handler = debug_simulator_status_handler},
        {.uri = "/api/debug/simulator", .method = HTTP_POST,
         .handler = debug_simulator_start_handler},
        {.uri = "/api/debug/panel", .method = HTTP_GET,
         .handler = debug_panel_handler},
        {.uri = "/api/debug/raw", .method = HTTP_GET,
         .handler = debug_raw_handler},
        {.uri = "/api/debug/homekit", .method = HTTP_GET,
         .handler = debug_homekit_handler},
        {.uri = "/api/debug/raw/stream", .method = HTTP_GET,
         .handler = debug_raw_stream_handler},
        {.uri = "/api/homekit/restart", .method = HTTP_POST,
         .handler = homekit_restart_handler},
        {.uri = "/api/audit", .method = HTTP_GET, .handler = audit_get_handler},
    };
    /* Compile time, because the failure at run time is not a degraded page: it is a
     * dead web server. HomeKit keeps working through it, which is what makes the
     * defect easy to live with and hard to notice. */
    _Static_assert(sizeof(uris) / sizeof(uris[0]) <= WEB_URI_HANDLER_SLOTS,
                   "route table outgrew WEB_URI_HANDLER_SLOTS; raise the slot count");
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        err = httpd_register_uri_handler(s_server, &uris[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Cannot register HTTP route %s: %s", uris[i].uri,
                     esp_err_to_name(err));
            s_server_running = false;
            httpd_stop(s_server);
            s_server = NULL;
            s_config = NULL;
            return err;
        }
    }
    /* Unmatched requests are answered by esp_http_server unless a handler is
     * registered, and an unanswered refusal is an unrecorded refusal. */
    err = httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND,
                                     unmatched_route_handler);
    if (err == ESP_OK) {
        err = httpd_register_err_handler(s_server, HTTPD_405_METHOD_NOT_ALLOWED,
                                         unmatched_route_handler);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot register the unmatched-route recorder: %s",
                 esp_err_to_name(err));
        s_server_running = false;
        httpd_stop(s_server);
        s_server = NULL;
        s_config = NULL;
        return err;
    }
    ESP_LOGI(TAG, "HTTP server started on port %u", server_config.server_port);
    return ESP_OK;
}

esp_err_t web_server_stop(void)
{
    if (s_server == NULL) {
        return ESP_OK;
    }
    s_server_running = false;
    esp_err_t err = httpd_stop(s_server);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot stop HTTP server: %s", esp_err_to_name(err));
        return err;
    }
    s_server = NULL;
    s_config = NULL;
    portENTER_CRITICAL(&s_session_mux);
    memset(s_session_token, 0, sizeof(s_session_token));
    memset(s_session_cookie, 0, sizeof(s_session_cookie));
    s_session_expires = 0;
    portEXIT_CRITICAL(&s_session_mux);
    if (s_config_mutex != NULL) {
        vSemaphoreDelete(s_config_mutex);
        s_config_mutex = NULL;
    }
    return ESP_OK;
}

void web_server_record_event(const paradox_event_t *event,
                             uint64_t timestamp_ms)
{
    paradox_event_log_push(event, timestamp_ms);
}
