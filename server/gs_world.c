/*
 * world: stats, spawning, and interest management.
 *
 * interest management is the main server-side fix for low client fps:
 * the c4 client creates an actor (mesh, animation, name plate) for every
 * object the server tells it about and processes every packet about
 * them. the original server told every client about everything in the
 * whole world. now each client only knows the objects within
 * CFG_VIEW_ENTER, and never more than CFG_MAX_KNOWN of them (nearest
 * first). visibility is computed for all players in parallel.
 */

#define PI_F 3.14159265f

static int knows(struct character *observer, u32 id)
{
    if (id >= 1024)
        return 0;
    return (observer->known[id >> 6] >> (id & 63)) & 1;
}

static void set_known(struct character *observer, u32 id, int value)
{
    u64 bit = (u64) 1 << (id & 63);
    if (value)
        observer->known[id >> 6] |= bit;
    else
        observer->known[id >> 6] &= ~bit;
}

static s32 heading_from(s32 dx, s32 dy)
{
    // l2 headings go from 0 to 65535 for a full turn.
    double angle = atan2((double) dy, (double) dx);
    if (angle < 0)
        angle += 2.0 * 3.14159265358979;
    return (s32) (angle * 65536.0 / (2.0 * 3.14159265358979)) & 0xffff;
}

static int is_dagger_class(u32 class_id)
{
    switch (class_id) {
    case 7:   // rogue
    case 8:   // treasure hunter
    case 22:  // elven scout
    case 23:  // plains walker
    case 35:  // assassin
    case 36:  // abyss walker
    case 93:  // adventurer
    case 101: // wind rider
    case 108: // ghost hunter
        return 1;
    default:
        return 0;
    }
}

static int is_mystic_class(u32 class_id)
{
    return (class_id >= 10 && class_id <= 17) ||
           (class_id >= 25 && class_id <= 30) ||
           (class_id >= 38 && class_id <= 43) ||
           (class_id >= 49 && class_id <= 52) ||
           (class_id >= 94 && class_id <= 98) ||
           (class_id >= 103 && class_id <= 105) ||
           (class_id >= 110 && class_id <= 112) ||
           (class_id >= 115 && class_id <= 116);
}

enum { race_human, race_elf, race_dark_elf, race_orc, race_dwarf };

static void character_init_stats(struct state *state, struct character *c)
{
    (void) state;
    u32 lvl = c->level ? c->level : 1;
    double lvl_mul = 1.0 + 0.04 * (double) (lvl - 1);

    // fighter baseline.
    double hp = 420, mp = 140, cp = 300;
    double p_atk = 80, p_def = 80, m_atk = 30, m_def = 60;
    u32 atk_spd = 300, cast_spd = 333, run = 120, walk = 80;
    u32 crit = 40, acc = 33, evasion = 33;

    if (is_mystic_class(c->class_id)) {
        hp = 300; mp = 320; cp = 200;
        p_atk = 40; p_def = 55; m_atk = 95; m_def = 80;
        atk_spd = 280; cast_spd = 380; run = 120; crit = 20;
    }
    if (is_dagger_class(c->class_id)) {
        // daggers: fast, critical, evasive, fragile.
        hp = 380; p_atk = 78; p_def = 70;
        atk_spd = 433; run = 132; crit = 90; evasion = 45; acc = 38;
    }
    if (c->race_id == race_dwarf) {
        // "dwarven might": the toughest hitters in melee range.
        hp *= 1.20;
        p_atk *= 1.18;
        p_def *= 1.12;
        crit += 15;
        acc += 3;
        run = 115;
        c->dwarven_craft = 1;
    }
    if (c->race_id == race_orc) {
        hp *= 1.10;
        p_atk *= 1.05;
    }
    if (c->race_id == race_elf) {
        run += 6;
        evasion += 3;
    }

    c->max_hp = hp * lvl_mul;
    c->max_mp = mp * lvl_mul;
    c->max_cp = cp * lvl_mul;
    if (c->current_hp <= 0 || c->current_hp > c->max_hp)
        c->current_hp = c->max_hp;
    if (c->current_mp <= 0 || c->current_mp > c->max_mp)
        c->current_mp = c->max_mp;
    if (c->current_cp <= 0 || c->current_cp > c->max_cp)
        c->current_cp = c->max_cp;
    c->p_atk = (u32) (p_atk * lvl_mul);
    c->p_def = (u32) (p_def * lvl_mul);
    c->m_atk = (u32) (m_atk * lvl_mul);
    c->m_def = (u32) (m_def * lvl_mul);
    c->p_atk_speed = atk_spd;
    c->m_atk_speed = cast_spd;
    c->run_speed = run;
    c->walk_speed = walk;
    c->swim_run_speed = run / 2;
    c->swim_walk_speed = walk / 2;
    c->fly_run_speed = run;
    c->fly_walk_speed = walk;
    c->critical_hit = crit;
    c->accuracy = acc;
    c->evasion_rate = evasion;
    c->max_load = 70000;
    c->collision_radius = c->race_id == race_dwarf ? 8.0 : 9.0;
    c->collision_height = c->race_id == race_dwarf ? 18.0 : 23.0;
    c->movement_speed_multiplier = 1;
    c->atk_speed_multiplier = 1;
}

