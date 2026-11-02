/*
 * combat: targeting, auto attack, skills.
 *
 * skills are data driven (skill_defs below). the ids are the official
 * ones so the c4 client shows the right icon/name/animation when it
 * knows the skill. Shadow Step (821) is an interlude skill: a c4 client
 * may show it without icon/animation, the effect works the same.
 */

enum skill_kind {
    skill_physical,     // p.atk + power
    skill_blow,         // dagger blow: positional, chance based, crit
    skill_shadow_step,  // teleport behind the target
    skill_area,         // physical damage around the caster
    skill_self_buff,    // battle roar: max hp + heal
};

enum skill_flags {
    skill_requires_behind = 1 << 0,
    skill_self_target     = 1 << 1,
};

enum skill_learn {
    learn_dagger  = 1 << 0,
    learn_dwarf   = 1 << 1,
    learn_fighter = 1 << 2,
};

struct skill_def {
    u32 id;
    u32 level;
    const char *name;
    enum skill_kind kind;
    u32 power;
    u32 mp;
    u32 hit_ms;
    u32 reuse_ms;
    u32 range;        // cast range (from the edges of both bodies).
    u32 radius;       // area skills.
    u32 stun_ms;
    u32 stun_chance;  // percent.
    u32 flags;
    u32 learn;
};

static const struct skill_def skill_defs[] = {
    // ---------------------------------------------------------- daggers
    {  16, 1, "Mortal Blow",  skill_blow,        120,  9,  800,  3000,  40,   0,    0,  0, 0,                     learn_dagger },
    {  30, 1, "Backstab",     skill_blow,        320, 20, 1000, 12000,  40,   0,    0,  0, skill_requires_behind, learn_dagger },
    { 263, 1, "Deadly Blow",  skill_blow,        240, 18,  900,  8000,  40,   0,    0,  0, 0,                     learn_dagger },
    // teleport behind the target, its attack is interrupted for a moment.
    { 821, 1, "Shadow Step",  skill_shadow_step,   0, 25,  300, 20000, 600,   0,    0,  0, 0,                     learn_dagger },
    // ----------------------------------------------------------- dwarves
    {   3, 1, "Power Strike", skill_physical,     45,  6,  900,  3000,  40,   0,    0,  0, 0,                     learn_dwarf | learn_fighter },
    { 100, 1, "Stun Attack",  skill_physical,     55, 10, 1000,  8000,  40,   0, 3000, 70, 0,                     learn_dwarf },
    { 260, 1, "Hammer Crush", skill_physical,    150, 20, 1100, 12000,  40,   0, 4000, 60, 0,                     learn_dwarf },
    { 245, 1, "Wild Sweep",   skill_area,         90, 25, 1200, 10000,   0, 200,    0,  0, skill_self_target,     learn_dwarf },
    { 190, 1, "Fatal Strike", skill_physical,    260, 22, 1400,  9000,  40,   0,    0,  0, 0,                     learn_dwarf },
    { 121, 1, "Battle Roar",  skill_self_buff,     0, 30, 1500, 60000,   0,   0,    0,  0, skill_self_target,     learn_dwarf },
};

static u32 learn_mask_for(struct character *c)
{
    u32 mask = 0;
    if (is_dagger_class(c->class_id))
        mask |= learn_dagger;
    if (c->race_id == race_dwarf)
        mask |= learn_dwarf;
    if (!is_mystic_class(c->class_id))
        mask |= learn_fighter;
    return mask;
}

static int find_skill(struct character *c, u32 skill_id)
{
    u32 mask = learn_mask_for(c);
    for (u32 i = 0; i < countof(skill_defs); i++)
        if (skill_defs[i].id == skill_id && (skill_defs[i].learn & mask))
            return (int) i;
    return -1;
}

static void send_skill_list(struct state *state, struct character *c)
{
    (void) state;
    if (!c->conn)
        return;
    u32 mask = learn_mask_for(c);
    byte list[1024];
    byte *end = list;
    u32 count = 0;
    for (u32 i = 0; i < countof(skill_defs); i++) {
        if (!(skill_defs[i].learn & mask))
            continue;
        end = bprintf(end, sizeof(list) - (size_t) (end - list),
                      "%u", 0,  // passive
                      "%u", skill_defs[i].level,
                      "%u", skill_defs[i].id);
#if CFG_SKILLLIST_DISABLED_BYTE
        end = bprintf(end, sizeof(list) - (size_t) (end - list), "%c", 0);
#endif
        count++;
    }
    push_response(c->conn, 1,
                  "%h", 0,
                  "%c", 0x58,
                  "%u", count,
                  "%b", (size_t) (end - list), list);
}

