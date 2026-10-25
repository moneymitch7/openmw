#ifndef OPENMW_COMPONENTS_RESOURCE_IMAGEMANAGER_H
#define OPENMW_COMPONENTS_RESOURCE_IMAGEMANAGER_H

#include <osg/Image>
#include <osg/Texture2D>
#include <osg/ref_ptr>

#include <components/vfs/pathutil.hpp>

#include "resourcemanager.hpp"

namespace osgDB
{
    class Options;
}

namespace Resource
{

    /// @brief Handles loading/caching of Images.
    /// @note May be used from any thread.
    class ImageManager : public ResourceManager<osg::ref_ptr<osg::Image>>
    {
    public:
        explicit ImageManager(const VFS::Manager* vfs, double expiryDelay);
        ~ImageManager();

        /// Create or retrieve an Image
        /// Returns the dummy image if the given image is not found.
        osg::ref_ptr<osg::Image> getImage(VFS::Path::NormalizedView path, bool disableFlip = false);

        osg::Image* getWarningImage();

        void reportStats(unsigned int frameNumber, osg::Stats* stats) const override;

        /// The images loaded and still cached, in bytes with their mipmaps: about what the textures made from them
        /// take on the GPU. Goes through the whole cache, so not for every frame.
        std::size_t getLoadedBytes() const;

        /// Textures of objects and the world (under textures/, not the interface's) loaded from now on are made no
        /// larger than @a size on either side by leaving out their largest mipmaps (or by scaling those without
        /// mipmaps). 0 for no limit.
        void setMaxTextureSize(int size) { mMaxTextureSize = size; }

        /// Tools that only need to know which textures a model uses (openmw-distantlandtool, github.com/moneymitch7/openmw-distantlandtool): images are not read, an
        /// existing one is a 1x1 placeholder named after its path.
        void setNamesOnly(bool namesOnly) { mNamesOnly = namesOnly; }

    private:
        osg::ref_ptr<osg::Image> mWarningImage;
        int mMaxTextureSize = 0;
        bool mNamesOnly = false;
        osg::ref_ptr<osgDB::Options> mOptions;

        ImageManager(const ImageManager&);
        void operator=(const ImageManager&);
    };

}

#endif
