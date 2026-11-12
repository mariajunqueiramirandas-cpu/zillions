#include "asset/gltf.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace zl {
namespace {

using json = nlohmann::json;

constexpr u32 kGlbMagic = 0x46546C67u;
constexpr u32 kChunkJson = 0x4E4F534Au;
constexpr u32 kChunkBin = 0x004E4942u;

bool fail(std::string& error, const std::string& message) {
    error = message;
    return false;
}

const json* field(const json& object, const char* key) {
    if (!object.is_object()) return nullptr;
    const auto it = object.find(key);
    return it == object.end() ? nullptr : &(*it);
}

i32 int_at(const json& object, const char* key, i32 fallback) {
    const json* value = field(object, key);
    if (!value || !value->is_number_integer()) return fallback;
    return static_cast<i32>(value->get<double>());
}

f32 float_at(const json& object, const char* key, f32 fallback) {
    const json* value = field(object, key);
    if (!value || !value->is_number()) return fallback;
    return static_cast<f32>(value->get<double>());
}

bool bool_at(const json& object, const char* key, bool fallback) {
    const json* value = field(object, key);
    if (!value || !value->is_boolean()) return fallback;
    return value->get<bool>();
}

std::string string_at(const json& object, const char* key, const char* fallback) {
    const json* value = field(object, key);
    if (!value || !value->is_string()) return fallback;
    return value->get<std::string>();
}

f32 number_at(const json& array, size_t index) {
    if (!array.is_array() || index >= array.size()) return 0.0f;
    const json& value = array[index];
    return value.is_number() ? static_cast<f32>(value.get<double>()) : 0.0f;
}

u32 read_u32(const u8* data) {
    return static_cast<u32>(data[0]) | (static_cast<u32>(data[1]) << 8) | (static_cast<u32>(data[2]) << 16) |
           (static_cast<u32>(data[3]) << 24);
}

i32 base64_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

bool decode_base64(const std::string& text, std::vector<u8>& out) {
    out.clear();
    out.reserve(text.size() * 3 / 4);
    u32 accumulator = 0;
    i32 bits = 0;
    for (char c : text) {
        if (c == '=') break;
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
        const i32 value = base64_value(c);
        if (value < 0) return false;
        accumulator = (accumulator << 6) | static_cast<u32>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<u8>((accumulator >> bits) & 0xFFu));
            if (bits > 0) accumulator &= (1u << bits) - 1u;
            else accumulator = 0;
        }
    }
    return true;
}

bool decode_data_uri(const std::string& uri, std::vector<u8>& out) {
    if (uri.rfind("data:", 0) != 0) return false;
    const size_t comma = uri.find(',');
    if (comma == std::string::npos) return false;
    const std::string header = uri.substr(0, comma);
    if (header.find(";base64") == std::string::npos) return false;
    return decode_base64(uri.substr(comma + 1), out);
}

bool read_file(const char* path, std::vector<u8>& out) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return false;
    const std::streamoff size = file.tellg();
    if (size <= 0) return false;
    out.resize(static_cast<size_t>(size));
    file.seekg(0, std::ios::beg);
    file.read(reinterpret_cast<char*>(out.data()), size);
    return file.gcount() == size;
}

i32 component_size(i32 type) {
    switch (type) {
        case 5120:
        case 5121:
            return 1;
        case 5122:
        case 5123:
            return 2;
        case 5125:
        case 5126:
            return 4;
        default:
            return 0;
    }
}

i32 type_components(const std::string& type) {
    if (type == "SCALAR") return 1;
    if (type == "VEC2") return 2;
    if (type == "VEC3") return 3;
    if (type == "VEC4") return 4;
    if (type == "MAT2") return 4;
    if (type == "MAT3") return 9;
    if (type == "MAT4") return 16;
    return 0;
}

struct BufferData {
    const u8* data = nullptr;
    size_t size = 0;
    std::vector<u8> owned;
};

struct BufferViewInfo {
    const u8* data = nullptr;
    size_t size = 0;
    size_t stride = 0;
};

struct AccessorInfo {
    const u8* data = nullptr;
    size_t stride = 0;
    size_t element_size = 0;
    i32 component_type = 0;
    i32 components = 0;
    i32 count = 0;
    bool normalized = false;
};

bool parse_accessor(const json& object, const std::vector<BufferViewInfo>& views, AccessorInfo& out, std::string& error) {
    if (field(object, "sparse")) return fail(error, "gltf: sparse accessors are not supported");
    out.component_type = int_at(object, "componentType", 0);
    const i32 size = component_size(out.component_type);
    if (size == 0) return fail(error, "gltf: accessor has an invalid componentType");
    out.components = type_components(string_at(object, "type", ""));
    if (out.components == 0) return fail(error, "gltf: accessor has an invalid type");
    out.count = int_at(object, "count", -1);
    if (out.count < 0) return fail(error, "gltf: accessor has an invalid count");
    out.normalized = bool_at(object, "normalized", false);
    out.element_size = static_cast<size_t>(size) * static_cast<size_t>(out.components);
    out.data = nullptr;
    out.stride = out.element_size;

    const i32 view_index = int_at(object, "bufferView", -1);
    if (view_index < 0) return true;
    if (view_index >= static_cast<i32>(views.size())) return fail(error, "gltf: accessor references a missing bufferView");

    const BufferViewInfo& view = views[static_cast<size_t>(view_index)];
    const i32 byte_offset = int_at(object, "byteOffset", 0);
    if (byte_offset < 0) return fail(error, "gltf: accessor has a negative byteOffset");
    out.stride = view.stride > 0 ? view.stride : out.element_size;
    if (out.stride < out.element_size) return fail(error, "gltf: bufferView byteStride is smaller than the accessor element");

    const size_t offset = static_cast<size_t>(byte_offset);
    const size_t needed =
        out.count > 0 ? offset + static_cast<size_t>(out.count - 1) * out.stride + out.element_size : offset;
    if (needed > view.size) return fail(error, "gltf: accessor reads past the end of its bufferView");

    out.data = view.data;
    if (offset > 0) out.data += offset;
    return true;
}

