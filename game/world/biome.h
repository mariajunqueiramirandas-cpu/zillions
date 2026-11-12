// Continuous ecology, shared by terrain classification and object placement.
#pragma once

#include <vector>

#include "math/vec.h"

namespace zl {

// Independent concentrations, not mutually exclusive terrain labels.
struct BiomeDensity {
    f32 woodland = 0, meadow = 0, scrub = 0, rock = 0;
    constexpr bool operator==(const BiomeDensity&) const = default;
};

class BiomeField {
public:
    static constexpr i32 kSpacing = 4; // tiles; only 16 MB on a 4096² world
    i32 width = 0, height = 0;
    std::vector<BiomeDensity> cells;

    bool empty() const { return cells.empty(); }
    void create(i32 map_width, i32 map_height);
    BiomeDensity sample(Vec2 position) const;
    // Smoothly incorporate small copses/outcrops into their surrounding biome.
    void add_patch(Vec2 center, f32 radius, BiomeDensity density);
};

} // namespace zl