static void action_failed(struct character *c)
{
    if (c && c->conn)
        push_response(c->conn, 1,
                      "%h", 0,
                      "%c", 0x25);
}

/* ---------------------------------------------------------------- helpers */

static int is_alive(struct character *c)
{
    return c && c->active && c->in_world && c->action_type != dead && c->current_hp > 0.0;
}

static int is_stunned(struct state *state, struct character *c)
{
    return state->now_ms < c->stunned_until_ms;
}

static float edge_distance(struct character *a, struct character *b)
{
    float d = sqrtf((float) distance_between_characters(a, b));
    d -= (float) (a->collision_radius + b->collision_radius);
    return d < 0.0f ? 0.0f : d;
}

static void face(struct character *a, struct character *b)
{
    if (a->x != b->x || a->y != b->y)
        a->heading = heading_from(b->x - a->x, b->y - a->y);
}

// 2 = attacker is behind the target, 1 = side, 0 = front.
static int relative_position(struct character *attacker, struct character *target)
{
    double to_attacker = atan2((double) (attacker->y - target->y), (double) (attacker->x - target->x));
    double facing = (double) target->heading * 2.0 * 3.14159265358979 / 65536.0;
    double diff = fabs(to_attacker - facing);
    while (diff > 3.14159265358979)
        diff = fabs(diff - 2.0 * 3.14159265358979);
    if (diff > 2.0 * 3.14159265358979 / 3.0)
        return 2;
    if (diff > 3.14159265358979 / 3.0)
        return 1;
    return 0;
}

static void send_attr_status(struct state *state, struct character *from, struct character *to,
                             enum attr_status *status, size_t n)
{
    assert(state);
    assert(status);
    if (!from || !to || !from->active || !to->active || !to->conn)
        return;

    byte attrs[256] = {0};
    byte *end = attrs;

    for (size_t i = 0; i < n; i++) {
        u32 value = 0;
        switch (status[i]) {
            case attr_status_level: value = from->level; break;
            case attr_status_xp: value = from->xp; break;
            case attr_status_str: value = from->attributes.str; break;
            case attr_status_dex: value = from->attributes.dex; break;
            case attr_status_con: value = from->attributes.con; break;
            case attr_status_int: value = from->attributes._int; break;
            case attr_status_wit: value = from->attributes.wit; break;
            case attr_status_men: value = from->attributes.men; break;
            case attr_status_current_hp: value = (u32) from->current_hp; break;
            case attr_status_max_hp: value = (u32) from->max_hp; break;
            case attr_status_current_mp: value = (u32) from->current_mp; break;
            case attr_status_max_mp: value = (u32) from->max_mp; break;
            case attr_status_sp: value = from->sp; break;
            case attr_status_current_load: value = from->current_load; break;
            case attr_status_max_load: value = from->max_load; break;
            case attr_status_p_atk: value = from->p_atk; break;
            case attr_status_atk_speed: value = from->p_atk_speed; break;
            case attr_status_p_def: value = from->p_def; break;
            case attr_status_evasion: value = from->evasion_rate; break;
            case attr_status_accuracy: value = from->accuracy; break;
            case attr_status_critical: value = from->critical_hit; break;
            case attr_status_m_atk: value = from->m_atk; break;
            case attr_status_cast_speed: value = from->m_atk_speed; break;
            case attr_status_m_def: value = from->m_def; break;
            case attr_status_pvp_flag: {
                value = 0;
                if (from->action_type == attacking) {
                    struct character *target = get_character_by_id(state, from->action_payload.attacking.obj_id);
                    if (target && target->active && target->conn)
                        value = 1;
                }
            } break;
            case attr_status_karma: value = from->karma; break;
            case attr_status_current_cp: value = (u32) from->current_cp; break;
            case attr_status_max_cp: value = (u32) from->max_cp; break;
            default:
            warn("invalid status %d" nl, status[i]);
            break;
        }
        end = bprintf(end,
                      sizeof(attrs) - (size_t) (end - attrs),
                      "%u", status[i],
                      "%u", value);
    }

