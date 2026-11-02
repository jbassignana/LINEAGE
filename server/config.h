#ifndef L2_CONFIG_H
#define L2_CONFIG_H
/*
 * game server tunables. everything that changes how much the client
 * has to process (and therefore its fps in crowded places) lives here.
 */

/* ---------------------------------------------------------------- threads */
// world simulation rate. the original server ticked once per second,
// which made movement, attacks and casts land in 1 second bursts.
#ifndef CFG_TICK_MS
#define CFG_TICK_MS 100
#endif
// threads that flush per-connection output buffers to sockets.
// 0 = automatic (cpu count - 1, clamped to [1, CFG_MAX_SENDERS]).
#ifndef CFG_SENDER_THREADS
#define CFG_SENDER_THREADS 0
#endif
#ifndef CFG_MAX_SENDERS
#define CFG_MAX_SENDERS 8
#endif
// worker threads used for parallel per-player work (visibility).
// 0 = automatic (cpu count).
#ifndef CFG_JOB_THREADS
#define CFG_JOB_THREADS 0
#endif
#ifndef CFG_MAX_JOB_THREADS
#define CFG_MAX_JOB_THREADS 16
#endif
// per connection outgoing buffer. a client that can't keep up and
// overflows it is disconnected (instead of corrupting memory). 96kb is
// ~10 seconds of a 300 player siege; pages are only touched when used.
#ifndef CFG_OUTBOX_BYTES
#define CFG_OUTBOX_BYTES (96 * 1024)
#endif

/* ------------------------------------------- interest management (fps!) */
// an object enters a player's view when closer than ENTER and leaves
// when farther than LEAVE (hysteresis avoids spawn/despawn flicker).
#ifndef CFG_VIEW_ENTER
#define CFG_VIEW_ENTER 3000
#endif
#ifndef CFG_VIEW_LEAVE
#define CFG_VIEW_LEAVE 3600
#endif
// hard cap of objects a client is told about. in a 500 player siege the
// c4 client only gets the nearest N, which is what keeps its fps sane.
// your current target and whoever is attacking you are always kept.
#ifndef CFG_MAX_KNOWN
#define CFG_MAX_KNOWN 120
#endif
// when over the cap, objects already shown count as this much closer
// than newcomers, so the shown set doesn't churn as people move.
#ifndef CFG_KNOWN_STICKY
#define CFG_KNOWN_STICKY 500
#endif
// how often visibility is recomputed.
#ifndef CFG_VISIBILITY_MS
#define CFG_VISIBILITY_MS 200
#endif

/* --------------------------------------------------------------- movement */
// max distance between where the client says it is and where the
// server thinks it is, before the server corrects the client.
#ifndef CFG_DESYNC_TOLERANCE
#define CFG_DESYNC_TOLERANCE 160
#endif
// never send position corrections to the same client more often.
#ifndef CFG_CORRECTION_COOLDOWN_MS
#define CFG_CORRECTION_COOLDOWN_MS 1000
#endif
// keyboard (arrow keys / wasd) movement. the client sends a far point
// in the direction it walks; clamp it so nobody walks across the map.
#ifndef CFG_KEYBOARD_MAX_LEAD
#define CFG_KEYBOARD_MAX_LEAD 1200
#endif
// other players are told about direction changes at most every N ms,
// unless the direction changed more than ANGLE degrees.
#ifndef CFG_KEYBOARD_BCAST_MS
#define CFG_KEYBOARD_BCAST_MS 150
#endif
#ifndef CFG_KEYBOARD_BCAST_ANGLE
#define CFG_KEYBOARD_BCAST_ANGLE 12
#endif
// following a target (attack/cast approach) re-paths at most every N ms
// or when the target moved more than DIST.
#ifndef CFG_FOLLOW_REPATH_MS
#define CFG_FOLLOW_REPATH_MS 400
#endif
#ifndef CFG_FOLLOW_REPATH_DIST
#define CFG_FOLLOW_REPATH_DIST 40
#endif

/* ------------------------------------------------------------------ combat */
#ifndef CFG_ORC_HP
#define CFG_ORC_HP 1500
#endif
#ifndef CFG_NPC_CORPSE_MS
#define CFG_NPC_CORPSE_MS 5000
#endif
// after shadow step the target can't turn around nor attack for this long.
#ifndef CFG_SHADOW_STEP_DISORIENT_MS
#define CFG_SHADOW_STEP_DISORIENT_MS 1200
#endif

/* ------------------------------------------------------- protocol (c4) */
// the interlude SkillList appends a 'disabled' byte per skill. c4 doesn't.
#ifndef CFG_SKILLLIST_DISABLED_BYTE
#define CFG_SKILLLIST_DISABLED_BYTE 0
#endif
// players never saw each other in the original server (CharInfo was a
// todo). the CharInfo layout below follows l2j c4-era code but has NOT
// been validated against a real c4 client yet: enable it and test.
#ifndef CFG_SEND_PLAYER_CHARINFO
#define CFG_SEND_PLAYER_CHARINFO 0
#endif

/* ------------------------------------------------------------ logging */
// the original logged every packet received and every send() to stderr,
// which at a few hundred players costs more cpu than the game itself.
#ifndef CFG_TRACE_PACKETS
#define CFG_TRACE_PACKETS 0
#endif
// print tick time / bandwidth stats every N ms (0 = off).
#ifndef CFG_STATS_MS
#define CFG_STATS_MS 10000
#endif

#endif
