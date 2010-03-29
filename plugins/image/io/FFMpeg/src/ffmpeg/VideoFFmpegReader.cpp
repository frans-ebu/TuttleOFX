#include "VideoFFmpegReader.hpp"

extern "C" {
#define __STDC_CONSTANT_MACROS
#include <stdint.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libswscale/swscale.h>
}

VideoFFmpegReader::VideoFFmpegReader( )
: _context( NULL )
, _format( NULL )
, _params( NULL )
, _avFrame( NULL )
, _videoCodec( NULL )
, _avformatOptions( NULL )
, _sws_context( NULL )
, _fpsNum( 0 )
, _fpsDen( 0 )
, _currVideoIdx( -1 )
, _nbFrames( 0 )
, _width( 0 /*720*/ )
, _height( 0 /*576*/ )
, _aspect( 1.0 )
, _offsetTime( true )
, _lastSearchPos( -1 )
, _lastDecodedPos( -1 )
, _isOpen( 0 )
{
	for( int i = 0; i < CODEC_TYPE_NB; ++i )
		_avctxOptions[i] = avcodec_alloc_context2( CodecType( i ) );
	_avformatOptions = avformat_alloc_context( );
	_avFrame = avcodec_alloc_frame( );

}

VideoFFmpegReader::~VideoFFmpegReader( )
{
	close( );

	av_free( _avFrame );
	for( int i = 0; i < CODEC_TYPE_NB; ++i )
		av_free( _avctxOptions[i] );
	av_free( _avformatOptions );

}

void VideoFFmpegReader::open( const std::string &filename )
{
	if( isOpen( ) )
		close( );

	_isOpen = 0;

	int error = av_open_input_file( &_context, filename.c_str( ), _format, 0, _params );
	if( error < 0 )
	{
		std::cout << "ffmpegReader: " << ffmpegError_toString( error ) << std::endl;
		_isOpen = 0;
		return;
	}
	// FIXME_GC: needs to know if it's streamable.
	error = av_find_stream_info( _context );
	if( error < 0 )
	{
		std::cout << "ffmpegReader: " << ffmpegError_toString( error ) << std::endl;
		return;
	}
	if( !setupStreamInfo( ) )
	{
		std::cout << "ffmpegReader: Unable to find codec." << std::endl;
		return;
	}
	AVCodecContext* codecContext = getVideoStream( )->codec;
	if( getVideoStream( )->sample_aspect_ratio.num )
		_aspect = av_q2d( getVideoStream( )->sample_aspect_ratio );
	else if( codecContext->sample_aspect_ratio.num )
		_aspect = av_q2d( codecContext->sample_aspect_ratio );

	_data.resize( width( ) * height( ) * 3 );

	// hack so seeking works from our intended position.
	if( !strcmp( codecContext->codec->name, "mjpeg" ) || !strcmp( codecContext->codec->name, "dvvideo" ) )
	{
		std::cerr << "ffmpegReader: [WARNING] codec is using a specific offsetTime" << codecContext->codec->name << std::endl;
		_offsetTime = false;
	}

	/*
		std::cout << "MetaData::CREATOR: " << _context->author << std::endl;
		std::cout << "MetaData::COPYRIGHT: " << _context->copyright << std::endl;
		std::cout << "MetaData::COMMENT: " << _context->comment << std::endl;
		std::cout << "MetaData::PROJECT: " << _context->album << std::endl;
		std::cout << "MetaData::FILE_CREATION_TIME: " << double(_context->timestamp ) << std::endl;
		std::cout << "ffmpeg/num_streams: " << _context->nb_streams << std::endl;
		std::cout << "MetaData::FRAME_RATE: " << fps( ) << std::endl;
		std::cout << "ffmpeg/codec/codecName: " << codecContext->codec->name << std::endl;

		meta.setData(MetaData::CREATOR, context_->author);
		meta.setData(MetaData::COPYRIGHT, context_->copyright);
		meta.setData(MetaData::COMMENT, context_->comment);
		meta.setData(MetaData::PROJECT, context_->album);
		meta.setData(MetaData::FILE_CREATION_TIME, double(context_->timestamp));
		meta.setData("ffmpeg/num_streams", context_->nb_streams);
		meta.setData(MetaData::FRAME_RATE, fps());
		meta.setData("ffmpeg/codec/codecName", codecContext->codec->name);
	 */
	_isOpen = 1;
}

