#include "imagemanager.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <osgDB/Registry>
#include <string_view>

#include <components/debug/debuglog.hpp>
#include <components/misc/pathhelpers.hpp>
#include <components/sceneutil/glextensions.hpp>
#include <components/vfs/manager.hpp>
#include <components/vfs/pathutil.hpp>

#include "objectcache.hpp"

#ifdef OSG_LIBRARY_STATIC
// This list of plugins should match with the list in the top-level CMakelists.txt.
USE_OSGPLUGIN(png)
USE_OSGPLUGIN(tga)
USE_OSGPLUGIN(dds)
USE_OSGPLUGIN(jpeg)
USE_OSGPLUGIN(bmp)
USE_OSGPLUGIN(osg)
USE_SERIALIZER_WRAPPER_LIBRARY(osg)
#endif

namespace
{

    osg::ref_ptr<osg::Image> createWarningImage()
    {
        osg::ref_ptr<osg::Image> warningImage = new osg::Image;

        int width = 8, height = 8;
        warningImage->allocateImage(width, height, 1, GL_RGB, GL_UNSIGNED_BYTE);
        assert(warningImage->isDataContiguous());
        unsigned char* data = warningImage->data();
        for (int i = 0; i < width * height; ++i)
        {
            data[3 * i] = (255);
            data[3 * i + 1] = (0);
            data[3 * i + 2] = (255);
        }
        return warningImage;
    }

    bool isS3TC(osg::Image* image)
    {
        switch (image->getPixelFormat())
        {
            case GL_COMPRESSED_RGB_S3TC_DXT1_EXT:
            case GL_COMPRESSED_RGBA_S3TC_DXT1_EXT:
            case GL_COMPRESSED_RGBA_S3TC_DXT3_EXT:
            case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT:
                return true;
        }
        return false;
    }

    // Interface textures stay as they are: shrinking them would blur menus and text.
    bool mayShrink(std::string_view path)
    {
        if (!path.starts_with("textures/"))
            return false;
        const std::string_view name = path.substr(9);
        for (std::string_view prefix : { "menu", "tx_menubook", "scroll", "compass", "target", "cursor", "book",
                 "mygui", "splash", "levelup", "birthsigns", "bookart", "icons", "fonts", "ui/", "interface" })
            if (name.starts_with(prefix))
                return false;
        return true;
    }

    // An image no larger than maxSize on either side, made by leaving out its largest mipmaps (or by scaling it when it
    // has none and isn't compressed). Returns nullptr if it can't or needn't be made smaller.
    osg::ref_ptr<osg::Image> shrinkImage(const osg::Image& image, int maxSize)
    {
        if (image.s() <= maxSize && image.t() <= maxSize)
            return nullptr;
        if (image.r() != 1)
            return nullptr;

        const unsigned int levels = image.getNumMipmapLevels();
        if (levels > 1)
        {
            unsigned int skip = 0;
            while (skip + 1 < levels && ((image.s() >> skip) > maxSize || (image.t() >> skip) > maxSize))
                ++skip;
            if (skip == 0)
                return nullptr;

            const unsigned int offset = image.getMipmapOffset(skip);
            const unsigned int total = image.getTotalSizeInBytesIncludingMipmaps();
            if (offset == 0 || offset >= total || !image.isDataContiguous())
                return nullptr;
            unsigned char* data = new unsigned char[total - offset];
            std::memcpy(data, image.data() + offset, total - offset);

            osg::ref_ptr<osg::Image> result = new osg::Image;
            result->setFileName(image.getFileName());
            result->setImage(std::max(1, image.s() >> skip), std::max(1, image.t() >> skip), 1,
                image.getInternalTextureFormat(), image.getPixelFormat(), image.getDataType(), data,
                osg::Image::USE_NEW_DELETE, image.getPacking());
            osg::Image::MipmapDataType mipmaps;
            for (unsigned int level = skip + 1; level < levels; ++level)
                mipmaps.push_back(image.getMipmapOffset(level) - offset);
            result->setMipmapLevels(mipmaps);
            result->setOrigin(image.getOrigin());
            return result;
        }

        if (image.isCompressed())
            return nullptr;
        const float scale = static_cast<float>(maxSize) / static_cast<float>(std::max(image.s(), image.t()));
        osg::ref_ptr<osg::Image> result = new osg::Image(image, osg::CopyOp::DEEP_COPY_ALL);
        result->scaleImage(
            std::max(1, static_cast<int>(image.s() * scale)), std::max(1, static_cast<int>(image.t() * scale)), 1);
        return result;
    }

    bool checkSupported(osg::Image* image)
    {
        // not bothering with checks for other compression formats right now
        if (!isS3TC(image))
            return true;

        // hashtag yolo (CS might not have context when loading assets)
        if (!SceneUtil::glExtensionsReady())
            return true;

        return SceneUtil::getGLExtensions().isTextureCompressionS3TCSupported;
    }

}

namespace Resource
{

    ImageManager::ImageManager(const VFS::Manager* vfs, double expiryDelay)
        : ResourceManager(vfs, expiryDelay)
        , mWarningImage(createWarningImage())
        , mOptions(new osgDB::Options("dds_dxt1_detect_rgba ignoreTga2Fields"))
    {
    }

    ImageManager::~ImageManager() {}

