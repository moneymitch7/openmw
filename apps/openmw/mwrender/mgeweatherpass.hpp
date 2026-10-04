#ifndef OPENMW_MWRENDER_MGEWEATHERPASS_H
#define OPENMW_MWRENDER_MGEWEATHERPASS_H

#include <osg/Camera>
#include <osg/Group>
#include <osg/Texture2D>
#include <osg/ref_ptr>

namespace Shader
{
    class ShaderManager;
}

namespace MWRender
{
    /// OpenMGE XE: runs the MGE weather model once per frame.
    ///
    /// The model (compatibility/mge_fog.glsl) only reads frame-constant uniforms, so instead of compiling it into
    /// every vertex shader and evaluating it per vertex, a tiny pre-render pass evaluates it once into an 8x2 float
    /// texture: row 0 with the scene flavour, row 1 with water's full core. The consumer vertex shaders read their
    /// eight values from that texture (mgeWxTex). Same GLSL, same uniforms - the pass sits under the scene root and
    /// inherits exactly what the main view's shaders see - so the result is identical, while the vertex programs
    /// shrink back to about stock size (faster to compile when new objects first appear).
    ///
    /// Only the main scene camera runs the pass, once per frame; every other view (reflections, maps) reads the
    /// main view's result.
    class MgeWeatherPass : public osg::Group
    {
    public:
        static constexpr int sWidth = 8;
        static constexpr int sHeight = 2;

        /// @param textureUnit the reserved global unit the scene reads the texture from; unbound inside the pass
        MgeWeatherPass(Shader::ShaderManager& shaderManager, int textureUnit);

        osg::Texture2D* getTexture() const { return mTexture.get(); }

    private:
        osg::ref_ptr<osg::Texture2D> mTexture;
        osg::ref_ptr<osg::Camera> mCamera;
    };
}

#endif