static struct character *spawn_npc(struct state *state, s32 x, s32 y, s32 z)
{
    struct character *npc = 0;
    for (u32 id = 1; id < countof(state->characters); id++) {
        if (state->characters[id].active)
            continue;
        npc = state->characters + id;
        break;
    }
    if (!npc)
        return 0;
    memset(npc, 0, sizeof(*npc));
    npc->active = 1;
    npc->in_world = 1;
    swprintf(npc->name, countof(npc->name) - 1, L"%ls", L"Orc");
    swprintf(npc->title, countof(npc->title) - 1, L"%ls", L"Orc");
    npc->template_id = 500 + 1000000;
    npc->x = x;
    npc->y = y;
    npc->z = z;
    npc->collision_radius = 8;
    npc->collision_height = 25;
    npc->level = 10;
    npc->current_hp = CFG_ORC_HP;
    npc->max_hp = CFG_ORC_HP;
    npc->current_mp = 102;
    npc->max_mp = 102;
    npc->attributes.str = 40;
    npc->attributes.con = 43;
    npc->attributes.dex = 30;
    npc->attributes._int = 21;
    npc->attributes.wit = 20;
    npc->attributes.men = 10;
    npc->p_atk = 41;
    npc->p_def = 55;
    npc->m_atk = 6;
    npc->m_def = 45;
    npc->p_atk_speed = 249;
    npc->m_atk_speed = 227;
    npc->walk_speed = 45;
    npc->run_speed = 110;
    npc->critical_hit = 20;
    return npc;
}

// tell everybody that knows `c` to forget it, right now (not in the next
// visibility pass): its slot/id may be reused by a new object.
static void forget_everywhere(struct state *state, struct character *c)
{
    u32 id = get_character_id(state, c);
    for (size_t i = 0; i < countof(state->characters); i++) {
        struct character *observer = state->characters + i;
        if (!knows(observer, id))
            continue;
        set_known(observer, id, 0);
        if (observer->known_count)
            observer->known_count--;
        if (observer->active && observer->conn && observer != c)
            push_response(observer->conn, 1,
                          "%h", 0,
                          "%c", 0x12,
                          "%u", id,
                          "%u", 0);
    }
}

static void character_leave_world(struct state *state, struct character *c)
{
    if (!c || !c->active)
        return;
    forget_everywhere(state, c);
    // whoever was targeting or fighting it lets go.
    u32 id = get_character_id(state, c);
    for (size_t i = 0; i < countof(state->characters); i++) {
        struct character *other = state->characters + i;
        if (!other->active)
            continue;
        if (other->target_id == id)
            other->target_id = 0;
        if (other->last_attacker_id == id)
            other->last_attacker_id = 0;
    }
    memset(c->known, 0, sizeof(c->known));
    c->known_count = 0;
    c->in_world = 0;
    c->active = 0;
}

