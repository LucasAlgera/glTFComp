#define TINYGLTF_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define JSON_NOEXCEPTION 
#define TINYGLTF_NOEXCEPTION 
#define TINYGLTF_ENABLE_DRACO  

#include "gltf_loader.h"

//tinygltf
#include "../external/tinygltf/tiny_gltf.h"

//pybind11
#include "../external/pybind11/include/pybind11/pybind11.h"
#include "../external/pybind11/include/pybind11/numpy.h"
#include <pybind11/stl.h>

//miniz
#include "../external/miniz/miniz.h"

//draco
#include "../external/draco/src/draco/compression/encode.h"
#include "../external/draco/src/draco/compression/decode.h"
#include "../external/draco/src/draco/mesh/mesh.h"
#include "../external/draco/src/draco/point_cloud/point_cloud.h"

//stl
#include <iostream>
#include <vector>
#include <string>
#include <unordered_map>
#include <memory>
#include <fstream>
#include <cstdio>

#include "Windows.h"

// profiling
#include <chrono>
#include <iomanip>

// Most of the GLTF exporter code was from the examples in https://github.com/syoyo/tinygltf

namespace py = pybind11;
draco::Encoder encoder;

// RAII profiler done by also watching cherno's profiling C++ video
// https://www.youtube.com/watch?v=YG4jexlSAjc

class Profiler {
private:
    std::string name;
    std::chrono::high_resolution_clock::time_point start;

public:
    Profiler(const std::string& funcName) : name(funcName) {
        start = std::chrono::high_resolution_clock::now();
    }

    ~Profiler() {
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        std::cout << "[PROFILE] " << name << ": " << duration << " ms" << std::endl;
    }
};
// Macros for the start of the function or scope. 
#define PROFILE_FUNCTION() Profiler profiler(__FUNCTION__)
#define PROFILE_SCOPE(name) Profiler profiler(name)


struct TextureData 
{
    std::string type;
    std::string filepath; // if filepath texture
    std::vector<uint8_t> data; // if packed texture
    int width = 0;
    int height = 0;
    int channels = 0;
    std::string name;
};

struct Vertex 
{
    float position[3];
    float normal[3];
    float texcoord[2];
};

struct Material 
{
    std::string name;
    float baseColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    float metallicFactor = 0.0f;
    float roughnessFactor = 1.0f;
    int baseColorTexture = -1;
    int normalTexture = -1;
    int metallicRoughnessTexture = -1;
};

struct Mesh 
{
    std::string name;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    int materialIndex = -1;
    int primitiveMode = TINYGLTF_MODE_TRIANGLES; // Default to triangles
    bool useDracoCompression = true;
    int dracoCompressionLevel = 7; // 7 default (most stable speed)
};

struct Node 
{
    std::string name;
    float transform[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 }; // Identity matrix
    int meshIndex = -1;
    std::vector<int> children;
};

class GLTFExporter 
{
private:
    tinygltf::Model model;
    std::unordered_map<std::string, int> textureCache;
    std::vector<TextureData> textureList;
    std::string exportDir;
    bool useJpg = true;
    int jpgLevel = 100;

public:
    GLTFExporter() 
    {
        // Set up default scene
        model.defaultScene = 0;
        model.scenes.resize(1);
        model.scenes[0].name = "Scene";

        // Set metadata
        model.asset.version = "2.0";
        model.asset.generator = "Custom GLTF Exporter";
    }