void VideoFFmpegReader::close( )
{
	closeVideoCodec( );
	if( _context )
	{
		av_close_input_file( _context );
		_context = NULL;
	}
}

int VideoFFmpegReader::read( const int frame )
{
	if( _lastDecodedPos + 1 != frame )
	{
		seek( 0 );
		seek( frame );
	}

	av_init_packet( &_pkt );

	bool hasPicture = false;
	int error = 0;
	//	int i = 0;
	while( error >= 0 && !hasPicture )
	{
		error = av_read_frame( _context, &_pkt );
		if( error < 0 ) // on error or end of file
			return 1;

		if( error >= 0 && _videoIdx.size( ) && _currVideoIdx != -1 && _pkt.stream_index == _videoIdx[_currVideoIdx] )
			hasPicture = decodeImage( frame );

		av_free_packet( &_pkt );
	}
	return 0;
}

bool VideoFFmpegReader::setupStreamInfo( )
{
	for( unsigned int i = 0; i < _context->nb_streams; ++i )
	{
		AVCodecContext *codecContext = _context->streams[i]->codec;
		if( codecContext->codec_id == CODEC_ID_NONE )
		{
			std::cerr << "ffmpegReader: Can't find decoder codec_id: CODEC_ID_NONE codecType:" << codecType_toString( codecContext->codec_type ) << std::endl;
			continue;
		}
		if( avcodec_find_decoder( codecContext->codec_id ) == NULL )
		{
			std::cerr << "ffmpegReader: Can't find decoder codec_id: " << codecContext->codec_id << " codecType:" << codecType_toString( codecContext->codec_type ) << std::endl;
			continue;
		}

		switch( codecContext->codec_type )
		{
			case CODEC_TYPE_VIDEO:
				_videoIdx.push_back( i );
				if( _currVideoIdx < 0 )
					_currVideoIdx = 0;
				_width = codecContext->width;
				_height = codecContext->height;
				break;

				// ignore all audio streams
			case CODEC_TYPE_AUDIO:
			case CODEC_TYPE_UNKNOWN:
			case CODEC_TYPE_DATA:
			case CODEC_TYPE_SUBTITLE:
			case CODEC_TYPE_ATTACHMENT:
			default:
				break;
		}
	}

	if( !hasVideo( ) )
		return false;

	AVStream* stream = getVideoStream( );
	if ( stream->codec->coded_frame->interlaced_frame )
	{
		if ( stream->codec->coded_frame->top_field_first )
		{
			_interlacment = eInterlacmentUpper;
		}
		else
		{
			_interlacment = eInterlacmentLower;
		}
	}
	else
	{
		_interlacment = eInterlacmentNone;
	}

	if( stream->r_frame_rate.num != 0 && stream->r_frame_rate.den != 0 )
	{
		_fpsNum = stream->r_frame_rate.num;
		_fpsDen = stream->r_frame_rate.den;
	}

	openVideoCodec( );

	// Set the duration
	if( _context->duration != AV_NOPTS_VALUE )
	{
		_nbFrames = uint64_t( ( fps( )*(double) _context->duration / (double) AV_TIME_BASE ) );
	}
	else
	{
		_nbFrames = 1 << 29;
	}

	// try to calculate the number of frames
	if( !_nbFrames )
	{
		seek( 0 );
		av_init_packet( &_pkt );
		av_read_frame( _context, &_pkt );
		uint64_t firstPts = _pkt.pts;
		uint64_t maxPts = firstPts;
		seek( 1 << 29 );
		av_init_packet( &_pkt );
		while( stream && av_read_frame( _context, &_pkt ) >= 0 )
		{
			uint64_t currPts = av_q2d( getVideoStream( )->time_base )*( _pkt.pts - firstPts ) * fps( );
			if( currPts > maxPts )
				maxPts = currPts;
		}

		_nbFrames = maxPts;
	}

	return true;
}

