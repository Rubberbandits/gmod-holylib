#include "httplib.h"
#include "mcp/http_transport.h"
#include <thread>
#include <cmath>
#include "LuaInterface.h"
#include "detours.h"
#include "lua.h"
#include <baseclient.h>
#include <inetchannel.h>
#include <netadr.h>
#include "icommandline.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

class CHTTPServerModule : public IModule
{
public:
	void InitDetour(bool bPreServer) override;
	void Shutdown() override;
	bool CanDisableAtRuntime() override;
	void LuaInit(GarrysMod::Lua::ILuaInterface* pLua, bool bServerInit) override;
	void LuaShutdown(GarrysMod::Lua::ILuaInterface* pLua) override;
	void LuaThink(GarrysMod::Lua::ILuaInterface* pLua) override;
	void OnClientDisconnect(CBaseClient* pClient) override;
	const char* Name() override { return "httpserver"; };
	int Compatibility() override { return LINUX32 | LINUX64 | WINDOWS32 | WINDOWS64; };
	bool SupportsMultipleLuaStates() override { return true; };
};

static CHTTPServerModule g_pHttpServerModule;
IModule* pHttpServerModule = &g_pHttpServerModule;
static std::atomic<bool> g_bHTTPShuttingDown{false};

struct HttpStream {
	std::shared_ptr<HolyHTTP::Stream> state;
	GarrysMod::Lua::ILuaInterface* lua = nullptr;
	int onClose = -1;
	bool notified = false;
};

class HttpServer;
struct HttpResponse {
	bool m_bSetContent = false;
	bool m_bSetRedirect = false;
	bool m_bSetHeader = false;
	int m_iRedirectCode = 302;
	int m_iStatusCode = -1;
	std::string m_strContent = "";
	std::string m_strContentType = "text/plain";
	std::string m_strRedirect = "";
	unordered_map<std::string, std::string> m_pHeaders;

	inline void DoResponse(httplib::Response& pResponse)
	{
		if (m_bSetContent)
			pResponse.set_content(m_strContent, m_strContentType);

		if (m_bSetRedirect)
			pResponse.set_redirect(m_strRedirect, m_iRedirectCode);

		if (m_iStatusCode >= 100 && m_iStatusCode < 600)
			pResponse.status = m_iStatusCode;

		if (m_bSetHeader)
			for (auto& [key, value] : m_pHeaders)
				pResponse.set_header(key, value);
	}
};

struct PreparedHttpResponse {
	HttpResponse m_pResponse;
	std::atomic<size_t> m_iUses = 0;
	std::string m_strPath = "";
	std::string m_strMethod = "";
	std::string m_strBody = "";
	unordered_map<std::string, std::string> m_pRequiredHeaders;

	inline bool ShouldRespond(const httplib::Request& pRequest)
	{
		if (m_iUses.load() == 0)
			return false;

		if (pRequest.method != m_strMethod)
			return false;

		if (pRequest.path != m_strPath)
			return false;

		if (pRequest.body != m_strBody)
			return false;

		if (m_pRequiredHeaders.size() > 0)
		{
			for (auto& [key, val] : m_pRequiredHeaders)
			{
				auto it = pRequest.headers.find(key);
				if (it == pRequest.headers.end())
					return false;

				if (it->second != val)
					return false;
			}
		}

		m_iUses.fetch_sub(1);
		return true;
	}

	inline void DoResponse(httplib::Response& pResponse)
	{
		m_pResponse.DoResponse(pResponse);
	}
};

struct HttpRequest {
	~HttpRequest();
	void MarkHandled();

	std::atomic<bool> m_bHandled = false;
	std::atomic<bool> m_bDelete{false};
	std::atomic<bool> m_bDisconnected{false};
	bool m_bDispatched = false, m_bWhitelist = false;
	std::mutex m_pReadyMutex;
	std::condition_variable m_pReady;
	std::shared_ptr<HttpStream> m_pStream;
	HttpServer* m_pOwner = nullptr;
	int m_iFunction = -1;
	std::string m_strPath;
	std::string m_strAddress;
	HttpResponse m_pResponseData;
	httplib::Response m_pResponse;
	httplib::Request m_pRequest;
	int m_pClientUserID = -1;
	GarrysMod::Lua::ILuaInterface* m_pLua = nullptr;
};

#undef isspace // unfuck 64x
namespace stringstuff
{
	static inline void ltrim(std::string &s) {
		s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch) {
			return !std::isspace(ch);
		}));
	}

	static inline void rtrim(std::string &s) {
		s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char ch) {
			return !std::isspace(ch);
		}).base(), s.end());
	}

	static inline void trim(std::string &s) {
		rtrim(s);
		ltrim(s);
	}
}

class HttpServer;
static unordered_set<HttpServer*> g_pHttpServers;
static std::shared_mutex g_pHttpServersMutex;
class HttpServer
{
public:
	HttpServer(GarrysMod::Lua::ILuaInterface* pLua) {
		m_pLua = pLua;

		SetName("NONAME");

		std::unique_lock<std::shared_mutex> lock(g_pHttpServersMutex);
		g_pHttpServers.insert(this);
	}

	~HttpServer()
	{
		if (!ThreadInMainThread())
		{
			Warning(PROJECT_NAME " - httpserver: Something deleted a HttpServer from another thread!\n"); // Spooky leaking references.
			return;
		}

		Stop();
		ClearStreams();

		if (m_pLua)
		{
			for (auto& ref : m_pHandlerReferences)
				Util::ReferenceFree(m_pLua, ref, "HttpServer::~HttpServer - Handler references");
		}

		m_pHandlerReferences.clear();

		// Should we lock the mutex?
		// Naaa it should be fine as the httpserver should NOT be running anymore.
		for (auto& [_, vec] : m_pPreparedResponses)
		{
			for (auto& pPreparedResponse : vec)
			{
				delete pPreparedResponse;
			}
		}
		m_pPreparedResponses.clear();

		std::unique_lock<std::shared_mutex> lock(g_pHttpServersMutex);
		g_pHttpServers.erase(this);
	}

	bool Start(const char* address, unsigned short port);
	void Stop();
	void Think();

	static SIMPLETHREAD_RETURNVALUE Server(void* params)
	{
		// ThreadSetDebugName(ThreadGetCurrentId(), PROJECT_NAME " - HttpServer::Server");

		HttpServer* pServer = (HttpServer*)params;
		pServer->GetServer().listen_after_bind();

		return 0;
	}

	void Get(const char* path, int func, bool ipWhitelist)
	{
		m_pServer.Get(path, CreateHandler(path, func, ipWhitelist));
	}

	void Post(const char* path, int func, bool ipWhitelist)
	{
		m_pServer.Post(path, CreateHandler(path, func, ipWhitelist));
	}

	void Put(const char* path, int func, bool ipWhitelist)
	{
		m_pServer.Put(path, CreateHandler(path, func, ipWhitelist));
	}

	void Patch(const char* path, int func, bool ipWhitelist)
	{
		m_pServer.Patch(path, CreateHandler(path, func, ipWhitelist));
	}

	void Delete(const char* path, int func, bool ipWhitelist)
	{
		m_pServer.Delete(path, CreateHandler(path, func, ipWhitelist));
	}

	void Options(const char* path, int func, bool ipWhitelist)
	{
		m_pServer.Options(path, CreateHandler(path, func, ipWhitelist));
	}

	httplib::Server::Handler CreateHandler(const char* path, int func, bool ipWhitelist);