    bool GLTFExporter::CompressToZip(const std::string& gltfPath,
        const std::string& zipPath,
        const std::vector<std::string>& texturePaths) 
    {
        PROFILE_FUNCTION();
        mz_zip_archive zip; //the zip file
        memset(&zip, 0, sizeof(zip));

        if (!mz_zip_writer_init_file(&zip, zipPath.c_str(), 0)) 
        {
            std::cerr << "Can't make ZIP: " << zipPath << std::endl;
            return false;
        }

        // glTF file
        if (!mz_zip_writer_add_file(&zip, "model.gltf", gltfPath.c_str(), nullptr, 0, MZ_BEST_COMPRESSION)) 
        {
            std::cerr << "Cant add glTF file" << std::endl;
            mz_zip_writer_end(&zip);
            return false;
        }

        // Add textures
        for (const auto& texPath : texturePaths) 
        {
            // extract filename 
            size_t lastSlash = texPath.find_last_of("/\\");
            std::string filename = (lastSlash != std::string::npos) ? texPath.substr(lastSlash + 1) : texPath;

            // does file exist?
            std::ifstream testFile(texPath);
            if (!testFile.good()) 
            {
                std::cerr << "File doesnt exist" << std::endl;
                continue;
            }
            testFile.close();

            if (!mz_zip_writer_add_file(&zip, filename.c_str(), texPath.c_str(), nullptr, 0, MZ_BEST_COMPRESSION)) 
            {
                std::cerr << "Cant add texture: " << filename << std::endl;
            }
        }

        mz_zip_writer_finalize_archive(&zip);
        mz_zip_writer_end(&zip);
        return true;
    }

    std::vector<uint8_t> CompressMesh(const Mesh& mesh)
    {
        PROFILE_FUNCTION();
        auto dracoMesh = std::make_unique<draco::Mesh>();

        size_t numVertices = mesh.vertices.size();
        size_t numFaces = mesh.indices.size() / 3;

        dracoMesh->set_num_points(numVertices);

        // first create attributes (tell draco positions normals and uvs exist)
        {
            PROFILE_SCOPE("Setting Attributes");

            // position 
            draco::GeometryAttribute pos_att;
            pos_att.Init(draco::GeometryAttribute::POSITION, nullptr, 3,
                draco::DT_FLOAT32, false, sizeof(float) * 3, 0);
            int pos_att_id = dracoMesh->AddAttribute(pos_att, true, numVertices);


            // normal 
            draco::GeometryAttribute norm_att;
            norm_att.Init(draco::GeometryAttribute::NORMAL, nullptr, 3,
                draco::DT_FLOAT32, false, sizeof(float) * 3, 0);
            int norm_att_id = dracoMesh->AddAttribute(norm_att, true, numVertices);

            // UV 
            draco::GeometryAttribute uv_att;
            uv_att.Init(draco::GeometryAttribute::TEX_COORD, nullptr, 2,
                draco::DT_FLOAT32, false, sizeof(float) * 2, 0);
            int uv_att_id = dracoMesh->AddAttribute(uv_att, true, numVertices);

            // Set vertex data to the newly created attributes
            for (size_t i = 0; i < numVertices; ++i) {
                const Vertex& v = mesh.vertices[i];

                dracoMesh->attribute(pos_att_id)->SetAttributeValue(
                    draco::AttributeValueIndex(i), v.position);
                dracoMesh->attribute(norm_att_id)->SetAttributeValue(
                    draco::AttributeValueIndex(i), v.normal);
                dracoMesh->attribute(uv_att_id)->SetAttributeValue(
                    draco::AttributeValueIndex(i), v.texcoord);
            }
        }
        {
            PROFILE_SCOPE("Adding faces");

            // Add faces to the draco mesh
            for (size_t i = 0; i < numFaces; ++i) {
                draco::Mesh::Face face;
                face[0] = mesh.indices[i * 3];
                face[1] = mesh.indices[i * 3 + 1];
                face[2] = mesh.indices[i * 3 + 2];
                dracoMesh->AddFace(face);
            }
        }

        {
            PROFILE_SCOPE("Setting Quantization");

            // set up encoder
            draco::Encoder encoder;
            // draco uses quantization to compress the data, here we feed the data we want to compress and to what bit level. 
            encoder.SetAttributeQuantization(draco::GeometryAttribute::POSITION, 14);
            encoder.SetAttributeQuantization(draco::GeometryAttribute::NORMAL, 10);
            encoder.SetAttributeQuantization(draco::GeometryAttribute::TEX_COORD, 12);
            // draco uses "speed options" to choose which compression algorithm should be used and at which "agression level. 
            // speed goes from 1 - 10
            encoder.SetSpeedOptions(10 - mesh.dracoCompressionLevel, 10 - mesh.dracoCompressionLevel);
        }


        {
            PROFILE_SCOPE("Draco Encoding");

            // Encode mesh
            draco::EncoderBuffer buffer;
            draco::Status status = encoder.EncodeMeshToBuffer(*dracoMesh, &buffer);
        

        if (!status.ok()) {
            std::cerr << "Draco encoding failed: " << status.error_msg() << std::endl;
            return {};
        }

        return std::vector<uint8_t>(buffer.data(), buffer.data() + buffer.size());
        }
    }