void VideoFFmpegReader::openVideoCodec( )
{
	AVStream* stream = getVideoStream( );
	if (stream)
	{
		AVCodecContext* codecContext = stream->codec;
		_videoCodec = avcodec_find_decoder( codecContext->codec_id );
		if( _videoCodec == NULL || avcodec_open( codecContext, _videoCodec ) < 0 )
		{
			_currVideoIdx = -1;
		}
	}
	else
	{
		_currVideoIdx = -1;
	}
}

void VideoFFmpegReader::closeVideoCodec( )
{
	AVStream* stream = getVideoStream( );
	if( stream && stream->codec )
		avcodec_close( stream->codec );
}

int64_t VideoFFmpegReader::getTimeStamp( int pos ) const
{
	int64_t timestamp = (int64_t) ( ( (double) pos / fps( ) ) * AV_TIME_BASE );
	if( (uint64_t) _context->start_time != AV_NOPTS_VALUE )
		timestamp += _context->start_time;
	return timestamp;
}

bool VideoFFmpegReader::seek( size_t pos )
{
	int64_t offset = getTimeStamp( pos );
	if( _offsetTime )
	{
		offset -= AV_TIME_BASE;
		if( offset < _context->start_time )
			offset = 0;
	}

	AVStream* stream = getVideoStream( );
	if (stream)
	{
		avcodec_flush_buffers( stream->codec );
		if( av_seek_frame( _context, -1, offset, AVSEEK_FLAG_BACKWARD ) < 0 )
		{
			return false;
		}
	}
	else
	{
		return false;
	}

	return true;
}

bool VideoFFmpegReader::decodeImage( const int frame )
{
	// search for our picture.
	double pts = 0;
	if( (uint64_t) _pkt.dts != AV_NOPTS_VALUE )
	{
		AVStream* stream = getVideoStream( );
		if (stream)
		{
			pts = av_q2d( stream->time_base ) * _pkt.dts;
		}
	}

	int curPos = int(pts * fps( ) + 0.5f );
	if( curPos == _lastSearchPos )
		curPos = _lastSearchPos + 1;
	_lastSearchPos = curPos;

	if( (uint64_t) _context->start_time != AV_NOPTS_VALUE )
		curPos -= int(_context->start_time * fps( ) / AV_TIME_BASE );

	int hasPicture = 0;
	int curSearch = 0;
	AVStream* stream = getVideoStream( );
	if (stream)
	{
		AVCodecContext* codecContext = stream->codec;
		if( curPos >= frame )
		{
			//		std::cout << "avcodec_decode_video" << std::endl;
			avcodec_decode_video( codecContext, _avFrame, &hasPicture, _pkt.data, _pkt.size );
		}
		else if( _offsetTime )
		{
			//		std::cout << "avcodec_decode_video" << std::endl;
			avcodec_decode_video( codecContext, _avFrame, &curSearch, _pkt.data, _pkt.size );
		}

		if( !hasPicture )
			return false;

		_lastDecodedPos = _lastSearchPos;

		AVPicture output;
		avpicture_fill( &output, &_data[0], PIX_FMT_RGB24, _width, _height );

		// patched to use swscale instead of img_convert:
		PixelFormat in_pixelFormat = codecContext->pix_fmt; // pixel format source
		PixelFormat out_pixelFormat = PIX_FMT_RGB24; // pixel format dest

		_sws_context = sws_getCachedContext( _sws_context, width( ), height( ), in_pixelFormat, width( ), height( ), out_pixelFormat, SWS_BICUBIC, NULL, NULL, NULL );

		if( !_sws_context )
		{
			std::cerr << "ffmpegReader: ffmpeg-conversion failed (" << in_pixelFormat << "->" << out_pixelFormat << ")" << std::endl;
			return false;
		}
		int error = sws_scale( _sws_context, _avFrame->data, _avFrame->linesize, 0, height( ), output.data, output.linesize );
		if( error < 0 )
		{
			std::cerr << "ffmpegReader: ffmpeg-conversion failed (" << in_pixelFormat << "->" << out_pixelFormat << ")" << std::endl;
			return false;
		}
	}

	//	std::cout << "decodeImage " << frame << " OK" << std::endl;
	return true;
}