	void AddPreparedResponse(int userID, PreparedHttpResponse* pResponse)
	{
		m_pPreparedResponsesMutex.lock();
		auto it = m_pPreparedResponses.find(userID);
		if (it == m_pPreparedResponses.end())
		{
			std::vector<PreparedHttpResponse*> pResponses;
			pResponses.push_back(pResponse);

			m_pPreparedResponses[userID] = pResponses;
			m_pPreparedResponsesMutex.unlock();
			return;
		}

		it->second.push_back(pResponse);
		m_pPreparedResponsesMutex.unlock();
	}

public:
	httplib::Server& GetServer() { return m_pServer; };
	std::string& GetAddress() { return m_strAddress; };
	unsigned short GetPort() { return m_iPort; };
	void SetThreadSleep(unsigned int threadSleep) { m_iThreadSleep = threadSleep; };
	const char* GetName() { return m_strName; };
	void SetName(const char* strName)
	{
		if (!strName)
		{
			SetName("NONAME");
			return;
		}

		V_strncpy(m_strName, strName, sizeof(m_strName));
	};

	// This function may be called by non-main threads!
	void ClearDisconnectedClient(int userID)
	{
		std::unique_lock<std::shared_mutex> lock(m_pPreparedResponsesMutex);
		auto it = m_pPreparedResponses.find(userID);
		if (it == m_pPreparedResponses.end())
			return;

		for (auto& pPreparedResponse : it->second)
			delete pPreparedResponse;

		m_pPreparedResponses.erase(it);
	}

	inline std::string GetAddressFromRequest(httplib::Request pRequest)
	{
		auto it = m_pAllowedProxies.find(pRequest.remote_addr);
		if (it != m_pAllowedProxies.end())
		{
			std::string realIP = pRequest.get_header_value(it->second.pHeaderName);
			if (!realIP.empty())
			{
				if (it->second.bUnshitAddress)
				{
					size_t pos = realIP.find_last_of(',');
					if (pos == std::string::npos) // It'll be fine... I think.
						return realIP;

					std::string ip = realIP.substr(pos + 1);
					stringstuff::trim(ip);
					return ip;
				}

				return realIP;
			}
		}

		return pRequest.remote_addr;
	}

	void AddProxy(std::string strProxyAddress, std::string strHeaderName, bool bUnshitAddress = false)
	{
		auto it = m_pAllowedProxies.find(strProxyAddress);
		if (it != m_pAllowedProxies.end())
		{
			it->second.pHeaderName = strHeaderName;
			it->second.bUnshitAddress = bUnshitAddress;
			return;
		}

		m_pAllowedProxies.emplace(
			std::move(strProxyAddress), 
			ProxyEntry{std::move(strHeaderName), bUnshitAddress}
		);
	};

	struct ProxyEntry
	{
		std::string pHeaderName = "";
		bool bUnshitAddress = false; // If true, it will use the second ip provided (if there is one) in the given header because proxies love to be shit.
	};

	void HandleRequests();
	void DrainStreams();
	void ClearStreams();
	bool IsDispatching() const { return m_bInUpdate; }
	void TrackStream(const std::shared_ptr<HttpStream>& stream) { m_pStreams.push_back(stream); }

private:
	unsigned short m_iPort = 0;
	bool m_bInUpdate = false;

	// 3 bytes free here.
	unsigned int m_iThreadSleep = 5; // Compatibility setting; deferred workers now wake through a condition variable.
	std::string m_strAddress = "";

	std::shared_mutex m_pRequestMutex;
	std::vector<std::shared_ptr<HttpRequest>> m_pRequests;
	std::vector<std::shared_ptr<HttpStream>> m_pStreams;
	std::atomic<bool> m_bStopping{true};

	std::vector<int> m_pHandlerReferences; // Contains the Lua references to the handler functions.
	unordered_map<std::string, ProxyEntry> m_pAllowedProxies;
	httplib::Server m_pServer;
	char m_strName[64] = {0};
	std::thread m_pServerThread;

	// userID - Response pairs.
	unordered_map<int, std::vector<PreparedHttpResponse*>> m_pPreparedResponses;
	std::shared_mutex m_pPreparedResponsesMutex;
	std::atomic<bool> m_bHandledPreparedResponse = false; // Set to true if any prepared response needs to be deleted

	GarrysMod::Lua::ILuaInterface* m_pLua = nullptr;
	HttpRequest* m_pLastHandledRequest = nullptr; // For error handling+
};

class LuaHttpServerModuleData : public Lua::ModuleData
{
public:
	bool shuttingDown = false;
	int nProtectedCallRef = -1;
	unordered_set<HttpServer*> pServers;
};

LUA_GetModuleData(LuaHttpServerModuleData, g_pHttpServerModule, HttpServer)

PushReferenced_LuaClass(HttpResponse)
Get_LuaClass(HttpResponse, "HttpResponse")

PushReferenced_LuaClass(HttpRequest)
Get_LuaClass(HttpRequest, "HttpRequest")

PushReferenced_LuaClass(HttpStream)
Get_LuaClass(HttpStream, "HttpStream")
Default__index(HttpStream);

static std::string StreamString(GarrysMod::Lua::ILuaInterface* lua, int index) {
    lua->CheckType(index, GarrysMod::Lua::Type::String);
    unsigned int length = 0;
    const char* text = lua->GetString(index, &length);
    return std::string(text, length);
}
static std::string StreamStringOpt(GarrysMod::Lua::ILuaInterface* lua, int index, const char* fallback) {
    return index > lua->Top() || lua->IsType(index, GarrysMod::Lua::Type::Nil) ? fallback : StreamString(lua, index);
}
LUA_FUNCTION_STATIC(HttpStream_IsOpen) {
    auto* stream = Get_HttpStream(LUA, 1, false);
    LUA->PushBool(stream && stream->state->IsOpen()); return 1;
}
LUA_FUNCTION_STATIC(HttpStream_GetBufferedBytes) {
    auto* stream = Get_HttpStream(LUA, 1, false);
    LUA->PushNumber(stream ? stream->state->Buffered() : 0); return 1;
}
LUA_FUNCTION_STATIC(HttpStream_WriteEvent) {
    auto* stream = Get_HttpStream(LUA, 1, false);
    std::string failure = "closed";
    const std::string data = StreamString(LUA, 2);
    const std::string event = StreamStringOpt(LUA, 3, "message");
    const std::string id = StreamStringOpt(LUA, 4, "");
    const bool ok = stream && stream->state->Event(data, event, id, failure);
    LUA->PushBool(ok); if (ok) LUA->PushNil(); else LUA->PushString(failure.c_str()); return 2;
}
LUA_FUNCTION_STATIC(HttpStream_WriteComment) {
    auto* stream = Get_HttpStream(LUA, 1, false);
    std::string failure = "closed";
    const auto text = StreamString(LUA, 2);
    const bool ok = stream && stream->state->Comment(text, failure);
    LUA->PushBool(ok); if (ok) LUA->PushNil(); else LUA->PushString(failure.c_str()); return 2;
}
LUA_FUNCTION_STATIC(HttpStream_Close) {
    auto* stream = Get_HttpStream(LUA, 1, false);
    if (stream) stream->state->Close(); return 0;
}
LUA_FUNCTION_STATIC(HttpStream_Abort) {
    auto* stream = Get_HttpStream(LUA, 1, false);
    if (stream) stream->state->Abort(LUA->CheckStringOpt(2, "aborted")); return 0;
}
LUA_FUNCTION_STATIC(HttpStream_OnClose) {
    auto* stream = Get_HttpStream(LUA, 1, false);
    LUA->CheckType(2, GarrysMod::Lua::Type::Function);
    if (!stream || stream->notified) { LUA->PushBool(false); return 1; }
    if (stream->onClose != -1) Util::ReferenceFree(LUA, stream->onClose, "HttpStream replaced OnClose");
    LUA->Push(2); stream->onClose = Util::ReferenceCreate(LUA, "HttpStream OnClose");
    LUA->PushBool(true); return 1;
}

