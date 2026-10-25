#include "distantbake.hpp"

#include <components/debug/debuglog.hpp>
#include <components/misc/constants.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/sceneutil/material.hpp>
#include <components/sceneutil/texturetype.hpp>
#include <components/sceneutil/util.hpp>
#include <components/vfs/manager.hpp>
#include <components/vfs/pathutil.hpp>

#include <osg/AlphaFunc>
#include <osg/Billboard>
#include <osg/FrontFace>
#include <osg/GLExtensions>
#include <osg/Geometry>
#include <osg/LOD>
#include <osg/MatrixTransform>
#include <osg/NodeVisitor>
#include <osg/PositionAttitudeTransform>
#include <osg/Sequence>
#include <osg/Switch>
#include <osg/TexMat>
#include <osg/Texture2D>
#include <osg/TriangleIndexFunctor>

#include <algorithm>
#include <bit>
#include <cstring>
#include <istream>

#ifndef GL_TEXTURE_2D_ARRAY_EXT
#define GL_TEXTURE_2D_ARRAY_EXT 0x8C1A
#endif

namespace MWRender
{
    namespace
    {
        constexpr std::uint32_t sFormat = 0x83F3; // GL_COMPRESSED_RGBA_S3TC_DXT5_EXT
        // GL 4.5 guarantees this many layers per array (a set with more is split)
        constexpr std::size_t sMaxLayers = 2048;

        std::size_t levelBytes(int size)
        {
            const std::size_t blocks = static_cast<std::size_t>((size + 3) / 4);
            return blocks * blocks * 16;
        }

        // Uploads an array's layers, every level at once (already compressed and in level order).
        class Upload : public osg::Texture2DArray::SubloadCallback
        {
        public:
            Upload(std::vector<std::vector<std::uint8_t>> levels, int size, int layers)
                : mLevels(std::move(levels))
                , mSize(size)
                , mLayers(layers)
            {
            }

            void load(const osg::Texture2DArray& texture, osg::State& state) const override
            {
                osg::GLExtensions* extensions = state.get<osg::GLExtensions>();
                if (extensions == nullptr || !extensions->isCompressedTexImage3DSupported())
                {
                    Log(Debug::Error) << "Distant texture set: no glCompressedTexImage3D";
                    return;
                }
                for (std::size_t level = 0; level < mLevels.size(); ++level)
                {
                    const int size = std::max(1, mSize >> level);
                    extensions->glCompressedTexImage3D(GL_TEXTURE_2D_ARRAY_EXT, static_cast<GLint>(level), sFormat,
                        size, size, mLayers, 0, static_cast<GLsizei>(mLevels[level].size()), mLevels[level].data());
                }
                texture.setNumMipmapLevels(static_cast<unsigned int>(mLevels.size()));
            }

            void subload(const osg::Texture2DArray& /*texture*/, osg::State& /*state*/) const override {}

        private:
            std::vector<std::vector<std::uint8_t>> mLevels;
            int mSize;
            int mLayers;
        };

        template <class T>
        bool get(std::istream& stream, T& value)
        {
            static_assert(std::endian::native == std::endian::little);
            stream.read(reinterpret_cast<char*>(&value), sizeof(value));
            return static_cast<bool>(stream);
        }

        // One shape of a model, in the model's space, ready to be appended to a chunk's merged shapes.
        struct Part
        {
            std::size_t mArray;
            int mKind;
            std::vector<osg::Vec3f> mPositions;
            std::vector<osg::Vec3f> mNormals;
            std::vector<std::uint32_t> mColors;
            std::vector<osg::Vec2f> mTexCoords;
            float mLayer;
            std::vector<std::uint32_t> mIndices;
        };

        struct ModelBake
        {
            bool mUsable = false;
            std::vector<Part> mParts;
        };

        struct TriangleCollector
        {
            std::vector<std::uint32_t>* mIndices = nullptr;
            void operator()(unsigned int a, unsigned int b, unsigned int c)
            {
                if (a == b || b == c || a == c)
                    return;
                mIndices->push_back(a);
                mIndices->push_back(b);
                mIndices->push_back(c);
            }
        };

        std::uint32_t packColor(const osg::Vec4f& c)
        {
            const auto byte = [](float v) {
                return static_cast<std::uint32_t>(std::clamp(static_cast<int>(v * 255.f + 0.5f), 0, 255));
            };
            return byte(c.r()) | (byte(c.g()) << 8) | (byte(c.b()) << 16) | (byte(c.a()) << 24);
        }