    void PushTextures(TextureData texture)
    {
        textureList.push_back(texture);
    }

    void SetExportDirectory(std::string dir)
    {
        exportDir = dir;
        //make sure the string ends with a "/"
        if (!exportDir.empty() && exportDir.back() != '/' && exportDir.back() != '\\') {
            exportDir += "/";
        }
    }
    void SetUseJpg(bool usejpg, int level)
    {
        useJpg = usejpg;
        jpgLevel = level;
    }

    int AddTexture(int idx) 
    {
        // We first either load a packed texture or load a texture file. 
        // We then set up all the tinygltf image and texture variables.
        // Finally we export the textures to either png or jpeg (specified by user).



        //if (textureCache.find(filepath) != textureCache.end()) {
        //    return textureCache[filepath];
        //}
        if (idx < 0 || idx >= static_cast<int>(textureList.size()))
        {
            return -1;
        }

        // standard is set to 8
        stbi_write_png_compression_level = 9;

        if (textureList[idx].type == "file")
        {
            // Load image data
            int width = 0, height = 0, channels = 0;
            unsigned char* data = stbi_load(textureList[idx].filepath.c_str(), &width, &height, &channels, 0);

            if (!data) {
                std::cerr << "Failed to load texture: " << textureList[idx].filepath.c_str() << std::endl;
                return -1;
            }

            size_t dataSize = width * height * channels;
            tinygltf::Image image;
            image.name = textureList[idx].name;
            image.width = width;
            image.height = height;
            image.component = channels;
            image.bits = 8;
            image.pixel_type = TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;
            std::string ext = useJpg ? ".jpg" : ".png";
            std::string fileName = std::to_string(idx) + ext;
            std::string fullPath = exportDir + fileName;

            image.uri = fileName;
            image.mimeType = useJpg ? "image/jpeg" : "image/png";

            if (useJpg) 
            {
                stbi_write_jpg(fullPath.c_str(), width, height, channels, data, jpgLevel);
            }
            else 
            {
                stbi_write_png(fullPath.c_str(), width, height, channels, data, width * channels);
            }

            int imageIndex = static_cast<int>(model.images.size());
            model.images.push_back(image);

            // Create texture
            tinygltf::Texture texture;
            texture.source = imageIndex;
            texture.sampler = 0;

            int textureIndex = static_cast<int>(model.textures.size());
            model.textures.push_back(texture);

            //textureCache[filepath] = textureIndex;

            std::cout << "Texture index: " << textureIndex << std::endl;
            return textureIndex;
        }


        if (textureList[idx].type == "packed")
        {
            int width = textureList[idx].width, height = textureList[idx].height, channels = textureList[idx].channels;

            size_t dataSize = width * height * channels;
            tinygltf::Image image;
            image.name = textureList[idx].name;
            image.width = width;
            image.height = height;
            image.component = channels;
            image.bits = 8;
            image.pixel_type = TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;

            std::string ext = useJpg ? ".jpg" : ".png";
            std::string fileName = std::to_string(idx) + ext;
            std::string fullPath = exportDir + fileName;

            image.uri = fileName;
            image.mimeType = useJpg ? "image/jpeg" : "image/png";

            if (useJpg) 
            {
                stbi_write_jpg(fullPath.c_str(), width, height, channels, textureList[idx].data.data(), jpgLevel);
            }
            else 
            {
                stbi_write_png(fullPath.c_str(), width, height, channels, textureList[idx].data.data(), width * channels);
            }

            int imageIndex = static_cast<int>(model.images.size());
            model.images.push_back(image);

            // Create texture
            tinygltf::Texture texture;
            texture.source = imageIndex;
            texture.sampler = 0;

            int textureIndex = static_cast<int>(model.textures.size());
            model.textures.push_back(texture);

            //textureCache[filepath] = textureIndex;

            std::cout << "Texture index: " << textureIndex << std::endl;
            return textureIndex;
        }
        return -1;
    }