LUA_FUNCTION_STATIC(HttpRequest_StartSSE) {
    auto* request = Get_HttpRequest(LUA, 1, true);
    size_t maxBuffered = 256 * 1024, maxEvent = 64 * 1024;
    double heartbeat = 15;
    if (LUA->Top() >= 2 && !LUA->IsType(2, GarrysMod::Lua::Type::Nil)) {
        LUA->CheckType(2, GarrysMod::Lua::Type::Table);
        auto number = [&](const char* key, double fallback, double low, double high) {
            LUA->GetField(2, key);
            double value = fallback;
            if (!LUA->IsType(-1, GarrysMod::Lua::Type::Nil)) { LUA->CheckType(-1, GarrysMod::Lua::Type::Number); value = LUA->GetNumber(-1); }
            LUA->Pop(1);
            if (!std::isfinite(value) || value < low || value > high || std::floor(value) != value) LUA->ThrowError("Invalid SSE option.");
            return value;
        };
        maxBuffered = static_cast<size_t>(number("max_buffered_bytes", maxBuffered, 65536, 8388608));
        maxEvent = static_cast<size_t>(number("max_event_bytes", maxEvent, 1024, maxBuffered));
        heartbeat = number("heartbeat_seconds", heartbeat, 1, 60);
    }
    auto stream = std::make_shared<HttpStream>();
    stream->lua = LUA;
    stream->state = std::make_shared<HolyHTTP::Stream>(maxBuffered, maxEvent, std::chrono::milliseconds(static_cast<int>(heartbeat * 1000)));
    request->m_pOwner->TrackStream(stream);
    request->m_pStream = stream;
    request->MarkHandled(); // Release the worker and invalidate the consumed response interface.
    Push_HttpStream(LUA, stream.get()); return 1;
}

void HttpServer::DrainStreams() {
    auto streams = m_pStreams;
    for (auto& stream : streams) {
        if (!stream->state->IsFinished() || stream->notified) continue;
        stream->notified = true;
        const int ref = stream->onClose; stream->onClose = -1;
        if (ref != -1) {
            Lua::ReferencePush(m_pLua, ref); m_pLua->PushString(stream->state->Reason().c_str());
            m_pLua->CallFunctionProtected(1, 0, true);
            Util::ReferenceFree(m_pLua, ref, "HttpStream terminal OnClose");
        }
        Delete_HttpStream(m_pLua, stream.get());
        m_pStreams.erase(std::remove(m_pStreams.begin(), m_pStreams.end(), stream), m_pStreams.end());
    }
}
void HttpServer::ClearStreams() {
    for (auto& stream : m_pStreams) {
        stream->state->Abort("shutdown");
        if (stream->onClose != -1) Util::ReferenceFree(m_pLua, stream->onClose, "HttpStream owner shutdown");
        Delete_HttpStream(m_pLua, stream.get());
    }
    m_pStreams.clear();
}

HttpRequest::~HttpRequest() = default; // Native-only destruction; a worker may own the last shared reference.

void HttpRequest::MarkHandled()
{
	{ std::lock_guard<std::mutex> lock(m_pReadyMutex); m_bHandled.store(true); }
	if (m_bDisconnected.load() && m_pStream) m_pStream->state->Finish("disconnected");
	m_pReady.notify_all();
	Delete_HttpRequest(m_pLua, this);
	// NOTE: We don't delete the HttpResponse as it will NEVER be pushed to Lua for a HttpRequest
}

LUA_FUNCTION_STATIC(HttpResponse__tostring)
{
	HttpResponse* pData = Get_HttpResponse(LUA, 1, false);
	if (!pData)
		LUA->PushString("HttpResponse [NULL]");
	else
		LUA->PushString("HttpResponse");
	return 1;
}

Default__index(HttpResponse);
Default__newindex(HttpResponse);
Default__GetTable(HttpResponse);
Default__IsValid(HttpResponse);

LUA_JIT_WRAPPED_3(HttpResponse_SetContent,
	LuaUserData*, pUD, Get_HttpResponse_Data(LUA, 1, true),
	const char*, pContent, LUA->CheckString(2),
	const char*, pContentType, LUA->CheckStringOpt(3, "text/plain")
)
{
	HttpResponse* pData = (HttpResponse*)pUD->GetData();
	if (!pData)
		return;

	pData->m_bSetContent = true;
	pData->m_strContent = pContent;
	pData->m_strContentType = pContentType ? pContentType : "text/plain";
}

LUA_JIT_WRAPPED_3(HttpResponse_SetRedirect,
	LuaUserData*, pUD, Get_HttpResponse_Data(LUA, 1, true),
	const char*, pRedirect, LUA->CheckString(2),
	int, iRedirectCode, (int)LUA->CheckNumberOpt(3, 302)
)
{
	HttpResponse* pData = (HttpResponse*)pUD->GetData();
	if (!pData)
		return;

	pData->m_bSetRedirect = true;
	pData->m_strRedirect = pRedirect;
	pData->m_iRedirectCode = iRedirectCode;
}

LUA_JIT_WRAPPED_3(HttpResponse_SetHeader,
	LuaUserData*, pUD, Get_HttpResponse_Data(LUA, 1, true),
	const char*, pHeader, LUA->CheckString(2),
	const char*, pHeaderValue, LUA->CheckString(3)
)
{
	HttpResponse* pData = (HttpResponse*)pUD->GetData();
	if (!pData)
		return;

	pData->m_bSetHeader = true;
	pData->m_pHeaders[pHeader] = pHeaderValue;
}

LUA_JIT_WRAPPED_2(HttpResponse_SetStatusCode,
	LuaUserData*, pUD, Get_HttpResponse_Data(LUA, 1, true),
	int, iStatusCode, (int)LUA->CheckNumber(2)
)
{
	HttpResponse* pData = (HttpResponse*)pUD->GetData();
	if (!pData)
		return;

	pData->m_iStatusCode = iStatusCode;
}

LUA_FUNCTION_STATIC(HttpRequest__tostring)
{
	HttpRequest* pData = Get_HttpRequest(LUA, 1, false);
	if (!pData)
		LUA->PushString("HttpRequest [NULL]");
	else
		LUA->PushString("HttpRequest");
	return 1;
}

Default__gc(HttpRequest, 
	if (pStoredData)
	{
		((HttpRequest*)pStoredData)->MarkHandled();
	}
);
Default__index(HttpRequest);
Default__newindex(HttpRequest);
Default__GetTable(HttpRequest);
Default__IsValid(HttpRequest);

LUA_FUNCTION_STATIC(HttpRequest_HasHeader)
{
	HttpRequest* pData = Get_HttpRequest(LUA, 1, true);

	LUA->PushBool(pData->m_pRequest.has_header(LUA->CheckString(2)));
	return 1;
}

LUA_FUNCTION_STATIC(HttpRequest_HasParam)
{
	HttpRequest* pData = Get_HttpRequest(LUA, 1, true);

	LUA->PushBool(pData->m_pRequest.has_param(LUA->CheckString(2)));
	return 1;
}

LUA_FUNCTION_STATIC(HttpRequest_GetHeader)
{
	HttpRequest* pData = Get_HttpRequest(LUA, 1, true);
	const char* header = LUA->CheckString(2);

	LUA->PushString(pData->m_pRequest.get_header_value(header).c_str());
	return 1;
}

LUA_FUNCTION_STATIC(HttpRequest_GetParam)
{
	HttpRequest* pData = Get_HttpRequest(LUA, 1, true);
	const char* param = LUA->CheckString(2);

	LUA->PushString(pData->m_pRequest.get_param_value(param).c_str());
	return 1;
}