f32 read_float_component(const AccessorInfo& accessor, i32 index, i32 component) {
    if (!accessor.data) return 0.0f;
    const u8* p = accessor.data + static_cast<size_t>(index) * accessor.stride +
                  static_cast<size_t>(component) * static_cast<size_t>(component_size(accessor.component_type));
    f32 value = 0.0f;
    switch (accessor.component_type) {
        case 5120: {
            i8 v = 0;
            std::memcpy(&v, p, 1);
            value = static_cast<f32>(v);
            break;
        }
        case 5121: {
            u8 v = 0;
            std::memcpy(&v, p, 1);
            value = static_cast<f32>(v);
            break;
        }
        case 5122: {
            i16 v = 0;
            std::memcpy(&v, p, 2);
            value = static_cast<f32>(v);
            break;
        }
        case 5123: {
            u16 v = 0;
            std::memcpy(&v, p, 2);
            value = static_cast<f32>(v);
            break;
        }
        case 5125: {
            u32 v = 0;
            std::memcpy(&v, p, 4);
            value = static_cast<f32>(v);
            break;
        }
        case 5126:
            std::memcpy(&value, p, 4);
            break;
        default:
            break;
    }
    if (accessor.normalized) {
        switch (accessor.component_type) {
            case 5120:
                value /= 127.0f;
                break;
            case 5121:
                value /= 255.0f;
                break;
            case 5122:
                value /= 32767.0f;
                break;
            case 5123:
                value /= 65535.0f;
                break;
            case 5125:
                value /= 4294967295.0f;
                break;
            default:
                break;
        }
    }
    return value;
}

u32 read_index_component(const AccessorInfo& accessor, i32 index, i32 component) {
    if (!accessor.data) return 0;
    const u8* p = accessor.data + static_cast<size_t>(index) * accessor.stride +
                  static_cast<size_t>(component) * static_cast<size_t>(component_size(accessor.component_type));
    switch (accessor.component_type) {
        case 5121: {
            u8 v = 0;
            std::memcpy(&v, p, 1);
            return v;
        }
        case 5123: {
            u16 v = 0;
            std::memcpy(&v, p, 2);
            return v;
        }
        case 5125: {
            u32 v = 0;
            std::memcpy(&v, p, 4);
            return v;
        }
        default:
            return 0;
    }
}

Vec3 read_vec3(const AccessorInfo& accessor, i32 index) {
    return {read_float_component(accessor, index, 0), read_float_component(accessor, index, 1),
            read_float_component(accessor, index, 2)};
}

bool parse_glb(const std::vector<u8>& file, const u8*& json_data, size_t& json_size, const u8*& bin_data,
               size_t& bin_size, std::string& error) {
    if (file.size() < 12) return fail(error, "gltf: GLB header is truncated");
    if (read_u32(file.data() + 4) != 2) return fail(error, "gltf: only GLB version 2 is supported");
    const u32 declared = read_u32(file.data() + 8);
    if (declared < 12) return fail(error, "gltf: GLB length is invalid");
    const size_t limit = std::min<size_t>(static_cast<size_t>(declared), file.size());

    size_t offset = 12;
    while (offset + 8 <= limit) {
        const u32 chunk_length = read_u32(file.data() + offset);
        const u32 chunk_type = read_u32(file.data() + offset + 4);
        offset += 8;
        if (static_cast<size_t>(chunk_length) > limit - offset) return fail(error, "gltf: GLB chunk is truncated");
        if (chunk_type == kChunkJson && !json_data) {
            json_data = file.data() + offset;
            json_size = chunk_length;
        } else if (chunk_type == kChunkBin && !bin_data) {
            bin_data = file.data() + offset;
            bin_size = chunk_length;
        }
        offset += chunk_length;
    }
    if (!json_data) return fail(error, "gltf: GLB has no JSON chunk");
    return true;
}