    push_response(to->conn, 1,
                  "%h", 0,
                  "%c", 0x0e,
                  "%u", get_character_id(state, from),
                  "%u", (u32) n,
                  "%b", (size_t) (end - attrs), attrs);
}

static void send_hp(struct state *state, struct character *target, struct character *attacker)
{
    enum attr_status status[] = {attr_status_current_hp, attr_status_max_hp};
    send_attr_status(state, target, target, status, 2);
    if (attacker && attacker != target)
        send_attr_status(state, target, attacker, status, 2);
}

static void send_mp(struct state *state, struct character *c)
{
    enum attr_status status[] = {attr_status_current_mp, attr_status_max_mp};
    send_attr_status(state, c, c, status, 2);
}

/* -------------------------------------------------------------- targeting */

static void select_target(struct state *state, struct character *src, u32 target_id)
{
    assert(state);
    assert(src);

    struct character *target = get_character_by_id(state, target_id);
    if (!target || !target->active || !target->in_world) {
        action_failed(src);
        return;
    }

    src->target_id = target_id;

    push_response(src->conn, 1,
                  "%h", 0,
                  "%c", 0xa6,
                  "%u", target_id,
                  "%h", 0);

    // display correct life and max life.
    enum attr_status status[] = {attr_status_current_hp, attr_status_max_hp};
    send_attr_status(state, target, src, status, 2);

    // validate position.
    if (src->conn)
        send_validate_location(state, src->conn, target);
}

static void handle_action(struct state *state, struct connection *conn, byte *req)
{
    assert(state);
    assert(conn);
    assert(req);

    u32 target_id = 0;
    s32 origin_x = 0;
    s32 origin_y = 0;
    s32 origin_z = 0;
    u8 action_id = 0;

    pscanf(req,
           "%u", &target_id,
           "%u", &origin_x,
           "%u", &origin_y,
           "%u", &origin_z,
           // 0 -> simple click, 1 -> shift
           "%c", &action_id);

    struct character *c = conn->character;
    if (c->action_type == dead || is_stunned(state, c)) {
        action_failed(c);
        return;
    }

    switch (c->action_type) {
        case idle:
        case moving:
        if (c->target_id == target_id) {
            start_interaction(state, c);
            return;
        }
        break;
        case attacking:
        if (c->target_id == target_id)
            return;
        break;
        default:
        break;
    }

    select_target(state, c, target_id);
}

static void handle_attack_request(struct state *state, struct connection *conn, byte *req)
{
    u32 target_id = 0;
    pscanf(req, "%u", &target_id);
    struct character *c = conn->character;
    if (c->action_type == dead || is_stunned(state, c)) {
        action_failed(c);
        return;
    }
    if (c->target_id != target_id)
        select_target(state, c, target_id);
    if (c->target_id == target_id)
        start_interaction(state, c);
}

static void handle_deselect_target(struct state *state, struct connection *conn)
{
    assert(state);
    assert(conn);

    struct character *c = conn->character;
    u32 target_id = c->target_id;
    c->target_id = 0;

    if (!get_character_by_id(state, target_id))
        return;

    if (c->action_type == attacking)
        c->action_type = idle;

    push_response(conn, 1,
                  "%h", 0,
                  "%c", 0x2a,
                  "%u", get_character_id(state, c),
                  "%u", c->x,
                  "%u", c->y,
                  "%u", c->z);
}

/* -------------------------------------------------------------- damage */

static void stop_attacking(struct state *state, struct character *c)
{
    if (c->action_type == attacking && c->action_payload.attacking.next_hit_ms)
        broadcast(state,
                  c, 1,
                  "%h", 0,
                  "%c", 0x2c,
                  "%u", get_character_id(state, c));
    c->action_type = idle;
    c->action_payload = (union action_payload) {0};
}

static void die(struct state *state, struct character *src)
{
    assert(state);
    assert(src);
    update_position(state, src);
    src->current_hp = 0;
    src->action_type = dead;
    src->action_payload = (union action_payload) {0};
    src->prev_action_type = idle;
    src->prev_action_payload = (union action_payload) {0};
    src->dead_since_ms = state->now_ms;
    src->stunned_until_ms = 0;
    broadcast(state,
              src, 1,
              "%h", 0,
              "%c", 0x06,
              "%u", get_character_id(state, src),
              "%u", 0x01);
}