    // Add a material
    int AddMaterial(const Material& mat) 
    {
        tinygltf::Material gltfMat;
        gltfMat.name = mat.name;

        // PBR Metallic Roughness
        gltfMat.pbrMetallicRoughness.baseColorFactor = {
            mat.baseColor[0], mat.baseColor[1], mat.baseColor[2], mat.baseColor[3]
        };
        gltfMat.pbrMetallicRoughness.metallicFactor = mat.metallicFactor;
        gltfMat.pbrMetallicRoughness.roughnessFactor = mat.roughnessFactor;

        // Add textures if provided
        if (mat.baseColorTexture != -1) 
        {
            int texIndex = AddTexture(mat.baseColorTexture);
            if (texIndex >= 0) 
            {
                gltfMat.pbrMetallicRoughness.baseColorTexture.index = texIndex;
            }
        }

        if (mat.metallicRoughnessTexture != -1) 
        {
            int texIndex = AddTexture(mat.metallicRoughnessTexture);
            if (texIndex >= 0) 
            {
                gltfMat.pbrMetallicRoughness.metallicRoughnessTexture.index = texIndex;
            }
        }

        if (mat.normalTexture != -1)
        {
            int texIndex = AddTexture(mat.normalTexture);
            if (texIndex >= 0)
            {
                gltfMat.normalTexture.index = texIndex;
            }
        }

        int materialIndex = static_cast<int>(model.materials.size());
        model.materials.push_back(gltfMat);
        return materialIndex;
    }

    // Create buffer and buffer view for data
    int CreateBufferView(const void* data, size_t byteLength, int target = 0) 
    {
        if (model.buffers.empty()) 
        {
            model.buffers.resize(1);
            model.buffers[0].name = "buffer";
        }

        tinygltf::Buffer& buffer = model.buffers[0];
        size_t byteOffset = buffer.data.size();

        // Align to 4 bytes
        while (byteOffset % 4 != 0) 
        {
            buffer.data.push_back(0);
            byteOffset++;
        }

        // Copy data
        const unsigned char* bytes = static_cast<const unsigned char*>(data);
        buffer.data.insert(buffer.data.end(), bytes, bytes + byteLength);

        // Create buffer view
        tinygltf::BufferView bufferView;
        bufferView.buffer = 0;
        bufferView.byteOffset = byteOffset;
        bufferView.byteLength = byteLength;
        bufferView.byteStride = (target == TINYGLTF_TARGET_ARRAY_BUFFER) ? sizeof(Vertex) : 0;
        bufferView.target = target;

        int bufferViewIndex = static_cast<int>(model.bufferViews.size());
        model.bufferViews.push_back(bufferView);
        return bufferViewIndex;
    }