static void send_npc_info(struct state *state, struct character *to, struct character *npc)
{
    u32 attackable = 1;
    u32 abnormal = state->now_ms < npc->stunned_until_ms ? 0x0040 : 0;
    push_response(to->conn, 1,
                  "%h", 0,
                  "%c", 0x16,
                  "%u", get_character_id(state, npc),
                  "%u", npc->template_id,
                  "%u", attackable,
                  "%u", npc->x,
                  "%u", npc->y,
                  "%u", npc->z,
                  "%u", npc->heading,
                  "%u", 0,
                  "%u", npc->m_atk_speed,
                  "%u", npc->p_atk_speed,
                  "%u", npc->run_speed,
                  "%u", npc->walk_speed,
                  // swim speed
                  "%u", npc->run_speed,
                  "%u", npc->walk_speed,
                  // fly speed
                  "%u", npc->run_speed,
                  "%u", npc->walk_speed,
                  "%u", npc->run_speed,
                  "%u", npc->walk_speed,
                  "%lf", 1.0,
                  "%lf", 1.0,
                  "%lf", npc->collision_radius,
                  "%lf", npc->collision_height,
                  // right hand weapon
                  "%u", 0,
                  // chest
                  "%u", 0,
                  // left hand weapon
                  "%u", 0,
                  "%c", 1,
                  // running?
                  "%c", 1,
                  // combat?
                  "%c", npc->action_type == attacking,
                  // alike dead?
                  "%c", npc->current_hp <= 0.0,
                  // summoned?
                  "%c", 0,
                  "%ls", countof(npc->name), npc->name,
                  "%ls", countof(npc->title), npc->title,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  // abnormal effect (0x40 = stunned)
                  "%u", abnormal,
                  // clan id, crest, ally id, ally crest
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%c", 0,
                  // aura color
                  "%c", 0,
                  "%lf", npc->collision_radius,
                  "%lf", npc->collision_height,
                  "%u", 0);
    // if it's walking, tell the newcomer where to.
    if (npc->action_type == moving && npc->action_payload.moving.duration_ms)
        push_response(to->conn, 1,
                      "%h", 0,
                      "%c", 0x01,
                      "%u", get_character_id(state, npc),
                      "%u", npc->action_payload.moving.target_x,
                      "%u", npc->action_payload.moving.target_y,
                      "%u", npc->action_payload.moving.target_z,
                      "%u", npc->x,
                      "%u", npc->y,
                      "%u", npc->z);
}