// npcs fight back against whoever hits them.
static void provoke(struct state *state, struct character *attacker, struct character *target)
{
    target->last_attacker_id = get_character_id(state, attacker);
    if (target->conn || !is_alive(target))
        return;
    if (target->action_type == attacking || target->action_type == casting)
        return;
    if (target->action_type == moving && target->action_payload.moving.target_id)
        return;
    target->action_type = attacking;
    target->action_payload = (union action_payload) {0};
    target->action_payload.attacking.obj_id = get_character_id(state, attacker);
    target->target_id = target->action_payload.attacking.obj_id;
}

static void apply_damage(struct state *state, struct character *attacker, struct character *target, u32 damage)
{
    if (!is_alive(target))
        return;
    target->current_hp -= (double) damage;
    if (target->current_hp <= 0.0)
        die(state, target);
    else
        provoke(state, attacker, target);
    send_hp(state, target, attacker);
}

static u32 physical_damage(struct state *state, struct character *a, struct character *t, u32 power, int *crit)
{
    double pdef = t->p_def ? (double) t->p_def : 1.0;
    double dmg = 70.0 * ((double) a->p_atk + (double) power) / pdef;
    dmg *= 0.9 + 0.2 * (double) rndf(state);
    // displayed critical (40 fighter, 90 dagger) -> 8% / 18% chance.
    if (crit) {
        *crit = rndf(state) * 500.0f < (float) a->critical_hit;
        if (*crit)
            dmg *= 2.0;
    }
    return dmg < 1.0 ? 1u : (u32) dmg;
}

static u32 attack_interval_ms(struct character *c)
{
    u32 spd = c->p_atk_speed ? c->p_atk_speed : 300;
    u32 ms = 500000u / spd;
    return ms < 300 ? 300 : ms;
}

static void start_interaction(struct state *state, struct character *src)
{
    assert(state);
    assert(src);

    // an interaction can begin only when the player has selected a target.
    if (!src->target_id)
        return;

    u32 attacker_id = get_character_id(state, src);
    u32 target_id = src->target_id;
    if (attacker_id == target_id)
        return;
    if (!is_alive(get_character_by_id(state, target_id))) {
        action_failed(src);
        return;
    }

    update_position(state, src);
    src->action_type = attacking;
    src->action_payload = (union action_payload) {0};
    src->action_payload.attacking.obj_id = target_id;

    attacking_update(state, src);
}

static void attacking_update(struct state *state, struct character *attacker)
{
    u32 attacker_id = get_character_id(state, attacker);
    u32 target_id = attacker->action_payload.attacking.obj_id;
    struct character *target = get_character_by_id(state, target_id);

    if (!is_alive(target) || !is_alive(attacker)) {
        stop_attacking(state, attacker);
        return;
    }
    if (is_stunned(state, attacker) || state->now_ms < attacker->disoriented_until_ms)
        return;

    u32 atk_range = 40;
    // check if the attacker is in attack range. if not, walk closer.
    if (edge_distance(attacker, target) > (float) atk_range) {
        u32 offset = atk_range + (u32) (attacker->collision_radius + target->collision_radius) - 10;
        move_to(state, attacker, target->x, target->y, target->z, offset, target_id, 1);
        return;
    }

    if (!attacker->action_payload.attacking.next_hit_ms) {
        // entering combat stance (sent once, not on every swing).
        broadcast(state,
                  attacker, 1,
                  "%h", 0,
                  "%c", 0x2b,
                  "%u", attacker_id);
        attacker->action_payload.attacking.next_hit_ms = state->now_ms;
    }
    if (state->now_ms < attacker->action_payload.attacking.next_hit_ms)
        return;

    face(attacker, target);
    int crit = 0;
    u32 damage = physical_damage(state, attacker, target, 0, &crit);
    broadcast(state,
              attacker, 1,
              "%h", 0,
              "%c", 0x05,
              "%u", attacker_id,
              "%u", target_id,
              "%u", damage,
              // flags: 0x20 critical
              "%c", crit ? 0x20 : 0,
              "%u", attacker->x,
              "%u", attacker->y,
              "%u", attacker->z,
              "%h", 0);
    apply_damage(state, attacker, target, damage);
    attacker->action_payload.attacking.next_hit_ms = state->now_ms + attack_interval_ms(attacker);
}

/* ------------------------------------------------------------------ skills */

