#include "world/tilemap.h"

namespace zl {

const char* terrain_name(Terrain t) {
    switch (t) {
        case Terrain::Water: return "water";
        case Terrain::Shallows: return "shallows";
        case Terrain::Sand: return "sand";
        case Terrain::Grass: return "grass";
        case Terrain::Dirt: return "dirt";
        case Terrain::Forest: return "forest";
        case Terrain::Mountain: return "mountain";
        case Terrain::Rocks: return "rocks";
        case Terrain::Count: break;
    }
    return "?";
}

const char* resource_name(Resource r) {
    switch (r) {
        case Resource::None: return "none";
        case Resource::Stone: return "stone";
        case Resource::Iron: return "iron";
        case Resource::Gold: return "gold";
        case Resource::Count: break;
    }
    return "?";
}

}  // namespace zl