static void send_char_info(struct state *state, struct character *to, struct character *p)
{
#if CFG_SEND_PLAYER_CHARINFO
    // layout follows l2j c4-era CharInfo. NOT validated in a c4 client yet.
    push_response(to->conn, 1,
                  "%h", 0,
                  "%c", 0x03,
                  "%u", p->x,
                  "%u", p->y,
                  "%u", p->z,
                  "%u", p->heading,
                  "%u", get_character_id(state, p),
                  "%ls", countof(p->name), p->name,
                  "%u", p->race_id,
                  "%u", p->sex,
                  "%u", p->class_id,
                  // paperdoll item ids (10)
                  "%x", (size_t) 40,
                  "%u", p->pvp_flag,
                  "%u", p->karma,
                  "%u", p->m_atk_speed,
                  "%u", p->p_atk_speed,
                  "%u", p->pvp_flag,
                  "%u", p->karma,
                  "%u", p->run_speed,
                  "%u", p->walk_speed,
                  "%u", p->swim_run_speed,
                  "%u", p->swim_walk_speed,
                  "%u", p->fly_run_speed,
                  "%u", p->fly_walk_speed,
                  "%u", p->fly_run_speed,
                  "%u", p->fly_walk_speed,
                  "%lf", p->movement_speed_multiplier,
                  "%lf", p->atk_speed_multiplier,
                  "%lf", p->collision_radius,
                  "%lf", p->collision_height,
                  "%u", p->hair_style_id,
                  "%u", p->hair_color_id,
                  "%u", p->face_id,
                  "%ls", countof(p->title), p->title,
                  "%u", p->clan_id,
                  "%u", p->crest_id,
                  "%u", p->ally_id,
                  "%u", p->ally_crest_id,
                  "%u", 0,
                  "%c", 1,  // standing
                  "%c", 1,  // running
                  "%c", p->action_type == attacking,
                  "%c", p->current_hp <= 0.0,
                  "%c", 0,  // invisible
                  "%c", p->mount_type,
                  "%c", p->private_store_type,
                  "%h", 0,  // cubics
                  "%c", 0,  // find party
                  "%u", p->abnormal_effect,
                  "%c", (u32) p->recommendations_left,
                  "%h", (u32) p->recommendations_have,
                  "%u", p->class_id,
                  "%u", (u32) p->max_cp,
                  "%u", (u32) p->current_cp,
                  "%c", 0,  // enchant
                  "%c", 0,  // team
                  "%u", p->clan_crest_large_id,
                  "%c", p->hero_symbol,
                  "%c", p->hero,
                  "%c", 0,  // fishing
                  "%u", p->fish_x,
                  "%u", p->fish_y,
                  "%u", p->fish_z,
                  "%u", p->name_color);
    if (p->action_type == moving && p->action_payload.moving.duration_ms)
        push_response(to->conn, 1,
                      "%h", 0,
                      "%c", 0x01,
                      "%u", get_character_id(state, p),
                      "%u", p->action_payload.moving.target_x,
                      "%u", p->action_payload.moving.target_y,
                      "%u", p->action_payload.moving.target_z,
                      "%u", p->x,
                      "%u", p->y,
                      "%u", p->z);
#else
    (void) state; (void) to; (void) p;
#endif
}

static void send_object_info(struct state *state, struct character *to, struct character *src)
{
    if (!to->conn || !src || !src->active)
        return;
    if (src->conn)
        send_char_info(state, to, src);
    else
        send_npc_info(state, to, src);
}

// re-send the info packet of `src` to everybody that knows it (used for
// visual state changes such as stun on/off).
static void broadcast_char_info(struct state *state, struct character *src)
{
    if (!src || !src->active)
        return;
    u32 id = get_character_id(state, src);
    for (size_t i = 0; i < countof(state->characters); i++) {
        struct character *observer = state->characters + i;
        if (!observer->active || !observer->conn || observer == src)
            continue;
        if (!knows(observer, id))
            continue;
        send_object_info(state, observer, src);
    }
}

/* ---------------------------------------------------- visibility (parallel) */

#define GRID_BUCKETS 4096

struct visibility_ctx {
    struct state *state;
    u32 players[1024];
    u32 player_count;
    s32 bucket_head[GRID_BUCKETS];
    s32 next[1024];
};

static u32 grid_bucket(s32 cx, s32 cy)
{
    u32 h = (u32) cx * 73856093u ^ (u32) cy * 19349663u;
    return h & (GRID_BUCKETS - 1);
}

static s32 grid_cell(s32 v)
{
    // floor division so negative coordinates map correctly.
    return v >= 0 ? v / CFG_VIEW_LEAVE : -((-v + CFG_VIEW_LEAVE - 1) / CFG_VIEW_LEAVE);
}

struct candidate {
    u64 d2;
    u32 known;
    u32 id;
};

// nearest first; on ties already known objects first, then by id, so
// the result is deterministic (a crowd standing on the same spot must
// not reshuffle who is shown every pass).
static int candidate_cmp(const void *a, const void *b)
{
    const struct candidate *x = (const struct candidate *) a;
    const struct candidate *y = (const struct candidate *) b;
    if (x->d2 != y->d2)
        return x->d2 < y->d2 ? -1 : 1;
    if (x->known != y->known)
        return x->known ? -1 : 1;
    return x->id < y->id ? -1 : x->id > y->id;
}