        // Reads a model as the set can draw it, or finds why it can't (then it is drawn as before).
        class Analyser : public osg::NodeVisitor
        {
        public:
            explicit Analyser(const DistantTextureSet& set)
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
                , mSet(set)
            {
                mMatrices.emplace_back();
                mStateSets.emplace_back(new osg::StateSet);
            }

            bool mUsable = true;
            std::vector<Part> mParts;

            void apply(osg::Node& node) override
            {
                if (!enter(node))
                    return;
                traverse(node);
                leave();
            }

            void apply(osg::Switch& node) override
            {
                if (!enter(node))
                    return;
                for (unsigned int i = 0; i < node.getNumChildren(); ++i)
                    if (node.getValue(i))
                        node.getChild(i)->accept(*this);
                leave();
            }

            void apply(osg::LOD& /*node*/) override
            {
                // which child shows depends on the distance: drawn as before
                mUsable = false;
            }

            void apply(osg::Sequence& /*node*/) override { mUsable = false; }

            void apply(osg::Billboard& /*node*/) override { mUsable = false; }

            void apply(osg::Transform& node) override
            {
                if (dynamic_cast<osg::MatrixTransform*>(&node) == nullptr
                    && dynamic_cast<osg::PositionAttitudeTransform*>(&node) == nullptr)
                {
                    mUsable = false;
                    return;
                }
                if (!enter(node))
                    return;
                osg::Matrix matrix = mMatrices.back();
                node.computeLocalToWorldMatrix(matrix, this);
                mMatrices.push_back(matrix);
                traverse(node);
                mMatrices.pop_back();
                leave();
            }

            void apply(osg::Drawable& drawable) override
            {
                if (!enter(drawable))
                    return;
                if (osg::Geometry* geometry = drawable.asGeometry())
                    addGeometry(*geometry);
                else
                    mUsable = false;
                leave();
            }

        private:
            const DistantTextureSet& mSet;
            std::vector<osg::Matrix> mMatrices;
            std::vector<osg::ref_ptr<osg::StateSet>> mStateSets;
            std::vector<bool> mPushed;

            bool enter(osg::Node& node)
            {
                if (node.getNodeMask() == 0)
                    return false;
                if (node.getUpdateCallback() != nullptr || node.getCullCallback() != nullptr
                    || SceneUtil::hasUserDescription(&node, Constants::NightDayLabel)
                    || SceneUtil::hasUserDescription(&node, Constants::HerbalismLabel))
                    mUsable = false;
                const osg::StateSet* stateset = node.getStateSet();
                if (stateset != nullptr && stateset->getUpdateCallback() != nullptr)
                    mUsable = false;
                if (stateset != nullptr)
                {
                    osg::ref_ptr<osg::StateSet> combined
                        = new osg::StateSet(*mStateSets.back(), osg::CopyOp::SHALLOW_COPY);
                    combined->merge(*stateset);
                    mStateSets.push_back(combined);
                }
                mPushed.push_back(stateset != nullptr);
                return true;
            }

            void leave()
            {
                if (mPushed.back())
                    mStateSets.pop_back();
                mPushed.pop_back();
            }