bool build_primitive(const json& object, const std::vector<AccessorInfo>& accessors, i32 material_count, bool skinned,
                     const Mat4& transform, GltfPrimitive& out, std::string& error) {
    if (int_at(object, "mode", 4) != 4) return fail(error, "gltf: only triangle primitives are supported");

    const json* attributes = field(object, "attributes");
    if (!attributes || !attributes->is_object()) return fail(error, "gltf: primitive has no attributes");

    const i32 position_index = int_at(*attributes, "POSITION", -1);
    if (position_index < 0) return fail(error, "gltf: primitive has no POSITION attribute");
    if (position_index >= static_cast<i32>(accessors.size())) return fail(error, "gltf: POSITION references a missing accessor");
    const AccessorInfo& positions = accessors[static_cast<size_t>(position_index)];
    if (positions.components != 3) return fail(error, "gltf: POSITION must be a VEC3 accessor");
    const i32 count = positions.count;
    if (count <= 0) return true;

    AccessorInfo normals;
    AccessorInfo uvs;
    AccessorInfo joints;
    AccessorInfo weights;
    bool has_normals = false;
    bool has_uvs = false;
    bool has_joints = false;
    bool has_weights = false;

    const auto optional = [&](const char* name, i32 components, const char* label, AccessorInfo& target,
                              bool& present) -> bool {
        const i32 index = int_at(*attributes, name, -1);
        present = index >= 0;
        if (!present) return true;
        if (index >= static_cast<i32>(accessors.size())) {
            return fail(error, std::string("gltf: ") + label + " references a missing accessor");
        }
        target = accessors[static_cast<size_t>(index)];
        if (target.components != components) {
            return fail(error, std::string("gltf: ") + label + " has the wrong component count");
        }
        if (target.count != count) {
            return fail(error, std::string("gltf: ") + label + " vertex count does not match POSITION");
        }
        return true;
    };

    if (!optional("NORMAL", 3, "NORMAL", normals, has_normals)) return false;
    if (!optional("TEXCOORD_0", 2, "TEXCOORD_0", uvs, has_uvs)) return false;
    if (!optional("JOINTS_0", 4, "JOINTS_0", joints, has_joints)) return false;
    if (!optional("WEIGHTS_0", 4, "WEIGHTS_0", weights, has_weights)) return false;

    if (has_joints && joints.component_type != 5121 && joints.component_type != 5123 && joints.component_type != 5125) {
        return fail(error, "gltf: JOINTS_0 must use unsigned integers");
    }
    if (has_weights && weights.component_type != 5126 && weights.component_type != 5121 &&
        weights.component_type != 5123 && weights.component_type != 5120 && weights.component_type != 5122) {
        return fail(error, "gltf: WEIGHTS_0 has an unsupported component type");
    }

    out.vertices.resize(static_cast<size_t>(count));
    for (i32 i = 0; i < count; ++i) {
        GltfVertex& vertex = out.vertices[static_cast<size_t>(i)];
        Vec3 position = read_vec3(positions, i);
        if (!skinned) position = transform.transform_point(position);
        vertex.position = position;
        if (has_normals) {
            Vec3 normal = read_vec3(normals, i);
            if (!skinned) normal = normalize(transform.transform_vector(normal));
            vertex.normal = normal;
        }
        if (has_uvs) vertex.uv = {read_float_component(uvs, i, 0), read_float_component(uvs, i, 1)};
        if (has_joints) {
            for (i32 c = 0; c < 4; ++c) {
                const u32 joint = read_index_component(joints, i, c);
                if (joint > 65535u) return fail(error, "gltf: joint index does not fit in 16 bits");
                vertex.joints[c] = static_cast<u16>(joint);
            }
        }
        if (has_weights) {
            for (i32 c = 0; c < 4; ++c) vertex.weights[c] = read_float_component(weights, i, c);
        }
        const f32 sum = vertex.weights[0] + vertex.weights[1] + vertex.weights[2] + vertex.weights[3];
        if (sum > 1.0e-8f) {
            const f32 inverse = 1.0f / sum;
            for (i32 c = 0; c < 4; ++c) vertex.weights[c] *= inverse;
        } else {
            vertex.weights[0] = 1.0f;
            vertex.weights[1] = 0.0f;
            vertex.weights[2] = 0.0f;
            vertex.weights[3] = 0.0f;
        }
    }

    const i32 index_accessor_index = int_at(object, "indices", -1);
    if (index_accessor_index >= 0) {
        if (index_accessor_index >= static_cast<i32>(accessors.size())) {
            return fail(error, "gltf: indices reference a missing accessor");
        }
        const AccessorInfo& indices = accessors[static_cast<size_t>(index_accessor_index)];
        if (indices.components != 1) return fail(error, "gltf: indices must be a SCALAR accessor");
        if (indices.component_type != 5121 && indices.component_type != 5123 && indices.component_type != 5125) {
            return fail(error, "gltf: indices must use unsigned integers");
        }
        out.indices.resize(static_cast<size_t>(indices.count));
        for (i32 i = 0; i < indices.count; ++i) {
            const u32 value = read_index_component(indices, i, 0);
            if (value >= static_cast<u32>(count)) return fail(error, "gltf: index is out of range");
            out.indices[static_cast<size_t>(i)] = value;
        }
    } else {
        out.indices.resize(static_cast<size_t>(count));
        for (i32 i = 0; i < count; ++i) out.indices[static_cast<size_t>(i)] = static_cast<u32>(i);
    }

    const i32 material = int_at(object, "material", -1);
    if (material >= material_count) return fail(error, "gltf: primitive references a missing material");
    out.material = material;
    return true;
}

