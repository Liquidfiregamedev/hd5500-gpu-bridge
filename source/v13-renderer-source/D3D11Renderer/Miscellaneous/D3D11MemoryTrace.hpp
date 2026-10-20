#pragma once
#include <d3d11.h>
#include <windows.h>
#include <cstdio>

namespace ashes::D3D11_NAMESPACE::memtrace
{
	inline volatile LONG64 nativeBufferBytes{};
	inline volatile LONG64 nativeBufferCount{};
	inline volatile LONG64 peakBufferBytes{};
	inline volatile LONG traceCounter{};
	inline volatile LONG tailTraceCounter{};
	inline volatile LONG failureTraceCounter{};
	// v12: native texture accounting (estimated from the native descriptor).
	inline volatile LONG64 nativeTextureBytes{};
	inline volatile LONG64 nativeTextureCount{};
	inline volatile LONG64 peakTextureBytes{};
	inline volatile LONG64 decodedTextureBytes{};
	inline volatile LONG64 decodedTextureCount{};
	inline void raisePeak(volatile LONG64 & peakValue, LONG64 total)
	{
		auto peak=InterlockedCompareExchange64(&peakValue,0,0);
		while(total>peak)
		{
			auto before=InterlockedCompareExchange64(&peakValue,total,peak);
			if(before==peak) break;
			peak=before;
		}
	}
	inline long long load(volatile LONG64 & value)
	{
		return static_cast<long long>(InterlockedCompareExchange64(&value,0,0));
	}
	inline bool enabled()
	{
		char value[2]{};
		return GetEnvironmentVariableA("RW_HD5500_MEMORY_TRACE",value,2u)==1u && value[0]=='1';
	}
	inline UINT bufferBytes(ID3D11Resource * resource)
	{
		if(!resource) return 0u;
		D3D11_RESOURCE_DIMENSION kind{}; resource->GetType(&kind);
		if(kind!=D3D11_RESOURCE_DIMENSION_BUFFER) return 0u;
		D3D11_BUFFER_DESC desc{};
		static_cast<ID3D11Buffer *>(resource)->GetDesc(&desc);
		return desc.ByteWidth;
	}
	inline void acquire(ID3D11Resource * resource)
	{
		if(auto bytes=bufferBytes(resource))
		{
			auto total=InterlockedAdd64(&nativeBufferBytes,LONG64(bytes));
			InterlockedIncrement64(&nativeBufferCount);
			raisePeak(peakBufferBytes,total);
		}
	}
	inline void release(ID3D11Resource * resource)
	{
		if(auto bytes=bufferBytes(resource))
		{
			InterlockedAdd64(&nativeBufferBytes,-LONG64(bytes));
			InterlockedDecrement64(&nativeBufferCount);
		}
	}
	inline void acquireTexture(unsigned long long bytes,bool decoded)
	{
		if(!bytes) return;
		raisePeak(peakTextureBytes,InterlockedAdd64(&nativeTextureBytes,LONG64(bytes)));
		InterlockedIncrement64(&nativeTextureCount);
		if(decoded)
		{
			InterlockedAdd64(&decodedTextureBytes,LONG64(bytes));
			InterlockedIncrement64(&decodedTextureCount);
		}
	}
	inline void releaseTexture(unsigned long long bytes,bool decoded)
	{
		if(!bytes) return;
		InterlockedAdd64(&nativeTextureBytes,-LONG64(bytes));
		InterlockedDecrement64(&nativeTextureCount);
		if(decoded)
		{
			InterlockedAdd64(&decodedTextureBytes,-LONG64(bytes));
			InterlockedDecrement64(&decodedTextureCount);
		}
	}
	inline void log(char const * stage,void const * parent,unsigned long long parentBytes,
		unsigned long long bindingOffset,unsigned long long logicalBytes,unsigned long long parentObjects,
		UINT nativeBytes,HRESULT hr)
	{
		if(!enabled()) return;
		auto ordinal=InterlockedIncrement(&traceCounter);
		if(ordinal>4096)
		{
			if(FAILED(hr)) { if(InterlockedIncrement(&failureTraceCounter)>128) return; }
			else if((ordinal%64)!=0 || InterlockedIncrement(&tailTraceCounter)>2048) return;
		}
		MEMORYSTATUSEX memory{}; memory.dwLength=sizeof(memory);
		auto valid=GlobalMemoryStatusEx(&memory);
		std::fprintf(stderr,"[RW-GPU-MEMORY] stage=%s parent=%p parentBytes=%llu bindingOffset=%llu logicalBytes=%llu retainedParentObjects=%llu nativeBufferWidth=%u nativeLiveBufferBytes=%lld nativeLiveBufferCount=%lld nativePeakBufferBytes=%lld nativeLiveTextureBytes=%lld nativePeakTextureBytes=%lld physicalAvailable=%llu commitAvailable=%llu memorySnapshotValid=%u hr=0x%08lX\n",
			stage,parent,parentBytes,bindingOffset,logicalBytes,parentObjects,nativeBytes,
			load(nativeBufferBytes),load(nativeBufferCount),load(peakBufferBytes),
			load(nativeTextureBytes),load(peakTextureBytes),
			static_cast<unsigned long long>(memory.ullAvailPhys),static_cast<unsigned long long>(memory.ullAvailPageFile),
			unsigned(valid!=FALSE),static_cast<unsigned long>(hr));
		std::fflush(stderr);
	}
	// One summary line, printed at device destruction (always; low volume).
	inline void summary(char const * stage)
	{
		MEMORYSTATUSEX memory{}; memory.dwLength=sizeof(memory);
		auto valid=GlobalMemoryStatusEx(&memory);
		std::fprintf(stderr,"[RW-GPU-MEMORY-SUMMARY] stage=%s liveBufferBytes=%lld liveBufferCount=%lld peakBufferBytes=%lld liveTextureBytes=%lld liveTextureCount=%lld peakTextureBytes=%lld liveDecodedBcBytes=%lld liveDecodedBcCount=%lld physicalAvailable=%llu memorySnapshotValid=%u note=texture-bytes-are-estimates-from-native-descriptors\n",
			stage,load(nativeBufferBytes),load(nativeBufferCount),load(peakBufferBytes),
			load(nativeTextureBytes),load(nativeTextureCount),load(peakTextureBytes),
			load(decodedTextureBytes),load(decodedTextureCount),
			static_cast<unsigned long long>(memory.ullAvailPhys),unsigned(valid!=FALSE));
		std::fflush(stderr);
	}
}