            void addGeometry(osg::Geometry& geometry)
            {
                if (!mUsable)
                    return;
                const osg::StateSet& stateset = *mStateSets.back();

                // only the base texture, and it in the set
                const osg::Texture2D* texture = dynamic_cast<const osg::Texture2D*>(
                    stateset.getTextureAttribute(0, osg::StateAttribute::TEXTURE));
                if (texture == nullptr || texture->getImage() == nullptr)
                {
                    mUsable = false;
                    return;
                }
                const std::string& type = SceneUtil::getTextureType(stateset, *texture, 0);
                if (!type.empty() && type != "diffuseMap")
                {
                    mUsable = false;
                    return;
                }
                const DistantTextureSet::Entry* entry = mSet.find(texture->getImage()->getFileName());
                if (entry == nullptr)
                {
                    mUsable = false;
                    return;
                }
                if (stateset.getTextureAttribute(0, osg::StateAttribute::TEXMAT) != nullptr)
                {
                    mUsable = false;
                    return;
                }
                // the other textures: those that only shade the lighting are left out from afar; those that change
                // the colour (glow maps of lit windows, dark, detail and decal maps) need the shape as it is
                for (unsigned int unit = 1; unit < stateset.getTextureAttributeList().size(); ++unit)
                    if (const osg::Texture* other = dynamic_cast<const osg::Texture*>(
                            stateset.getTextureAttribute(unit, osg::StateAttribute::TEXTURE)))
                    {
                        const std::string& otherType = SceneUtil::getTextureType(stateset, *other, unit);
                        if (otherType != "normalMap" && otherType != "normalHeightMap" && otherType != "specularMap"
                            && otherType != "bumpMap" && otherType != "envMap" && otherType != "glossMap")
                        {
                            mUsable = false;
                            return;
                        }
                    }
                // blended shapes are sorted and drawn apart
                if ((stateset.getMode(GL_BLEND) & osg::StateAttribute::ON) != 0)
                {
                    mUsable = false;
                    return;
                }
                const SceneUtil::Material* material
                    = dynamic_cast<const SceneUtil::Material*>(stateset.getAttribute(osg::StateAttribute::MATERIAL));
                osg::Vec4f diffuse(1, 1, 1, 1);
                SceneUtil::VertexColorModes colorMode = SceneUtil::VertexColorModes::AmbientAndDiffuse;
                if (material != nullptr)
                {
                    const osg::Vec4f emission = material->getEmission();
                    if (emission.r() + emission.g() + emission.b() > 0.03f
                        || material->getVertexColorMode() == SceneUtil::VertexColorModes::Emission)
                    {
                        mUsable = false;
                        return;
                    }
                    diffuse = material->getDiffuse();
                    colorMode = material->getVertexColorMode();
                }
                int kind = DistantTextureSet::Solid;
                if (const osg::AlphaFunc* alpha
                    = dynamic_cast<const osg::AlphaFunc*>(stateset.getAttribute(osg::StateAttribute::ALPHAFUNC)))
                    if (alpha->getFunction() != osg::AlphaFunc::ALWAYS)
                        kind |= DistantTextureSet::AlphaTested;
                const osg::StateAttribute::GLModeValue cull = stateset.getMode(GL_CULL_FACE);
                if (cull != osg::StateAttribute::INHERIT && (cull & osg::StateAttribute::ON) == 0)
                    kind |= DistantTextureSet::TwoSided;
                bool clockwise = false;
                if (const osg::FrontFace* frontFace
                    = dynamic_cast<const osg::FrontFace*>(stateset.getAttribute(osg::StateAttribute::FRONTFACE)))
                    clockwise = frontFace->getMode() == osg::FrontFace::CLOCKWISE;

                const osg::Vec3Array* positions = dynamic_cast<const osg::Vec3Array*>(geometry.getVertexArray());
                if (positions == nullptr || positions->empty())
                    return;
                const std::size_t count = positions->size();
                const osg::Vec3Array* normals = dynamic_cast<const osg::Vec3Array*>(geometry.getNormalArray());
                if (normals != nullptr
                    && (normals->getBinding() != osg::Array::BIND_PER_VERTEX || normals->size() < count))
                    normals = nullptr;
                const osg::Array* colors = geometry.getColorArray();
                if (colors != nullptr
                    && (colors->getBinding() != osg::Array::BIND_PER_VERTEX || colors->getNumElements() < count))
                    colors = nullptr;
                const osg::Vec4Array* colorsF = dynamic_cast<const osg::Vec4Array*>(colors);
                const osg::Vec4ubArray* colorsUB = dynamic_cast<const osg::Vec4ubArray*>(colors);
                const osg::Vec2Array* texCoords = dynamic_cast<const osg::Vec2Array*>(geometry.getTexCoordArray(0));
                if (texCoords == nullptr || texCoords->size() < count)
                {
                    mUsable = false;
                    return;
                }

                Part part;
                part.mArray = entry->mArray;
                part.mKind = kind;
                part.mLayer = static_cast<float>(entry->mLayer);
                osg::TriangleIndexFunctor<TriangleCollector> functor;
                functor.mIndices = &part.mIndices;
                geometry.accept(functor);
                for (std::uint32_t index : part.mIndices)
                    if (index >= count)
                    {
                        mUsable = false;
                        return;
                    }
                if (part.mIndices.empty())
                    return;

                const osg::Matrix& matrix = mMatrices.back();
                const osg::Matrix normalMatrix = osg::Matrix::inverse(matrix);
                const bool mirrored = (osg::Vec3d(matrix(0, 0), matrix(0, 1), matrix(0, 2))
                                          ^ osg::Vec3d(matrix(1, 0), matrix(1, 1), matrix(1, 2)))
                        * osg::Vec3d(matrix(2, 0), matrix(2, 1), matrix(2, 2))
                    < 0;
                if (mirrored != clockwise)
                    for (std::size_t i = 0; i + 2 < part.mIndices.size(); i += 3)
                        std::swap(part.mIndices[i + 1], part.mIndices[i + 2]);

                const bool vertexColors = colors != nullptr && colorMode != SceneUtil::VertexColorModes::None;
                part.mPositions.reserve(count);
                part.mNormals.reserve(count);
                part.mColors.reserve(count);
                part.mTexCoords.reserve(count);
                for (std::size_t i = 0; i < count; ++i)
                {
                    part.mPositions.push_back((*positions)[i] * matrix);
                    osg::Vec3f normal(0, 0, 1);
                    if (normals != nullptr)
                    {
                        normal = osg::Matrix::transform3x3(normalMatrix, (*normals)[i]);
                        normal.normalize();
                    }
                    part.mNormals.push_back(normal);
                    osg::Vec4f color(1, 1, 1, 1);
                    if (vertexColors && colorsF != nullptr)
                        color = (*colorsF)[i];
                    else if (vertexColors && colorsUB != nullptr)
                        color = osg::Vec4f((*colorsUB)[i].r() / 255.f, (*colorsUB)[i].g() / 255.f,
                            (*colorsUB)[i].b() / 255.f, (*colorsUB)[i].a() / 255.f);
                    // the material's colour, which the merged shapes share as white, in the vertex colour
                    color = osg::Vec4f(color.r() * diffuse.r(), color.g() * diffuse.g(), color.b() * diffuse.b(),
                        color.a() * diffuse.a());
                    part.mColors.push_back(packColor(color));
                    part.mTexCoords.push_back((*texCoords)[i]);
                }
                mParts.push_back(std::move(part));
            }
        };