    int AddMesh(const Mesh& mesh) 
    {
        tinygltf::Mesh gltfMesh;
        gltfMesh.name = mesh.name;

        // Create primitive
        tinygltf::Primitive primitive;

        if (mesh.useDracoCompression)
        {
            std::vector<uint8_t> dracoData = CompressMesh(mesh);
            if (dracoData.empty())
            {
                std::cout << "Draco compression failed.. \n";
                // Do the uncompressed version
            }
            else
            {
                // buffer view for Draco
                int dracoBufferView = CreateBufferView(dracoData.data(), dracoData.size());

                tinygltf::Accessor posAccessor;
                posAccessor.bufferView = -1; // using the custom bufferview
                posAccessor.byteOffset = 0;
                posAccessor.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
                posAccessor.count = mesh.vertices.size();
                posAccessor.type = TINYGLTF_TYPE_VEC3;

                // Calculate bounds from original data https://discussions.unity.com/t/how-to-get-the-min-max-vertexs-pos-of-a-mesh-in-object-space/841241/5
                for (const auto& vertex : mesh.vertices) 
                {
                    for (int i = 0; i < 3; i++) {
                        if (posAccessor.minValues.empty()) 
                        {
                            posAccessor.minValues = { vertex.position[0], vertex.position[1], vertex.position[2] };
                            posAccessor.maxValues = { vertex.position[0], vertex.position[1], vertex.position[2] };
                        }
                        else {
                            posAccessor.minValues[i] = std::min(posAccessor.minValues[i], static_cast<double>(vertex.position[i]));
                            posAccessor.maxValues[i] = std::max(posAccessor.maxValues[i], static_cast<double>(vertex.position[i]));
                        }
                    }
                }

                int posAccessorIndex = static_cast<int>(model.accessors.size());
                model.accessors.push_back(posAccessor);
                primitive.attributes["POSITION"] = posAccessorIndex;

                // Normal accessor
                tinygltf::Accessor normalAccessor;
                normalAccessor.bufferView = -1;
                normalAccessor.byteOffset = 0;
                normalAccessor.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
                normalAccessor.count = mesh.vertices.size();
                normalAccessor.type = TINYGLTF_TYPE_VEC3;

                int normalAccessorIndex = static_cast<int>(model.accessors.size());
                model.accessors.push_back(normalAccessor);
                primitive.attributes["NORMAL"] = normalAccessorIndex;

                // Texture coordinate accessor
                tinygltf::Accessor texAccessor;
                texAccessor.bufferView = -1;
                texAccessor.byteOffset = 0;
                texAccessor.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
                texAccessor.count = mesh.vertices.size();
                texAccessor.type = TINYGLTF_TYPE_VEC2;

                int texAccessorIndex = static_cast<int>(model.accessors.size());
                model.accessors.push_back(texAccessor);
                primitive.attributes["TEXCOORD_0"] = texAccessorIndex;

                // Indices accessor
                tinygltf::Accessor indexAccessor;
                indexAccessor.bufferView = -1;
                indexAccessor.byteOffset = 0;
                indexAccessor.componentType = TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT;
                indexAccessor.count = mesh.indices.size();
                indexAccessor.type = TINYGLTF_TYPE_SCALAR;

                int indexAccessorIndex = static_cast<int>(model.accessors.size());
                model.accessors.push_back(indexAccessor);
                primitive.indices = indexAccessorIndex;

                // Add Draco extension to primitive
                tinygltf::Value dracoExtension(tinygltf::Value::Object{});
                auto& dracoObj = dracoExtension.Get<tinygltf::Value::Object>();
                dracoObj["bufferView"] = tinygltf::Value(dracoBufferView);

                tinygltf::Value attributes(tinygltf::Value::Object{});
                auto& attribObj = attributes.Get<tinygltf::Value::Object>();
                attribObj["POSITION"] = tinygltf::Value(0);
                attribObj["NORMAL"] = tinygltf::Value(1);
                attribObj["TEXCOORD_0"] = tinygltf::Value(2);

                dracoObj["attributes"] = std::move(attributes);
                primitive.extensions["KHR_draco_mesh_compression"] = std::move(dracoExtension);

                // Set primitive mode
                primitive.mode = mesh.primitiveMode;

                // Material
                if (mesh.materialIndex >= 0)
                {
                    primitive.material = mesh.materialIndex;
                }

                gltfMesh.primitives.push_back(primitive);

                int meshIndex = static_cast<int>(model.meshes.size());
                model.meshes.push_back(gltfMesh);

                return meshIndex;
            }
        }

        // Vertices
        if (!mesh.vertices.empty()) 
        {
            // Position accessor
            int posBufferView = CreateBufferView(
                mesh.vertices.data(),
                mesh.vertices.size() * sizeof(Vertex),
                TINYGLTF_TARGET_ARRAY_BUFFER
            );

            tinygltf::Accessor posAccessor;
            posAccessor.bufferView = posBufferView;
            posAccessor.byteOffset = offsetof(Vertex, position);
            posAccessor.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
            posAccessor.count = mesh.vertices.size();
            posAccessor.type = TINYGLTF_TYPE_VEC3;

            // Calculate bounds
            for (const auto& vertex : mesh.vertices) 
            {
                for (int i = 0; i < 3; i++) {
                    if (posAccessor.minValues.empty()) 
                    {
                        posAccessor.minValues = { vertex.position[0], vertex.position[1], vertex.position[2] };
                        posAccessor.maxValues = { vertex.position[0], vertex.position[1], vertex.position[2] };
                    }
                    else 
                    {
                        posAccessor.minValues[i] = std::min(posAccessor.minValues[i], static_cast<double>(vertex.position[i]));
                        posAccessor.maxValues[i] = std::max(posAccessor.maxValues[i], static_cast<double>(vertex.position[i]));
                    }
                }
            }

            int posAccessorIndex = static_cast<int>(model.accessors.size());
            model.accessors.push_back(posAccessor);
            primitive.attributes["POSITION"] = posAccessorIndex;

            // Normal accessor
            tinygltf::Accessor normalAccessor;
            normalAccessor.bufferView = posBufferView;
            normalAccessor.byteOffset = offsetof(Vertex, normal);
            normalAccessor.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
            normalAccessor.count = mesh.vertices.size();
            normalAccessor.type = TINYGLTF_TYPE_VEC3;

            int normalAccessorIndex = static_cast<int>(model.accessors.size());
            model.accessors.push_back(normalAccessor);
            primitive.attributes["NORMAL"] = normalAccessorIndex;

            // Texture coordinate accessor
            tinygltf::Accessor texAccessor;
            texAccessor.bufferView = posBufferView;
            texAccessor.byteOffset = offsetof(Vertex, texcoord);
            texAccessor.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
            texAccessor.count = mesh.vertices.size();
            texAccessor.type = TINYGLTF_TYPE_VEC2;

            int texAccessorIndex = static_cast<int>(model.accessors.size());
            model.accessors.push_back(texAccessor);
            primitive.attributes["TEXCOORD_0"] = texAccessorIndex;
        }

        // Indices
        if (!mesh.indices.empty()) 
        {
            int indexBufferView = CreateBufferView(
                mesh.indices.data(),
                mesh.indices.size() * sizeof(uint32_t),
                TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER
            );

            tinygltf::Accessor indexAccessor;
            indexAccessor.bufferView = indexBufferView;
            indexAccessor.componentType = TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT;
            indexAccessor.count = mesh.indices.size();
            indexAccessor.type = TINYGLTF_TYPE_SCALAR;

            int indexAccessorIndex = static_cast<int>(model.accessors.size());
            model.accessors.push_back(indexAccessor);
            primitive.indices = indexAccessorIndex;
        }

        // Set primitive mode (how to interpret vertex data)
        primitive.mode = mesh.primitiveMode;

        // Material
        if (mesh.materialIndex >= 0) {
            primitive.material = mesh.materialIndex;
        }

        gltfMesh.primitives.push_back(primitive);

        int meshIndex = static_cast<int>(model.meshes.size());
        model.meshes.push_back(gltfMesh);
        return meshIndex;
    }

