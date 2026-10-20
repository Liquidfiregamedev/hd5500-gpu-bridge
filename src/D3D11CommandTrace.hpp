#pragma once
#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <windows.h>
#include <cstdio>
#include <vector>

namespace ashes::D3D11_NAMESPACE::cmdtrace
{
	inline bool enabled()
	{
		char flag[2]{};
		return GetEnvironmentVariableA("RW_HD5500_COMMAND_TRACE",flag,2u)==1u && flag[0]=='1';
	}
	inline bool serialEnabled()
	{
		char flag[2]{};
		return GetEnvironmentVariableA("RW_HD5500_SERIAL_CHECKPOINTS",flag,2u)==1u && flag[0]=='1';
	}
	inline volatile LONG commandCounter{};
	inline volatile LONG printedMessageCounter{};
	class Trace
	{
	public:
		explicit Trace(ID3D11Device * device,ID3D11DeviceContext * context)
			: m_device(device),m_context(context),m_enabled(enabled()),m_serial(serialEnabled())
		{
			if (!m_enabled) return;
			if (m_serial)
			{
				D3D11_QUERY_DESC desc{}; desc.Query=D3D11_QUERY_EVENT;
				auto hr=m_device->CreateQuery(&desc,&m_query);
				if (FAILED(hr) || !m_query)
				{
					std::fprintf(stderr,"[RW-GPU-CHECKPOINT] query-unavailable hr=0x%08lX; trace-only, no completion claim\n",static_cast<unsigned long>(hr));
					m_serial=false;
				}
			}
			m_device->QueryInterface(__uuidof(ID3D11InfoQueue),reinterpret_cast<void **>(&m_queue));
			if(m_queue) m_messageCursor=m_queue->GetNumStoredMessagesAllowedByRetrievalFilter();
			m_start=GetTickCount64();
		}
		~Trace()
		{
			if(m_queue) m_queue->Release();
			if(m_query) m_query->Release();
		}
		Trace(Trace const &)=delete;
		Trace & operator=(Trace const &)=delete;
		LONG before(char const * operation)
		{
			if(!m_enabled) return 0;
			auto id=InterlockedIncrement(&commandCounter);
			if(id<=8192)
			{
				std::fprintf(stderr,"[RW-GPU-COMMAND] BEGIN id=%ld tick=%llu type=%s removedReason=0x%08lX\n",long(id),static_cast<unsigned long long>(GetTickCount64()),operation,static_cast<unsigned long>(m_device->GetDeviceRemovedReason()));
				std::fflush(stderr);
			}
			else if(id==8193)
			{
				std::fprintf(stderr,"[RW-GPU-COMMAND] trace/serial limit reached (8192); rendering continues unmodified\n");
				std::fflush(stderr);
			}
			return id;
		}
		void after(LONG id,char const * operation)
		{
			if(!m_enabled || !id || id>8192) return;
			auto reason=m_device->GetDeviceRemovedReason();
			HRESULT queryHr=S_FALSE; BOOL completed=FALSE; bool timedOut=false;
			if(m_serial && m_query && SUCCEEDED(reason))
			{
				m_context->End(m_query);
				m_context->Flush();
				auto begin=GetTickCount64();
				for(;;)
				{
					queryHr=m_context->GetData(m_query,&completed,sizeof(completed),D3D11_ASYNC_GETDATA_DONOTFLUSH);
					if(FAILED(queryHr) || (queryHr==S_OK && completed)) break;
					reason=m_device->GetDeviceRemovedReason();
					if(FAILED(reason)) break;
					if(GetTickCount64()-begin>=2000u || GetTickCount64()-m_start>=30000u)
					{
						timedOut=true; m_serial=false; break;
					}
					Sleep(1u);
				}
			}
			reason=m_device->GetDeviceRemovedReason();
			std::fprintf(stderr,"[RW-GPU-COMMAND] END id=%ld type=%s removedReason=0x%08lX queryHr=0x%08lX gpuComplete=%u timeout=%u serialStillActive=%u\n",long(id),operation,static_cast<unsigned long>(reason),static_cast<unsigned long>(queryHr),unsigned(queryHr==S_OK && completed),unsigned(timedOut),unsigned(m_serial));
			if(m_queue)
			{
				auto end=m_queue->GetNumStoredMessagesAllowedByRetrievalFilter();
				if(end<m_messageCursor) m_messageCursor=0u;
				for(auto i=m_messageCursor;i<end;++i)
				{
					SIZE_T size{};
					if(FAILED(m_queue->GetMessage(i,nullptr,&size)) || !size || size>65536u) continue;
					try
					{
						std::vector<unsigned char> buffer(size);
						auto message=reinterpret_cast<D3D11_MESSAGE *>(buffer.data());
						if(SUCCEEDED(m_queue->GetMessage(i,message,&size)) && message->Severity<=D3D11_MESSAGE_SEVERITY_WARNING && message->pDescription)
						{
							if(InterlockedIncrement(&printedMessageCounter)<=128)
								std::fprintf(stderr,"[RW-GPU-COMMAND-MESSAGE] commandId=%ld type=%s messageId=%u severity=%u text=%.1536s\n",long(id),operation,unsigned(message->ID),unsigned(message->Severity),message->pDescription);
						}
					}
					catch(...) { /* Optional capture must not replace the real rendering result. */ }
				}
				m_messageCursor=end;
			}
			std::fflush(stderr);
		}
	private:
		ID3D11Device * m_device{};
		ID3D11DeviceContext * m_context{};
		ID3D11Query * m_query{};
		ID3D11InfoQueue * m_queue{};
		bool m_enabled{},m_serial{};
		UINT64 m_messageCursor{};
		ULONGLONG m_start{};
	};
}
