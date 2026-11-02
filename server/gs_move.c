/*
 * movement.
 *
 * - positions are interpolated from real time (ms), so they are right
 *   between ticks and independent of CFG_TICK_MS.
 * - the client position is trusted within CFG_DESYNC_TOLERANCE (smooth,
 *   no rubber banding) and corrected beyond it (no teleport hacks). the
 *   original sent a ValidateLocation to the moving player on EVERY tick
 *   while it was in sync (the comparison was inverted), which made the
 *   client snap/stutter while walking.
 * - keyboard movement (arrow keys / wasd, `movement == 0` in the move
 *   packet) is first class: the mover always gets an immediate echo, the
 *   rest of the players get direction changes throttled, and the far
 *   point the client sends is clamped.
 */

static void update_position(struct state *state, struct character *c)
{
    if (c->action_type != moving)
        return;
    struct action_moving *m = &c->action_payload.moving;
    u64 t = state->now_ms - m->start_ms;
    if (!m->duration_ms || t >= m->duration_ms) {
        c->x = m->target_x;
        c->y = m->target_y;
        c->z = m->target_z;
        return;
    }
    float f = (float) t / (float) m->duration_ms;
    c->x = m->src_x + (s32) ((float) (m->target_x - m->src_x) * f);
    c->y = m->src_y + (s32) ((float) (m->target_y - m->src_y) * f);
    c->z = m->src_z + (s32) ((float) (m->target_z - m->src_z) * f);
}

static void send_validate_location(struct state *state, struct connection *to, struct character *c)
{
    push_response(to, 1,
                  "%h", 0,
                  "%c", 0x61,
                  "%u", get_character_id(state, c),
                  "%u", c->x,
                  "%u", c->y,
                  "%u", c->z,
                  "%u", c->heading);
}

static void keyboard_broadcast_move(struct state *state, struct character *c)
{
    struct action_moving *m = &c->action_payload.moving;
    update_position(state, c);
    broadcast_others(state,
                     c, 1,
                     "%h", 0,
                     "%c", 0x01,
                     "%u", get_character_id(state, c),
                     "%u", m->target_x,
                     "%u", m->target_y,
                     "%u", m->target_z,
                     "%u", c->x,
                     "%u", c->y,
                     "%u", c->z);
    float dx = (float) (m->target_x - c->x);
    float dy = (float) (m->target_y - c->y);
    float len = sqrtf(dx * dx + dy * dy);
    if (len > 0.0f) {
        c->last_bcast_dir_x = dx / len;
        c->last_bcast_dir_y = dy / len;
    }
    c->last_others_move_bcast_ms = state->now_ms;
    c->keyboard_bcast_pending = 0;
}