LUA_FUNCTION_STATIC(HttpRequest_GetPathParam)
{
	HttpRequest* pData = Get_HttpRequest(LUA, 1, true);
	const char* param = LUA->CheckString(2);

	auto it = pData->m_pRequest.path_params.find(param);
	if (it != pData->m_pRequest.path_params.end())
	{
		LUA->PushString(it->second.c_str());
		return 1;
	}

	LUA->PushNil();
	return 1;
}

LUA_FUNCTION_STATIC(HttpRequest_GetPath)
{
	HttpRequest* pData = Get_HttpRequest(LUA, 1, true);

	LUA->PushString(pData->m_pRequest.path.c_str());
	return 1;
}

LUA_FUNCTION_STATIC(HttpRequest_GetBody)
{
	HttpRequest* pData = Get_HttpRequest(LUA, 1, true);

	LUA->PushString(pData->m_pRequest.body.data(), static_cast<unsigned int>(pData->m_pRequest.body.size()));
	return 1;
}

LUA_FUNCTION_STATIC(HttpRequest_GetRemoteAddr)
{
	HttpRequest* pData = Get_HttpRequest(LUA, 1, true);

	LUA->PushString(pData->m_strAddress.c_str());
	return 1;
}

LUA_FUNCTION_STATIC(HttpRequest_GetRemotePort)
{
	HttpRequest* pData = Get_HttpRequest(LUA, 1, true);

	LUA->PushNumber(pData->m_pRequest.remote_port);
	return 1;
}

LUA_FUNCTION_STATIC(HttpRequest_GetLocalAddr)
{
	HttpRequest* pData = Get_HttpRequest(LUA, 1, true);

	LUA->PushString(pData->m_pRequest.local_addr.c_str());
	return 1;
}

LUA_FUNCTION_STATIC(HttpRequest_GetLocalPort)
{
	HttpRequest* pData = Get_HttpRequest(LUA, 1, true);

	LUA->PushNumber(pData->m_pRequest.local_port);
	return 1;
}

LUA_FUNCTION_STATIC(HttpRequest_GetMethod)
{
	HttpRequest* pData = Get_HttpRequest(LUA, 1, true);

	LUA->PushString(pData->m_pRequest.method.c_str());
	return 1;
}

LUA_FUNCTION_STATIC(HttpRequest_GetAuthorizationCount)
{
	HttpRequest* pData = Get_HttpRequest(LUA, 1, true);

	LUA->PushNumber(pData->m_pRequest.authorization_count_);
	return 1;
}

LUA_FUNCTION_STATIC(HttpRequest_GetContentLength)
{
	HttpRequest* pData = Get_HttpRequest(LUA, 1, true);

	LUA->PushNumber(pData->m_pRequest.content_length_);
	return 1;
}

LUA_FUNCTION_STATIC(HttpRequest_GetClient)
{
	HttpRequest* pData = Get_HttpRequest(LUA, 1, true);

#if MODULE_EXISTS_GAMESERVER
	Push_CBaseClient(LUA, Util::GetClientByUserID(pData->m_pClientUserID));
#else
	MISSING_MODULE_ERROR(LUA, gameserver);
#endif

	return 1;
}

LUA_FUNCTION_STATIC(HttpRequest_GetPlayer)
{
	HttpRequest* pData = Get_HttpRequest(LUA, 1, true);
	CBaseClient* pClient = Util::GetClientByUserID(pData->m_pClientUserID);
	CBasePlayer* pPlayer = pClient ? Util::GetPlayerByClient(pClient) : nullptr;

	if (pPlayer)
		Util::Push_Entity(LUA, (CBaseEntity*)pPlayer);
	else
		LUA->PushNil();

	return 1;
}

LUA_FUNCTION_STATIC(HttpRequest_MarkHandled)
{
	HttpRequest* pData = Get_HttpRequest(LUA, 1, true);
	pData->MarkHandled();

	return 0;
}

LUA_FUNCTION_STATIC(HttpRequest_SetContent)
{
	HttpRequest* pData = Get_HttpRequest(LUA, 1, true);
	pData->m_pResponseData.m_bSetContent = true;
	LUA->CheckType(2, GarrysMod::Lua::Type::String);
	unsigned int length = 0; const char* content = LUA->GetString(2, &length);
	pData->m_pResponseData.m_strContent.assign(content, length);
	pData->m_pResponseData.m_strContentType = LUA->CheckStringOpt(3, "text/plain");

	return 0;
}

LUA_FUNCTION_STATIC(HttpRequest_SetRedirect)
{
	HttpRequest* pData = Get_HttpRequest(LUA, 1, true);
	pData->m_pResponseData.m_bSetRedirect = true;
	pData->m_pResponseData.m_strRedirect = LUA->CheckString(2);
	pData->m_pResponseData.m_iRedirectCode = (int)LUA->CheckNumberOpt(3, 302);

	return 0;
}

LUA_FUNCTION_STATIC(HttpRequest_SetHeader)
{
	HttpRequest* pData = Get_HttpRequest(LUA, 1, true);
	pData->m_pResponseData.m_bSetHeader = true;
	pData->m_pResponseData.m_pHeaders[LUA->CheckString(2)] = LUA->CheckString(3);

	return 0;
}

LUA_FUNCTION_STATIC(HttpRequest_SetStatusCode)
{
	HttpRequest* pData = Get_HttpRequest(LUA, 1, true);
	pData->m_pResponseData.m_iStatusCode = (int)LUA->CheckNumber(2);

	return 0;
}

void CallFunc(GarrysMod::Lua::ILuaInterface* pLua, int callbackFunction, HttpRequest* request, HttpResponse* response)
{
	Lua::ReferencePush(pLua, callbackFunction);

	if (g_pHttpServerModule.InDebug())
		Msg(PROJECT_NAME " - httpserver: pushed handler function %i with type %i\n", callbackFunction, pLua->GetType(-1));

	Push_HttpRequest(pLua, request);

	Util::func_lua_call(pLua->GetState(), 1, 1);
	if (!request->m_bHandled.load() && (!pLua->IsType(-1, GarrysMod::Lua::Type::Bool) || pLua->GetBool(-1)))
		request->MarkHandled();

	pLua->Pop(1);
}

bool HttpServer::Start(const char* address, unsigned short port)
{
    Stop();
    m_strAddress = address; m_iPort = port;
    if (!m_pServer.bind_to_port(address, port)) return false;
    m_bStopping.store(false);
    try { m_pServerThread = std::thread([this] { m_pServer.listen_after_bind(); }); }
    catch (...) { m_bStopping.store(true); m_pServer.stop(); return false; }
    return true;
}

void HttpServer::Stop()
{
    m_bStopping.store(true);
    // Wake every deferred handler before joining the listener's worker pool.
    std::vector<std::shared_ptr<HttpRequest>> requests;
    { std::lock_guard<std::shared_mutex> lock(m_pRequestMutex); requests = m_pRequests; }
    for (auto& request : requests) {
        std::lock_guard<std::mutex> lock(request->m_pReadyMutex);
        if (!request->m_bHandled.load()) {
            request->m_pResponseData.m_iStatusCode = 503;
            request->m_bHandled.store(true);
        }
        request->m_pReady.notify_all();
    }
    // An addon may already have queued its final result and called Close(). Give
    // those responses a bounded, shared grace period; open/slow streams abort.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
    for (auto& stream : m_pStreams) {
        if (!stream->state->IsOpen()) stream->state->WaitUntil(deadline);
        stream->state->Abort("shutdown");
    }
    m_pServer.stop();
    if (m_pServerThread.joinable()) m_pServerThread.join();
    for (auto& stream : m_pStreams) stream->state->Finish("shutdown");
    for (auto& request : requests) Delete_HttpRequest(m_pLua, request.get());
    { std::lock_guard<std::shared_mutex> lock(m_pRequestMutex); m_pRequests.clear(); }
    // Deliver terminal callbacks exactly once, without dispatching routes or
    // recursively entering Think(). References are still owned by this state.
    const bool wasUpdating = m_bInUpdate;
    m_bInUpdate = true;
    DrainStreams();
    m_bInUpdate = wasUpdating;
}

