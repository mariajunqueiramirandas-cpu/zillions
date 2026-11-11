#include "survival/survival.h"

#include <algorithm>
#include <cmath>
#include <queue>

#include "core/rng.h"

namespace zl {

const char* item_name(Item item) {
    static const char* names[] = {"Empty", "Wood", "Stone", "Fiber", "Rope", "Stone axe", "Stone pickaxe", "Chest"};
    const int i = static_cast<int>(item);
    return i >= 0 && i < static_cast<int>(Item::Count) ? names[i] : "Unknown";
}
int stack_limit(Item item) { return item == Item::Axe || item == Item::Pickaxe || item == Item::Chest ? 1 : 50; }
int Inventory::count(Item item) const {
    int total = 0;
    for (const auto& s : slots_) if (s.item == item) total += s.count;
    return total;
}
int Inventory::capacity(Item item) const {
    if (item == Item::None) return 0;
    int total = 0;
    for (const auto& s : slots_) if (!s.count || s.item == item) total += stack_limit(item) - s.count;
    return total;
}
bool Inventory::add(Item item, int amount) {
    if (item == Item::None || amount < 0 || capacity(item) < amount) return false;
    if (amount == 0) return true;
    for (int pass = 0; pass < 2; ++pass) for (auto& s : slots_) {
        if ((pass == 0 && s.item == item && s.count) || (pass == 1 && !s.count)) {
            const int n = std::min(amount, stack_limit(item) - s.count);
            s.item = item; s.count += n; amount -= n;
            if (!amount) return true;
        }
    }
    return amount == 0;
}
bool Inventory::remove(Item item, int amount) {
    if (amount < 0 || count(item) < amount) return false;
    for (auto& s : slots_) if (s.item == item) {
        const int n = std::min(amount, s.count);
        s.count -= n; amount -= n;
        if (!s.count) s = {};
    }
    return true;
}
int Inventory::transfer(Inventory& destination, int slot, int amount) {
    if (&destination == this || slot < 0 || slot >= static_cast<int>(slots_.size()) || amount <= 0) return 0;
    auto& s = slots_[static_cast<size_t>(slot)];
    const int n = std::min({amount, s.count, destination.capacity(s.item)});
    if (n > 0 && destination.add(s.item, n)) { s.count -= n; if (!s.count) s = {}; return n; }
    return 0;
}
void Inventory::click(int slot, Stack& hand, bool split) {
    if (slot < 0 || slot >= static_cast<int>(slots_.size())) return;
    auto& s = slots_[static_cast<size_t>(slot)];
    if (!hand.count) {
        const int n = split ? (s.count + 1) / 2 : s.count;
        hand = {s.item, n}; s.count -= n;
        if (!s.count) s = {};
    } else if (!s.count || s.item == hand.item) {
        const int n = std::min(split ? 1 : hand.count, stack_limit(hand.item) - s.count);
        s.item = hand.item; s.count += n; hand.count -= n;
        if (!hand.count) hand = {};
    } else if (!split) std::swap(s, hand);
}

const std::array<Recipe, 4>& recipes() {
    static const std::array<Recipe, 4> data{{
        {Item::Rope, {{{Item::Fiber, 3}, {}, {}}}, 0.6f},
        {Item::Axe, {{{Item::Wood, 3}, {Item::Stone, 2}, {Item::Rope, 1}}}, 1.5f},
        {Item::Pickaxe, {{{Item::Wood, 3}, {Item::Stone, 4}, {Item::Rope, 1}}}, 1.5f},
        {Item::Chest, {{{Item::Wood, 12}, {Item::Rope, 2}, {}}}, 2.0f}
    }};
    return data;
}

void Survival::reset(TileMap& map) {
    *this = Survival{};
    if (!map.scenery.ready) map.scenery = build_scenery(map);
    // Reachable open ground guarantees a bootstrap without giving free tools.
    // Use a flood fill, rather than distance alone, so cliffs cannot strand it.
    std::vector<u8> seen(static_cast<size_t>(map.tile_count()), 0);
    std::queue<int> visit;
    const int start = map.index(map.start.x, map.start.y);
    visit.push(start); seen[static_cast<size_t>(start)] = 1;
    std::vector<int> starter;
    while (!visit.empty() && starter.size() < 24) {
        const int i = visit.front(); visit.pop();
        const int x = i % map.width, y = i / map.width;
        const Vec2 p{static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f};
        if (i != start && map.buildable(x, y) && !map.scenery.overlaps(p, 0.45f, collision_floor(map, p), 1.5f)) starter.push_back(i);
        for (const IVec2 d : {IVec2{1,0}, IVec2{-1,0}, IVec2{0,1}, IVec2{0,-1}}) {
            const int nx = x + d.x, ny = y + d.y;
            if (!map.in_bounds(nx, ny)) continue;
            const int n = map.index(nx, ny);
            if (!seen[static_cast<size_t>(n)] && map.can_step(i, n)) { seen[static_cast<size_t>(n)] = 1; visit.push(n); }
        }
    }
    for (size_t j = 0; j < starter.size(); ++j) {
        const int i = starter[j];
        const SceneryKind kind = j % 3 == 0 ? SceneryKind::Branch : (j % 3 == 1 ? SceneryKind::Pebble : SceneryKind::Bush);
        map.scenery.objects.push_back({kind, hash32(static_cast<u32>(i)), {static_cast<float>(i % map.width) + 0.5f, static_cast<float>(i / map.width) + 0.5f}, kind==SceneryKind::Branch?0.65f:0.85f, {216,207,180,255}, i});
    }
    for (u32 j = 0; j < map.scenery.objects.size(); ++j) {
        const auto& o = map.scenery.objects[j];
        switch (o.kind) {
            case SceneryKind::Broadleaf: case SceneryKind::Conifer: nodes.push_back({j, Item::Wood, Item::Axe, 10, 0.85f}); break;
            case SceneryKind::Boulder: nodes.push_back({j, Item::Stone, Item::Pickaxe, 12, 0.95f}); break;
            case SceneryKind::Stone: nodes.push_back({j, Item::Stone, Item::Pickaxe, 30, 0.95f}); break;
            case SceneryKind::Bush: case SceneryKind::Reed: nodes.push_back({j, Item::Fiber, Item::None, 4, 0.45f}); break;
            case SceneryKind::Pebble: nodes.push_back({j, Item::Stone, Item::None, 3, 0.45f}); break;
            case SceneryKind::Branch: nodes.push_back({j, Item::Wood, Item::None, 4, 0.45f}); break;
            default: break;
        }
    }
    first_node_.assign(static_cast<size_t>(map.tile_count()), -1);
    next_node_.resize(nodes.size(), -1);
    for (int n = 0; n < static_cast<int>(nodes.size()); ++n) {
        const int tile = map.scenery.objects[nodes[static_cast<size_t>(n)].object].tile;
        next_node_[static_cast<size_t>(n)] = first_node_[static_cast<size_t>(tile)];
        first_node_[static_cast<size_t>(tile)] = n;
    }
    ++world_revision;
}
int Survival::pick_node(const TileMap& map, Vec2 point) const {
    int best = -1; float distance = 0.85f;
    const int cx = static_cast<int>(point.x), cy = static_cast<int>(point.y);
    for (int y = cy-1; y <= cy+1; ++y) for (int x = cx-1; x <= cx+1; ++x) {
        if (!map.in_bounds(x,y)) continue;
        for (int n = first_node_[static_cast<size_t>(map.index(x,y))]; n >= 0; n = next_node_[static_cast<size_t>(n)]) {
            const auto& node = nodes[static_cast<size_t>(n)];
            const float d = length(map.scenery.objects[node.object].position-point);
            if (node.remaining > 0 && d < distance) { distance = d; best = n; }
        }
    }
    return best;
}
int Survival::node_at(u32 object) const {
    const auto i = std::lower_bound(nodes.begin(), nodes.end(), object, [](const ResourceNode& n, u32 id) { return n.object < id; });
    return i != nodes.end() && i->object == object && i->remaining > 0 ? static_cast<int>(i - nodes.begin()) : -1;
}
bool Survival::in_reach(const TileMap& map, Vec2 player, u32 object) const {
    if (object >= map.scenery.objects.size()) return false;
    const auto& o = map.scenery.objects[object];
    if (!o.active || length(o.position - player) > 1.65f) return false;
    const int x = static_cast<int>(player.x), y = static_cast<int>(player.y);
    if (!map.in_bounds(x, y)) return false;
    // Sample the short interaction segment: no reaching through cliffs/water.
    int previous = map.index(x, y);
    for (int k = 1; k <= 12; ++k) {
        const Vec2 q = lerp(player, o.position, static_cast<float>(k) / 12);
        const int qx = static_cast<int>(q.x), qy = static_cast<int>(q.y);
        if (!map.in_bounds(qx, qy)) return false;
        const int next = map.index(qx, qy);
        const size_t n = static_cast<size_t>(next), p = static_cast<size_t>(previous);
        if (map.terrain[n] == Terrain::Water || map.terrain[n] == Terrain::Mountain) return false;
        if (map.level[n] != map.level[p] && !((map.flags[n] | map.flags[p]) & TileFlag::Ramp)) return false;
        previous = next;
    }
    ObjectContact hit;
    if (map.scenery.sweep(player, o.position-player, 0.02f, collision_floor(map,player)+0.4f, 0.25f, hit) &&
        map.scenery.colliders[hit.collider].object != object) return false;
    return true;
}
const char* Survival::harvest_problem(const TileMap& map, Vec2 player, int node) const {
    if (node < 0 || node >= static_cast<int>(nodes.size()) || nodes[static_cast<size_t>(node)].remaining <= 0) return "Select branches, loose stones, bushes, trees or stone deposits";
    const auto& n = nodes[static_cast<size_t>(node)];
    if (!in_reach(map, player, n.object)) return "Move closer on accessible ground";
    if (recipe_ >= 0) return "Finish crafting first";
    if (n.tool == Item::Axe && !axe) return "Craft a stone axe to cut this tree";
    if (n.tool == Item::Pickaxe && !pickaxe) return "Craft a stone pickaxe to mine this rock";
    if (bag.capacity(n.item) < 1) return "Inventory full - store supplies at camp";
    return nullptr;
}
void Survival::cancel_harvest() { active_node_ = -1; harvest_time_ = 0; held_tool = Item::None; }
void Survival::harvest(TileMap& map, Vec2 player, int node, bool held, bool moving, float dt) {
    if (!held || moving || harvest_problem(map, player, node)) { cancel_harvest(); return; }
    if (active_node_ != node) { cancel_harvest(); active_node_ = node; }
    auto& n = nodes[static_cast<size_t>(node)];
    held_tool = n.tool;
    harvest_time_ += std::max(dt, 0.0f);
    if (harvest_time_ >= n.seconds) {
        if (bag.add(n.item, 1)) {
            --n.remaining;
            message = std::string("+1 ") + item_name(n.item); message_time = 2.5f;
            if (!n.remaining) { deplete(map, n); cancel_harvest(); }
            else harvest_time_ -= n.seconds;
        } else cancel_harvest();
    }
}
void Survival::deplete(TileMap& map, ResourceNode& node) {
    auto& o = map.scenery.objects[node.object];
    if (node.tool == Item::Axe) o.kind = SceneryKind::Stump;
    else if (node.tool == Item::Pickaxe) o.kind = SceneryKind::Rubble;
    else o.active = false;
    const int tile = o.tile;
    bool solid = false;
    for (const auto& s : map.scenery.objects) if (s.active && s.tile == tile && scenery_blocks(s.kind)) { solid = true; break; }
    if (!solid && (map.terrain[static_cast<size_t>(tile)] == Terrain::Forest || map.terrain[static_cast<size_t>(tile)] == Terrain::Rocks || map.resource[static_cast<size_t>(tile)] != Resource::None)) {
        map.terrain[static_cast<size_t>(tile)] = Terrain::Dirt;
        map.resource[static_cast<size_t>(tile)] = Resource::None;
        map.flags[static_cast<size_t>(tile)] |= TileFlag::Walkable;
        if (!(map.flags[static_cast<size_t>(tile)] & TileFlag::Ramp)) map.flags[static_cast<size_t>(tile)] |= TileFlag::Buildable;
    }
    rebuild_scenery_colliders(map);
    ++world_revision;
}
bool Survival::can_craft(int recipe) const {
    if (recipe < 0 || recipe >= static_cast<int>(recipes().size())) return false;
    const auto& r = recipes()[static_cast<size_t>(recipe)];
    if ((r.output == Item::Axe && axe) || (r.output == Item::Pickaxe && pickaxe)) return false;
    Inventory trial = bag;
    for (const auto& s : r.ingredients) if (s.count && !trial.remove(s.item, s.count)) return false;
    return r.output == Item::Axe || r.output == Item::Pickaxe || trial.capacity(r.output) >= 1;
}
bool Survival::start_craft(int recipe, int amount) {
    if (recipe_ >= 0 || amount <= 0 || !can_craft(recipe)) return false;
    cancel_harvest(); recipe_ = recipe; craft_time_ = 0; crafts_left_ = std::min(amount, 99);
    return true;
}
bool Survival::craft_one(int recipe) {
    if (!can_craft(recipe)) return false;
    const auto& r = recipes()[static_cast<size_t>(recipe)];
    for (const auto& s : r.ingredients) if (s.count) bag.remove(s.item, s.count);
    if (r.output == Item::Axe) axe = true;
    else if (r.output == Item::Pickaxe) pickaxe = true;
    else bag.add(r.output, 1);
    message = std::string("Crafted ") + item_name(r.output); message_time = 3;
    return true;
}
void Survival::step(float dt) {
    message_time = std::max(0.0f, message_time - dt);
    if (recipe_ < 0) return;
    craft_time_ += dt;
    if (craft_time_ >= recipes()[static_cast<size_t>(recipe_)].seconds) {
        const bool success = craft_one(recipe_);
        if (!success) { message = "Crafting stopped: ingredients or inventory space changed"; message_time = 4; }
        if (!success || --crafts_left_ <= 0 || !can_craft(recipe_)) { recipe_ = -1; crafts_left_ = 0; }
        craft_time_ = 0;
    }
}
float Survival::harvest_progress() const { return active_node_ < 0 ? 0 : saturate(harvest_time_ / nodes[static_cast<size_t>(active_node_)].seconds); }
float Survival::craft_progress() const { return recipe_ < 0 ? 0 : saturate(craft_time_ / recipes()[static_cast<size_t>(recipe_)].seconds); }
bool Survival::can_place_chest(const TileMap& map, Vec2 player, Vec2 point) const {
    const int x = static_cast<int>(std::floor(point.x)), y = static_cast<int>(std::floor(point.y));
    if (!map.in_bounds(x,y) || !map.buildable(x,y) || length(point - player) > 2.3f || length(point - player) < 0.8f) return false;
    if (bag.count(Item::Chest) < 1) return false;
    const int px = static_cast<int>(player.x), py = static_cast<int>(player.y);
    if (!map.in_bounds(px,py) || map.level[static_cast<size_t>(map.index(x,y))] != map.level[static_cast<size_t>(map.index(px,py))]) return false;
    for (int i=1;i<=16;++i) {
        const Vec2 q=lerp(player,point,static_cast<float>(i)/16);
        const int qx=static_cast<int>(q.x),qy=static_cast<int>(q.y);
        if (!map.in_bounds(qx,qy)) return false;
        const size_t tile=static_cast<size_t>(map.index(qx,qy));
        if (map.terrain[tile]==Terrain::Water||map.terrain[tile]==Terrain::Mountain||map.level[tile]!=map.level[static_cast<size_t>(map.index(px,py))]) return false;
    }
    ObjectContact hit;
    if(map.scenery.sweep(player,point-player,0.02f,collision_floor(map,player)+0.4f,0.25f,hit))return false;
    return !map.scenery.overlaps(point, 0.62f, collision_floor(map, point), 0.8f);
}
bool Survival::place_chest(TileMap& map, Vec2 player, Vec2 point, u8 facing) {
    point = {std::floor(point.x) + 0.5f, std::floor(point.y) + 0.5f};
    if (!can_place_chest(map, player, point)) return false;
    bag.remove(Item::Chest, 1);
    const u32 id = static_cast<u32>(map.scenery.objects.size());
    map.scenery.objects.push_back({SceneryKind::Chest, 0, point, 1, {255,255,255,255}, map.index(static_cast<int>(point.x), static_cast<int>(point.y))});
    map.scenery.objects.back().facing=facing%4;
    chests.push_back({id, Inventory{24}});
    map.flags[static_cast<size_t>(map.scenery.objects.back().tile)] &= static_cast<u8>(~TileFlag::Buildable);
    rebuild_scenery_colliders(map); ++world_revision;
    message = "Camp chest placed. Click it nearby to store supplies."; message_time = 5;
    return true;
}
int Survival::chest_at(u32 object) const {
    for (size_t i = 0; i < chests.size(); ++i) if (chests[i].object == object) return static_cast<int>(i);
    return -1;
}

} // namespace zl
