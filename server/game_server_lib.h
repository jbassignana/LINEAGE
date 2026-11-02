#include <assert.h>
#include <stdlib.h> // calloc, size_t
#include <stddef.h> // offsetof
#include <stdio.h>  // fprintf
#include <string.h> // memset, memcpy, strncpy, strnlen
#include <stdint.h> // fixed int types
#include <locale.h> // setlocale
#include <wchar.h>  // wchar_t
#include <time.h>   // time_t, now
#include <stdarg.h> // va_arg, va_list
#include <math.h>   // sqrtf

#include "thread.h"
#include "config.h"
#include "directory.h"
#include "net.h"
#include "pevent.h"

typedef uint8_t byte;

typedef  uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef   int8_t  s8;
typedef  int16_t s16;
typedef  int32_t s32;
typedef  int64_t s64;

typedef float seconds;

#define nl "\n"
#define kb *1024
#define mb *1024*1024
#define sqr(x) ((x)*(x))
#define trace(...) fprintf(stderr, __VA_ARGS__)
#if CFG_TRACE_PACKETS
#define ptrace(...) trace(__VA_ARGS__)
#else
#define ptrace(...) ((void) 0)
#endif
#define warn(...)  trace("warning / " __VA_ARGS__)
#define error(...) trace("  error / " __VA_ARGS__)
#define countof(x) (sizeof(x) / sizeof(*(x)))

#define bprintf(dest, n, ...) \
    bprintf_((dest), (n), __VA_ARGS__, 0)

#define bscanf(src, n, ...) \
    bscanf_((src), (n), __VA_ARGS__, 0)

#define pscanf(src, ...) \
    bscanf_((src) + 3, packet_size(src) - 3, __VA_ARGS__, 0)

#define push_response(conn, _encrypt, ...) \
    push_response_((conn), (_encrypt), __VA_ARGS__, 0)

#define broadcast(state, character, _encrypt, ...) \
    broadcast_((state), (character), (_encrypt), __VA_ARGS__, 0)

#define coroutine(x)                    \
    struct coroutine *__coro = &(x);    \
    switch (__coro->line)               \
    case 0:

#define yield                       \
    {                               \
    __coro->line = __COUNTER__ + 1; \
    break;                          \
    }                               \
    case __COUNTER__:

#define syield(sleep, delta)        \
    {                               \
    __coro->sleep_for = (sleep);    \
    yield;                          \
    __coro->sleep_for -= (delta);   \
    if (__coro->sleep_for > 0)      \
    break;                          \
    }

#define reset *__coro = (struct coroutine) {0}

struct coroutine {
    u32 line;
    seconds sleep_for;
};

struct connection {
    // locks first: conn_reset() zeroes everything after them.
    l2_mutex out_lock;
    l2_mutex io_lock;

    int socket;
    int encrypted;
    
    u32 login_ok1;
    u32 login_ok2;
    
    u32 play_ok1;
    u32 play_ok2;
    
    wchar_t username[32];
    
    byte encrypt_key[8];
    byte decrypt_key[8];
    
    struct coroutine state;

    /*
     * output path (multithreaded):
     * game logic encrypts packets and appends them to `to_send`
     * holding `out_lock` (short critical section). a sender thread
     * swaps them into `inflight` and writes them to the socket
     * holding only `io_lock`, so a slow client never blocks the world.
     */
    byte to_send[CFG_OUTBOX_BYTES];
    size_t to_send_count;
    int overflowed;
    int queued;          // already in its sender's ready list.
    u64 stat_bytes;
    u64 stat_packets;

    u64 retry_at_ms;     // socket buffer was full, retry later.
    int io_socket;       // socket as seen by the sender (0 = closed).
    byte inflight[CFG_OUTBOX_BYTES];
    size_t inflight_count;
    size_t sent;

    byte request[16 kb];
    size_t request_count;

    struct character *character;
};

struct character {
    int active;
    struct connection *conn;
    wchar_t name[32];
    int wait_before_attacking;
    u32 template_id;
    s32 x;
    s32 y;
    s32 z;
    s32 heading;
    s32 client_x;
    s32 client_y;
    u32 race_id;
    u32 sex;
    u32 class_id;
    u32 level;
    u32 xp;
    struct attributes {
        u32 str;
        u32 dex;
        u32 con;
        u32 _int;
        u32 wit;
        u32 men;
    } attributes;
    double max_hp;
    double current_hp;
    double max_mp;
    double current_mp;
    double max_cp;
    double current_cp;
    u32 sp;
    u32 current_load;
    u32 max_load;
    u32 p_atk;
    u32 p_atk_speed;
    u32 p_def;
    u32 evasion_rate;
    u32 accuracy;
    u32 critical_hit;
    u32 m_atk;
    u32 m_atk_speed;
    u32 m_def;
    u32 pvp_flag;
    u32 karma;
    u32 run_speed;
    u32 walk_speed;
    u32 swim_run_speed;
    u32 swim_walk_speed;
    u32 fly_run_speed;
    u32 fly_walk_speed;
    double movement_speed_multiplier;
    double atk_speed_multiplier;
    double collision_radius;
    double collision_height;
    u32 hair_style_id;
    u32 hair_color_id;
    u32 face_id;
    u32 access_level;
    wchar_t title[32];
    u32 clan_id;
    u32 crest_id;
    u32 ally_id;
    u32 ally_crest_id;
    u8 mount_type;
    u8 private_store_type;
    u8 dwarven_craft;
    u32 pk_kills;
    u32 pvp_kills;
    u16 cubics;
    u8 party_members;
    u32 abnormal_effect;
    u32 clan_privileges;
    u16 recommendations_left;
    u16 recommendations_have;
    u16 inventory_limit;
    u8 mounted;
    u32 clan_crest_large_id;
    u8 hero_symbol;
    u8 hero;
    s32 fish_x;
    s32 fish_y;
    s32 fish_z;
    u32 name_color;
    
