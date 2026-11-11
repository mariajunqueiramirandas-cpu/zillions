// Headless glTF/GLB loader regression tests.
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "asset/gltf.h"

using namespace zl;

namespace {
int failures = 0;
#define CHECK(condition) do { if (!(condition)) { ++failures; std::printf("FAIL line %d: %s\n", __LINE__, #condition); } } while (0)

bool close(f32 a, f32 b, f32 epsilon) { return std::fabs(a - b) <= epsilon; }

void check_primitive_counts(const GltfModel& model, size_t vertices, size_t indices, bool skinned) {
    CHECK(model.primitives.size() == 1);
    if (model.primitives.size() != 1) return;
    CHECK(model.primitives[0].vertices.size() == vertices);
    CHECK(model.primitives[0].indices.size() == indices);
    CHECK(model.skinned == skinned);
}

void check_base_color(const GltfModel& model) {
    CHECK(model.materials.size() == 1);
    if (model.materials.empty()) return;
    CHECK(model.materials[0].base_color.width == 1024);
    CHECK(model.materials[0].base_color.height == 1024);
    CHECK(model.materials[0].base_color.pixels.size() == 1024u * 1024u);
}

void plague_character() {
    GltfModel model;
    std::string error;
    const bool loaded = model.load(ZL_ASSET_DIR "/models/plague_character.glb", error);
    CHECK(loaded);
    if (!loaded) {
        std::printf("  load error: %s\n", error.c_str());
        return;
    }

    check_primitive_counts(model, 239173, 862395, true);
    CHECK(model.skin_index == 0);
    CHECK(model.skins.size() == 1);
    if (!model.skins.empty()) {
        CHECK(model.skins[0].joints.size() == 24);
        CHECK(model.skins[0].inverse_bind.size() == 24);
    }

    CHECK(model.animations.size() == 5);
    struct Expected {
        const char* name;
        f32 duration;
    };
    const Expected expected[] = {
        {"Idle", 4.0f},
        {"Walking", 1.0333333f},
        {"Running", 0.6333333f},
        {"Heavy_Hammer_Swing", 1.8333334f},
        {"Right_Hand_Sword_Slash", 1.5f},
    };
    for (const Expected& entry : expected) {
        const GltfAnimation* animation = model.animation(entry.name);
        CHECK(animation != nullptr);
        if (animation) {
            CHECK(animation->name == entry.name);
            CHECK(close(animation->duration, entry.duration, 1.0e-3f));
        }
    }
    // Case-insensitive lookup must find the same clips.
    CHECK(model.animation("idle") != nullptr);
    CHECK(model.animation("WALKING") != nullptr);

    CHECK(model.hand_node >= 0);
    if (model.hand_node >= 0) {
        CHECK(model.hand_node < static_cast<i32>(model.nodes.size()));
        CHECK(model.nodes[static_cast<size_t>(model.hand_node)].name.find("RightHand") != std::string::npos);
    }
    CHECK(model.node_by_name("mixamorig:RightHand") == model.hand_node);

    CHECK(close(model.bounds_max.y, 1.7f, 2.0e-2f));
    CHECK(model.bounds_min.y >= -1.0e-3f);
    CHECK(close(model.bounds_min.x, -0.525f, 5.0e-2f));
    CHECK(close(model.bounds_max.x, 0.525f, 5.0e-2f));

    check_base_color(model);

    if (model.primitives.size() == 1) {
        const GltfPrimitive& primitive = model.primitives[0];
        bool weights_ok = true;
        bool joints_ok = true;
        bool normals_ok = true;
        for (const GltfVertex& vertex : primitive.vertices) {
            const f32 sum = vertex.weights[0] + vertex.weights[1] + vertex.weights[2] + vertex.weights[3];
            if (!close(sum, 1.0f, 1.0e-3f)) weights_ok = false;
            for (u16 joint : vertex.joints) {
                if (joint >= 24) joints_ok = false;
            }
            if (!std::isfinite(vertex.normal.x) || !std::isfinite(vertex.normal.y) ||
                !std::isfinite(vertex.normal.z)) {
                normals_ok = false;
            }
        }
        CHECK(weights_ok);
        CHECK(joints_ok);
        CHECK(normals_ok);
    }

    bool times_ok = true;
    bool channels_ok = true;
    for (const GltfAnimation& animation : model.animations) {
        if (animation.channels.empty()) channels_ok = false;
        for (const GltfChannel& channel : animation.channels) {
            if (channel.times.empty()) channels_ok = false;
            if (channel.path == GltfPath::Rotation && channel.quat.size() != channel.times.size()) {
                channels_ok = false;
            }
            if (channel.path != GltfPath::Rotation && channel.vec3.size() != channel.times.size()) {
                channels_ok = false;
            }
            for (size_t i = 0; i < channel.times.size(); ++i) {
                if (!std::isfinite(channel.times[i])) times_ok = false;
                if (i > 0 && channel.times[i] < channel.times[i - 1]) times_ok = false;
            }
        }
    }
    CHECK(times_ok);
    CHECK(channels_ok);
}

void tool_model(const char* path, size_t vertices, size_t indices, f32 max_x, f32 max_y) {
    GltfModel model;
    std::string error;
    const bool loaded = model.load(path, error);
    CHECK(loaded);
    if (!loaded) {
        std::printf("  load error: %s\n", error.c_str());
        return;
    }
    check_primitive_counts(model, vertices, indices, false);
    CHECK(close(model.bounds_max.x, max_x, 1.0e-2f));
    CHECK(close(model.bounds_min.x, -max_x, 1.0e-2f));
    CHECK(close(model.bounds_max.y, max_y, 1.0e-2f));
    CHECK(close(model.bounds_min.y, -max_y, 1.0e-2f));
    check_base_color(model);
}

void downsample_checks() {
    const Color sample{12, 34, 56, 78};

    Image large(4096, 4096, sample);
    Image out = downsample_image(large, 1024);
    CHECK(out.width == 1024);
    CHECK(out.height == 1024);
    CHECK(out.pixels.size() == 1024u * 1024u);
    CHECK(out.at(0, 0).r == 12 && out.at(0, 0).g == 34 && out.at(0, 0).b == 56 && out.at(0, 0).a == 78);

    Image exact(1024, 1024, sample);
    out = downsample_image(exact, 1024);
    CHECK(out.width == 1024);
    CHECK(out.height == 1024);
    CHECK(out.pixels.size() == exact.pixels.size());

    Image small(512, 512, sample);
    out = downsample_image(small, 1024);
    CHECK(out.width == 512);
    CHECK(out.height == 512);
    CHECK(out.pixels.size() == small.pixels.size());

    Image wide(2048, 1024, sample);
    out = downsample_image(wide, 1024);
    CHECK(out.width == 1024);
    CHECK(out.height == 512);
    CHECK(out.pixels.size() == 1024u * 512u);

    Image single(1, 1, sample);
    out = downsample_image(single, 1024);
    CHECK(out.width == 1);
    CHECK(out.height == 1);
    CHECK(out.at(0, 0).r == 12 && out.at(0, 0).a == 78);

    Image empty;
    out = downsample_image(empty, 1024);
    CHECK(out.width == 0 && out.height == 0);

    // A 2x2 block with four distinct colours must average, not pick one.
    Image block(2, 2);
    block.at(0, 0) = Color{0, 0, 0, 255};
    block.at(1, 0) = Color{100, 0, 0, 255};
    block.at(0, 1) = Color{0, 200, 0, 255};
    block.at(1, 1) = Color{0, 0, 60, 255};
    out = downsample_image(block, 1);
    CHECK(out.width == 1 && out.height == 1);
    CHECK(out.at(0, 0).r == 25);
    CHECK(out.at(0, 0).g == 50);
    CHECK(out.at(0, 0).b == 15);
    CHECK(out.at(0, 0).a == 255);
}

void failure_cases() {
    GltfModel model;
    std::string error;
    CHECK(!model.load(ZL_ASSET_DIR "/models/does_not_exist.glb", error));
    CHECK(!error.empty());
    CHECK(model.primitives.empty());
    CHECK(model.nodes.empty());

    const char* bad_path = "zl_bad_model.glb";
    {
        std::ofstream file(bad_path, std::ios::binary);
        CHECK(file.good());
        if (file) file << "this is not a glb";
    }
    error.clear();
    CHECK(!model.load(bad_path, error));
    CHECK(!error.empty());
    CHECK(model.primitives.empty());
    std::remove(bad_path);
}

void gltf_json_file() {
    const char* path = "zl_triangle.gltf";
    {
        std::ofstream file(path, std::ios::binary);
        CHECK(file.good());
        if (file) {
            file << R"({"asset":{"version":"2.0"},)"
                    R"("buffers":[{"byteLength":36,"uri":"data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA"}],)"
                    R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36}],)"
                    R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"}],)"
                    R"("meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}],)"
                    R"("nodes":[{"mesh":0,"translation":[2,0,0],"scale":[2,2,2]}],"scenes":[{"nodes":[0]}],"scene":0})";
        }
    }

    GltfModel model;
    std::string error;
    const bool loaded = model.load(path, error);
    CHECK(loaded);
    if (loaded) {
        CHECK(model.primitives.size() == 1);
        if (model.primitives.size() == 1) {
            const GltfPrimitive& primitive = model.primitives[0];
            CHECK(primitive.vertices.size() == 3);
            CHECK(primitive.indices.size() == 3);
            CHECK(primitive.indices[0] == 0 && primitive.indices[1] == 1 && primitive.indices[2] == 2);
            CHECK(primitive.material == -1);
            CHECK(close(primitive.vertices[0].position.x, 2.0f, 1.0e-6f));
            CHECK(close(primitive.vertices[1].position.x, 4.0f, 1.0e-6f));
            CHECK(close(primitive.vertices[2].position.y, 2.0f, 1.0e-6f));
        }
        CHECK(close(model.bounds_min.x, 2.0f, 1.0e-6f));
        CHECK(close(model.bounds_max.x, 4.0f, 1.0e-6f));
        CHECK(close(model.bounds_max.y, 2.0f, 1.0e-6f));
        CHECK(!model.skinned);
        CHECK(model.root == 0);
    }
    std::remove(path);
}
}  // namespace

void report(const char* label, int before) {
    std::printf("%s: %s\n", label, failures == before ? "PASS" : "FAIL");
}

int main() {
    int before = failures;
    plague_character();
    report("plague_character.glb", before);

    before = failures;
    tool_model(ZL_ASSET_DIR "/models/axe.glb", 153732, 861744, 0.33f, 0.95f);
    report("axe.glb", before);

    before = failures;
    tool_model(ZL_ASSET_DIR "/models/pickaxe.glb", 120689, 669360, 0.77f, 0.95f);
    report("pickaxe.glb", before);

    before = failures;
    downsample_checks();
    report("downsample_image", before);

    before = failures;
    failure_cases();
    report("failure cases", before);

    before = failures;
    gltf_json_file();
    report("gltf JSON with data URI", before);

    std::printf("glTF loader checks: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
