#include "WriterPlugin.hpp"

namespace tuttle {
namespace plugin {

namespace bfs = boost::filesystem;

WriterPlugin::WriterPlugin( OfxImageEffectHandle handle )
: ImageEffect( handle )
{
	_clipSrc        = fetchClip( kOfxImageEffectSimpleSourceClipName );
	_clipDst        = fetchClip( kOfxImageEffectOutputClipName );
	_paramFilepath       = fetchStringParam( kTuttlePluginWriterParamFilename );
	_paramRenderButton   = fetchPushButtonParam( kTuttlePluginWriterParamRender );
	_paramRenderAlways   = fetchBooleanParam( kTuttlePluginWriterParamRenderAlways );
	_paramBitDepth       = fetchChoiceParam( kTuttlePluginWriterParamBitDepth );
	_filePattern.reset(_paramFilepath->getValue(), false, 0, 1);
}

WriterPlugin::~WriterPlugin()
{
}

void WriterPlugin::getClipPreferences( OFX::ClipPreferencesSetter& clipPreferences )
{
	// If pattern detected (frame varying on time)
	clipPreferences.setOutputFrameVarying( varyOnTime() );
}

void WriterPlugin::changedParam( const OFX::InstanceChangedArgs& args, const std::string& paramName )
{
	if( paramName == kTuttlePluginWriterParamFilename )
	{
		_filePattern.reset(_paramFilepath->getValue(), false, 0, 1);
	}
}

}
}