static void visibility_for(struct visibility_ctx *ctx, struct character *self)
{
    struct state *state = ctx->state;
    u32 self_id = get_character_id(state, self);
    struct candidate cand[1024];
    u32 n = 0;

    const u64 enter2 = (u64) CFG_VIEW_ENTER * CFG_VIEW_ENTER;
    const u64 leave2 = (u64) CFG_VIEW_LEAVE * CFG_VIEW_LEAVE;

    s32 cx = grid_cell(self->x);
    s32 cy = grid_cell(self->y);
    u32 visited[9];
    u32 visited_count = 0;
    for (s32 oy = -1; oy <= 1; oy++) {
        for (s32 ox = -1; ox <= 1; ox++) {
            u32 b = grid_bucket(cx + ox, cy + oy);
            int dup = 0;
            for (u32 k = 0; k < visited_count; k++)
                dup |= visited[k] == b;
            if (dup)
                continue;
            visited[visited_count++] = b;
            for (s32 it = ctx->bucket_head[b]; it >= 0; it = ctx->next[it]) {
                u32 id = (u32) it;
                if (id == self_id)
                    continue;
                struct character *other = state->characters + id;
                u64 d2 = distance_between(self->x, self->y, self->z, other->x, other->y, other->z);
                int known = knows(self, id);
                if (d2 > (known ? leave2 : enter2))
                    continue;
                // when crowded, objects already on screen win ties against
                // newcomers (they count as CFG_KNOWN_STICKY units closer).
                // without this the nearest-N set churns as people move, and
                // every churn is a despawn + spawn the client pays for.
                if (known) {
                    double d = sqrt((double) d2) - CFG_KNOWN_STICKY;
                    d2 = d > 0 ? (u64) (d * d) : 0;
                }
                // never lose your target or whoever is hitting you.
                if (id == self->target_id || id == self->last_attacker_id)
                    d2 = 0;
                cand[n].d2 = d2;
                cand[n].known = (u32) known;
                cand[n].id = id;
                n++;
            }
        }
    }

    if (n > CFG_MAX_KNOWN) {
        // crowded: keep the nearest only (qsort is thread safe).
        qsort(cand, n, sizeof(*cand), candidate_cmp);
        n = CFG_MAX_KNOWN;
    }

    u64 next_known[1024 / 64] = {0};
    for (u32 i = 0; i < n; i++)
        next_known[cand[i].id >> 6] |= (u64) 1 << (cand[i].id & 63);

    // diff old vs new: only this player's data and connection are written,
    // which is what makes running this for all players in parallel safe.
    for (u32 w = 0; w < countof(next_known); w++) {
        u64 added = next_known[w] & ~self->known[w];
        u64 removed = self->known[w] & ~next_known[w];
        while (removed) {
            u32 bit = 0;
            while (!((removed >> bit) & 1)) bit++;
            removed &= removed - 1;
            push_response(self->conn, 1,
                          "%h", 0,
                          "%c", 0x12,
                          "%u", w * 64 + bit,
                          "%u", 0);
        }
        while (added) {
            u32 bit = 0;
            while (!((added >> bit) & 1)) bit++;
            added &= added - 1;
            send_object_info(state, self, state->characters + w * 64 + bit);
        }
        self->known[w] = next_known[w];
    }
    self->known_count = n;
}

static void visibility_job(void *p, size_t from, size_t to)
{
    struct visibility_ctx *ctx = (struct visibility_ctx *) p;
    for (size_t i = from; i < to; i++)
        visibility_for(ctx, ctx->state->characters + ctx->players[i]);
}