PushReferenced_LuaClass(HttpServer)
Get_LuaClass(HttpServer, "HttpServer")

static int ResolveClient(const std::string& remote)
{
    // Called only by the owning main-thread Lua dispatch, never a socket worker.
    for (auto* client : Util::GetClients()) {
        if (!client->IsConnected() || !client->GetNetChannel()) continue;
        std::string address = client->GetNetChannel()->GetRemoteAddress().ToString();
        address = address.substr(0, address.find(':'));
        if (address == remote || (remote == "127.0.0.1" && address == "loopback")) return client->GetUserID();
    }
    return -1;
}

void HttpServer::HandleRequests()
{
    std::vector<std::shared_ptr<HttpRequest>> requests;
    {
        std::lock_guard<std::shared_mutex> lock(m_pRequestMutex);
        for (auto it = m_pRequests.begin(); it != m_pRequests.end();) {
            if ((*it)->m_bDelete.load()) {
                if ((*it)->m_bDisconnected.load() && (*it)->m_pStream) (*it)->m_pStream->state->Finish("disconnected");
                Delete_HttpRequest(m_pLua, it->get()); it = m_pRequests.erase(it);
            }
            else ++it;
        }
        requests = m_pRequests;
    }
    for (auto& entry : requests) {
        auto* request = entry.get();
        if (m_bStopping.load() || request->m_bDispatched || request->m_bHandled.load()) continue;
        request->m_bDispatched = true;
        request->m_strAddress = GetAddressFromRequest(request->m_pRequest);
        request->m_pClientUserID = ResolveClient(request->m_strAddress);
        if (request->m_bWhitelist && request->m_pClientUserID == -1) {
            request->m_pResponseData.m_iStatusCode = 403; request->MarkHandled(); continue;
        }
        bool prepared = false;
        {
            std::unique_lock<std::shared_mutex> lock(m_pPreparedResponsesMutex);
            auto found = m_pPreparedResponses.find(request->m_pClientUserID);
            if (found != m_pPreparedResponses.end()) for (auto& response : found->second) {
                if (!response->ShouldRespond(request->m_pRequest)) continue;
                request->m_pResponseData = response->m_pResponse;
                m_bHandledPreparedResponse.store(true); prepared = true; break;
            }
        }
        if (prepared) { request->MarkHandled(); continue; }
        m_pLastHandledRequest = request;
        CallFunc(m_pLua, request->m_iFunction, request, &request->m_pResponseData);
        m_pLastHandledRequest = nullptr;
    }
}

LUA_FUNCTION_STATIC(HttpServer_ProtectedCalls)
{
    Get_HttpServer(LUA, 1, true)->HandleRequests(); return 0;
}

void HttpServer::Think()
{
    if (m_bInUpdate || !ThreadInMainThread()) return;
    m_bInUpdate = true;
    auto* data = GetHttpServerLuaData(m_pLua);
    if (data) {
        Lua::ReferencePush(m_pLua, data->nProtectedCallRef);
        Push_HttpServer(m_pLua, this);
        if (!m_pLua->CallFunctionProtected(1, 0, true) && m_pLastHandledRequest) {
            if (m_pLastHandledRequest->m_pStream) m_pLastHandledRequest->m_pStream->state->Abort("error");
            else { m_pLastHandledRequest->m_pResponseData.m_iStatusCode = 500; m_pLastHandledRequest->MarkHandled(); }
        }
        m_pLastHandledRequest = nullptr;
    }
    DrainStreams();
    if (m_bHandledPreparedResponse.exchange(false)) {
        std::unique_lock<std::shared_mutex> lock(m_pPreparedResponsesMutex);
        for (auto& pair : m_pPreparedResponses) {
            auto& responses = pair.second;
            for (auto it = responses.begin(); it != responses.end();) {
                if (!(*it)->m_iUses.load()) { delete *it; it = responses.erase(it); } else ++it;
            }
        }
    }
    m_bInUpdate = false;
}

httplib::Server::Handler HttpServer::CreateHandler(const char* path, int func, bool ipWhitelist)
{
    m_pHandlerReferences.push_back(func);
    const std::string route(path);
    return [this, route, func, ipWhitelist](const httplib::Request& req, httplib::Response& res) {
        auto request = std::make_shared<HttpRequest>();
        request->m_strPath = route; request->m_pRequest = req; request->m_iFunction = func;
        request->m_pLua = m_pLua; request->m_bWhitelist = ipWhitelist; request->m_pOwner = this;
        {
            std::lock_guard<std::shared_mutex> lock(m_pRequestMutex);
            if (m_bStopping.load()) { res.status = 503; return; }
            m_pRequests.push_back(request);
        }
        {
            std::unique_lock<std::mutex> lock(request->m_pReadyMutex);
            while (!request->m_bHandled.load() && !m_bStopping.load()) {
                request->m_pReady.wait_for(lock, std::chrono::milliseconds(250));
                if (req.is_connection_closed()) { request->m_bDisconnected.store(true); request->m_bDelete.store(true); return; }
            }
        }
        if (m_bStopping.load() && !request->m_pStream) res.status = 503;
        else {
            request->m_pResponseData.DoResponse(res);
            if (request->m_pStream) HolyHTTP::InstallSSE(res, request->m_pStream->state, req.is_connection_closed);
        }
        request->m_bDelete.store(true);
    };
}

LUA_FUNCTION_STATIC(HttpServer_Think)
{
	HttpServer* pServer = Get_HttpServer(LUA, 1, true);
	pServer->Think();

	return 0;
}

inline int CheckFunction(GarrysMod::Lua::ILuaInterface* LUA, int iStackPos)
{
	LUA->CheckType(iStackPos, GarrysMod::Lua::Type::Function);
	LUA->Push(iStackPos);
	return Util::ReferenceCreate(LUA, "CheckFunction - HttpServer blackbox");
}

inline bool CheckBool(GarrysMod::Lua::ILuaInterface* LUA, int iStackPos)
{
	LUA->CheckType(iStackPos, GarrysMod::Lua::Type::Bool);
	return LUA->GetBool(iStackPos);
}

LUA_FUNCTION_STATIC(HttpServer__tostring)
{
	HttpServer* pServer = Get_HttpServer(LUA, 1, false);
	if (!pServer)
	{
		LUA->PushString("HttpServer [NULL]");
		return 1;
	}

	char szBuf[96] = {};
	V_snprintf(szBuf, sizeof(szBuf), "HttpServer [%s:%s - %s]", 
		pServer->GetAddress().c_str(),
		std::to_string(pServer->GetPort()).c_str(),
		pServer->GetName()
	); 
	LUA->PushString(szBuf);
	return 1;
}

Default__index(HttpServer);
Default__newindex(HttpServer);
Default__GetTable(HttpServer);
Default__IsValid(HttpServer);

LUA_FUNCTION_STATIC(HttpServer_Get)
{
	HttpServer* pServer = Get_HttpServer(LUA, 1, true);
	const char* path = LUA->CheckString(2);
	int func = CheckFunction(LUA, 3);
	bool ipWhitelist = LUA->GetBool(4);

	pServer->Get(path, func, ipWhitelist);

	return 0;
}