static void move_to_ex(struct state *state, struct character *character,
                       s32 x, s32 y, s32 z, u32 offset, u32 target_id,
                       int queue_action, int keyboard)
{
    assert(state);
    assert(character);

    update_position(state, character);

    if (queue_action && character->action_type != moving) {
        character->prev_action_type = character->action_type;
        character->prev_action_payload = character->action_payload;
    }

    int was_keyboard = character->action_type == moving &&
                       character->action_payload.moving.keyboard;

    character->action_type = moving;
    character->action_payload = (union action_payload) {0};

    struct action_moving *m = &character->action_payload.moving;
    m->queue_action = queue_action;
    m->offset = offset;
    m->target_id = target_id;
    m->keyboard = keyboard;
    m->src_x = character->x;
    m->src_y = character->y;
    m->src_z = character->z;

    float dx = (float) (x - character->x);
    float dy = (float) (y - character->y);
    float dz = (float) (z - character->z);
    float d = sqrtf(dx * dx + dy * dy + dz * dz);
    float travel = d - (float) offset;
    if (travel < 0.0f)
        travel = 0.0f;

    if (d > 0.0f) {
        m->target_x = character->x + (s32) (dx / d * travel);
        m->target_y = character->y + (s32) (dy / d * travel);
        m->target_z = character->z + (s32) (dz / d * travel);
        character->heading = heading_from((s32) dx, (s32) dy);
    } else {
        m->target_x = character->x;
        m->target_y = character->y;
        m->target_z = character->z;
    }

    float speed = (float) (character->run_speed ? character->run_speed : 1);
    m->duration_ms = (u64) (travel * 1000.0f / speed);
    m->start_ms = state->now_ms;
    m->last_repath_ms = state->now_ms;

    if (target_id) {
        character->keyboard_moving = 0;
        character->keyboard_bcast_pending = 0;
        broadcast(state,
                  character, 1,
                  "%h", 0,
                  "%c", 0x60,
                  "%u", get_character_id(state, character),
                  "%u", target_id,
                  "%u", offset,
                  "%u", character->x,
                  "%u", character->y,
                  "%u", character->z);
        return;
    }

    if (!keyboard || !character->conn) {
        character->keyboard_moving = 0;
        character->keyboard_bcast_pending = 0;
        broadcast(state,
                  character, 1,
                  "%h", 0,
                  "%c", 0x01,
                  "%u", get_character_id(state, character),
                  "%u", m->target_x,
                  "%u", m->target_y,
                  "%u", m->target_z,
                  "%u", character->x,
                  "%u", character->y,
                  "%u", character->z);
        return;
    }

    // keyboard: the mover always gets an immediate answer, so the input
    // feels instant on its screen.
    push_response(character->conn, 1,
                  "%h", 0,
                  "%c", 0x01,
                  "%u", get_character_id(state, character),
                  "%u", m->target_x,
                  "%u", m->target_y,
                  "%u", m->target_z,
                  "%u", character->x,
                  "%u", character->y,
                  "%u", character->z);

    // everybody else: only when the direction really changed or every
    // CFG_KEYBOARD_BCAST_MS. holding a key and steering with the camera
    // makes the client send many tiny corrections; observers don't need
    // all of them, and their client doesn't have to process them.
    float ndx = 0.0f, ndy = 0.0f;
    float len = sqrtf(dx * dx + dy * dy);
    if (len > 0.0f) {
        ndx = dx / len;
        ndy = dy / len;
    }
    float dot = ndx * character->last_bcast_dir_x + ndy * character->last_bcast_dir_y;
    float cos_limit = cosf((float) CFG_KEYBOARD_BCAST_ANGLE * PI_F / 180.0f);
    int turned = dot < cos_limit;
    int stale = state->now_ms - character->last_others_move_bcast_ms >= CFG_KEYBOARD_BCAST_MS;
    if (!was_keyboard || !character->keyboard_moving || turned || stale)
        keyboard_broadcast_move(state, character);
    else
        character->keyboard_bcast_pending = 1;
    character->keyboard_moving = 1;
}

static void move_to(struct state *state, struct character *character, s32 x, s32 y, s32 z, u32 offset, u32 target_id, int queue_action)
{
    move_to_ex(state, character, x, y, z, offset, target_id, queue_action, 0);
}

static void stop_moving(struct state *state, struct character *c, int notify_self)
{
    update_position(state, c);
    c->action_type = idle;
    c->action_payload = (union action_payload) {0};
    c->prev_action_type = idle;
    c->prev_action_payload = (union action_payload) {0};
    c->keyboard_moving = 0;
    c->keyboard_bcast_pending = 0;
    if (notify_self)
        broadcast(state,
                  c, 1,
                  "%h", 0,
                  "%c", 0x47,
                  "%u", get_character_id(state, c),
                  "%u", c->x,
                  "%u", c->y,
                  "%u", c->z,
                  "%u", c->heading);
    else
        broadcast_others(state,
                         c, 1,
                         "%h", 0,
                         "%c", 0x47,
                         "%u", get_character_id(state, c),
                         "%u", c->x,
                         "%u", c->y,
                         "%u", c->z,
                         "%u", c->heading);
}

