/* -*-c++-*- */
/* osgEarth - Dynamic map generation toolkit for OpenSceneGraph
* Copyright 2008-2010 Pelican Mapping
* http://osgearth.org
*
* osgEarth is free software; you can redistribute it and/or modify
* it under the terms of the GNU Lesser General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU Lesser General Public License for more details.
*
* You should have received a copy of the GNU Lesser General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>
*/
#include <osgEarthUtil/OverlayDecorator>
#include <osgEarth/FindNode>
#include <osgEarth/Registry>
#include <osgEarth/TextureCompositor>
#include <osg/Texture2D>
#include <osg/TexEnv>

#define LC "[OverlayDecorator] "

using namespace osgEarth;
using namespace osgEarth::Util;
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------

OverlayDecorator::OverlayDecorator( const Map* map ) :
_textureUnit( 1 ),
_textureSize( 1024 ),
_mapInfo( map ),
_reservedTextureUnit( false )
{
    // force an update traversal:
    ADJUST_UPDATE_TRAV_COUNT( this, 1 );

    reinit();
}

void
OverlayDecorator::reinit()
{
    // need to pre-allocate the image here, otherwise the RTT images won't have an alpha channel:
    osg::Image* image = new osg::Image();
    image->allocateImage( *_textureSize, *_textureSize, 1, GL_RGBA, GL_UNSIGNED_BYTE );
    image->setInternalTextureFormat( GL_RGBA8 );    

    _projTexture = new osg::Texture2D( image );
    _projTexture->setTextureSize( *_textureSize, *_textureSize );
    _projTexture->setFilter( osg::Texture::MIN_FILTER, osg::Texture::LINEAR );
    _projTexture->setFilter( osg::Texture::MAG_FILTER, osg::Texture::LINEAR );
    _projTexture->setWrap( osg::Texture::WRAP_S, osg::Texture::CLAMP );
    _projTexture->setWrap( osg::Texture::WRAP_T, osg::Texture::CLAMP );
    _projTexture->setWrap( osg::Texture::WRAP_R, osg::Texture::CLAMP );

    // set up the RTT camera:
    _rttCamera = new osg::Camera();
    _rttCamera->setClearColor( osg::Vec4f(0,0,0,0) );
    _rttCamera->setReferenceFrame( osg::Camera::ABSOLUTE_RF );
    _rttCamera->setViewport( 0, 0, *_textureSize, *_textureSize );
    _rttCamera->setComputeNearFarMode( osg::CullSettings::DO_NOT_COMPUTE_NEAR_FAR );
    _rttCamera->setRenderOrder( osg::Camera::PRE_RENDER );
    _rttCamera->setRenderTargetImplementation( osg::Camera::FRAME_BUFFER_OBJECT );
    _rttCamera->attach( osg::Camera::COLOR_BUFFER, _projTexture.get() );
    _rttCamera->getOrCreateStateSet()->setMode( GL_LIGHTING, osg::StateAttribute::OFF | osg::StateAttribute::PROTECTED );

    // texture coordinate generator:
    _texGenNode = new osg::TexGenNode();
    _texGenNode->setTextureUnit( *_textureUnit );
    
    if ( _overlayGraph.valid() && ( _overlayGraph->getNumParents() == 0 || _overlayGraph->getParent(0) != _rttCamera.get() ))
    {
        if ( _rttCamera->getNumChildren() > 0 )
            _rttCamera->replaceChild( 0, _overlayGraph.get() );
        else
            _rttCamera->addChild( _overlayGraph.get() );
    }
}

void
OverlayDecorator::setOverlayGraph( osg::Node* node )
{
    if ( _overlayGraph.get() != node )
    {
        _overlayGraph = node;
        reinit();
    }
}

void
OverlayDecorator::setTextureSize( int texSize )
{
    if ( texSize != _textureSize.value() )
    {
        _textureSize = texSize;
        reinit();
    }
}

void
OverlayDecorator::setTextureUnit( int texUnit )
{
    if ( texUnit != _textureUnit.value() )
    {
        _textureUnit = texUnit;
        reinit();
    }
}

void
OverlayDecorator::onInstall( TerrainEngineNode* engine )
{
    if ( !_textureUnit.isSet() )
    {
        int texUnit;
        if ( engine->getTextureCompositor()->reserveTextureImageUnit( texUnit ) )
        {
            _textureUnit = texUnit;
            _reservedTextureUnit = true;
            OE_INFO << LC << "Reserved texture image unit " << *_textureUnit << std::endl;
        }
    }

    if ( !_textureSize.isSet() )
    {
        int maxSize = Registry::instance()->getCapabilities().getMaxTextureSize();
        _textureSize = osg::minimum( 1024, maxSize );

        OE_INFO << LC << "Using texture size = " << *_textureSize << std::endl;
    }

    reinit();

    // set up the child to receive the projected texture:
    osg::StateSet* set = getChild(0)->getOrCreateStateSet();
    set->setTextureMode( *_textureUnit, GL_TEXTURE_GEN_S, osg::StateAttribute::ON );
    set->setTextureMode( *_textureUnit, GL_TEXTURE_GEN_T, osg::StateAttribute::ON );
    set->setTextureMode( *_textureUnit, GL_TEXTURE_GEN_R, osg::StateAttribute::ON );
    set->setTextureMode( *_textureUnit, GL_TEXTURE_GEN_Q, osg::StateAttribute::ON );
    set->setTextureAttributeAndModes( *_textureUnit, _projTexture.get(), osg::StateAttribute::ON );

    osg::TexEnv* env = new osg::TexEnv();
    env->setMode( osg::TexEnv::DECAL );
    set->setTextureAttributeAndModes( *_textureUnit, env, osg::StateAttribute::ON );
}