    // Add a node
    int AddNode(const Node& node) 
    {
        tinygltf::Node gltfNode;
        gltfNode.name = node.name;

        // Transform matrix
        gltfNode.matrix = {
            node.transform[0], node.transform[1], node.transform[2], node.transform[3],
            node.transform[4], node.transform[5], node.transform[6], node.transform[7],
            node.transform[8], node.transform[9], node.transform[10], node.transform[11],
            node.transform[12], node.transform[13], node.transform[14], node.transform[15]
        };

        // Mesh reference
        if (node.meshIndex >= 0) {
            gltfNode.mesh = node.meshIndex;
        }

        // Children
        gltfNode.children = node.children;

        int nodeIndex = static_cast<int>(model.nodes.size());
        model.nodes.push_back(gltfNode);

        // Add to scene
        model.scenes[0].nodes.push_back(nodeIndex);

        return nodeIndex;
    }

    // Set up default sampler
    void SetupDefaultSampler() 
    {
        if (model.samplers.empty()) 
        {
            tinygltf::Sampler sampler;
            sampler.magFilter = TINYGLTF_TEXTURE_FILTER_LINEAR;
            sampler.minFilter = TINYGLTF_TEXTURE_FILTER_LINEAR_MIPMAP_LINEAR;
            sampler.wrapS = TINYGLTF_TEXTURE_WRAP_REPEAT;
            sampler.wrapT = TINYGLTF_TEXTURE_WRAP_REPEAT;
            model.samplers.push_back(sampler);
        }
    }
    void DeclareExtensions() 
    {
        model.extensionsUsed.push_back("KHR_draco_mesh_compression");
        model.extensionsRequired.push_back("KHR_draco_mesh_compression");
    }
    // Export to file
    bool ExportToFile(const std::string& filename, bool binary = false) 
    {
        SetupDefaultSampler();
        DeclareExtensions();

        tinygltf::TinyGLTF gltf;

        if (binary) {
            return gltf.WriteGltfSceneToFile(&model, filename, true, true, true, false);
        }
        else {
            return gltf.WriteGltfSceneToFile(&model, filename, true, true, false, false);
        }
    }

