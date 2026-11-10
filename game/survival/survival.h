#pragma once

#include <array>
#include <string>
#include <vector>

#include "world/tilemap.h"

namespace zl {

enum class Item : u8 { None, Wood, Stone, Fiber, Rope, Axe, Pickaxe, Chest, Count };
const char* item_name(Item item);
int stack_limit(Item item);

struct Stack { Item item = Item::None; int count = 0; };
class Inventory {
public:
    explicit Inventory(int slots = 20) : slots_(static_cast<size_t>(slots)) {}
    int count(Item item) const;
    int capacity(Item item) const;
    bool add(Item item, int amount);
    bool remove(Item item, int amount);
    int transfer(Inventory& destination, int slot, int amount);
    // Clicking with an empty hand picks up a stack (half on right click).
    // Clicking with a held stack merges, places one, or swaps compatible slots.
    void click(int slot, Stack& hand, bool split);
    const std::vector<Stack>& slots() const { return slots_; }
private:
    std::vector<Stack> slots_;
};

struct Recipe {
    Item output;
    std::array<Stack, 3> ingredients;
    float seconds;
};
const std::array<Recipe, 4>& recipes();

struct ResourceNode {
    u32 object;
    Item item;
    Item tool;
    int remaining;
    float seconds;
};
struct CampChest { u32 object; Inventory inventory{24}; };

// Fixed-step, headless gameplay. Stable scenery indices survive harvesting;
// no generator rebuild can resurrect depleted objects.
class Survival {
public:
    Inventory bag;
    bool axe = false, pickaxe = false;
    std::vector<ResourceNode> nodes;
    std::vector<CampChest> chests;
    std::string message = "Gather branches, loose stone and fiber. Craft your first tools.";
    float message_time = 8;
    u32 world_revision = 0;

    void reset(TileMap& map);
    int node_at(u32 object) const;
    int pick_node(const TileMap& map, Vec2 point) const;
    bool in_reach(const TileMap& map, Vec2 player, u32 object) const;
    const char* harvest_problem(const TileMap& map, Vec2 player, int node) const;
    void harvest(TileMap& map, Vec2 player, int node, bool held, bool moving, float dt);
    void cancel_harvest();
    bool can_craft(int recipe) const;
    bool start_craft(int recipe, int amount = 1);
    void step(float dt);
    bool can_place_chest(const TileMap& map, Vec2 player, Vec2 point) const;
    bool place_chest(TileMap& map, Vec2 player, Vec2 point);
    int chest_at(u32 object) const;
    int active_node() const { return active_node_; }
    float harvest_progress() const;
    int crafting() const { return recipe_; }
    int crafting_left() const { return crafts_left_; }
    float craft_progress() const;
    Item held_tool = Item::None;
private:
    int active_node_ = -1;
    float harvest_time_ = 0;
    int recipe_ = -1, crafts_left_ = 0;
    float craft_time_ = 0;
    std::vector<int> first_node_, next_node_;
    void deplete(TileMap& map, ResourceNode& node);
    bool craft_one(int recipe);
};

} // namespace zl