void
OverlayDecorator::onUninstall( TerrainEngineNode* engine )
{
    if ( _reservedTextureUnit )
    {
        engine->getTextureCompositor()->releaseTextureImageUnit( *_textureUnit );
        _textureUnit.unset();
        _reservedTextureUnit = false;
    }

    //TODO: remove the proj-tex state attributes from the child
}

void
OverlayDecorator::updateRTTCamera( osg::NodeVisitor& nv )
{
    if ( nv.getVisitorType() == osg::NodeVisitor::UPDATE_VISITOR )
    {
        // configure the RTT camera:
        _rttCamera->setViewMatrix( _rttViewMatrix );
        _rttCamera->setProjectionMatrix( _rttProjMatrix );

        // configure the Projector camera:
        osg::Matrix MVP = _projectorViewMatrix * _projectorProjMatrix;
        osg::Matrix MVPT = MVP * osg::Matrix::translate(1.0,1.0,1.0) * osg::Matrix::scale(0.5,0.5,0.5);
        _texGenNode->getTexGen()->setMode( osg::TexGen::EYE_LINEAR );
        _texGenNode->getTexGen()->setPlanesFromMatrix( MVPT );
    }

    else if ( nv.getVisitorType() == osg::NodeVisitor::CULL_VISITOR )
    {
        osgUtil::CullVisitor* cv = dynamic_cast<osgUtil::CullVisitor*>( &nv );
        if ( !cv ) return;

        double re = _mapInfo.getProfile()->getSRS()->getEllipsoid()->getRadiusEquator();
        double rp = _mapInfo.getProfile()->getSRS()->getEllipsoid()->getRadiusPolar();

        osg::Vec3 eye = cv->getEyePoint();
        double eyeLen = eye.length();

        // point the RTT camera straight down from the eyepoint:
        _rttViewMatrix = osg::Matrixd::lookAt( eye, osg::Vec3(0,0,0), osg::Vec3(0,0,1) );

        // calculate the approximate distance from the eye to the horizon. This is out maximum
        // possible RTT extent:
        double hae = eyeLen - re; // height above "max spheroid"
        double haeAdj = hae*1.5;    // wiggle room, since the ellipsoid is different from the spheroid.
        double eMax = sqrt( haeAdj*haeAdj + 2.0*re*haeAdj ); // distance to the horizon

        // calculate the approximate extent viewed from the camera if it's pointing
        // at the ground. This is the minimum acceptable RTT extent.
        double vfov, ar, znear, zfar;
        cv->getProjectionMatrix()->getPerspective( vfov, ar, znear, zfar );
        double eMin = haeAdj * tan( osg::DegreesToRadians(0.5*vfov) );

        // calculate the deviation between the RTT camera's look-vector and the main camera's
        // look-vector (cross product). This gives us a [0..1] multiplier that will vary the
        // RTT extent as the camera's pitch varies from [-90..0].
        osg::Vec3 from, to, up;
        
        const osg::Matrix& mvMatrix = *cv->getModelViewMatrix();
        mvMatrix.getLookAt( from, to, up, eyeLen);
        osg::Vec3 camLookVec = to-from;
        camLookVec.normalize();

        _rttViewMatrix.getLookAt(from,to,up,eyeLen);
        osg::Vec3 rttLookVec = to-from;
        rttLookVec.normalize();

        double deviation = (rttLookVec ^ camLookVec).length();
        double t = deviation; // (deviation*deviation); // interpolation factor
        double eIdeal = eMin + t * (eMax-eMin); 

        //OE_INFO << "dev=" << deviation << ", ext=" << eIdeal << std::endl;

        _rttProjMatrix = osg::Matrix::ortho( -eIdeal, eIdeal, -eIdeal, eIdeal, 1.0, eyeLen );

        // projector matrices are the same as for the RTT camera. Tim was right.
        _projectorViewMatrix = _rttViewMatrix;
        _projectorProjMatrix = _rttProjMatrix;
    }
}

void
OverlayDecorator::traverse( osg::NodeVisitor& nv )
{
    // update the RTT camera if necessary:
    updateRTTCamera( nv );
    
    _rttCamera->accept( nv );

    _texGenNode->accept( nv );

    TerrainDecorator::traverse( nv );
}