std::string lower_copy(const std::string& text) {
    std::string out = text;
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

bool ends_with(const std::string& text, const std::string& suffix) {
    if (text.size() < suffix.size()) return false;
    return text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool parse_model(const std::vector<u8>& file, GltfModel& model, std::string& error, i32 max_texture_size) {
    const u8* json_data = nullptr;
    size_t json_size = 0;
    const u8* bin_data = nullptr;
    size_t bin_size = 0;
    bool is_glb = false;

    if (file.size() >= 4 && read_u32(file.data()) == kGlbMagic) {
        is_glb = true;
        if (!parse_glb(file, json_data, json_size, bin_data, bin_size, error)) return false;
    } else {
        json_data = file.data();
        json_size = file.size();
    }
    if (!json_data || json_size == 0) return fail(error, "gltf: missing JSON data");

    const json root = json::parse(reinterpret_cast<const char*>(json_data),
                                  reinterpret_cast<const char*>(json_data) + json_size, nullptr, false);
    if (root.is_discarded()) return fail(error, "gltf: malformed JSON");
    if (!root.is_object()) return fail(error, "gltf: root is not an object");

    std::vector<BufferData> buffers;
    if (const json* buffers_json = field(root, "buffers"); buffers_json && buffers_json->is_array()) {
        buffers.reserve(buffers_json->size());
        for (size_t i = 0; i < buffers_json->size(); ++i) {
            const json& object = (*buffers_json)[i];
            BufferData buffer;
            const i32 byte_length = int_at(object, "byteLength", -1);
            if (byte_length < 0) return fail(error, "gltf: buffer has no byteLength");
            const json* uri = field(object, "uri");
            if (uri && uri->is_string()) {
                const std::string& text = uri->get_ref<const std::string&>();
                if (text.rfind("data:", 0) != 0) {
                    return fail(error, "gltf: external buffer URIs are not supported: " + text);
                }
                if (!decode_data_uri(text, buffer.owned)) return fail(error, "gltf: could not decode a data-URI buffer");
                buffer.data = buffer.owned.data();
                buffer.size = buffer.owned.size();
            } else {
                if (!is_glb || i != 0 || !bin_data) return fail(error, "gltf: buffer has no data");
                buffer.data = bin_data;
                buffer.size = bin_size;
            }
            if (buffer.size < static_cast<size_t>(byte_length)) {
                return fail(error, "gltf: buffer is shorter than its byteLength");
            }
            buffers.push_back(std::move(buffer));
        }
    }

    std::vector<BufferViewInfo> views;
    if (const json* views_json = field(root, "bufferViews"); views_json && views_json->is_array()) {
        views.reserve(views_json->size());
        for (size_t i = 0; i < views_json->size(); ++i) {
            const json& object = (*views_json)[i];
            const i32 buffer_index = int_at(object, "buffer", -1);
            const i32 byte_offset = int_at(object, "byteOffset", 0);
            const i32 byte_length = int_at(object, "byteLength", -1);
            const i32 byte_stride = int_at(object, "byteStride", 0);
            if (buffer_index < 0 || buffer_index >= static_cast<i32>(buffers.size())) {
                return fail(error, "gltf: bufferView references a missing buffer");
            }
            if (byte_offset < 0 || byte_length < 0) {
                return fail(error, "gltf: bufferView has a negative offset or length");
            }
            const BufferData& buffer = buffers[static_cast<size_t>(buffer_index)];
            const size_t offset = static_cast<size_t>(byte_offset);
            const size_t length = static_cast<size_t>(byte_length);
            if (offset > buffer.size || length > buffer.size - offset) {
                return fail(error, "gltf: bufferView reads past the end of its buffer");
            }
            BufferViewInfo view;
            view.data = buffer.data;
            if (offset > 0) view.data += offset;
            view.size = length;
            view.stride = byte_stride > 0 ? static_cast<size_t>(byte_stride) : 0;
            views.push_back(view);
        }
    }

    std::vector<AccessorInfo> accessors;
    if (const json* accessors_json = field(root, "accessors"); accessors_json && accessors_json->is_array()) {
        accessors.resize(accessors_json->size());
        for (size_t i = 0; i < accessors_json->size(); ++i) {
            if (!parse_accessor((*accessors_json)[i], views, accessors[i], error)) return false;
        }
    }

    struct ImageSource {
        const u8* data = nullptr;
        size_t size = 0;
        std::vector<u8> owned;
        bool external = false;
    };

    std::vector<ImageSource> images;
    if (const json* images_json = field(root, "images"); images_json && images_json->is_array()) {
        images.reserve(images_json->size());
        for (size_t i = 0; i < images_json->size(); ++i) {
            const json& object = (*images_json)[i];
            ImageSource source;
            const i32 view_index = int_at(object, "bufferView", -1);
            if (view_index >= 0) {
                if (view_index >= static_cast<i32>(views.size())) {
                    return fail(error, "gltf: image references a missing bufferView");
                }
                source.data = views[static_cast<size_t>(view_index)].data;
                source.size = views[static_cast<size_t>(view_index)].size;
            } else {
                const std::string uri = string_at(object, "uri", "");
                if (uri.empty()) return fail(error, "gltf: image has neither a bufferView nor a uri");
                if (uri.rfind("data:", 0) == 0) {
                    if (!decode_data_uri(uri, source.owned)) {
                        return fail(error, "gltf: could not decode a data-URI image");
                    }
                    source.data = source.owned.data();
                    source.size = source.owned.size();
                } else {
                    source.external = true;
                }
            }
            images.push_back(std::move(source));
        }
    }

    std::vector<i32> texture_sources;
    if (const json* textures_json = field(root, "textures"); textures_json && textures_json->is_array()) {
        texture_sources.reserve(textures_json->size());
        for (const json& object : *textures_json) texture_sources.push_back(int_at(object, "source", -1));
    }

    if (const json* materials_json = field(root, "materials"); materials_json && materials_json->is_array()) {
        model.materials.reserve(materials_json->size());
        for (size_t i = 0; i < materials_json->size(); ++i) {
            const json& object = (*materials_json)[i];
            GltfMaterial material;
            Image base_color;
            bool decoded = false;
            if (const json* pbr = field(object, "pbrMetallicRoughness"); pbr && pbr->is_object()) {
                if(const json* value=field(*pbr,"metallicFactor");value && value->is_number())
                    material.metallic_factor=clamp(static_cast<f32>(value->get<double>()),0.0f,1.0f);
                if(const json* value=field(*pbr,"roughnessFactor");value && value->is_number())
                    material.roughness_factor=clamp(static_cast<f32>(value->get<double>()),0.0f,1.0f);
                if (const json* factor = field(*pbr, "baseColorFactor"); factor && factor->is_array() && factor->size() >= 4) {
                    for (i32 c = 0; c < 4; ++c) {
                        const json& value = (*factor)[static_cast<size_t>(c)];
                        if (value.is_number()) material.base_color_factor[c] = static_cast<f32>(value.get<double>());
                    }
                }
                i32 texture_index = -1;
                if (const json* texture = field(*pbr, "baseColorTexture"); texture && texture->is_object()) {
                    texture_index = int_at(*texture, "index", -1);
                }
                if (texture_index >= 0) {
                    if (texture_index >= static_cast<i32>(texture_sources.size())) {
                        return fail(error, "gltf: material references a missing texture");
                    }
                    const i32 image_index = texture_sources[static_cast<size_t>(texture_index)];
                    if (image_index < 0 || image_index >= static_cast<i32>(images.size())) {
                        return fail(error, "gltf: texture references a missing image");
                    }
                    const ImageSource& image = images[static_cast<size_t>(image_index)];
                    if (image.external) return fail(error, "gltf: external image URIs are not supported");
                    if (!image.data || image.size == 0) return fail(error, "gltf: base colour image has no data");
                    if (!decode_image(image.data, image.size, base_color)) {
                        return fail(error, "gltf: could not decode the base colour image");
                    }
                    decoded = true;
                }
                if(const json* texture=field(*pbr,"metallicRoughnessTexture");texture && texture->is_object()) {
                    const i32 index=int_at(*texture,"index",-1);
                    if(index<0 || index>=static_cast<i32>(texture_sources.size()))
                        return fail(error,"gltf: material references a missing metallic-roughness texture");
                    const i32 image_index=texture_sources[static_cast<size_t>(index)];
                    if(image_index<0 || image_index>=static_cast<i32>(images.size()))
                        return fail(error,"gltf: metallic-roughness texture references a missing image");
                    const ImageSource& image=images[static_cast<size_t>(image_index)];
                    Image data;
                    if(image.external || !image.data || !decode_image(image.data,image.size,data))
                        return fail(error,"gltf: could not decode the metallic-roughness image");
                    material.metallic_roughness=downsample_image(data,max_texture_size);
                }
            }
            material.double_sided = bool_at(object, "doubleSided", false);
            if (!decoded) base_color = Image(1, 1, Color{255, 255, 255, 255});
            material.base_color = downsample_image(base_color, max_texture_size);
            model.materials.push_back(std::move(material));
        }
    }

    if (const json* nodes_json = field(root, "nodes"); nodes_json && nodes_json->is_array()) {
        model.nodes.resize(nodes_json->size());
        for (size_t i = 0; i < nodes_json->size(); ++i) {
            const json& object = (*nodes_json)[i];
            GltfNode& node = model.nodes[i];
            node.name = string_at(object, "name", "");
            if (const json* matrix = field(object, "matrix"); matrix && matrix->is_array() && matrix->size() >= 16) {
                node.has_matrix = true;
                for (size_t c = 0; c < 16; ++c) node.matrix.m[c] = number_at(*matrix, c);
            }
            if (const json* translation = field(object, "translation");
                translation && translation->is_array() && translation->size() >= 3) {
                node.translation = {number_at(*translation, 0), number_at(*translation, 1), number_at(*translation, 2)};
            }
            if (const json* rotation = field(object, "rotation"); rotation && rotation->is_array() && rotation->size() >= 4) {
                node.rotation = {number_at(*rotation, 0), number_at(*rotation, 1), number_at(*rotation, 2),
                                 number_at(*rotation, 3)};
            }
            if (const json* scale = field(object, "scale"); scale && scale->is_array() && scale->size() >= 3) {
                node.scale = {number_at(*scale, 0), number_at(*scale, 1), number_at(*scale, 2)};
            }
            if (const json* children = field(object, "children"); children && children->is_array()) {
                node.children.reserve(children->size());
                for (const json& child : *children) {
                    if (!child.is_number()) return fail(error, "gltf: node child is not an index");
                    node.children.push_back(static_cast<i32>(child.get<double>()));
                }
            }
            node.mesh = int_at(object, "mesh", -1);
            node.skin = int_at(object, "skin", -1);
        }
        for (size_t i = 0; i < model.nodes.size(); ++i) {
            for (i32 child : model.nodes[i].children) {
                if (child < 0 || child >= static_cast<i32>(model.nodes.size())) {
                    return fail(error, "gltf: node child index is out of range");
                }
                if (child == static_cast<i32>(i)) return fail(error, "gltf: node is its own child");
                GltfNode& target = model.nodes[static_cast<size_t>(child)];
                if (target.parent >= 0 && target.parent != static_cast<i32>(i)) {
                    return fail(error, "gltf: node has more than one parent");
                }
                target.parent = static_cast<i32>(i);
            }
        }
    }

    i32 root_node = -1;
    if (const json* scenes_json = field(root, "scenes"); scenes_json && scenes_json->is_array() && !scenes_json->empty()) {
        const i32 scene_index = int_at(root, "scene", 0);
        if (scene_index < 0 || scene_index >= static_cast<i32>(scenes_json->size())) {
            return fail(error, "gltf: scene index is out of range");
        }
        const json& scene = (*scenes_json)[static_cast<size_t>(scene_index)];
        if (const json* scene_nodes = field(scene, "nodes");
            scene_nodes && scene_nodes->is_array() && !scene_nodes->empty()) {
            const json& first = (*scene_nodes)[0];
            if (!first.is_number()) return fail(error, "gltf: scene node is not an index");
            root_node = static_cast<i32>(first.get<double>());
            if (root_node < 0 || root_node >= static_cast<i32>(model.nodes.size())) {
                return fail(error, "gltf: scene node is out of range");
            }
        }
    }
    if (root_node < 0) {
        for (size_t i = 0; i < model.nodes.size(); ++i) {
            if (model.nodes[i].parent < 0) {
                root_node = static_cast<i32>(i);
                break;
            }
        }
    }
    model.root = root_node;

    std::vector<Mat4> globals(model.nodes.size(), Mat4::identity());
    std::vector<u8> state(model.nodes.size(), 0);
    std::vector<i32> chain;
    for (size_t i = 0; i < model.nodes.size(); ++i) {
        if (state[i] == 2) continue;
        chain.clear();
        i32 current = static_cast<i32>(i);
        while (current >= 0 && state[static_cast<size_t>(current)] == 0) {
            state[static_cast<size_t>(current)] = 1;
            chain.push_back(current);
            current = model.nodes[static_cast<size_t>(current)].parent;
        }
        if (current >= 0 && state[static_cast<size_t>(current)] == 1) {
            return fail(error, "gltf: node hierarchy contains a cycle");
        }
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            const i32 index = *it;
            const GltfNode& node = model.nodes[static_cast<size_t>(index)];
            const Mat4 local = node.has_matrix ? node.matrix : Mat4::trs(node.translation, node.rotation, node.scale);
            globals[static_cast<size_t>(index)] =
                node.parent >= 0 ? globals[static_cast<size_t>(node.parent)] * local : local;
            state[static_cast<size_t>(index)] = 2;
        }
    }

    if (const json* skins_json = field(root, "skins"); skins_json && skins_json->is_array()) {
        model.skins.resize(skins_json->size());
        for (size_t i = 0; i < skins_json->size(); ++i) {
            const json& object = (*skins_json)[i];
            GltfSkin& skin = model.skins[i];
            const json* joints = field(object, "joints");
            if (!joints || !joints->is_array() || joints->empty()) return fail(error, "gltf: skin has no joints");
            skin.joints.reserve(joints->size());
            for (const json& joint : *joints) {
                if (!joint.is_number()) return fail(error, "gltf: skin joint is not an index");
                const i32 index = static_cast<i32>(joint.get<double>());
                if (index < 0 || index >= static_cast<i32>(model.nodes.size())) {
                    return fail(error, "gltf: skin joint is out of range");
                }
                skin.joints.push_back(index);
            }
            const i32 inverse_index = int_at(object, "inverseBindMatrices", -1);
            if (inverse_index >= 0) {
                if (inverse_index >= static_cast<i32>(accessors.size())) {
                    return fail(error, "gltf: skin references a missing accessor");
                }
                const AccessorInfo& inverse = accessors[static_cast<size_t>(inverse_index)];
                if (inverse.components != 16 || inverse.component_type != 5126) {
                    return fail(error, "gltf: inverseBindMatrices must be a MAT4 float accessor");
                }
                if (inverse.count < static_cast<i32>(skin.joints.size())) {
                    return fail(error, "gltf: inverseBindMatrices has too few matrices");
                }
                skin.inverse_bind.resize(skin.joints.size());
                for (size_t j = 0; j < skin.joints.size(); ++j) {
                    Mat4& matrix = skin.inverse_bind[j];
                    for (i32 c = 0; c < 16; ++c) {
                        matrix.m[c] = read_float_component(inverse, static_cast<i32>(j), c);
                    }
                }
            } else {
                skin.inverse_bind.assign(skin.joints.size(), Mat4::identity());
            }
        }
    }

    const json* meshes_json = field(root, "meshes");
    for (size_t i = 0; i < model.nodes.size(); ++i) {
        const GltfNode& node = model.nodes[i];
        if (node.mesh < 0) continue;
        if (!meshes_json || !meshes_json->is_array() || node.mesh >= static_cast<i32>(meshes_json->size())) {
            return fail(error, "gltf: node references a missing mesh");
        }
        const bool node_skinned = node.skin >= 0;
        if (node_skinned) {
            if (node.skin >= static_cast<i32>(model.skins.size())) {
                return fail(error, "gltf: node references a missing skin");
            }
            if (!model.skinned) {
                model.skinned = true;
                model.skin_index = node.skin;
            }
        }
        const Mat4 transform = node_skinned ? Mat4::identity() : globals[i];
        const json& mesh = (*meshes_json)[static_cast<size_t>(node.mesh)];
        const json* primitives = field(mesh, "primitives");
        if (!primitives || !primitives->is_array()) return fail(error, "gltf: mesh has no primitives");
        for (const json& primitive_json : *primitives) {
            GltfPrimitive primitive;
            if (!build_primitive(primitive_json, accessors, static_cast<i32>(model.materials.size()), node_skinned,
                                 transform, primitive, error)) {
                return false;
            }
            if (!primitive.vertices.empty()) model.primitives.push_back(std::move(primitive));
        }
    }

    if (const json* animations_json = field(root, "animations"); animations_json && animations_json->is_array()) {
        for (const json& object : *animations_json) {
            GltfAnimation animation;
            animation.name = string_at(object, "name", "");

            struct Sampler {
                AccessorInfo input;
                AccessorInfo output;
                bool step = false;
            };

            std::vector<Sampler> samplers;
            const json* samplers_json = field(object, "samplers");
            if (!samplers_json || !samplers_json->is_array() || samplers_json->empty()) {
                return fail(error, "gltf: animation has no samplers");
            }
            samplers.reserve(samplers_json->size());
            for (const json& sampler_json : *samplers_json) {
                const i32 input_index = int_at(sampler_json, "input", -1);
                const i32 output_index = int_at(sampler_json, "output", -1);
                if (input_index < 0 || input_index >= static_cast<i32>(accessors.size())) {
                    return fail(error, "gltf: animation sampler has a missing input accessor");
                }
                if (output_index < 0 || output_index >= static_cast<i32>(accessors.size())) {
                    return fail(error, "gltf: animation sampler has a missing output accessor");
                }
                Sampler sampler;
                sampler.input = accessors[static_cast<size_t>(input_index)];
                sampler.output = accessors[static_cast<size_t>(output_index)];
                const std::string interpolation = string_at(sampler_json, "interpolation", "LINEAR");
                if (interpolation == "LINEAR") {
                    sampler.step = false;
                } else if (interpolation == "STEP") {
                    sampler.step = true;
                } else if (interpolation == "CUBICSPLINE") {
                    return fail(error, "gltf: CUBICSPLINE animation samplers are not supported");
                } else {
                    return fail(error, "gltf: unknown animation interpolation: " + interpolation);
                }
                samplers.push_back(sampler);
            }

            const json* channels_json = field(object, "channels");
            if (!channels_json || !channels_json->is_array()) return fail(error, "gltf: animation has no channels");
            for (const json& channel_json : *channels_json) {
                const i32 sampler_index = int_at(channel_json, "sampler", -1);
                if (sampler_index < 0 || sampler_index >= static_cast<i32>(samplers.size())) {
                    return fail(error, "gltf: animation channel references a missing sampler");
                }
                const json* target = field(channel_json, "target");
                if (!target || !target->is_object()) return fail(error, "gltf: animation channel has no target");
                const i32 node_index = int_at(*target, "node", -1);
                const std::string path_name = string_at(*target, "path", "");
                if (path_name == "weights") continue;
                GltfPath path;
                if (path_name == "translation") {
                    path = GltfPath::Translation;
                } else if (path_name == "rotation") {
                    path = GltfPath::Rotation;
                } else if (path_name == "scale") {
                    path = GltfPath::Scale;
                } else {
                    return fail(error, "gltf: unknown animation target path: " + path_name);
                }
                if (node_index < 0 || node_index >= static_cast<i32>(model.nodes.size())) {
                    return fail(error, "gltf: animation channel targets a missing node");
                }
                const Sampler& sampler = samplers[static_cast<size_t>(sampler_index)];
                if (sampler.input.components != 1 || sampler.input.component_type != 5126) {
                    return fail(error, "gltf: animation input must be a SCALAR float accessor");
                }
                const i32 key_count = sampler.input.count;
                if (sampler.output.count < key_count) {
                    return fail(error, "gltf: animation output has fewer keys than its input");
                }

                GltfChannel channel;
                channel.node = node_index;
                channel.path = path;
                channel.step = sampler.step;
                channel.times.resize(static_cast<size_t>(key_count));
                for (i32 k = 0; k < key_count; ++k) {
                    const f32 time = read_float_component(sampler.input, k, 0);
                    channel.times[static_cast<size_t>(k)] = time;
                    animation.duration = std::max(animation.duration, time);
                }

                if (path == GltfPath::Rotation) {
                    if (sampler.output.components != 4) {
                        return fail(error, "gltf: rotation animation output must be VEC4");
                    }
                    channel.quat.resize(static_cast<size_t>(key_count));
                    for (i32 k = 0; k < key_count; ++k) {
                        channel.quat[static_cast<size_t>(k)] =
                            quat_normalize({read_float_component(sampler.output, k, 0),
                                            read_float_component(sampler.output, k, 1),
                                            read_float_component(sampler.output, k, 2),
                                            read_float_component(sampler.output, k, 3)});
                    }
                } else {
                    if (sampler.output.components != 3) {
                        return fail(error, "gltf: translation and scale animation output must be VEC3");
                    }
                    channel.vec3.resize(static_cast<size_t>(key_count));
                    for (i32 k = 0; k < key_count; ++k) channel.vec3[static_cast<size_t>(k)] = read_vec3(sampler.output, k);
                }
                animation.channels.push_back(std::move(channel));
            }
            model.animations.push_back(std::move(animation));
        }
    }

    for (size_t i = 0; i < model.nodes.size(); ++i) {
        if (model.nodes[i].name == "mixamorig:RightHand") {
            model.hand_node = static_cast<i32>(i);
            break;
        }
    }
    if (model.hand_node < 0) {
        for (size_t i = 0; i < model.nodes.size(); ++i) {
            if (model.nodes[i].name.find("RightHand") != std::string::npos) {
                model.hand_node = static_cast<i32>(i);
                break;
            }
        }
    }

    bool any_position = false;
    f32 min_x = std::numeric_limits<f32>::max();
    f32 min_y = std::numeric_limits<f32>::max();
    f32 min_z = std::numeric_limits<f32>::max();
    f32 max_x = std::numeric_limits<f32>::lowest();
    f32 max_y = std::numeric_limits<f32>::lowest();
    f32 max_z = std::numeric_limits<f32>::lowest();
    for (const GltfPrimitive& primitive : model.primitives) {
        for (const GltfVertex& vertex : primitive.vertices) {
            min_x = std::min(min_x, vertex.position.x);
            min_y = std::min(min_y, vertex.position.y);
            min_z = std::min(min_z, vertex.position.z);
            max_x = std::max(max_x, vertex.position.x);
            max_y = std::max(max_y, vertex.position.y);
            max_z = std::max(max_z, vertex.position.z);
            any_position = true;
        }
    }
    if (any_position) {
        model.bounds_min = {min_x, min_y, min_z};
        model.bounds_max = {max_x, max_y, max_z};
    }
    return true;
}

}  // namespace