        std::mutex sCacheMutex;
        std::unordered_map<const osg::Node*, std::pair<osg::ref_ptr<const osg::Node>, std::shared_ptr<const ModelBake>>>
            sCache;

        std::shared_ptr<const ModelBake> analyse(const DistantTextureSet& set, const osg::Node& model)
        {
            {
                const std::lock_guard lock(sCacheMutex);
                const auto it = sCache.find(&model);
                if (it != sCache.end())
                    return it->second.second;
            }
            Analyser analyser(set);
            const_cast<osg::Node&>(model).accept(analyser);
            auto bake = std::make_shared<ModelBake>();
            bake->mUsable = analyser.mUsable && !analyser.mParts.empty();
            if (bake->mUsable)
                bake->mParts = std::move(analyser.mParts);
            const std::lock_guard lock(sCacheMutex);
            // the models of a few cells' worth of distant chunks; forgotten all at once when there are many
            if (sCache.size() > 8192)
                sCache.clear();
            sCache.emplace(&model, std::make_pair(osg::ref_ptr<const osg::Node>(&model), bake));
            return bake;
        }
    }

    std::unique_ptr<DistantTextureSet> DistantTextureSet::load(const VFS::Manager& vfs)
    {
        constexpr VFS::Path::NormalizedView path("distantland/textures.bin");
        if (!vfs.exists(path))
            return nullptr;
        try
        {
            Files::IStreamPtr stream = vfs.get(path);
            char magic[8];
            stream->read(magic, sizeof(magic));
            std::uint32_t version = 0, size = 0, levels = 0, count = 0, format = 0;
            if (!*stream || std::memcmp(magic, "OMWDLTX1", 8) != 0 || !get(*stream, version) || version != 1
                || !get(*stream, size) || !get(*stream, levels) || !get(*stream, count) || !get(*stream, format))
                throw std::runtime_error("not a distant texture set");
            if (format != sFormat || size < 4 || size > 4096 || !std::has_single_bit(size)
                || levels != static_cast<std::uint32_t>(std::bit_width(size)) || count == 0 || count > 65535)
                throw std::runtime_error("unsupported distant texture set");

            auto set = std::make_unique<DistantTextureSet>();
            std::vector<std::string> paths(count);
            for (std::uint32_t i = 0; i < count; ++i)
            {
                std::uint16_t length = 0;
                std::uint8_t flags = 0;
                if (!get(*stream, length))
                    throw std::runtime_error("truncated");
                paths[i].resize(length);
                stream->read(paths[i].data(), length);
                if (!get(*stream, flags))
                    throw std::runtime_error("truncated");
            }
            std::size_t perLayer = 0;
            for (std::uint32_t level = 0; level < levels; ++level)
                perLayer += levelBytes(std::max(1, static_cast<int>(size >> level)));

            const std::size_t arrays = (count + sMaxLayers - 1) / sMaxLayers;
            std::vector<std::uint8_t> layerData(perLayer);
            for (std::size_t a = 0; a < arrays; ++a)
            {
                const std::size_t first = a * sMaxLayers;
                const std::size_t layers = std::min<std::size_t>(sMaxLayers, count - first);
                std::vector<std::vector<std::uint8_t>> levelData(levels);
                for (std::uint32_t level = 0; level < levels; ++level)
                    levelData[level].resize(layers * levelBytes(std::max(1, static_cast<int>(size >> level))));
                for (std::size_t layer = 0; layer < layers; ++layer)
                {
                    stream->read(reinterpret_cast<char*>(layerData.data()), static_cast<std::streamsize>(perLayer));
                    if (!*stream)
                        throw std::runtime_error("truncated");
                    std::size_t offset = 0;
                    for (std::uint32_t level = 0; level < levels; ++level)
                    {
                        const std::size_t bytes = levelBytes(std::max(1, static_cast<int>(size >> level)));
                        std::memcpy(levelData[level].data() + layer * bytes, layerData.data() + offset, bytes);
                        offset += bytes;
                    }
                    set->mEntries[paths[first + layer]]
                        = Entry{ static_cast<std::uint16_t>(a), static_cast<std::uint16_t>(layer) };
                }
                osg::ref_ptr<osg::Texture2DArray> texture = new osg::Texture2DArray;
                texture->setTextureSize(static_cast<int>(size), static_cast<int>(size), static_cast<int>(layers));
                texture->setInternalFormat(static_cast<GLint>(sFormat));
                texture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR_MIPMAP_LINEAR);
                texture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
                texture->setWrap(osg::Texture::WRAP_S, osg::Texture::REPEAT);
                texture->setWrap(osg::Texture::WRAP_T, osg::Texture::REPEAT);
                texture->setMaxAnisotropy(4.f);
                texture->setUseHardwareMipMapGeneration(false);
                texture->setResizeNonPowerOfTwoHint(false);
                texture->setSubloadCallback(
                    new Upload(std::move(levelData), static_cast<int>(size), static_cast<int>(layers)));
                set->mArrays.push_back(texture);
            }
            Log(Debug::Info) << "Distant texture set: " << count << " textures at " << size << "x" << size << " in "
                             << arrays << " array(s)";
            return set;
        }
        catch (const std::exception& e)
        {
            Log(Debug::Error) << "Distant texture set " << path << " is not used: " << e.what();
            return nullptr;
        }
    }

    const DistantTextureSet::Entry* DistantTextureSet::find(const std::string& path) const
    {
        const auto it = mEntries.find(path);
        return it == mEntries.end() ? nullptr : &it->second;
    }

    osg::StateSet* DistantTextureSet::getStateSet(Resource::SceneManager& sceneManager, std::size_t array, int kind)
    {
        const std::lock_guard lock(mStateSetsMutex);
        osg::ref_ptr<osg::StateSet>& result = mStateSets[{ array, kind }];
        if (result != nullptr)
            return result.get();

        osg::ref_ptr<osg::StateSet> stateset = new osg::StateSet;
        stateset->setTextureAttributeAndModes(0, mArrays[array], osg::StateAttribute::ON);
        stateset->setTextureAttribute(0, new SceneUtil::TextureType("diffuseMapArray"), osg::StateAttribute::ON);
        stateset->setAttribute(
            new SceneUtil::Material({ .mVertexColorMode = SceneUtil::VertexColorModes::AmbientAndDiffuse }),
            osg::StateAttribute::ON);
        if (kind & AlphaTested)
            stateset->setAttributeAndModes(new osg::AlphaFunc(osg::AlphaFunc::GEQUAL, 0.5f), osg::StateAttribute::ON);
        if (kind & TwoSided)
            stateset->setMode(GL_CULL_FACE, osg::StateAttribute::OFF);

        // the shaders: made for a stand-in with the merged shapes' arrays
        osg::ref_ptr<osg::Geometry> standIn = new osg::Geometry;
        standIn->setVertexArray(new osg::Vec3Array(1));
        standIn->setNormalArray(new osg::Vec3Array(1), osg::Array::BIND_PER_VERTEX);
        standIn->setColorArray(new osg::Vec4ubArray(1), osg::Array::BIND_PER_VERTEX);
        standIn->setTexCoordArray(0, new osg::Vec2Array(1), osg::Array::BIND_PER_VERTEX);
        standIn->setTexCoordArray(1, new osg::FloatArray(1), osg::Array::BIND_PER_VERTEX);
        standIn->setStateSet(stateset);
        osg::ref_ptr<osg::Group> holder = new osg::Group;
        holder->addChild(standIn);
        sceneManager.recreateShaders(holder);
        result = standIn->getStateSet();
        return result.get();
    }

    DistantChunkBaker::DistantChunkBaker(DistantTextureSet& set, Resource::SceneManager& sceneManager)
        : mSet(set)
        , mSceneManager(sceneManager)
    {
    }

    bool DistantChunkBaker::add(const osg::Node& model, const osg::Matrixf& matrix)
    {
        const std::shared_ptr<const ModelBake> bake = analyse(mSet, model);
        if (!bake->mUsable)
            return false;
        const osg::Matrixf normalMatrix = osg::Matrixf::inverse(matrix);
        for (const Part& part : bake->mParts)
        {
            Buffer& buffer = mBuffers[{ part.mArray, part.mKind }];
            const std::uint32_t base = static_cast<std::uint32_t>(buffer.mPositions.size());
            for (const osg::Vec3f& position : part.mPositions)
                buffer.mPositions.push_back(position * matrix);
            for (const osg::Vec3f& normal : part.mNormals)
            {
                osg::Vec3f n = osg::Matrixf::transform3x3(normalMatrix, normal);
                n.normalize();
                buffer.mNormals.push_back(n);
            }
            buffer.mColors.insert(buffer.mColors.end(), part.mColors.begin(), part.mColors.end());
            buffer.mTexCoords.insert(buffer.mTexCoords.end(), part.mTexCoords.begin(), part.mTexCoords.end());
            buffer.mLayers.insert(buffer.mLayers.end(), part.mPositions.size(), part.mLayer);
            for (std::uint32_t index : part.mIndices)
                buffer.mIndices.push_back(base + index);
        }
        ++mInstances;
        return true;
    }

    void DistantChunkBaker::build(osg::Group& parent)
    {
        for (auto& [key, buffer] : mBuffers)
        {
            if (buffer.mIndices.empty())
                continue;
            osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry;
            geometry->setVertexArray(new osg::Vec3Array(buffer.mPositions.begin(), buffer.mPositions.end()));
            geometry->setNormalArray(
                new osg::Vec3Array(buffer.mNormals.begin(), buffer.mNormals.end()), osg::Array::BIND_PER_VERTEX);
            osg::ref_ptr<osg::Vec4ubArray> colors = new osg::Vec4ubArray(buffer.mColors.size());
            if (!buffer.mColors.empty())
                std::memcpy(static_cast<void*>(&colors->front()), buffer.mColors.data(), buffer.mColors.size() * 4);
            colors->setNormalize(true);
            geometry->setColorArray(colors, osg::Array::BIND_PER_VERTEX);
            geometry->setTexCoordArray(
                0, new osg::Vec2Array(buffer.mTexCoords.begin(), buffer.mTexCoords.end()), osg::Array::BIND_PER_VERTEX);
            geometry->setTexCoordArray(
                1, new osg::FloatArray(buffer.mLayers.begin(), buffer.mLayers.end()), osg::Array::BIND_PER_VERTEX);
            geometry->addPrimitiveSet(
                new osg::DrawElementsUInt(GL_TRIANGLES, buffer.mIndices.begin(), buffer.mIndices.end()));
            geometry->setUseDisplayList(false);
            geometry->setUseVertexBufferObjects(true);
            geometry->setStateSet(mSet.getStateSet(mSceneManager, key.first, key.second));
            geometry->setDataVariance(osg::Object::STATIC);
            parent.addChild(geometry);
        }
        mBuffers.clear();
    }

    void DistantChunkBaker::clearCache()
    {
        const std::lock_guard lock(sCacheMutex);
        sCache.clear();
    }
}
