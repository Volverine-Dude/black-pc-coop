// Co-op module (LAN): additive hooks on top of the loader. See coop.cpp for the plan and facts.
#pragma once

#include <cstdint>

// Install every co-op hook (player bridge, capture hooks, marker sockets, clone pilot).
void install_coop_module();
// the CO-OP lobby starts a mission: configure co-op at run time (role 1 host / 2 joiner, the peer's IPv4 in network order)
void coop_activate(int role, uint32_t peer_ip);
// the shared cinematic skip: tell the partner we pressed skip; the partner's press (CLSK) and when it came
void coop_send_skip();
void coop_poll_control();   // during a briefing video: read the partner's CLSK (the play tick does not run)
// SESSION END (day 15): reason 0 = we left (CLQT to the partner), 1 = the partner left, 2 = the connection was lost.
// Co-op goes off on this copy, the menus go to the title with a message.
void coop_session_end(int reason);
void coop_on_exit();         // the process is closing: tell the partner (CLQT)
bool coop_in_mission();      // the game's current mode is the mission (the play receive loop owns the co-op socket)
extern volatile uint32_t g_coop_peer_skip;
extern volatile uint64_t g_coop_peer_skip_ms;

// Joiner perception (coop_joiner_perception.cpp): the host's enemies notice and engage the joiner's clone.
void coop_jp_install(void);
void coop_jp_frame(uint32_t clone_body);
bool coop_game_paused(void);   // co-op no-pause: the in-level pause menu is open

// Route B: the marker overlay's draw helpers (device.cpp D3DDevice_Swap_after_compose).
// coop_marker_screen fills out[] with fractions of the picture rect, false when the
// marker is off (stale packet, off camera). g_coop_marker_on: the listening flag.
bool coop_marker_screen(float out[2]);
extern volatile uint32_t g_coop_marker_on;
extern volatile uint32_t g_coop_peer_downed; // the PEER is downed (their state packets carry the flag) -> the survivor's HUD cue
extern uint64_t g_coop_host_ms;              // tick count of the last received peer state packet (staleness checks)

// Generic world-to-screen projection through the guest camera; plus the pilot clone's
// dot state (set by coop_level_post when --coop-clone lands a clone).
bool coop_project_point(const float world[3], float out[2]);
extern volatile float g_coop_clone_pos[3];
// COOP_GUN_MARK=1 diagnostics: world points of the clone's hand, measured muzzle, wanted aim
// point, chest (x,y,z each) - drawn as boxes by D3DDevice_Swap_after_compose
extern volatile float g_gunmark[12];
extern volatile int g_gunmark_on;
extern volatile float g_gunmodel[30];   // [0..3] hand, muzzle, want, chest; [4..9] hand +- 0.5 * bone-24 rows r0 r1 r2   // the same points in the clone's MODEL space (bone space)
extern volatile float g_clone_mvp[16];  // the clone body draw's c10..c13 (model -> clip rows), captured in draw.cpp
extern volatile int g_clone_mvp_ok;
extern volatile uint32_t g_coop_skin_mark;
extern volatile uint32_t g_coop_clone_on;
extern volatile uint32_t g_coop_skin_mark;   // skin hook -> draw.cpp (--helmet-scan): the clone's body draw is next