const GltfChannel* GltfAnimation::channel(i32 node, GltfPath path) const {
    for (const GltfChannel& candidate : channels) {
        if (candidate.node == node && candidate.path == path) return &candidate;
    }
    return nullptr;
}

i32 GltfModel::node_by_name(const char* name) const {
    if (!name) return -1;
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i].name == name) return static_cast<i32>(i);
    }
    return -1;
}

const GltfAnimation* GltfModel::animation(const char* name) const {
    if (!name || !*name) return nullptr;
    const std::string needle = lower_copy(name);
    for (const GltfAnimation& candidate : animations) {
        if (lower_copy(candidate.name) == needle) return &candidate;
    }
    for (const GltfAnimation& candidate : animations) {
        if (ends_with(lower_copy(candidate.name), needle)) return &candidate;
    }
    for (const GltfAnimation& candidate : animations) {
        if (lower_copy(candidate.name).find(needle) != std::string::npos) return &candidate;
    }
    return nullptr;
}

bool GltfModel::load(const char* path, std::string& error, i32 max_texture_size) {
    *this = GltfModel{};
    error.clear();
    if (!path || !*path) {
        error = "gltf: empty path";
        return false;
    }
    std::vector<u8> file;
    if (!read_file(path, file)) {
        error = std::string("gltf: could not read ") + path;
        return false;
    }
    GltfModel model;
    if (!parse_model(file, model, error, max_texture_size)) return false;
    *this = std::move(model);
    return true;
}