static void moving_update(struct state *state, struct character *character)
{
    assert(state);
    assert(character);

    struct action_moving *m = &character->action_payload.moving;
    update_position(state, character);
    int done = state->now_ms - m->start_ms >= m->duration_ms;

    if (m->target_id) {
        struct character *target = get_character_by_id(state, m->target_id);

        // stop movement if the target is no longer valid.
        if (!target || !target->active || !target->in_world) {
            stop_moving(state, character, 1);
            return;
        }

        // arrived next to the target: continue with the queued action.
        u64 d2 = distance_between_characters(character, target);
        u64 reach = (u64) m->offset + 4;
        if (d2 <= reach * reach) {
            m->target_x = character->x;
            m->target_y = character->y;
            m->target_z = character->z;
            goto arrived;
        }

        // the target moved: re-path, but not every tick (the original
        // re-sent MoveToPawn + a full NpcInfo every single update).
        u64 drift = distance_between(m->target_x, m->target_y, m->target_z,
                                     target->x, target->y, target->z);
        u64 drift_limit = (u64) m->offset + CFG_FOLLOW_REPATH_DIST;
        int repath_due = state->now_ms - m->last_repath_ms >= CFG_FOLLOW_REPATH_MS;
        if (done || (repath_due && drift > drift_limit * drift_limit)) {
            move_to(state, character, target->x, target->y, target->z,
                    m->offset, m->target_id, 0);
        }
        return;
    }

    if (!done)
        return;

arrived:
    character->x = m->target_x;
    character->y = m->target_y;
    character->z = m->target_z;

    int was_keyboard = m->keyboard;
    character->action_type = character->prev_action_type;
    character->action_payload = character->prev_action_payload;

    character->prev_action_type = idle;
    character->prev_action_payload = (union action_payload) {0};
    character->keyboard_moving = 0;
    character->keyboard_bcast_pending = 0;

    // final position for the observers (not for the mover itself, it
    // already is there on its screen).
    if (!was_keyboard)
        broadcast_others(state,
                         character, 1,
                         "%h", 0,
                         "%c", 0x61,
                         "%u", get_character_id(state, character),
                         "%u", character->x,
                         "%u", character->y,
                         "%u", character->z,
                         "%u", character->heading);

    // when arrived, if there was a queued action (walk closer -> attack
    // or cast), run it right away instead of waiting for the next tick.
    if (character->action_type != moving)
        character_update(state, character);
}

static int position_acceptable(struct character *c, s32 x, s32 y, s32 z)
{
    u64 tol = CFG_DESYNC_TOLERANCE;
    return distance_between(c->x, c->y, c->z, x, y, z) <= tol * tol;
}

static void correct_client(struct state *state, struct character *c)
{
    if (!c->conn)
        return;
    if (state->now_ms - c->last_correction_ms < CFG_CORRECTION_COOLDOWN_MS)
        return;
    c->last_correction_ms = state->now_ms;
    send_validate_location(state, c->conn, c);
}

static void handle_movement(struct state *state, struct connection *conn, byte *req)
{
    assert(state);
    assert(conn);
    assert(req);

    s32 target_x = 0;
    s32 target_y = 0;
    s32 target_z = 0;
    s32 origin_x = 0;
    s32 origin_y = 0;
    s32 origin_z = 0;
    s32 movement = 1;

    pscanf(req,
           "%u", &target_x,
           "%u", &target_y,
           "%u", &target_z,
           "%u", &origin_x,
           "%u", &origin_y,
           "%u", &origin_z,
           "%u", &movement);

    struct character *c = conn->character;
    if (c->action_type == dead || state->now_ms < c->stunned_until_ms) {
        action_failed(c);
        return;
    }

    update_position(state, c);
    // the original trusted the origin blindly (free teleport).
    if (position_acceptable(c, origin_x, origin_y, origin_z)) {
        c->x = origin_x;
        c->y = origin_y;
        c->z = origin_z;
    } else {
        c->last_correction_ms = 0;
        correct_client(state, c);
    }
    c->last_client_update_ms = state->now_ms;

    // 0 = cursor keys / wasd, 1 = mouse.
    int keyboard = movement == 0;
    if (keyboard) {
        float dx = (float) (target_x - c->x);
        float dy = (float) (target_y - c->y);
        float len = sqrtf(dx * dx + dy * dy);
        if (len > CFG_KEYBOARD_MAX_LEAD) {
            target_x = c->x + (s32) (dx / len * CFG_KEYBOARD_MAX_LEAD);
            target_y = c->y + (s32) (dy / len * CFG_KEYBOARD_MAX_LEAD);
        }
    }

    // moving cancels whatever was queued (attack after approach, cast).
    c->prev_action_type = idle;
    c->prev_action_payload = (union action_payload) {0};
    if (c->action_type == casting || c->action_type == attacking)
        c->action_type = idle;

    if (keyboard)
        c->last_keyboard_move_ms = state->now_ms;
    move_to_ex(state, c, target_x, target_y, target_z, 0, 0, 0, keyboard);
}