static void visibility_build(struct state *state, struct visibility_ctx *ctx)
{
    ctx->state = state;
    ctx->player_count = 0;
    for (u32 b = 0; b < GRID_BUCKETS; b++)
        ctx->bucket_head[b] = -1;
    for (u32 id = 1; id < countof(state->characters); id++) {
        struct character *c = state->characters + id;
        ctx->next[id] = -1;
        if (!c->active || !c->in_world)
            continue;
        u32 b = grid_bucket(grid_cell(c->x), grid_cell(c->y));
        ctx->next[id] = ctx->bucket_head[b];
        ctx->bucket_head[b] = (s32) id;
        if (c->conn && c->conn->socket)
            ctx->players[ctx->player_count++] = id;
    }
}

static struct visibility_ctx visibility_scratch;

static void update_visibility(struct state *state)
{
    struct visibility_ctx *ctx = &visibility_scratch;
    visibility_build(state, ctx);
    jobs_parallel_for(&state->jobs, ctx->player_count, visibility_job, ctx);
}

static void update_visibility_for(struct state *state, struct character *c)
{
    struct visibility_ctx *ctx = &visibility_scratch;
    visibility_build(state, ctx);
    if (c->conn)
        visibility_for(ctx, c);
}

/* ----------------------------------------------------------- per tick misc */

static void world_character_tick(struct state *state, struct character *c)
{
    // stun ended: refresh the visual for the observers.
    if (c->stunned_until_ms && state->now_ms >= c->stunned_until_ms) {
        c->stunned_until_ms = 0;
        if (!c->conn)
            broadcast_char_info(state, c);
    }
    // slow regeneration for players (mp is what skills burn).
    if (c->conn && c->action_type != dead && state->now_ms - c->last_regen_ms >= 1000) {
        c->last_regen_ms = state->now_ms;
        int changed = 0;
        if (c->current_mp < c->max_mp) {
            c->current_mp += c->max_mp * 0.03;
            if (c->current_mp > c->max_mp)
                c->current_mp = c->max_mp;
            changed = 1;
        }
        if (c->current_hp < c->max_hp && c->action_type != attacking) {
            c->current_hp += c->max_hp * 0.01;
            if (c->current_hp > c->max_hp)
                c->current_hp = c->max_hp;
            changed = 1;
        }
        if (changed) {
            enum attr_status status[] = {attr_status_current_hp, attr_status_max_hp,
                                         attr_status_current_mp, attr_status_max_mp};
            send_attr_status(state, c, c, status, 4);
        }
    }
    // keyboard movement: deliver a throttled direction change.
    if (c->keyboard_bcast_pending && c->action_type == moving &&
        state->now_ms - c->last_others_move_bcast_ms >= CFG_KEYBOARD_BCAST_MS) {
        keyboard_broadcast_move(state, c);
    }
}

static void handle_show_map(struct state *state, struct connection *conn)
{
    assert(state);
    assert(conn);

    // TODO(fmontenegro): show map. for now (testing) it spawns an orc
    // in front of the player.
    struct character *me = conn->character;
    float a = (float) me->heading * 2.0f * PI_F / 65536.0f;
    struct character *orc = spawn_npc(state,
                                      me->x + (s32) (80.0f * cosf(a)),
                                      me->y + (s32) (80.0f * sinf(a)),
                                      me->z);
    if (!orc) {
        warn("no free slot to spawn an orc." nl);
        return;
    }
    orc->heading = (me->heading + 32768) & 0xffff;
    ptrace("spawning orc!" nl);
    // show it right away to whoever is close (instead of waiting for the
    // next visibility pass).
    for (size_t i = 0; i < countof(state->characters); i++) {
        struct character *p = state->characters + i;
        if (!p->active || !p->in_world || !p->conn)
            continue;
        if (distance_between_characters(p, orc) <= (u64) CFG_VIEW_ENTER * CFG_VIEW_ENTER &&
            p->known_count < CFG_MAX_KNOWN) {
            u32 id = get_character_id(state, orc);
            set_known(p, id, 1);
            p->known_count++;
            send_object_info(state, p, orc);
        }
    }
}