    u32 target_id;

    // set when the player sent EnterWorld (npcs: when spawned).
    int in_world;
    // interest management: objects this client has been told about.
    u64 known[1024 / 64];
    u32 known_count;
    // ms timestamps.
    u64 last_client_update_ms;
    u64 last_correction_ms;
    u64 last_others_move_bcast_ms;
    u64 stunned_until_ms;
    u64 disoriented_until_ms;  // shadow step: can't turn/attack.
    u64 dead_since_ms;
    u64 skill_reuse_until_ms[32];
    u64 last_regen_ms;
    // keyboard (wasd) movement state.
    int keyboard_moving;
    u64 last_keyboard_move_ms;
    s32 last_validate_x;
    s32 last_validate_y;
    int keyboard_bcast_pending;
    float last_bcast_dir_x;
    float last_bcast_dir_y;
    // last attacker (kept visible even over the visibility cap).
    u32 last_attacker_id;

    enum action_type {
        idle,
        moving,
        attacking,
        dead,
        casting,
    } action_type, prev_action_type;
    
    union action_payload {
        struct action_moving {
            int queue_action;
            u32 offset;
            int keyboard;

            // time based interpolation (ms), independent of tick rate.
            u64 start_ms;
            u64 duration_ms;
            u64 last_repath_ms;
            
            s32 src_x;
            s32 src_y;
            s32 src_z;
            
            s32 target_x;
            s32 target_y;
            s32 target_z;
            
            u32 target_id;
        } moving;
        
        struct action_attacking {
            u32 obj_id;
            u64 next_hit_ms;
        } attacking;

        struct action_casting {
            u32 skill_index;
            u32 target_id;
            u64 hit_at_ms;
        } casting;
    } action_payload, prev_action_payload;
};

enum attr_status {
    attr_status_level = 0x01,
    attr_status_xp = 0x02,
    attr_status_str = 0x03,
    attr_status_dex = 0x04,
    attr_status_con = 0x05,
    attr_status_int = 0x06,
    attr_status_wit = 0x07,
    attr_status_men = 0x08,
    attr_status_current_hp = 0x09,
    attr_status_max_hp = 0x0a,
    attr_status_current_mp = 0x0b,
    attr_status_max_mp = 0x0c,
    attr_status_sp = 0x0d,
    attr_status_current_load = 0x0e,
    attr_status_max_load = 0x0f,
    attr_status_p_atk = 0x11,
    attr_status_atk_speed = 0x12,
    attr_status_p_def = 0x13,
    attr_status_evasion = 0x14,
    attr_status_accuracy = 0x15,
    attr_status_critical = 0x16,
    attr_status_m_atk = 0x17,
    attr_status_cast_speed = 0x18,
    attr_status_m_def = 0x19,
    attr_status_pvp_flag = 0x1a,
    attr_status_karma = 0x1b,
    attr_status_current_cp = 0x21,
    attr_status_max_cp = 0x22,
};

struct jobs {
    l2_mutex lock;
    l2_cond start;
    l2_cond done;
    l2_thread threads[CFG_MAX_JOB_THREADS];
    int count;
    u64 generation;
    int pending;
    int stop;
    void (*fn)(void *ctx, size_t from, size_t to);
    void *ctx;
    size_t n;
};

struct sender {
    struct state *state;
    l2_thread thread;
    l2_mutex lock;
    l2_cond wake;
    u32 ready[1024];      // ring of connection indexes.
    u32 head;
    u32 count;
    int stop;
};

struct state {
    float d;              // ms elapsed in the current tick.
    double run_time;
    u64 ticks;            // 100ms units (kept for compatibility).
    u64 now_ms;           // monotonic server time.
    u64 last_visibility_ms;
    u32 rng;

    struct connection connections[1024];
    struct character characters[1024];

    // world lock: every mutation of characters/connections game state.
    l2_mutex lock;
    l2_thread timer;
    int stop_timer;

    struct sender senders[CFG_MAX_SENDERS];
    int sender_count;
    struct jobs jobs;

    // stats (printed every 10s).
    u64 stat_ticks;
    double stat_tick_ms_total;
    double stat_tick_ms_max;
};