static void handle_validate_position(struct state *state, struct connection *conn, byte *req)
{
    assert(state);
    assert(conn);
    assert(req);

    s32 x = 0, y = 0, z = 0;
    u32 heading = 0;
    pscanf(req,
           "%u", &x,
           "%u", &y,
           "%u", &z,
           "%u", &heading);

    struct character *c = conn->character;
    c->client_x = x;
    c->client_y = y;
    c->last_client_update_ms = state->now_ms;
    update_position(state, c);

#ifdef L2_TEST_TELEPORT
    // test builds only: lets the bots spread around the map instantly.
    if (heading == 0x7e57) {
        c->x = x;
        c->y = y;
        c->z = z;
        if (c->action_type == moving)
            c->action_type = idle;
        return;
    }
#endif

    int client_still = c->last_validate_x == x && c->last_validate_y == y;
    c->last_validate_x = x;
    c->last_validate_y = y;

    if (!position_acceptable(c, x, y, z)) {
        correct_client(state, c);
        return;
    }

    // keyboard released: the client stands still but we are walking it.
    if (client_still && c->action_type == moving && c->action_payload.moving.keyboard &&
        state->now_ms - c->last_keyboard_move_ms >= CFG_KEYBOARD_RELEASE_MS) {
        c->x = x;
        c->y = y;
        c->z = z;
        c->action_payload.moving.target_x = x;
        c->action_payload.moving.target_y = y;
        c->action_payload.moving.target_z = z;
        stop_moving(state, c, 0);
        return;
    }
    if (c->action_type != moving)
        c->heading = (s32) (heading & 0xffff);

    // in sync: follow the client, it's the one drawing the character.
    // when moving, re-base the interpolation from the client position so
    // the server doesn't slowly drift away from it.
    c->x = x;
    c->y = y;
    c->z = z;
    if (c->action_type == moving) {
        struct action_moving *m = &c->action_payload.moving;
        float dx = (float) (m->target_x - x);
        float dy = (float) (m->target_y - y);
        float dz = (float) (m->target_z - z);
        float left = sqrtf(dx * dx + dy * dy + dz * dz);
        float speed = (float) (c->run_speed ? c->run_speed : 1);
        m->src_x = x;
        m->src_y = y;
        m->src_z = z;
        m->start_ms = state->now_ms;
        m->duration_ms = (u64) (left * 1000.0f / speed);
    }
}

static void handle_cannot_move_anymore(struct state *state, struct connection *conn, byte *req)
{
    s32 x = 0, y = 0, z = 0;
    u32 heading = 0;
    pscanf(req,
           "%u", &x,
           "%u", &y,
           "%u", &z,
           "%u", &heading);

    struct character *c = conn->character;
    update_position(state, c);
    if (position_acceptable(c, x, y, z)) {
        c->x = x;
        c->y = y;
        c->z = z;
    } else {
        correct_client(state, c);
    }
    c->heading = (s32) (heading & 0xffff);
    if (c->action_type == moving && !c->action_payload.moving.target_id)
        stop_moving(state, c, 0);
}
