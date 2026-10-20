// Native Windows D3D11 fixture. This deliberately uses real ID3D11 interfaces.
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

#define D3D11_NAMESPACE d3d11
namespace ashes::d3d11
{
	// Fixture-local minimum contract required by D3D11ResourceMap.hpp.
	struct ObjectMemory
	{
		ID3D11Resource * resource{};
		D3D11_MAP mapFlags{};
		mutable ID3D11Resource * mapStaging{};
	};
}

#include "D3D11ResourceMap.hpp"

namespace
{
	constexpr uint32_t kOriginalWord = 0x11223344u;
	constexpr uint32_t kWrittenWord = 0x55667788u;
	constexpr UINT kByteWidth = 4096u;

	void release( IUnknown * value ) noexcept
	{
		if ( value ) value->Release();
	}

	bool check( bool condition, char const * description )
	{
		std::printf( "%s: %s\n", condition ? "PASS" : "FAIL", description );
		return condition;
	}

	bool checkHr( HRESULT hr, char const * description )
	{
		if ( FAILED( hr ) )
		{
			std::printf( "FAIL: %s (hr=0x%08lX)\n", description,
				static_cast< unsigned long >( hr ) );
			return false;
		}
		std::printf( "PASS: %s\n", description );
		return true;
	}

	bool verifyFirstWord( ID3D11Device * device, ID3D11DeviceContext * context,
		ID3D11Buffer * source, uint32_t expected )
	{
		D3D11_BUFFER_DESC desc{};
		desc.ByteWidth = kByteWidth;
		desc.Usage = D3D11_USAGE_STAGING;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

		ID3D11Buffer * readback{};
		HRESULT hr = device->CreateBuffer( &desc, nullptr, &readback );
		if ( !checkHr( hr, "independent staging readback buffer created" ) ) return false;

		context->CopyResource( readback, source );
		D3D11_MAPPED_SUBRESOURCE mapped{};
		hr = context->Map( readback, 0u, D3D11_MAP_READ, 0u, &mapped );
		bool success = checkHr( hr, "independent staging READ map succeeds" );
		if ( success )
		{
			uint32_t actual{};
			std::memcpy( &actual, mapped.pData, sizeof( actual ) );
			success = check( actual == expected, "independent staging READ observes expected first word" );
			context->Unmap( readback, 0u );
		}
		release( readback );
		return success;
	}

	bool runDefaultBufferTest( ID3D11Device * device, ID3D11DeviceContext * context )
	{
		std::array< uint8_t, kByteWidth > initial{};
		std::memcpy( initial.data(), &kOriginalWord, sizeof( kOriginalWord ) );
		for ( size_t i = sizeof( kOriginalWord ); i < initial.size(); ++i )
		{
			initial[i] = static_cast< uint8_t >( i & 0xFFu );
		}

		D3D11_SUBRESOURCE_DATA initialData{};
		initialData.pSysMem = initial.data();
		D3D11_BUFFER_DESC desc{};
		desc.ByteWidth = kByteWidth;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;

		ID3D11Buffer * buffer{};
		HRESULT hr = device->CreateBuffer( &desc, &initialData, &buffer );
		if ( !checkHr( hr, "DEFAULT 4 KiB buffer with known initial data created" ) ) return false;

		ashes::d3d11::ObjectMemory memory{ buffer, D3D11_MAP_READ_WRITE, nullptr };
		D3D11_MAPPED_SUBRESOURCE mapped{};
		hr = ashes::d3d11::resmap::lock( memory, context, 0u, mapped );
		bool success = checkHr( hr, "resmap::lock READ_WRITE on DEFAULT buffer succeeds" );
		success &= check( mapped.pData != nullptr, "DEFAULT lock returns a real mapped pointer" );
		success &= check( memory.mapStaging != nullptr, "DEFAULT lock allocates real STAGING companion" );
		if ( success )
		{
			success &= check( std::memcmp( mapped.pData, initial.data(), initial.size() ) == 0,
				"DEFAULT staged map reads all original 4 KiB bytes" );
			std::memcpy( mapped.pData, &kWrittenWord, sizeof( kWrittenWord ) );
		}
		if ( memory.mapStaging || mapped.pData )
		{
			ashes::d3d11::resmap::unlock( memory, context, 0u );
		}
		success &= check( memory.mapStaging == nullptr, "DEFAULT unlock releases STAGING companion" );
		success &= verifyFirstWord( device, context, buffer, kWrittenWord );

		// A buffer has exactly one native subresource. The helper must reject 1
		// before it allocates or maps anything.
		D3D11_MAPPED_SUBRESOURCE invalid{};
		invalid.pData = reinterpret_cast< void * >( static_cast< uintptr_t >( 1u ) );
		hr = ashes::d3d11::resmap::lock( memory, context, 1u, invalid );
		success &= check( FAILED( hr ), "invalid nativeSubresource 1 fails with HRESULT" );
		success &= check( invalid.pData == nullptr, "invalid nativeSubresource returns null pointer" );
		success &= check( memory.mapStaging == nullptr, "invalid nativeSubresource leaves no companion" );

		release( buffer );
		return success;
	}