static int on_init(void **buf);
static void init_threads(struct state *state);
static void on_tick(struct state *state);
static void on_connection(void **buf, int socket);
static void on_request(void **buf, int socket, void *request, size_t len);
static void on_disconnect(void **buf, int socket);

static void character_update(struct state *state, struct character *character);


static byte *bscanf_va(byte *src, size_t n, va_list va);
static byte *bscanf_(byte *src, size_t n, ...);
static byte *bprintf_va(byte *dest, size_t n, va_list va);
static byte *bprintf_(byte *dest, size_t n, ...);

static u16 packet_size(byte *src);
static void push_response_va(struct connection *conn, int _encrypt, va_list va);
static void push_response_(struct connection *conn, int _encrypt, ...);
static void broadcast_(struct state *state, struct character *character, int _encrypt, ...);
static void encrypt(struct connection *conn, byte *packet);
static void decrypt(struct connection *conn, byte *request);

static struct connection *get_connection_from_socket(struct state *state, int socket);

static int handle_request(struct state *state, struct connection *conn);
static void handle_send_protocol(struct state *state, struct connection *conn);
static void handle_auth(struct state *state, struct connection *conn, byte *req);
static void handle_show_create_character_screen(struct state *state, struct connection *conn);
static void handle_character_creation(struct state *state, struct connection *conn, byte *req);
static void handle_send_character_list(struct state *state, struct connection *conn);
static void handle_select_character(struct state *state, struct connection *conn, byte *req);
static void handle_auto_ss_bsps(struct state *state, struct connection *conn);
static void handle_send_quest_list(struct state *state, struct connection *conn);
static void handle_enter_world(struct state *state, struct connection *conn);
static void handle_leave_world(struct state *state, struct connection *conn);
static void handle_restart(struct state *state, struct connection *conn);
static void handle_movement(struct state *state, struct connection *conn, byte *req);
static void handle_validate_position(struct state *state, struct connection *conn, byte *req);
static void handle_show_map(struct state *state, struct connection *conn);
static void handle_action(struct state *state, struct connection *conn, byte *req);
static void handle_deselect_target(struct state *state, struct connection *conn);

// order character to walk to a point.
static void move_to(struct state *state, struct character *src, s32 x, s32 y, s32 z, u32 offset, u32 target_id, int queue_action);
// make character select a target. this is the first step
// in order to interact with the target character.
static void select_target(struct state *state, struct character *src, u32 target_id);
// make character interact with a target.
// - if target is attackable, then an attack order will take place.
// - if target isn't attackable, then a dialog will show up.
static void start_interaction(struct state *state, struct character *src);
// NOTE(fmontenegro): not sure about this one...
static void broadcast_char_info(struct state *state, struct character *src);
//
static void send_attr_status(struct state *state, struct character *from, struct character *to, enum attr_status *status, size_t n);

// get character by id. if none is found, null will be returned.
static struct character *get_character_by_id(struct state *state, u32 id);
static u32 get_character_id(struct state *state, struct character *src);

static void moving_update(struct state *state, struct character *character);
static void attacking_update(struct state *state, struct character *character);
static void casting_update(struct state *state, struct character *character);

// threads & output
static void stop_threads(struct state *state);
static void conn_flush(struct state *state, struct connection *conn);
static void flush_all(struct state *state);
static void kick(struct connection *conn);
static void jobs_parallel_for(struct jobs *jobs, size_t n, void (*fn)(void *, size_t, size_t), void *ctx);

// world
static void character_init_stats(struct state *state, struct character *c);
static void character_leave_world(struct state *state, struct character *c);
static void update_visibility(struct state *state);
static void update_visibility_for(struct state *state, struct character *c);
static int  knows(struct character *observer, u32 id);
static void send_object_info(struct state *state, struct character *to, struct character *src);
static void broadcast_others_(struct state *state, struct character *src, int _encrypt, ...);
#define broadcast_others(state, character, _encrypt, ...) \
    broadcast_others_((state), (character), (_encrypt), __VA_ARGS__, 0)
static struct character *spawn_npc(struct state *state, s32 x, s32 y, s32 z);
static s32 heading_from(s32 dx, s32 dy);
static void stop_moving(struct state *state, struct character *c, int notify_self);
static void world_character_tick(struct state *state, struct character *c);
static void keyboard_broadcast_move(struct state *state, struct character *c);
static void update_position(struct state *state, struct character *c);
static void send_validate_location(struct state *state, struct connection *to, struct character *c);
static void die(struct state *state, struct character *src);

// skills
static void send_skill_list(struct state *state, struct character *c);
static void handle_magic_skill_use(struct state *state, struct connection *conn, byte *req);
static void handle_attack_request(struct state *state, struct connection *conn, byte *req);
static void handle_cannot_move_anymore(struct state *state, struct connection *conn, byte *req);
static void action_failed(struct character *c);

// squared distances. u64: l2 coordinates are large enough to overflow u32.
static u64 distance_between(s32 x, s32 y, s32 z, s32 x2, s32 y2, s32 z2);
static u64 distance_between_characters(struct character *a, struct character *b);