    osg::ref_ptr<osg::Image> ImageManager::getImage(VFS::Path::NormalizedView path, bool disableFlip)
    {
        osg::ref_ptr<osg::Object> obj = mCache->getRefFromObjectCache(path);
        if (obj)
            return osg::ref_ptr<osg::Image>(static_cast<osg::Image*>(obj.get()));
        else
        {
            Files::IStreamPtr stream;
            try
            {
                stream = mVFS->get(path);
            }
            catch (std::exception& e)
            {
                Log(Debug::Error) << "Failed to open image: " << e.what();
                mCache->addEntryToObjectCache(path.value(), mWarningImage);
                return mWarningImage;
            }

            std::string ext(Misc::getFileExtension(path.value()));

            // Non-standard, but Morrowind supports this
            if (ext == "targa")
                ext = "tga";

            osgDB::ReaderWriter* reader = osgDB::Registry::instance()->getReaderWriterForExtension(ext);
            if (!reader)
            {
                Log(Debug::Error) << "Error loading " << path << ": no readerwriter for '" << ext << "' found";
                mCache->addEntryToObjectCache(path.value(), mWarningImage);
                return mWarningImage;
            }

            bool killAlpha = false;
            if (reader->supportedExtensions().count("tga"))
            {
                // Morrowind ignores the alpha channel of 16bpp TGA files even when the header says not to
                unsigned char header[18];
                stream->read((char*)header, 18);
                if (stream->gcount() != 18)
                {
                    Log(Debug::Error) << "Error loading " << path << ": couldn't read TGA header";
                    mCache->addEntryToObjectCache(path.value(), mWarningImage);
                    return mWarningImage;
                }
                int type = header[2];
                int depth;
                if (type == 1 || type == 9)
                    depth = header[7];
                else
                    depth = header[16];
                int alphaBPP = header[17] & 0x0F;
                killAlpha = depth == 16 && alphaBPP == 1;
                stream->seekg(0);
            }

            osgDB::ReaderWriter::ReadResult result = reader->readImage(*stream, mOptions);
            if (!result.success())
            {
                Log(Debug::Error) << "Error loading " << path << ": " << result.message() << " code "
                                  << result.status();
                mCache->addEntryToObjectCache(path.value(), mWarningImage);
                return mWarningImage;
            }

            osg::ref_ptr<osg::Image> image = result.getImage();

            image->setFileName(std::string(path.value()));
            if (!checkSupported(image))
            {
                static bool uncompress = (getenv("OPENMW_DECOMPRESS_TEXTURES") != nullptr);
                if (!uncompress)
                {
                    Log(Debug::Error) << "Error loading " << path << ": no S3TC texture compression support installed";
                    mCache->addEntryToObjectCache(path.value(), mWarningImage);
                    return mWarningImage;
                }
                else
                {
                    // decompress texture in software if not supported by GPU
                    // requires update to getColor() to be released with OSG 3.6
                    osg::ref_ptr<osg::Image> newImage = new osg::Image;
                    newImage->setFileName(image->getFileName());
                    newImage->setOrigin(image->getOrigin());
                    newImage->allocateImage(image->s(), image->t(), image->r(),
                        image->isImageTranslucent() ? GL_RGBA : GL_RGB, GL_UNSIGNED_BYTE);
                    for (int s = 0; s < image->s(); ++s)
                        for (int t = 0; t < image->t(); ++t)
                            for (int r = 0; r < image->r(); ++r)
                                newImage->setColor(image->getColor(s, t, r), s, t, r);
                    image = newImage;
                }
            }
            else if (killAlpha)
            {
                osg::ref_ptr<osg::Image> newImage = new osg::Image;
                newImage->setFileName(image->getFileName());
                newImage->setOrigin(image->getOrigin());
                newImage->allocateImage(image->s(), image->t(), image->r(), GL_RGB, GL_UNSIGNED_BYTE);
                // OSG just won't write the alpha as there's nowhere to put it.
                for (int s = 0; s < image->s(); ++s)
                    for (int t = 0; t < image->t(); ++t)
                        for (int r = 0; r < image->r(); ++r)
                            newImage->setColor(image->getColor(s, t, r), s, t, r);
                image = newImage;
            }

            // OSG might not set the right origin for DDS
            if (ext == "dds")
                image->setOrigin(osg::Image::TOP_LEFT);

            // Convert the image to the convention we expect
            if (image->getOrigin() == osg::Image::BOTTOM_LEFT && !disableFlip)
            {
                if (image->isCompressed() && !isS3TC(image))
                {
                    // This is most likely a KTX texture that OSG can't flip
                    // We don't want it to be corrupted or displayed incorrectly, so bail
                    // OSGoS *can* flip RGTC, but we can't verify that (yet?)
                    Log(Debug::Error) << "Error loading " << path << ": cannot flip non-S3TC compressed texture";
                    mCache->addEntryToObjectCache(path.value(), mWarningImage);
                    return mWarningImage;
                }

                image->flipVertical();
                image->setOrigin(osg::Image::TOP_LEFT);
            }

            if (mMaxTextureSize > 0 && mayShrink(path.value()))
                if (osg::ref_ptr<osg::Image> smaller = shrinkImage(*image, mMaxTextureSize))
                    image = std::move(smaller);

            mCache->addEntryToObjectCache(path.value(), image);
            return image;
        }
    }

    osg::Image* ImageManager::getWarningImage()
    {
        return mWarningImage;
    }

    std::size_t ImageManager::getLoadedBytes() const
    {
        std::size_t bytes = 0;
        mCache->call([&](const auto& /*key*/, osg::Object* object) {
            if (const osg::Image* image = dynamic_cast<const osg::Image*>(object))
                if (image != mWarningImage.get())
                    bytes += image->getTotalSizeInBytesIncludingMipmaps();
        });
        return bytes;
    }

    void ImageManager::reportStats(unsigned int frameNumber, osg::Stats* stats) const
    {
        Resource::reportStats("Image", frameNumber, mCache->getStats(), *stats);
    }

}