Image downsample_image(const Image& image, i32 max_size) {
    if (image.width <= 0 || image.height <= 0) return {};
    if (image.pixels.size() != static_cast<size_t>(image.width) * static_cast<size_t>(image.height)) return {};
    if (max_size <= 0) return image;
    const i32 largest = std::max(image.width, image.height);
    if (largest <= max_size) return image;

    const f32 scale = static_cast<f32>(max_size) / static_cast<f32>(largest);
    const i32 width = std::max(1, static_cast<i32>(std::lround(static_cast<f64>(image.width) * scale)));
    const i32 height = std::max(1, static_cast<i32>(std::lround(static_cast<f64>(image.height) * scale)));

    Image out(width, height);
    for (i32 y = 0; y < height; ++y) {
        const i32 y0 = static_cast<i32>((static_cast<i64>(y) * image.height) / height);
        i32 y1 = static_cast<i32>((static_cast<i64>(y + 1) * image.height) / height);
        if (y1 <= y0) y1 = y0 + 1;
        for (i32 x = 0; x < width; ++x) {
            const i32 x0 = static_cast<i32>((static_cast<i64>(x) * image.width) / width);
            i32 x1 = static_cast<i32>((static_cast<i64>(x + 1) * image.width) / width);
            if (x1 <= x0) x1 = x0 + 1;
            u32 red = 0;
            u32 green = 0;
            u32 blue = 0;
            u32 alpha = 0;
            u32 count = 0;
            for (i32 sy = y0; sy < y1; ++sy) {
                for (i32 sx = x0; sx < x1; ++sx) {
                    const Color& pixel = image.at(sx, sy);
                    red += pixel.r;
                    green += pixel.g;
                    blue += pixel.b;
                    alpha += pixel.a;
                    ++count;
                }
            }
            const u32 half = count / 2;
            out.at(x, y) = Color{static_cast<u8>((red + half) / count), static_cast<u8>((green + half) / count),
                                 static_cast<u8>((blue + half) / count), static_cast<u8>((alpha + half) / count)};
        }
    }
    return out;
}

}  // namespace zl