LUA_FUNCTION_STATIC(HttpServer_Post)
{
	HttpServer* pServer = Get_HttpServer(LUA, 1, true);
	const char* path = LUA->CheckString(2);
	int func = CheckFunction(LUA, 3);
	bool ipWhitelist = LUA->GetBool(4);

	pServer->Post(path, func, ipWhitelist);

	return 0;
}

LUA_FUNCTION_STATIC(HttpServer_Put)
{
	HttpServer* pServer = Get_HttpServer(LUA, 1, true);
	const char* path = LUA->CheckString(2);
	int func = CheckFunction(LUA, 3);
	bool ipWhitelist = LUA->GetBool(4);

	pServer->Put(path, func, ipWhitelist);

	return 0;
}

LUA_FUNCTION_STATIC(HttpServer_Patch)
{
	HttpServer* pServer = Get_HttpServer(LUA, 1, true);
	const char* path = LUA->CheckString(2);
	int func = CheckFunction(LUA, 3);
	bool ipWhitelist = LUA->GetBool(4);

	pServer->Patch(path, func, ipWhitelist);

	return 0;
}

LUA_FUNCTION_STATIC(HttpServer_Delete)
{
	HttpServer* pServer = Get_HttpServer(LUA, 1, true);
	const char* path = LUA->CheckString(2);
	int func = CheckFunction(LUA, 3);
	bool ipWhitelist = LUA->GetBool(4);

	pServer->Delete(path, func, ipWhitelist);

	return 0;
}

LUA_FUNCTION_STATIC(HttpServer_Options)
{
	HttpServer* pServer = Get_HttpServer(LUA, 1, true);
	const char* path = LUA->CheckString(2);
	int func = CheckFunction(LUA, 3);
	bool ipWhitelist = LUA->GetBool(4);

	pServer->Options(path, func, ipWhitelist);

	return 0;
}

LUA_FUNCTION_STATIC(HttpServer_IsRunning)
{
	HttpServer* pServer = Get_HttpServer(LUA, 1, true);
	LUA->PushBool(pServer->GetServer().is_running());

	return 1;
}

LUA_FUNCTION_STATIC(HttpServer_SetTCPnodelay)
{
	HttpServer* pServer = Get_HttpServer(LUA, 1, true);
	pServer->GetServer().set_tcp_nodelay(CheckBool(LUA, 2));

	return 0;
}

LUA_FUNCTION_STATIC(HttpServer_SetReadTimeout)
{
	HttpServer* pServer = Get_HttpServer(LUA, 1, true);
	pServer->GetServer().set_read_timeout((time_t)LUA->CheckNumber(2), (time_t)LUA->CheckNumber(3));

	return 0;
}

LUA_FUNCTION_STATIC(HttpServer_SetWriteTimeout)
{
	HttpServer* pServer = Get_HttpServer(LUA, 1, true);
	pServer->GetServer().set_write_timeout((time_t)LUA->CheckNumber(2), (time_t)LUA->CheckNumber(3));

	return 0;
}

LUA_FUNCTION_STATIC(HttpServer_SetPayloadMaxLength)
{
	HttpServer* pServer = Get_HttpServer(LUA, 1, true);
	pServer->GetServer().set_payload_max_length((size_t)LUA->CheckNumber(2));

	return 0;
}

LUA_FUNCTION_STATIC(HttpServer_SetKeepAliveTimeout)
{
	HttpServer* pServer = Get_HttpServer(LUA, 1, true);
	pServer->GetServer().set_keep_alive_timeout((time_t)LUA->CheckNumber(2));

	return 0;
}

LUA_FUNCTION_STATIC(HttpServer_SetKeepAliveMaxCount)
{
	HttpServer* pServer = Get_HttpServer(LUA, 1, true);
	pServer->GetServer().set_keep_alive_max_count((size_t)LUA->CheckNumber(2));

	return 0;
}

LUA_FUNCTION_STATIC(HttpServer_SetMountPoint)
{
	HttpServer* pServer = Get_HttpServer(LUA, 1, true);
	pServer->GetServer().set_mount_point(LUA->CheckString(2), LUA->CheckString(3));

	return 0;
}

LUA_FUNCTION_STATIC(HttpServer_RemoveMountPoint)
{
	HttpServer* pServer = Get_HttpServer(LUA, 1, true);
	pServer->GetServer().remove_mount_point(LUA->CheckString(2));

	return 0;
}

LUA_FUNCTION_STATIC(HttpServer_Start)
{
	HttpServer* pServer = Get_HttpServer(LUA, 1, true);
	const char* address = LUA->CheckString(2);
	unsigned short port = (unsigned short)LUA->CheckNumber(3);

	if (!ThreadInMainThread() || pServer->IsDispatching() || GetHttpServerLuaData(LUA)->shuttingDown || g_bHTTPShuttingDown.load())
		LUA->ThrowError("HTTPServer Start must run on the main thread outside callbacks and shutdown.");
	if (!pServer->Start(address, port)) LUA->ThrowError("HTTPServer failed to bind the configured address/port.");

	return 0;
}

LUA_FUNCTION_STATIC(HttpServer_Stop)
{
	HttpServer* pServer = Get_HttpServer(LUA, 1, true);
	if (!ThreadInMainThread() || pServer->IsDispatching()) LUA->ThrowError("Cannot stop HTTPServer from a route/close callback or worker thread.");
	pServer->Stop();

	return 0;
}

LUA_FUNCTION_STATIC(HttpServer_SetThreadSleep)
{
	HttpServer* pServer = Get_HttpServer(LUA, 1, true);
	pServer->SetThreadSleep((unsigned int)LUA->CheckNumber(2));

	return 0;
}

LUA_FUNCTION_STATIC(HttpServer_GetPort)
{
	HttpServer* pServer = Get_HttpServer(LUA, 1, true);

	LUA->PushNumber(pServer->GetPort());
	return 1;
}

LUA_FUNCTION_STATIC(HttpServer_GetAddress)
{
	HttpServer* pServer = Get_HttpServer(LUA, 1, true);

	LUA->PushString(pServer->GetAddress().c_str());
	return 1;
}

LUA_FUNCTION_STATIC(HttpServer_GetName)
{
	HttpServer* pServer = Get_HttpServer(LUA, 1, true);

	LUA->PushString(pServer->GetName());
	return 1;
}

LUA_FUNCTION_STATIC(HttpServer_SetName)
{
	HttpServer* pServer = Get_HttpServer(LUA, 1, true);

	pServer->SetName(LUA->CheckString(2));
	return 0;
}

LUA_FUNCTION_STATIC(HttpServer_AddPreparedResponse)
{
	HttpServer* pServer = Get_HttpServer(LUA, 1, true);
	int userID = (int)LUA->CheckNumber(2);
	const char* pPath = LUA->CheckString(3);
	const char* pMethod = LUA->CheckString(4);
	LUA->CheckType(5, GarrysMod::Lua::Type::Table);
	LUA->CheckType(6, GarrysMod::Lua::Type::Function);
	int uses = (int)LUA->CheckNumberOpt(7, 1);

	PreparedHttpResponse* pResponse = new PreparedHttpResponse;
	pResponse->m_strPath = pPath;
	pResponse->m_strMethod = pMethod;
	pResponse->m_iUses = uses;

	LUA->Push(5);
	LUA->PushNil();
	while (LUA->Next(-2)) {
		LUA->Push(-2);

		const char* key = LUA->GetString(-1);
		const char* value = LUA->GetString(-2);
		pResponse->m_pRequiredHeaders[key] = value;

		LUA->Pop(2);
	}
	LUA->Pop(1);

	Push_HttpResponse(LUA, &pResponse->m_pResponse);
	LUA->Push(6);
	LUA->Push(-2);
	LUA->CallFunctionProtected(1, 0, true);
	Delete_HttpResponse(LUA, &pResponse->m_pResponse);
	pServer->AddPreparedResponse(userID, pResponse);

	return 0;
}