static void stun(struct state *state, struct character *target, u32 ms)
{
    if (!is_alive(target))
        return;
    int was_stunned = is_stunned(state, target);
    target->stunned_until_ms = state->now_ms + ms;
    // a stunned character stops what it was doing.
    if (target->action_type == moving) {
        stop_moving(state, target, 1);
    } else if (target->action_type == casting) {
        target->action_type = idle;
    }
    if (target->action_type == attacking)
        target->action_payload.attacking.next_hit_ms = target->stunned_until_ms;
    if (!was_stunned && !target->conn)
        broadcast_char_info(state, target);
}

static void shadow_step(struct state *state, struct character *c, struct character *target)
{
    // land right behind the target, facing the same way it faces.
    float facing = (float) target->heading * 2.0f * PI_F / 65536.0f;
    float back = (float) (target->collision_radius + c->collision_radius + 12.0);
    if (c->action_type == moving)
        stop_moving(state, c, 0);
    c->x = target->x - (s32) (cosf(facing) * back);
    c->y = target->y - (s32) (sinf(facing) * back);
    c->z = target->z;
    c->heading = target->heading;
    c->last_correction_ms = state->now_ms;
    // ValidateLocation snaps the object on every client (the caster
    // included) without a loading screen, unlike TeleportToLocation.
    broadcast(state,
              c, 1,
              "%h", 0,
              "%c", 0x61,
              "%u", get_character_id(state, c),
              "%u", c->x,
              "%u", c->y,
              "%u", c->z,
              "%u", c->heading);
    // the target loses track of you for a moment: it won't turn around
    // nor hit for CFG_SHADOW_STEP_DISORIENT_MS. that's the window for a
    // backstab.
    provoke(state, c, target);
    target->disoriented_until_ms = state->now_ms + CFG_SHADOW_STEP_DISORIENT_MS;
}

static void skill_effect(struct state *state, struct character *c, const struct skill_def *def, struct character *target)
{
    switch (def->kind) {
    case skill_physical: {
        int crit = 0;
        u32 damage = physical_damage(state, c, target, def->power, &crit);
        apply_damage(state, c, target, damage);
        if (def->stun_ms && is_alive(target) && rndf(state) * 100.0f < (float) def->stun_chance)
            stun(state, target, def->stun_ms);
    } break;
    case skill_blow: {
        int pos = relative_position(c, target);
        // blows land for sure from behind, often from the side.
        float chance = pos == 2 ? 1.0f : pos == 1 ? 0.75f : 0.55f;
        if (rndf(state) >= chance) {
            apply_damage(state, c, target, 0);
            break;
        }
        float bonus = pos == 2 ? 1.5f : pos == 1 ? 1.2f : 1.0f;
        u32 damage = physical_damage(state, c, target, def->power, 0);
        damage = (u32) ((float) damage * 1.5f * bonus);
        apply_damage(state, c, target, damage);
    } break;
    case skill_shadow_step:
        shadow_step(state, c, target);
        break;
    case skill_area: {
        u64 r = (u64) def->radius;
        for (size_t i = 0; i < countof(state->characters); i++) {
            struct character *other = state->characters + i;
            if (other == c || other->conn || !is_alive(other))
                continue;
            if (distance_between_characters(c, other) > r * r)
                continue;
            int crit = 0;
            apply_damage(state, c, other, physical_damage(state, c, other, def->power, &crit));
        }
    } break;
    case skill_self_buff: {
        if (!c->abnormal_effect) {
            // the roar stays for the session (abnormal_effect as a flag).
            c->max_hp *= 1.10;
            c->abnormal_effect = 1;
        }
        c->current_hp += c->max_hp * 0.15;
        if (c->current_hp > c->max_hp)
            c->current_hp = c->max_hp;
        send_hp(state, c, 0);
    } break;
    }
}

static int skill_needs_target(const struct skill_def *def)
{
    return !(def->flags & skill_self_target);
}

