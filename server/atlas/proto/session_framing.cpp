// =============================================================================
// FrameReader 설치와 프레임 송신 구현
// =============================================================================

#include "atlas/proto/session_framing.h"

#include <utility>
#include <vector>

#include "atlas/core/ids.h"
#include "atlas/core/log.h"
#include "atlas/net/session.h"
#include "atlas/proto/frame_reader.h"

namespace atlas
{

void AttachFrameReader( const std::shared_ptr< Session >& session, FrameHandler on_frame )
{
    // 값 캡처가 아니라 shared_ptr
    // std::function 은 복사 가능한 callable 을 요구한다
    // 복사된 람다는 재조립 버퍼를 하나 더 들고 가 스트림에서 뒤처진다
    session->SetBytesHandler(
        [reader = std::make_shared< FrameReader >(), handler = std::move( on_frame )](
            const std::shared_ptr< Session >& source, std::span< const Byte > bytes )
        {
            std::vector< Frame > frames;
            const FrameError error = reader->Feed( bytes, frames );

            for ( const Frame& frame : frames )
            {
                if ( handler )
                {
                    handler( source, frame );
                }
            }

            if ( error != FrameError::None )
            {
                ATLAS_LOG_WARN( "session {} framing error: {}", IdValue( source->Id() ),
                                FrameReader::Describe( error ) );
                source->Close();
            }
        } );
}

bool SendFrame( Session& session, UInt16 opcode, UInt32 seq, std::span< const Byte > payload )
{
    std::vector< Byte > bytes;
    if ( !EncodeFrame( bytes, opcode, seq, payload ) )
    {
        return false;
    }
    session.Send( bytes );
    return true;
}

}  // namespace atlas
