#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <algorithm>
struct Device { ID3D11Device *m_d3dDevice{}; mutable HRESULT forwarded=S_OK; mutable std::string forwardedMessage; void onCheckHResultCommand(HRESULT,std::string const&)const; };
		void Device::onCheckHResultCommand( HRESULT hresult
			, std::string const & message )const
		{
			if ( FAILED( hresult ) && m_d3dDevice )
			{
				try
				{
				static std::atomic_uint errors{};
				auto diagnostic = std::getenv( "RW_HD5500_D3D_DIAGNOSTICS" );
				if ( diagnostic && diagnostic[0] == '1' && errors.fetch_add( 1u ) < 32u )
				{
					auto const reason = m_d3dDevice->GetDeviceRemovedReason();
					char const * name = "OTHER_HRESULT";
					switch ( reason )
					{
					case S_OK: name = "S_OK_DEVICE_NOT_REMOVED"; break;
					case DXGI_ERROR_DEVICE_HUNG: name = "DXGI_ERROR_DEVICE_HUNG"; break;
					case DXGI_ERROR_DEVICE_REMOVED: name = "DXGI_ERROR_DEVICE_REMOVED"; break;
					case DXGI_ERROR_DEVICE_RESET: name = "DXGI_ERROR_DEVICE_RESET"; break;
					case DXGI_ERROR_DRIVER_INTERNAL_ERROR: name = "DXGI_ERROR_DRIVER_INTERNAL_ERROR"; break;
					case DXGI_ERROR_INVALID_CALL: name = "DXGI_ERROR_INVALID_CALL"; break;
					}
					std::fprintf( stderr, "[RW-GPU-DEVICE-FAIL] tick=%llu operation=%s callHr=0x%08lX removedReason=0x%08lX reasonName=%s\n"
						, static_cast< unsigned long long >( GetTickCount64() ), message.c_str()
						, static_cast< unsigned long >( hresult ), static_cast< unsigned long >( reason ), name );
					ID3D11InfoQueue * queue{};
					auto const queueHr = m_d3dDevice->QueryInterface( __uuidof( ID3D11InfoQueue ), reinterpret_cast< void ** >( &queue ) );
					if ( SUCCEEDED( queueHr ) && queue )
					{
						auto const count = queue->GetNumStoredMessagesAllowedByRetrievalFilter();
						unsigned printed{};
						for ( UINT64 index = 0u; index < count && index < 256u && printed < 64u; ++index )
						{
							SIZE_T bytes{};
							if ( FAILED( queue->GetMessage( index, nullptr, &bytes ) ) || bytes < sizeof( D3D11_MESSAGE ) || bytes > 65536u ) continue;
							std::vector< uint64_t > storage( ( bytes + 7u ) / 8u );
							auto nativeMessage = reinterpret_cast< D3D11_MESSAGE * >( storage.data() );
							if ( SUCCEEDED( queue->GetMessage( index, nativeMessage, &bytes ) ) && nativeMessage->Severity <= D3D11_MESSAGE_SEVERITY_WARNING )
							{
								std::fprintf( stderr, "[RW-GPU-D3D-MESSAGE] index=%llu id=%u severity=%u text=%.*s\n"
									, static_cast< unsigned long long >( index ), unsigned( nativeMessage->ID ), unsigned( nativeMessage->Severity )
									, int( std::min< SIZE_T >( nativeMessage->DescriptionByteLength, 1536u ) ), nativeMessage->pDescription ? nativeMessage->pDescription : "" );
								++printed;
							}
						}
						std::fprintf( stderr, "[RW-GPU-D3D-QUEUE] stored=%llu warningErrorPrinted=%u\n", static_cast< unsigned long long >( count ), printed );
						queue->Release();
					}
					else std::fprintf( stderr, "[RW-GPU-D3D-QUEUE] unavailable hr=0x%08lX; removedReason above is still a real device query\n", static_cast< unsigned long >( queueHr ) );
					std::fflush( stderr );
				}
				}
				catch ( ... )
				{
					std::fprintf( stderr, "[RW-GPU-DEVICE-FAIL] optional diagnostic capture failed; original HRESULT 0x%08lX remains unchanged\n", static_cast< unsigned long >( hresult ) );
					std::fflush( stderr );
				}
			}
			forwarded = hresult; forwardedMessage = message;
		}

int main(){SetEnvironmentVariableA("RW_HD5500_D3D_DIAGNOSTICS","1");_putenv_s("RW_HD5500_D3D_DIAGNOSTICS","1");Device d;ID3D11DeviceContext *c{};D3D_FEATURE_LEVEL fl{};auto hr=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_DEBUG,nullptr,0,D3D11_SDK_VERSION,&d.m_d3dDevice,&fl,&c);if(hr==DXGI_ERROR_SDK_COMPONENT_MISSING)hr=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&d.m_d3dDevice,&fl,&c);if(FAILED(hr))return 2;D3D11_BUFFER_DESC bad{};bad.Usage=D3D11_USAGE_DEFAULT;bad.BindFlags=D3D11_BIND_VERTEX_BUFFER;ID3D11Buffer *b{};hr=d.m_d3dDevice->CreateBuffer(&bad,nullptr,&b);if(b)b->Release();if(SUCCEEDED(hr))return 3;d.onCheckHResultCommand(hr,"FixtureRealInvalidBuffer");bool ok=d.forwarded==hr&&d.forwardedMessage=="FixtureRealInvalidBuffer"&&d.m_d3dDevice->GetDeviceRemovedReason()==S_OK;std::printf("LOGGER_FIXTURE: %s nativeCreateHr=0x%08lX forwardedUnchanged=%u healthyReason=%u\n",ok?"PASS":"FAIL",(unsigned long)hr,d.forwarded==hr,d.m_d3dDevice->GetDeviceRemovedReason()==S_OK);c->Release();d.m_d3dDevice->Release();return ok?0:4;}