    // Export to string (JSON format only)
    std::string ExportToString() 
    {
        SetupDefaultSampler();

        tinygltf::TinyGLTF gltf;

        // Write to temporary file then read back
        std::string tempFile = "temp_export.gltf";
        bool success = gltf.WriteGltfSceneToFile(&model, tempFile, false, true, false, false);

        if (!success) {
            return "";
        }

        // Read file content
        std::ifstream file(tempFile);
        if (!file.is_open()) {
            return "";
        }

        std::string json_string((std::istreambuf_iterator<char>(file)),
            std::istreambuf_iterator<char>());

        // Clean up temp file
        std::remove(tempFile.c_str());

        return json_string;
    }
};

template <typename T>
std::vector<T> NumpyArrayToVector(py::array_t<T>& arrIn)
{
    PROFILE_FUNCTION();
    std::vector<T> vecOut;

    py::buffer_info buf = arrIn.request();
    T* data = static_cast<T*>(buf.ptr);
    size_t num_vertices = 1;
    for (auto dim : buf.shape) {
        num_vertices *= dim;
    }

    vecOut.reserve(num_vertices);

    for (size_t i = 0; i < num_vertices; i++)
    {
        vecOut.push_back(data[i]);
    }
    return vecOut;
}

std::vector<Vertex> StoreInVertex(
    const std::vector<float>& positions,
    const std::vector<float>& normals,
    const std::vector<float>& uvs,
    const std::vector<uint32_t>& indices)
{
    PROFILE_FUNCTION();
    size_t num_face_vertices = normals.size() / 3;
    std::vector<Vertex> vertices;
    vertices.reserve(num_face_vertices);

    bool hasUVs = !uvs.empty() && (uvs.size() >= num_face_vertices * 2);

    for (size_t i = 0; i < num_face_vertices; i++)
    {
        // Bounds check on indices
        if (i >= indices.size()) {
            std::cerr << "Index i=" << i << " out of bounds for indices" << std::endl;
            break;
        }

        Vertex v{};
        uint32_t pos_index = indices[i];

        // positions
        if (pos_index * 3 + 2 >= positions.size()) {
            std::cerr << "Position index " << pos_index << " out of bounds" << std::endl;
            continue;
        }

        // We switch Y and Z because blender is Z-Up whilst gltf is Y-Up
        // Y also becomes inverted to avoid a mirrored mesh

        v.position[0] = positions[pos_index * 3 + 0];
        v.position[1] = positions[pos_index * 3 + 2];
        v.position[2] = -positions[pos_index * 3 + 1];

        // normals
        if (i * 3 + 2 >= normals.size()) {
            std::cerr << "Normal index " << i << " out of bounds" << std::endl;
            v.normal[0] = v.normal[1] = v.normal[2] = 0.0f;
        }
        else {
            v.normal[0] = normals[i * 3 + 0];
            v.normal[1] = normals[i * 3 + 2];
            v.normal[2] = -normals[i * 3 + 1];
        }

        // UV 
        if (hasUVs && i * 2 + 1 < uvs.size()) {
            v.texcoord[0] = uvs[i * 2 + 0];
            v.texcoord[1] = uvs[i * 2 + 1];
        }
        else {
            v.texcoord[0] = 0.0f;
            v.texcoord[1] = 0.0f;
        }

        vertices.push_back(v);
    }

    return vertices;
}