LUA_FUNCTION_STATIC(HttpServer_AddProxyAddress)
{
	HttpServer* pServer = Get_HttpServer(LUA, 1, true);

	pServer->AddProxy(LUA->CheckString(2), LUA->CheckString(3), LUA->GetBool(4));
	return 0;
}

LUA_FUNCTION_STATIC(HttpServer_SetThreads)
{
	HttpServer* pServer = Get_HttpServer(LUA, 1, true);
	const double workers = LUA->CheckNumber(2), queued = LUA->CheckNumber(3);
	if (!std::isfinite(workers) || workers < 1 || workers > 1024 || workers != std::floor(workers)
		|| !std::isfinite(queued) || queued < 0 || queued > 1048576 || queued != std::floor(queued))
		LUA->ThrowError("SetThreads expects worker count (1..1024) and maximum queued requests (0..1048576; 0 means unlimited).");
	size_t minThreads = (size_t)workers;
	size_t maxThreads = (size_t)queued;

	pServer->GetServer().new_task_queue = [minThreads, maxThreads] { return new httplib::ThreadPool(minThreads, maxThreads); };
	return 0;
}

LUA_FUNCTION_STATIC(httpserver_Create)
{
	if (!ThreadInMainThread()) LUA->ThrowError("HTTPServer must be created on the main thread.");
	LuaHttpServerModuleData* pLuaData = GetHttpServerLuaData(LUA);
	if (pLuaData->shuttingDown || g_bHTTPShuttingDown.load()) LUA->ThrowError("HTTPServer is shutting down.");
	HttpServer* pServer = new HttpServer(LUA);
	pLuaData->pServers.insert(pServer);

	Push_HttpServer(LUA, pServer);
	return 1;
}

LUA_FUNCTION_STATIC(httpserver_Destroy)
{
	HttpServer* pServer = Get_HttpServer(LUA, 1, true);

	if (!ThreadInMainThread() || pServer->IsDispatching() || GetHttpServerLuaData(LUA)->shuttingDown)
		LUA->ThrowError("Cannot destroy HTTPServer during dispatch, Lua shutdown, or from a worker thread.");
	pServer->Stop();

	GetHttpServerLuaData(LUA)->pServers.erase(pServer);
	Delete_HttpServer(LUA, pServer);
	delete pServer;

	return 0;
}

LUA_FUNCTION_STATIC(httpserver_GetAll)
{
	LuaHttpServerModuleData* pLuaData = GetHttpServerLuaData(LUA);
	LUA->PreCreateTable(pLuaData->pServers.size(), 0);
		int idx = 0;
		for (auto& server : pLuaData->pServers)
		{
			Push_HttpServer(LUA, server);
			Lua::RawSetI(LUA, -2, ++idx);
		}

	return 1;
}

LUA_FUNCTION_STATIC(httpserver_FindByName)
{
	std::string strName = LUA->CheckString(1);
	LuaHttpServerModuleData* pLuaData = GetHttpServerLuaData(LUA);
	for (auto& server : pLuaData->pServers)
	{
		if (server->GetName() == strName)
		{
			Push_HttpServer(LUA, server);
			return 1;
		}
	}

	LUA->PushNil();
	return 1;
}

void CHTTPServerModule::OnClientDisconnect(CBaseClient* pClient)
{
	// Verify: As stated above in the HttpServer class, should we add a mutex for this?
	int userID = pClient->GetUserID();
	for (auto& server : g_pHttpServers)
	{
		server->ClearDisconnectedClient(userID);
	}
}

static Detouring::Hook detour_CRConServer_CreateSocket;
bool hook_CRConServer_CreateSocket(void* _this)
{
	if (CommandLine()->FindParm("-norcon"))
		return false;

	return detour_CRConServer_CreateSocket.GetTrampoline<Symbols::CRConServer_CreateSocket>()(_this);
}

void CHTTPServerModule::InitDetour(bool bPreServer)
{
	if (bPreServer) {
		g_bHTTPShuttingDown.store(false);
		return;
	}

	SourceSDK::FactoryLoader engine_loader("engine");
	Detour::Create(
		&detour_CRConServer_CreateSocket, "CRConServer::CreateSocket",
		engine_loader.GetModule(), Symbols::CRConServer_CreateSocketSym,
		(void*)hook_CRConServer_CreateSocket, m_pID
	);
}