	bool runDirectStagingTest( ID3D11Device * device, ID3D11DeviceContext * context )
	{
		D3D11_BUFFER_DESC desc{};
		desc.ByteWidth = kByteWidth;
		desc.Usage = D3D11_USAGE_STAGING;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
		ID3D11Buffer * buffer{};
		HRESULT hr = device->CreateBuffer( &desc, nullptr, &buffer );
		if ( !checkHr( hr, "STAGING CPU_READ|CPU_WRITE buffer created" ) ) return false;

		ashes::d3d11::ObjectMemory memory{ buffer, D3D11_MAP_READ_WRITE, nullptr };
		D3D11_MAPPED_SUBRESOURCE mapped{};
		hr = ashes::d3d11::resmap::lock( memory, context, 0u, mapped );
		bool success = checkHr( hr, "resmap::lock READ_WRITE direct STAGING succeeds" );
		success &= check( mapped.pData != nullptr, "direct STAGING map returns pointer" );
		success &= check( memory.mapStaging == nullptr, "direct STAGING map creates no companion" );
		if ( success )
		{
			std::memcpy( mapped.pData, &kWrittenWord, sizeof( kWrittenWord ) );
			ashes::d3d11::resmap::unlock( memory, context, 0u );
			D3D11_MAPPED_SUBRESOURCE verify{};
			hr = context->Map( buffer, 0u, D3D11_MAP_READ, 0u, &verify );
			success &= checkHr( hr, "direct STAGING verification READ map succeeds" );
			if ( SUCCEEDED( hr ) )
			{
				uint32_t actual{};
				std::memcpy( &actual, verify.pData, sizeof( actual ) );
				success &= check( actual == kWrittenWord, "direct STAGING preserves modification" );
				context->Unmap( buffer, 0u );
			}
		}
		else if ( mapped.pData )
		{
			ashes::d3d11::resmap::unlock( memory, context, 0u );
		}
		release( buffer );
		return success;
	}

	bool runMultisampleRejectionTest( ID3D11Device * device, ID3D11DeviceContext * context )
	{
		UINT qualityLevels{};
		HRESULT hr = device->CheckMultisampleQualityLevels( DXGI_FORMAT_R8G8B8A8_UNORM, 4u, &qualityLevels );
		if ( FAILED( hr ) || qualityLevels == 0u )
		{
			std::printf( "SKIP: RGBA8 4x MSAA unavailable (hr=0x%08lX, qualityLevels=%u)\n",
				static_cast< unsigned long >( hr ), qualityLevels );
			return true;
		}

		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = 16u;
		desc.Height = 16u;
		desc.MipLevels = 1u;
		desc.ArraySize = 1u;
		desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.SampleDesc.Count = 4u;
		desc.SampleDesc.Quality = 0u;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_RENDER_TARGET;
		ID3D11Texture2D * texture{};
		hr = device->CreateTexture2D( &desc, nullptr, &texture );
		if ( FAILED( hr ) )
		{
			std::printf( "SKIP: RGBA8 4x MSAA texture creation unavailable (hr=0x%08lX)\n",
				static_cast< unsigned long >( hr ) );
			return true;
		}
		std::printf( "PASS: RGBA8 4x MSAA texture created; rejection path is tested\n" );

		ashes::d3d11::ObjectMemory memory{ texture, D3D11_MAP_READ_WRITE, nullptr };
		D3D11_MAPPED_SUBRESOURCE mapped{};
		mapped.pData = reinterpret_cast< void * >( static_cast< uintptr_t >( 1u ) );
		hr = ashes::d3d11::resmap::lock( memory, context, 0u, mapped );
		bool success = check( hr == E_NOTIMPL, "4x MSAA lock rejects with E_NOTIMPL" );
		success &= check( memory.mapStaging == nullptr, "4x MSAA rejection creates no companion" );
		success &= check( mapped.pData == nullptr, "4x MSAA rejection returns null pointer" );
		release( texture );
		return success;
	}
}

int main()
{
	std::puts( "D3D11ResourceMap real-D3D11 native fixture" );
	std::puts( "Device policy: D3D_DRIVER_TYPE_HARDWARE only; no WARP fallback." );

	ID3D11Device * device{};
	ID3D11DeviceContext * context{};
	D3D_FEATURE_LEVEL featureLevel{};
	HRESULT hr = D3D11CreateDevice( nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0u,
		nullptr, 0u, D3D11_SDK_VERSION, &device, &featureLevel, &context );
	if ( FAILED( hr ) )
	{
		std::printf( "SKIP_NO_HARDWARE: D3D11CreateDevice(HARDWARE) failed (hr=0x%08lX); WARP was not attempted.\n",
			static_cast< unsigned long >( hr ) );
		return 77;
	}
	std::printf( "PASS: D3D11CreateDevice(HARDWARE) succeeded (feature level 0x%X)\n", unsigned( featureLevel ) );

	bool success = runDefaultBufferTest( device, context );
	success &= runDirectStagingTest( device, context );
	success &= runMultisampleRejectionTest( device, context );
	context->Flush();
	release( context );
	release( device );
	std::printf( "%s: fixture complete\n", success ? "PASS" : "FAIL" );
	return success ? 0 : 1;
}