void ReadBlenderData(const py::dict& mesh_data, const std::string& exportDir, const std::string& filepath, py::list textures, bool useDraco, int dracoLevel, bool useJpg, int jpgLevel, bool zip)
{
    PROFILE_FUNCTION();
    GLTFExporter exporter;
    exporter.SetExportDirectory(exportDir);
    exporter.SetUseJpg(useJpg, jpgLevel);


    // Process mesh data: 
    
    auto vertices = mesh_data["vertices"].cast<py::array_t<float>>();
    auto normals = mesh_data["normals"].cast<py::array_t<float>>();
    auto indices = mesh_data["indices"].cast<py::array_t<uint32_t>>();

    // Handle optional UVs
    py::array_t<float> uvs;
    if (!mesh_data["uvs"].is_none()) {
        uvs = mesh_data["uvs"].cast<py::array_t<float>>();
    }

    std::vector<float> vecVertices = NumpyArrayToVector(vertices);
    std::vector<float> vecNormals = NumpyArrayToVector(normals);
    std::vector<uint32_t> vecIndices = NumpyArrayToVector(indices);
    std::vector<float> vecUvs = NumpyArrayToVector(uvs);
    std::vector <Vertex> vertexData = StoreInVertex(vecVertices, vecNormals, vecUvs, vecIndices);
    
    
    // Process all the textures
    for (size_t i = 0; i < textures.size(); i++) 
    {
        // Cast it to a dict so we can lookup the type
        py::dict tex = textures[i].cast<py::dict>();
        std::string type = tex["type"].cast<std::string>();

        TextureData texData;
        texData.type = type;
        texData.name = tex["name"].cast<std::string>();

        if (type == "file") {
            texData.filepath = tex["path"].cast<std::string>();
        }
        else if (type == "packed") 
        {
            py::array_t<uint8_t> pixel_data = tex["data"].cast<py::array_t<uint8_t>>();
            auto buf = pixel_data.request();

            texData.data = std::vector<uint8_t>(
                static_cast<uint8_t*>(buf.ptr),
                static_cast<uint8_t*>(buf.ptr) + buf.size
            );
            texData.width = tex["width"].cast<int>();
            texData.height = tex["height"].cast<int>();
            texData.channels = tex["channels"].cast<int>();
        }

        exporter.PushTextures(texData);
    }



    auto materials = mesh_data["materials"].cast<std::vector<py::dict>>();
    auto name = mesh_data["name"].cast<std::string>();

    std::vector<uint32_t> newIndices;
    newIndices.reserve(vertexData.size());
    for (size_t i = 0; i < vertexData.size(); i++) {
        newIndices.push_back(static_cast<uint32_t>(i));
    }


    // Create material with optional texture
    Material mat;
    mat.name = "TestMaterial";
    mat.metallicFactor = 0.0f;
    mat.roughnessFactor = 0.8f;

    mat.baseColorTexture = 0;  // AddMaterial will call AddTexture internally
    mat.normalTexture = 1;
    mat.metallicRoughnessTexture = 2;
    int materialIndex = exporter.AddMaterial(mat);

    Mesh mesh;
    mesh.name = name;
    mesh.vertices = vertexData;
    mesh.indices = newIndices;
    mesh.materialIndex = materialIndex;
    mesh.useDracoCompression = useDraco;
    mesh.dracoCompressionLevel = dracoLevel;

    int meshIndex = exporter.AddMesh(mesh);

    Node node;
    node.name = name;
    node.meshIndex = meshIndex;
    exporter.AddNode(node);

    // Export to file
    std::cout << "Attempting to export to file..." << std::endl;

    bool success = exporter.ExportToFile(filepath, false);  // Use the provided filepath

    if (success) {
        std::cout << "GLTF exported successfully to: " << filepath << std::endl;
    }
    else {
        std::cout << "Failed to export GLTF to: " << filepath << std::endl;
    }


    if (zip)
    {
        std::vector<std::string> texturePaths;
        for (size_t i = 0; i < textures.size(); i++) {
            std::string ext = useJpg ? ".jpg" : ".png";
            std::string texFilePath = exportDir + std::string("\\") + std::to_string(i) + ext;
            texturePaths.push_back(texFilePath);
        }
        std::string zipPath = filepath;
        size_t lastDot = zipPath.find_last_of('.');
        if (lastDot != std::string::npos) {
            zipPath = zipPath.substr(0, lastDot) + ".zip";
        }
        else {
            zipPath += ".zip";
        }

        if (exporter.CompressToZip(filepath, zipPath, texturePaths))
        {
            // Remove remaining files
            std::remove(filepath.c_str());
            for (const auto& texPath : texturePaths) {
                std::remove(texPath.c_str());
            }
        }
    }


}
