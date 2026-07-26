#pragma once

// Glue between the composition root (main.cpp), the companion wrapper
// (wrap_companion.cpp) and the /multi web page + JSON endpoints
// (web_multi.cpp).

#include <stddef.h>
#include <stdint.h>
#include <esp_http_server.h>

class SharedRadioCore;
class WebPanelServer;

// ---- provided by main.cpp ----
SharedRadioCore* multiCore();
// run a console command (same dispatcher as serial console / web /api/command:
// supports repeater/room/companion prefixes plus composition intercepts)
void multiRunConsole(const char* cmd, char* reply, size_t reply_size);
// Run fn(arg) on the super-loop task and wait for it. Anything that touches
// live mesh state from the HTTPS server task must go through this: the loop is
// mutating those objects concurrently, and a torn read is the good outcome.
// Returns false if it could not be run in time — in which case the loop task
// will STILL run it later, so `arg` must remain valid forever (heap-allocate it
// and leak on failure; the alternative is a write into a freed frame).
typedef void (*MultiLoopFn)(void*);
bool multiRunInLoop(MultiLoopFn fn, void* arg, uint32_t timeout_ms);
bool multiOnLoopTask();
// receives this identity dropped because its packet pool was empty (by port index)
uint32_t multiPortPoolFull(int port_idx);
void multiGetRadioParams(float* freq, float* bw, uint8_t* sf, uint8_t* cr);
namespace fs { class FS; }
fs::FS* multiSysFS();   // composition prefs area of the shared partition
uint8_t  multiLoadPct();      // main-task duty cycle %, 1s window
uint32_t multiLoopsPerSec();  // super-loop iterations/sec

// ---- provided by wrap_slots.cpp (optional chat identity slots) ----
struct IdentityModule;
void multiChatSlotsInit();
bool multiChatSlotEnabled(int i);
void multiChatSlotSetEnabled(int i, bool en);
int  multiChatSlotPort(int i);
IdentityModule* multiChatSlotModule(int i);
const char* multiChatSlotFsDir(int i);
bool multiChatSlotRunning(int i);
bool multiChatSlotClient(int i);
int  multiChatSlotFrameExchange(int i, const uint8_t* frame, size_t len, uint8_t* out, size_t out_cap,
                                uint32_t total_ms, uint32_t idle_ms);
uint32_t multiChatSlotArchiveSeq(int i);
int  multiChatSlotArchiveCopy(int i, uint32_t after, uint8_t* out, size_t cap);

// ---- provided by wrap_room.cpp ----
// stored room posts as a JSON array [{t,a,x}...]; returns bytes written
int roomGetPostsJson(char* out, size_t cap);

// ---- provided by wrap_companion.cpp ----
// Inject one app-protocol frame into the companion mesh and collect its
// response frames. out receives [u16 len LE][frame bytes]... concatenated.
// Returns total bytes written to out, 0 if no response before timeout,
// -1 if the companion isn't up or another exchange is in progress.
int  compWebFrameExchange(const uint8_t* frame, size_t len,
                          uint8_t* out, size_t out_cap,
                          uint32_t total_ms, uint32_t idle_ms);
bool compTcpStarted();
bool compTcpClientConnected();
// read-only mirror of synced message frames (does NOT consume the queue)
uint32_t compArchiveSeq();
int  compArchiveCopy(uint32_t after_seq, uint8_t* out, size_t cap);

// ---- provided by web_multi.cpp ----
// registered via WebPanelServer::setExtRoutesRegistrar()
void multiWebRegisterRoutes(httpd_handle_t server, WebPanelServer* panel);
// called from the main loop: samples stats history for the panel's Stats tab
void multiWebTick();