static void start_cast(struct state *state, struct character *c, u32 index, struct character *target)
{
    const struct skill_def *def = skill_defs + index;
    if (c->action_type == moving)
        stop_moving(state, c, 0);
    update_position(state, c);
    if (target != c)
        face(c, target);

    c->current_mp -= (double) def->mp;
    c->skill_reuse_until_ms[index] = state->now_ms + def->reuse_ms;
    send_mp(state, c);

    c->action_type = casting;
    c->action_payload = (union action_payload) {0};
    c->action_payload.casting.skill_index = index;
    c->action_payload.casting.target_id = get_character_id(state, target);
    c->action_payload.casting.hit_at_ms = state->now_ms + def->hit_ms;

    broadcast(state,
              c, 1,
              "%h", 0,
              "%c", 0x48,
              "%u", get_character_id(state, c),
              "%u", get_character_id(state, target),
              "%u", def->id,
              "%u", def->level,
              "%u", def->hit_ms,
              "%u", def->reuse_ms,
              "%u", c->x,
              "%u", c->y,
              "%u", c->z,
              "%h", 0,
              "%h", 0);
}

// checks everything except range. returns the target or 0.
static struct character *cast_check(struct state *state, struct character *c, u32 index, u32 target_id)
{
    const struct skill_def *def = skill_defs + index;
    if (!is_alive(c) || is_stunned(state, c))
        return 0;
    if (state->now_ms < c->skill_reuse_until_ms[index])
        return 0;
    if (c->current_mp < (double) def->mp)
        return 0;
    if (!skill_needs_target(def))
        return c;
    struct character *target = get_character_by_id(state, target_id);
    if (!is_alive(target) || target == c)
        return 0;
    if ((def->flags & skill_requires_behind) && relative_position(c, target) != 2 &&
        edge_distance(c, target) <= (float) def->range + 20.0f)
        return 0;
    return target;
}

static void handle_magic_skill_use(struct state *state, struct connection *conn, byte *req)
{
    u32 skill_id = 0;
    u32 ctrl = 0;
    u8 shift = 0;
    pscanf(req, "%u", &skill_id, "%u", &ctrl, "%c", &shift);

    struct character *c = conn->character;
    int index = find_skill(c, skill_id);
    if (index < 0 || c->action_type == casting) {
        action_failed(c);
        return;
    }
    const struct skill_def *def = skill_defs + index;
    struct character *target = cast_check(state, c, (u32) index, c->target_id);
    if (!target) {
        action_failed(c);
        return;
    }

    update_position(state, c);
    if (target != c && edge_distance(c, target) > (float) def->range) {
        // walk closer, then cast (queued action).
        c->action_type = casting;
        c->action_payload = (union action_payload) {0};
        c->action_payload.casting.skill_index = (u32) index;
        c->action_payload.casting.target_id = get_character_id(state, target);
        c->action_payload.casting.hit_at_ms = 0;
        u32 offset = def->range + (u32) (c->collision_radius + target->collision_radius);
        offset = offset > 10 ? offset - 10 : offset;
        move_to(state, c, target->x, target->y, target->z, offset,
                get_character_id(state, target), 1);
        return;
    }
    start_cast(state, c, (u32) index, target);
}

static void casting_update(struct state *state, struct character *c)
{
    struct action_casting *cast = &c->action_payload.casting;
    const struct skill_def *def = skill_defs + cast->skill_index;

    if (!cast->hit_at_ms) {
        // just arrived after walking closer.
        struct character *target = cast_check(state, c, cast->skill_index, cast->target_id);
        if (!target || (target != c && edge_distance(c, target) > (float) def->range + 30.0f)) {
            c->action_type = idle;
            action_failed(c);
            return;
        }
        start_cast(state, c, cast->skill_index, target);
        return;
    }
    if (is_stunned(state, c)) {
        c->action_type = idle;
        return;
    }
    if (state->now_ms < cast->hit_at_ms)
        return;

    struct character *target = skill_needs_target(def) ? get_character_by_id(state, cast->target_id) : c;
    int ok = is_alive(target);
    if (ok && target != c)
        ok = edge_distance(c, target) <= (float) def->range + 120.0f;
    if (ok) {
        skill_effect(state, c, def, target);
        broadcast(state,
                  c, 1,
                  "%h", 0,
                  "%c", 0x76,
                  "%u", get_character_id(state, c),
                  "%u", def->id,
                  "%u", def->level,
                  "%u", 1,
                  "%u", get_character_id(state, target));
    }

    // like l2: after an offensive skill, keep auto attacking the target.
    c->action_type = idle;
    c->action_payload = (union action_payload) {0};
    if (ok && target != c && is_alive(target) && is_alive(c)) {
        c->action_type = attacking;
        c->action_payload.attacking.obj_id = get_character_id(state, target);
        c->action_payload.attacking.next_hit_ms = state->now_ms + 200;
    }
}
