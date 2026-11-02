// letters and digits only, 1..16 characters (used in file paths).
static int safe_name(const wchar_t *name)
{
    size_t n = 0;
    for (; name[n]; n++) {
        wchar_t ch = name[n];
        int ok = (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') ||
                 (ch >= L'0' && ch <= L'9') || ch == L'_';
        if (!ok || n >= 16)
            return 0;
    }
    return n > 0;
}

static void handle_send_protocol(struct state *state, struct connection *conn)
{
    assert(state);
    assert(conn);
    
    byte type = 0x00;
    byte protocol[] = {
        0x01,
        // crypt key
        0x94,
        0x35,
        0x00,
        0x00,
        0xa1,
        0x6c,
        0x54,
        0x87,
    };
    
    push_response(conn, 0,
                  "%h", 0,
                  "%c", type,
                  "%b", sizeof(protocol), protocol);
}

static void handle_auth(struct state *state, struct connection *conn, byte *req)
{
    assert(state);
    assert(conn);
    assert(req);
    
    pscanf(req,
           "%ls", countof(conn->username), conn->username,
           "%u", &conn->play_ok2,
           "%u", &conn->play_ok1,
           "%u", &conn->login_ok1,
           "%u", &conn->login_ok2);

    ptrace("username is %ls" nl, conn->username);
    if (!safe_name(conn->username)) {
        warn("rejected unsafe account name." nl);
        kick(conn);
        return;
    }

    char access_path[512] = {0};
    snprintf(access_path, 
             sizeof(access_path) - 1, 
             "data/accounts/%ls/access.txt",
             conn->username);
    FILE *access_file = fopen(access_path, "r");
    if (!access_file) {
        error("unable to read %s. can't check if this is a valid auth."
              "the user will be dropped." nl, 
              access_path);
        kick(conn);
        return;
    }
    
    u32 stored_login_ok1 = 0;
    u32 stored_login_ok2 = 0;
    u32 stored_created_at = 0;
    u32 stored_valid_until = 0;
    fscanf(access_file, 
           "login_ok1=%u" nl
           "login_ok2=%u" nl
           "created_at=%u" nl
           "valid_until=%u" nl,
           &stored_login_ok1,
           &stored_login_ok2,
           &stored_created_at,
           &stored_valid_until);
    
    if (stored_login_ok1 != conn->login_ok1 || stored_login_ok2 != conn->login_ok2) {
        warn("invalid loginok1 or loginok2 from %ls. the connection will be dropped." nl, 
             conn->username);
        kick(conn);
        return;
    }
    
    if (stored_valid_until < time(0)) {
        trace("keys expired for %ls. the connection will be dropped." nl,
              conn->username);
        kick(conn);
        return;
    }
    
    handle_send_character_list(state, conn);
}

static void handle_show_create_character_screen(struct state *state, struct connection *conn)
{
    assert(state);
    assert(conn);
    
    byte type = 0x17;
    u32 count = 0;
    
    push_response(conn, 1,
                  "%h", 0,
                  "%c", type,
                  "%u", count);
}

static void handle_character_creation(struct state *state, struct connection *conn, byte *req)
{
    assert(state);
    assert(conn);
    
    wchar_t name[32] = {0};
    u32 race_id = 0;
    u32 sex = 0;
    u32 class_id = 0;
    u32 ignore = 0;
    u32 hair_style_id = 0;
    u32 hair_color_id = 0;
    u32 face_id = 0;
    
    pscanf(req, 
           "%ls", countof(name), name,
           "%u", &race_id,
           "%u", &sex,
           "%u", &class_id,
           "%u", &ignore,
           "%u", &ignore,
           "%u", &ignore,
           "%u", &ignore,
           "%u", &ignore,
           "%u", &ignore,
           "%u", &hair_style_id,
           "%u", &hair_color_id,
           "%u", &face_id);
    
    // names end up in file paths: "../../whatever" let a client write
    // files anywhere the server can.
    if (!safe_name(name)) {
        warn("rejected character name from %ls." nl, conn->username);
        push_response(conn, 1,
                      "%h", 0,
                      "%c", 0x1a,   // CharCreateFail
                      "%u", 0x04);  // reason: incorrect name
        return;
    }

    // TODO(fmontenegro): check if name exists
    
    char characters_path[512] = {0};
    snprintf(characters_path, 
             sizeof(characters_path) - 1, 
             "data/accounts/%ls/characters",
             conn->username);
    directory_create(characters_path);
    
    char info_path[1024] = {0};
    snprintf(info_path, 
             sizeof(info_path) - 1, 
             "data/accounts/%ls/characters/%ls.txt",
             conn->username,
             name);
    
    FILE *info_file = fopen(info_path, "w");
    if (!info_file) {
        error("failed to store data for new characters in %s" nl,
              info_path);
        return;
    }
    
    s32 x = -83968;
    s32 y = 244634;
    s32 z = -3730;
    u32 level = 1;
    u32 xp = 1;
    u32 str = 10;
    u32 dex = 10;
    u32 con = 10;
    u32 _int = 10;
    u32 wit = 10;
    u32 men = 10;
    double max_hp = 500.0;
    double current_hp = 500.0;
    double max_mp = 500.0;
    double current_mp = 500.0;
    double max_cp = 500.0;
    double current_cp = 500.0;
    u32 sp = 1;
    u32 current_load = 0;
    u32 max_load = 100;
    u32 p_atk = 10;
    u32 p_atk_speed = 10;
    u32 p_def = 10;
    u32 evasion_rate = 10;
    u32 accuracy = 10;
    u32 critical_hit = 10;
    u32 m_atk = 10;
    u32 m_atk_speed = 10;
    u32 m_def = 10;
    u32 karma = 10;
    u32 run_speed = 20;
    u32 walk_speed = 10;
    double collision_radius = 20.0;
    double collision_height = 20.0;
    u32 access_level = 1;
    wchar_t title[32] = L"l2auth";
    u32 clan_id = 0;
    u32 crest_id = 0;
    u32 ally_id = 0;
    u32 ally_crest_id = 0;
    u32 pk_kills = 0;
    u32 pvp_kills = 0;
    u16 recommendations_left = 0;
    u16 recommendations_have = 32;
    u32 inventory_limit = 10;
    u32 clan_crest_large_id = 0;
    u8 hero_symbol = 0;
    u8 hero = 0;
    s32 fish_x = 0;
    s32 fish_y = 0;
    s32 fish_z = 0;
    u32 name_color = 0xffffff;
    
    fprintf(info_file,
            "name=%ls" nl
            "race_id=%u" nl
            "sex=%u" nl
            "class_id=%u" nl
            "hair_style_id=%u" nl
            "hair_color_id=%u" nl
            "face_id=%u" nl
            "x=%d" nl
            "y=%d" nl
            "z=%d" nl
            "level=%u" nl
            "xp=%u" nl
            "str=%u" nl
            "dex=%u" nl
            "con=%u" nl
            "int=%u" nl
            "wit=%u" nl
            "men=%u" nl
            "max_hp=%lf" nl
            "current_hp=%lf" nl
            "max_mp=%lf" nl
            "current_mp=%lf" nl
            "max_cp=%lf" nl
            "current_cp=%lf" nl
            "sp=%u" nl
            "current_load=%u" nl
            "max_load=%u" nl
            "p_atk=%u" nl
            "p_atk_speed=%u" nl
            "p_def=%u" nl
            "evasion_rate=%u" nl
            "accuracy=%u" nl
            "critical_hit=%u" nl
            "m_atk=%u" nl
            "m_atk_speed=%u" nl
            "m_def=%u" nl
            "karma=%u" nl
            "run_speed=%u" nl
            "walk_speed=%u" nl
            "collision_radius=%lf" nl
            "collision_height=%lf" nl
            "access_level=%u" nl
            "title=%ls" nl
            "clan_id=%u" nl
            "crest_id=%u" nl
            "ally_id=%u" nl
            "ally_crest_id=%u" nl
            "pk_kills=%u" nl
            "pvp_kills=%u" nl
            "recommendations_left=%u" nl
            "recommendations_have=%u" nl
            "inventory_limit=%u" nl
            "clan_crest_large_id=%u" nl
            "hero_symbol=%u" nl
            "hero=%u" nl
            "fish_x=%d" nl
            "fish_y=%d" nl
            "fish_z=%d" nl
            "name_color=%u" nl,
            name,
            race_id,
            sex,
            class_id,
            hair_style_id,
            hair_color_id,
            face_id,
            x,
            y,
            z,
            level,
            xp,
            str,
            dex,
            con,
            _int,
            wit,
            men,
            max_hp,
            current_hp,
            max_mp,
            current_mp,
            max_cp,
            current_cp,
            sp,
            current_load,
            max_load,
            p_atk,
            p_atk_speed,
            p_def,
            evasion_rate,
            accuracy,
            critical_hit,
            m_atk,
            m_atk_speed,
            m_def,
            karma,
            run_speed,
            walk_speed,
            collision_radius,
            collision_height,
            access_level,
            title,
            clan_id,
            crest_id,
            ally_id,
            ally_crest_id,
            pk_kills,
            pvp_kills,
            recommendations_left,
            recommendations_have,
            inventory_limit,
            clan_crest_large_id,
            hero_symbol,
            hero,
            fish_x,
            fish_y,
            fish_z,
            name_color);
    fclose(info_file);
    
    byte type = 0x19;
    u32 success = 1;
    push_response(conn, 1,
                  "%h", 0,
                  "%c", type,
                  "%u", success);
    
    handle_send_character_list(state, conn);
}

static void handle_send_character_list(struct state *state, struct connection *conn)
{
    assert(state);
    assert(conn);
    
    byte type = 0x13;
    u32 count = 0;
    
    char characters_path[512] = {0};
    snprintf(characters_path, 
             sizeof(characters_path) - 1, 
             "data/accounts/%ls/characters",
             conn->username);
    
    in_directory(characters_path)
        count++;
    
    byte list[1024] = {0};
    byte *end = list;
    
    in_directory(characters_path) {
        FILE *character_file = fopen(directory.full_path, "r");
        if (!character_file)
            continue;
        
        u32 id = 0;
        wchar_t name[32];
        u32 clan_id = 0;
        u32 sex = 0;
        u32 race_id = 0;
        // u32 base_race_id = 0;
        u32 active = 1;
        double current_hp = 500;
        double current_mp = 500;
        u32 sp = 0;
        u32 xp = 0;
        u32 level = 1;
        u32 karma = 0;
        u32 hair_style_id = 0;
        u32 hair_color_id = 0;
        u32 face_id = 0;
        double max_hp = 500;
        double max_mp = 500;
        u32 delete_time = 0;
        u32 class_id = 0;
        u32 auto_select = 0;
        u8 enchant_effect = 0;
        
        fscanf(character_file, 
               "name=%ls" nl
               "race_id=%u" nl
               "sex=%u" nl
               "class_id=%u" nl
               "hair_style_id=%u" nl
               "hair_color_id=%u" nl
               "face_id=%u" nl,
               name,
               &race_id,
               &sex,
               &class_id,
               &hair_style_id,
               &hair_color_id,
               &face_id);
        
        end = bprintf(end, sizeof(list) - (size_t) (end - list),
                      "%ls", countof(name), name,
                      "%u", id,
                      "%ls", countof(conn->username), conn->username,
                      "%u", conn->play_ok1,
                      "%u", clan_id,
                      "%u", 0,
                      "%u", sex,
                      "%u", race_id,
                      "%u", class_id,
                      "%u", active,
                      "%u", 0, // x
                      "%u", 0, // y
                      "%u", 0, // z
                      "%lf", current_hp,
                      "%lf", current_mp,
                      "%u", sp,
                      "%u", xp,
                      "%u", level,
                      "%u", karma,
                      "%u", 0,
                      "%u", 0,
                      "%u", 0,
                      "%u", 0,
                      "%u", 0,
                      "%u", 0,
                      "%u", 0,
                      "%u", 0,
                      "%u", 0,
                      
                      "%u", 0,
                      "%u", 0,
                      "%u", 0,
                      "%u", 0,
                      "%u", 0,
                      "%u", 0,
                      "%u", 0,
                      "%u", 0,
                      "%u", 0,
                      "%u", 0,
                      "%u", 0,
                      "%u", 0,
                      "%u", 0,
                      "%u", 0,
                      "%u", 0,
                      "%u", 0,
                      
                      "%u", 0,
                      "%u", 0,
                      "%u", 0,
                      "%u", 0,
                      "%u", 0,
                      "%u", 0,
                      "%u", 0,
                      "%u", 0,
                      "%u", 0,
                      "%u", 0,
                      "%u", 0,
                      "%u", 0,
                      "%u", 0,
                      "%u", 0,
                      "%u", 0,
                      "%u", 0,
                      
                      "%u", hair_style_id,
                      "%u", hair_color_id,
                      "%u", face_id,
                      "%lf", max_hp,
                      "%lf", max_mp,
                      "%u", delete_time,
                      "%u", class_id,
                      "%u", auto_select,
                      "%c", enchant_effect);
        
        fclose(character_file);
    }
    
    push_response(conn, 1,
                  "%h", 0,
                  "%c", type,
                  "%u", count,
                  "%b", (size_t) (end - list), list);
}

static void handle_select_character(struct state *state, struct connection *conn, byte *req)
{
    assert(state);
    assert(conn);
    assert(req);
    
    u32 index = 0;
    pscanf(req, "%u", &index);
    
    char characters_path[512] = {0};
    snprintf(characters_path, 
             sizeof(characters_path) - 1, 
             "data/accounts/%ls/characters",
             conn->username);
    
    conn->character = 0;
    for (size_t i = 1; i < countof(state->characters); i++) {
        if (state->characters[i].active)
            continue;
        memset(state->characters + i, 0, sizeof(struct character));
        conn->character = state->characters + i;
        conn->character->active = 1;
        conn->character->conn = conn;
        break;
    }
    if (!conn->character) {
        error("can't enter more characters in the world. dropping %ls." nl,
              conn->username);
        kick(conn);
        return;
    }
    
    u32 i = 0;
    int found = 0;
    
    in_directory(characters_path) {
        if (index != i) {
            i++;
            continue;
        }
        
        FILE *character_file = fopen(directory.full_path, "r");
        if (character_file) {
            found = 1;
            u32 recommendations_left = 0;
            u32 recommendations_have = 0;
            u32 inventory_limit = 0;
            u32 hero_symbol = 0;
            u32 hero = 0;

            fscanf(character_file,
                   "name=%ls" nl
                   "race_id=%u" nl
                   "sex=%u" nl
                   "class_id=%u" nl
                   "hair_style_id=%u" nl
                   "hair_color_id=%u" nl
                   "face_id=%u" nl
                   "x=%d" nl
                   "y=%d" nl
                   "z=%d" nl
                   "level=%u" nl
                   "xp=%u" nl
                   "str=%u" nl
                   "dex=%u" nl
                   "con=%u" nl
                   "int=%u" nl
                   "wit=%u" nl
                   "men=%u" nl
                   "max_hp=%lf" nl
                   "current_hp=%lf" nl
                   "max_mp=%lf" nl
                   "current_mp=%lf" nl
                   "max_cp=%lf" nl
                   "current_cp=%lf" nl
                   "sp=%u" nl
                   "current_load=%u" nl
                   "max_load=%u" nl
                   "p_atk=%u" nl
                   "p_atk_speed=%u" nl
                   "p_def=%u" nl
                   "evasion_rate=%u" nl
                   "accuracy=%u" nl
                   "critical_hit=%u" nl
                   "m_atk=%u" nl
                   "m_atk_speed=%u" nl
                   "m_def=%u" nl
                   "karma=%u" nl
                   "run_speed=%u" nl
                   "walk_speed=%u" nl
                   "collision_radius=%lf" nl
                   "collision_height=%lf" nl
                   "access_level=%u" nl
                   "title=%ls" nl
                   "clan_id=%u" nl
                   "crest_id=%u" nl
                   "ally_id=%u" nl
                   "ally_crest_id=%u" nl
                   "pk_kills=%u" nl
                   "pvp_kills=%u" nl
                   "recommendations_left=%u" nl
                   "recommendations_have=%u" nl
                   "inventory_limit=%u" nl
                   "clan_crest_large_id=%u" nl
                   "hero_symbol=%u" nl
                   "hero=%u" nl
                   "fish_x=%d" nl
                   "fish_y=%d" nl
                   "fish_z=%d" nl
                   "name_color=%u" nl,
                   conn->character->name,
                   &conn->character->race_id,
                   &conn->character->sex,
                   &conn->character->class_id,
                   &conn->character->hair_style_id,
                   &conn->character->hair_color_id,
                   &conn->character->face_id,
                   &conn->character->x,
                   &conn->character->y,
                   &conn->character->z,
                   &conn->character->level,
                   &conn->character->xp,
                   &conn->character->attributes.str,
                   &conn->character->attributes.dex,
                   &conn->character->attributes.con,
                   &conn->character->attributes._int,
                   &conn->character->attributes.wit,
                   &conn->character->attributes.men,
                   &conn->character->max_hp,
                   &conn->character->current_hp,
                   &conn->character->max_mp,
                   &conn->character->current_mp,
                   &conn->character->max_cp,
                   &conn->character->current_cp,
                   &conn->character->sp,
                   &conn->character->current_load,
                   &conn->character->max_load,
                   &conn->character->p_atk,
                   &conn->character->p_atk_speed,
                   &conn->character->p_def,
                   &conn->character->evasion_rate,
                   &conn->character->accuracy,
                   &conn->character->critical_hit,
                   &conn->character->m_atk,
                   &conn->character->m_atk_speed,
                   &conn->character->m_def,
                   &conn->character->karma,
                   &conn->character->run_speed,
                   &conn->character->walk_speed,
                   &conn->character->collision_radius,
                   &conn->character->collision_height,
                   &conn->character->access_level,
                   conn->character->title,
                   &conn->character->clan_id,
                   &conn->character->crest_id,
                   &conn->character->ally_id,
                   &conn->character->ally_crest_id,
                   &conn->character->pk_kills,
                   &conn->character->pvp_kills,
                   &recommendations_left,
                   &recommendations_have,
                   &inventory_limit,
                   &conn->character->clan_crest_large_id,
                   &hero_symbol,
                   &hero,
                   &conn->character->fish_x,
                   &conn->character->fish_y,
                   &conn->character->fish_z,
                   &conn->character->name_color);
            fclose(character_file);

            conn->character->recommendations_left = (u16) recommendations_left;
            conn->character->recommendations_have = (u16) recommendations_have;
            conn->character->inventory_limit = (u16) inventory_limit;
            conn->character->hero_symbol = (u8) hero_symbol;
            conn->character->hero = (u8) hero;
        }
        break;
    }
    
    if (!found) {
        error("character with index %u not found for %ls. the connection will be dropped." nl,
              index,
              conn->username);
        conn->character->active = 0;
        kick(conn);
        return;
    }
    
    u32 in_world_id = (u32) (size_t) (conn->character - state->characters);
    conn->character->movement_speed_multiplier = 1;
    conn->character->atk_speed_multiplier = 1;
    // derive combat stats from race/class (the stored file only had
    // placeholder values: run speed 20, p.atk 10, ...).
    character_init_stats(state, conn->character);
    
    byte type = 0x15;
    push_response(conn, 1,
                  "%h", 0,
                  "%c", type,
                  "%ls", countof(conn->character->name), conn->character->name,
                  "%u", in_world_id,
                  "%ls", countof(conn->character->title), conn->character->title,
                  "%u", conn->play_ok1,
                  "%u", conn->character->clan_id,
                  "%u", 0,
                  "%u", conn->character->sex,
                  "%u", conn->character->race_id,
                  "%u", conn->character->class_id,
                  "%u", conn->character->active,
                  "%u", conn->character->x,
                  "%u", conn->character->y,
                  "%u", conn->character->z,
                  "%lf", conn->character->current_hp,
                  "%lf", conn->character->current_mp,
                  "%u", conn->character->sp,
                  "%u", conn->character->xp,
                  "%u", conn->character->level,
                  "%u", 0,
                  "%u", 0,
                  "%u", conn->character->attributes._int,
                  "%u", conn->character->attributes.str,
                  "%u", conn->character->attributes.con,
                  "%u", conn->character->attributes.men,
                  "%u", conn->character->attributes.dex,
                  "%u", conn->character->attributes.wit,
                  
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0);
}

static void handle_auto_ss_bsps(struct state *state, struct connection *conn)
{
    assert(state);
    assert(conn);
    
    byte type = 0x1b;
    u16 empty = 0;
    u32 manor_size = 0;
    
    push_response(conn, 1,
                  "%h", 0,
                  "%c", type,
                  "%h", empty,
                  "%u", manor_size);
}

static void handle_send_quest_list(struct state *state, struct connection *conn)
{
    assert(state);
    assert(conn);
    
    byte type = 0x80;
    u8 empty[7] = {0};
    
    push_response(conn, 1,
                  "%h", 0,
                  "%c", type,
                  "%b", sizeof(empty), empty);
}

// NOTE(fmontenegro): this is userinfo packet.
static void handle_enter_world(struct state *state, struct connection *conn)
{
    assert(state);
    assert(conn);
    
    byte type = 0x04;
    u32 clan_leader = 0;
    
    push_response(conn, 1,
                  "%h", 0,
                  "%c", type,
                  "%u", conn->character->x,
                  "%u", conn->character->y,
                  "%u", conn->character->z,
                  "%u", conn->character->heading,
                  "%u", get_character_id(state, conn->character),
                  "%ls", countof(conn->character->name), conn->character->name,
                  "%u", conn->character->race_id,
                  "%u", conn->character->sex,
                  "%u", conn->character->class_id,
                  "%u", conn->character->level,
                  "%u", conn->character->xp,
                  "%u", conn->character->attributes.str,
                  "%u", conn->character->attributes.dex,
                  "%u", conn->character->attributes.con,
                  "%u", conn->character->attributes._int,
                  "%u", conn->character->attributes.wit,
                  "%u", conn->character->attributes.men,
                  "%u", (u32) conn->character->max_hp,
                  "%u", (u32) conn->character->current_hp,
                  "%u", (u32) conn->character->max_mp,
                  "%u", (u32) conn->character->current_mp,
                  "%u", conn->character->sp,
                  "%u", conn->character->current_load,
                  "%u", conn->character->max_load,
                  "%u", 0x28,
                  
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  
                  "%u", conn->character->p_atk,
                  "%u", conn->character->p_atk_speed,
                  "%u", conn->character->p_def,
                  "%u", conn->character->evasion_rate,
                  "%u", conn->character->accuracy,
                  "%u", conn->character->critical_hit,
                  "%u", conn->character->m_atk,
                  "%u", conn->character->m_atk_speed,
                  "%u", conn->character->p_atk_speed,
                  "%u", conn->character->m_def,
                  "%u", conn->character->pvp_flag,
                  "%u", conn->character->karma,
                  "%u", conn->character->run_speed,
                  "%u", conn->character->walk_speed,
                  "%u", conn->character->swim_run_speed,
                  "%u", conn->character->swim_walk_speed,
                  "%u", conn->character->fly_run_speed,
                  "%u", conn->character->fly_walk_speed,
                  "%u", conn->character->fly_run_speed,
                  "%u", conn->character->fly_walk_speed,
                  "%lf", conn->character->movement_speed_multiplier,
                  "%lf", conn->character->atk_speed_multiplier,
                  "%lf", conn->character->collision_radius,
                  "%lf", conn->character->collision_height,
                  "%u", conn->character->hair_style_id,
                  "%u", conn->character->hair_color_id,
                  "%u", conn->character->face_id,
                  "%u", conn->character->access_level,
                  "%ls", countof(conn->character->title), conn->character->title,
                  "%u", conn->character->clan_id,
                  "%u", conn->character->crest_id,
                  "%u", conn->character->ally_id,
                  "%u", conn->character->ally_crest_id,
                  "%u", clan_leader,
                  "%c", conn->character->mount_type,
                  "%c", conn->character->private_store_type,
                  "%c", conn->character->dwarven_craft,
                  "%u", conn->character->pk_kills,
                  "%u", conn->character->pvp_kills,
                  "%h", conn->character->cubics,
                  "%c", conn->character->party_members,
                  "%u", conn->character->abnormal_effect,
                  "%c", 0,
                  "%u", conn->character->clan_privileges,
                  
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  "%u", 0,
                  
                  "%h", conn->character->recommendations_left,
                  "%h", conn->character->recommendations_have,
                  "%u", 0,
                  "%h", conn->character->inventory_limit,
                  "%u", conn->character->class_id,
                  "%u", 0,
                  "%u", (u32) conn->character->max_cp,
                  "%u", (u32) conn->character->current_cp,
                  "%c", conn->character->mounted,
                  "%c", 0, // 1 = blue, 2 = red
                  "%u", conn->character->clan_crest_large_id,
                  "%c", conn->character->hero_symbol,
                  "%c", conn->character->hero,
                  "%c", 0,
                  "%u", conn->character->fish_x,
                  "%u", conn->character->fish_y,
                  "%u", conn->character->fish_z,
                  "%u", conn->character->name_color);
    
    // npcs and players around are sent by the visibility pass
    // (update_visibility), only the nearest CFG_MAX_KNOWN of them.
    conn->character->in_world = 1;
    conn->character->last_client_update_ms = state->now_ms;
    send_skill_list(state, conn->character);
    update_visibility_for(state, conn->character);
}

static void handle_leave_world(struct state *state, struct connection *conn)
{
    assert(state);
    assert(conn);
    if (conn->character) {
        character_leave_world(state, conn->character);
        // the slot can be reused by an npc: never keep pointing at it.
        conn->character->conn = 0;
        conn->character = 0;
    }
    push_response(conn, 1,
                  "%h", 0,
                  "%c", 0x7e);
}

static void handle_restart(struct state *state, struct connection *conn)
{
    assert(state);
    assert(conn);
    character_leave_world(state, conn->character);
    conn->character->conn = 0;
    conn->character = 0;
    push_response(conn, 1,
                  "%h", 0,
                  "%c", 0x5f,
                  "%u", 1);
    handle_send_character_list(state, conn);
}