void CHTTPServerModule::LuaInit(GarrysMod::Lua::ILuaInterface* pLua, bool bServerInit)
{
	if (bServerInit)
		return;

	LuaHttpServerModuleData* pModuleData = new LuaHttpServerModuleData;
	Lua::GetLuaData(pLua)->SetModuleData(m_pID, pModuleData);
	pLua->PushCFunction(HttpServer_ProtectedCalls);
	pModuleData->nProtectedCallRef = Util::ReferenceCreate(pLua, "HttpServer - ProtectedCall");

	Lua::GetLuaData(pLua)->RegisterMetaTable(Lua::HttpStream, pLua->CreateMetaTable("HttpStream"));
		Util::AddFunc(pLua, HttpStream__index, "__index");
		Util::AddFunc(pLua, HttpStream_IsOpen, "IsOpen");
		Util::AddFunc(pLua, HttpStream_GetBufferedBytes, "GetBufferedBytes");
		Util::AddFunc(pLua, HttpStream_WriteEvent, "WriteEvent");
		Util::AddFunc(pLua, HttpStream_WriteComment, "WriteComment");
		Util::AddFunc(pLua, HttpStream_Close, "Close");
		Util::AddFunc(pLua, HttpStream_Abort, "Abort");
		Util::AddFunc(pLua, HttpStream_OnClose, "OnClose");
	pLua->Pop(1);

	Lua::GetLuaData(pLua)->RegisterMetaTable(Lua::HttpServer, pLua->CreateMetaTable("HttpServer"));
		Util::AddFunc(pLua, HttpServer__tostring, "__tostring");
		Util::AddFunc(pLua, HttpServer__index, "__index");
		Util::AddFunc(pLua, HttpServer__newindex, "__newindex");
		LUA_REGISTER_JIT(pLua, HttpServer_GetTable, "GetTable");
		LUA_REGISTER_JIT(pLua, HttpServer_IsValid, "IsValid");

		Util::AddFunc(pLua, HttpServer_Think, "Think");
		Util::AddFunc(pLua, HttpServer_Start, "Start");
		Util::AddFunc(pLua, HttpServer_Stop, "Stop");

		Util::AddFunc(pLua, HttpServer_IsRunning, "IsRunning");
		Util::AddFunc(pLua, HttpServer_SetTCPnodelay, "SetTCPnodelay");
		Util::AddFunc(pLua, HttpServer_SetReadTimeout, "SetReadTimeout");
		Util::AddFunc(pLua, HttpServer_SetWriteTimeout, "SetWriteTimeout");
		Util::AddFunc(pLua, HttpServer_SetPayloadMaxLength, "SetPayloadMaxLength");
		Util::AddFunc(pLua, HttpServer_SetKeepAliveTimeout, "SetKeepAliveTimeout");
		Util::AddFunc(pLua, HttpServer_SetKeepAliveMaxCount, "SetKeepAliveMaxCount");
		Util::AddFunc(pLua, HttpServer_SetThreadSleep, "SetThreadSleep");
		Util::AddFunc(pLua, HttpServer_SetThreads, "SetThreads");

		Util::AddFunc(pLua, HttpServer_SetMountPoint, "SetMountPoint");
		Util::AddFunc(pLua, HttpServer_RemoveMountPoint, "RemoveMountPoint");

		Util::AddFunc(pLua, HttpServer_Get, "Get");
		Util::AddFunc(pLua, HttpServer_Post, "Post");
		Util::AddFunc(pLua, HttpServer_Put, "Put");
		Util::AddFunc(pLua, HttpServer_Patch, "Patch");
		Util::AddFunc(pLua, HttpServer_Delete, "Delete");
		Util::AddFunc(pLua, HttpServer_Options, "Options");

		Util::AddFunc(pLua, HttpServer_GetPort, "GetPort");
		Util::AddFunc(pLua, HttpServer_GetAddress, "GetAddress");
		Util::AddFunc(pLua, HttpServer_GetName, "GetName");
		Util::AddFunc(pLua, HttpServer_SetName, "SetName");

		Util::AddFunc(pLua, HttpServer_AddPreparedResponse, "AddPreparedResponse");
		Util::AddFunc(pLua, HttpServer_AddProxyAddress, "AddProxyAddress");
	pLua->Pop(1);

	// This class purely exists just for the HttpServer:AddPreparedResponse callback!
	Lua::GetLuaData(pLua)->RegisterMetaTable(Lua::HttpResponse, pLua->CreateMetaTable("HttpResponse"));
		Util::AddFunc(pLua, HttpResponse__tostring, "__tostring");
		Util::AddFunc(pLua, HttpResponse__index, "__index");
		Util::AddFunc(pLua, HttpResponse__newindex, "__newindex");
		LUA_REGISTER_JIT(pLua, HttpResponse_GetTable, "GetTable");
		LUA_REGISTER_JIT(pLua, HttpResponse_IsValid, "IsValid");

		LUA_REGISTER_JIT(pLua, HttpResponse_SetContent, "SetContent");
		LUA_REGISTER_JIT(pLua, HttpResponse_SetHeader, "SetHeader");
		LUA_REGISTER_JIT(pLua, HttpResponse_SetRedirect, "SetRedirect");
		LUA_REGISTER_JIT(pLua, HttpResponse_SetStatusCode, "SetStatusCode");
	pLua->Pop(1);

	Lua::GetLuaData(pLua)->RegisterMetaTable(Lua::HttpRequest, pLua->CreateMetaTable("HttpRequest"));
		Util::AddFunc(pLua, HttpRequest__gc, "__gc");
		Util::AddFunc(pLua, HttpRequest__tostring, "__tostring");
		Util::AddFunc(pLua, HttpRequest__index, "__index");
		Util::AddFunc(pLua, HttpRequest__newindex, "__newindex");
		LUA_REGISTER_JIT(pLua, HttpRequest_GetTable, "GetTable");
		LUA_REGISTER_JIT(pLua, HttpRequest_IsValid, "IsValid");

		Util::AddFunc(pLua, HttpRequest_HasHeader, "HasHeader");
		Util::AddFunc(pLua, HttpRequest_HasParam, "HasParam");
		Util::AddFunc(pLua, HttpRequest_GetHeader, "GetHeader");
		Util::AddFunc(pLua, HttpRequest_GetParam, "GetParam");
		Util::AddFunc(pLua, HttpRequest_GetPathParam, "GetPathParam");
		Util::AddFunc(pLua, HttpRequest_GetPath, "GetPath");
		Util::AddFunc(pLua, HttpRequest_GetBody, "GetBody");
		Util::AddFunc(pLua, HttpRequest_GetRemoteAddr, "GetRemoteAddr");
		Util::AddFunc(pLua, HttpRequest_GetRemotePort, "GetRemotePort");
		Util::AddFunc(pLua, HttpRequest_GetLocalAddr, "GetLocalAddr");
		Util::AddFunc(pLua, HttpRequest_GetLocalPort, "GetLocalPort");
		Util::AddFunc(pLua, HttpRequest_GetMethod, "GetMethod");
		Util::AddFunc(pLua, HttpRequest_GetAuthorizationCount, "GetAuthorizationCount");
		Util::AddFunc(pLua, HttpRequest_GetContentLength, "GetContentLength");
		Util::AddFunc(pLua, HttpRequest_MarkHandled, "MarkHandled");
		Util::AddFunc(pLua, HttpRequest_StartSSE, "StartSSE");

		Util::AddFunc(pLua, HttpRequest_GetClient, "GetClient");
		Util::AddFunc(pLua, HttpRequest_GetPlayer, "GetPlayer");

		// Response functions - this decision was made to reduce the Lua GC.
		Util::AddFunc(pLua, HttpRequest_SetContent, "SetContent");
		Util::AddFunc(pLua, HttpRequest_SetHeader, "SetHeader");
		Util::AddFunc(pLua, HttpRequest_SetRedirect, "SetRedirect");
		Util::AddFunc(pLua, HttpRequest_SetStatusCode, "SetStatusCode");
	pLua->Pop(1);

	Util::StartTable(pLua);
		Util::AddFunc(pLua, httpserver_Create, "Create");
		pLua->PushNumber(1); pLua->SetField(-2, "SSE_VERSION");
		Util::AddFunc(pLua, httpserver_Destroy, "Destroy");
		Util::AddFunc(pLua, httpserver_GetAll, "GetAll");
		Util::AddFunc(pLua, httpserver_FindByName, "FindByName");
	Util::FinishTable(pLua, "httpserver");
}

void CHTTPServerModule::LuaShutdown(GarrysMod::Lua::ILuaInterface* pLua)
{
	LuaHttpServerModuleData* pLuaData = GetHttpServerLuaData(pLua);
	if (!pLuaData) return;
	pLuaData->shuttingDown = true;
	for (auto& httpserver : pLuaData->pServers)
		httpserver->Stop();

	Util::NukeTable(pLua, "httpserver");

	// Listeners and Lua subscriptions stop at state teardown. Native console history persists.
	DeleteAll_HttpResponse(pLua);
	DeleteAll_HttpRequest(pLua);
	DeleteAll_HttpServer(pLua);

	for (auto& httpserver : pLuaData->pServers)
		delete httpserver;

	if (pLuaData->nProtectedCallRef != -1)
		Util::ReferenceFree(pLua, pLuaData->nProtectedCallRef, "HttpServer - ProtectedCall");
}

void CHTTPServerModule::Shutdown()
{
	g_bHTTPShuttingDown.store(true);
	// LuaShutdown normally destroys these first. Plugin/module shutdown must
	// nevertheless join every worker even when the Lua state is still alive.
	std::vector<HttpServer*> servers;
	{ std::shared_lock<std::shared_mutex> lock(g_pHttpServersMutex); servers.assign(g_pHttpServers.begin(), g_pHttpServers.end()); }
	for (auto* server : servers) {
		bool alive;
		{ std::shared_lock<std::shared_mutex> lock(g_pHttpServersMutex); alive = g_pHttpServers.count(server) != 0; }
		if (alive) server->Stop();
	}
}

bool CHTTPServerModule::CanDisableAtRuntime()
{
	if (!ThreadInMainThread()) return false;
	std::shared_lock<std::shared_mutex> lock(g_pHttpServersMutex);
	for (auto* server : g_pHttpServers) if (server->IsDispatching()) return false;
	return true;
}

void CHTTPServerModule::LuaThink(GarrysMod::Lua::ILuaInterface* pLua)
{
	VPROF_BUDGET("HolyLib - CHTTPServerModule::LuaThink", VPROF_BUDGETGROUP_HOLYLIB);

	LuaHttpServerModuleData* pLuaData = GetHttpServerLuaData(pLua);
	if (!pLuaData)
		return;

	const std::vector<HttpServer*> servers(pLuaData->pServers.begin(), pLuaData->pServers.end());
	for (auto* httpserver : servers)
		if (pLuaData->pServers.count(httpserver)) httpserver->Think();
}
