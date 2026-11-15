// The CO-OP menu pages' logic (fe/pages.cpp builds the pages): the host lobby with its code, the joiner's code entry,
// and the LAN lobby link between them (fe/coop_menu.cpp).
#pragma once

#include <cstdint>

// page ids (PCPage): 200 / 201 CO-OP, 210 HOST GAME, 211 JOIN GAME, 220 the host lobby (LAN), 230 the join code (LAN)
bool coop_menu_page(int id);           // every PCPage: true for a co-op page (no PC SETTINGS values); others leave a lobby
void coop_menu_action(int id);         // PCCoop(id): an action row
void coop_menu_change(int id, int dir); // PCCoopChange(id, -1 / +1): a choice row's arrows
void coop_menu_poll();                 // once per front-end update: network, values
void coop_menu_char(wchar_t c);        // a typed character (window thread)
// every game call of the front end (before the game runs it): true = the lobby holds it (the game does not run it)
bool coop_menu_game_call(const char *name, const char *p1, const char *p2, const char *p3);
// the shared movie skip, once per pad poll after every input is merged: may eat or inject the skip press
void coop_intro_gate(uint16_t &buttons, bool enter);   // the shared intro-text skip (input/xinput.cpp)
void coop_skip_gate(uint16_t &buttons, uint8_t analog[8], bool &enter);
bool coop_skip_holding();   // a co-op movie holds the skip press (the menu's Enter select is eaten too)
extern volatile int g_coop_code_len;    // the join code page is up: the code's length (Backspace deletes); -1 otherwise

// the lobby the menus made (for the start of play, step 3): role 1 = host, 2 = joiner; false when there is none
bool coop_lobby_peer(int *role, uint32_t *ip, uint16_t *port);   // ip / port in network order
// a co-op level ended (coop/coop.cpp): after the results and the autosave the game opens mission select - the
// players go back to their lobby page instead (same code, the joiner rejoins by itself, READY off)
void coop_menu_after_mission(int role);
// the co-op session ended (coop/coop.cpp coop_session_end): leave any lobby, the menus go to the title
void coop_menu_session_ended(int reason);
